// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <Operations/Copying.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/States/FilePanels/Actions/InlineRename.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <initializer_list>
#include <optional>
#include <string>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using nc::panel::actions::InlineRenameLiveContext;
using nc::panel::actions::InlineRenamePlanningResult;
using nc::panel::actions::InlineRenameStatus;
using nc::panel::actions::MakeInlineRenameOperation;
using nc::panel::actions::PlanInlineRename;
using nc::panel::actions::RevalidateInlineRenameRuntime;

std::string ASCIILower(std::string _value)
{
    std::ranges::transform(_value, _value.begin(), [](const unsigned char _character) {
        return static_cast<char>(std::tolower(_character));
    });
    return _value;
}

class InlineRenameTestHost final : public nc::vfs::Host
{
public:
    InlineRenameTestHost(const bool _native,
                         const bool _rename_supported,
                         const std::optional<bool> _case_sensitive)
        : Host("/", nullptr, "inline_rename_test"), m_Native{_native}, m_CaseSensitive{_case_sensitive}
    {
        if( _rename_supported )
            AddFeatures(nc::vfs::HostFeatures::Rename);
    }

    bool IsNativeFS() const noexcept override { return m_Native; }
    bool IsWritable() const override { return writable; }
    bool IsWritableAtPath(std::string_view) const override { return writable; }
    bool IsCaseSensitiveAtPath(std::string_view) const override { return m_CaseSensitive.value_or(true); }
    std::optional<bool> CaseSensitivityAtPath(std::string_view) const override { return m_CaseSensitive; }

    std::expected<VFSStat, nc::Error> Stat(std::string_view _path,
                                           unsigned long,
                                           const VFSCancelChecker &) override
    {
        if( const auto found = entries.find(std::string{_path}); found != entries.end() )
            return found->second;
        if( m_CaseSensitive == std::optional{false} ) {
            const std::string folded = ASCIILower(std::string{_path});
            for( const auto &[path, stat] : entries ) {
                if( ASCIILower(path) == folded )
                    return stat;
            }
        }
        return std::unexpected(nc::Error{nc::Error::POSIX, ENOENT});
    }

    void SetCaseSensitivity(const std::optional<bool> _case_sensitive) noexcept
    {
        m_CaseSensitive = _case_sensitive;
    }

    bool writable{true};
    std::unordered_map<std::string, VFSStat> entries;

private:
    bool m_Native{false};
    std::optional<bool> m_CaseSensitive{true};
};

VFSStat EntryStat(const uint64_t _inode, const int32_t _device = 7)
{
    VFSStat stat;
    stat.mode = S_IFREG | S_IRUSR | S_IWUSR;
    stat.inode = _inode;
    stat.dev = _device;
    stat.meaning.mode = 1;
    stat.meaning.inode = 1;
    stat.meaning.dev = 1;
    return stat;
}

struct Fixture final {
    std::shared_ptr<InlineRenameTestHost> host;
    VFSListingPtr listing;
    InlineRenameLiveContext live;
};

Fixture MakeFixture(const bool _native = false,
                    const bool _rename_supported = true,
                    const std::optional<bool> _case_sensitive = true,
                    const std::initializer_list<std::string_view> _filenames = {"Report.txt"})
{
    auto host = std::make_shared<InlineRenameTestHost>(_native, _rename_supported, _case_sensitive);

    nc::vfs::ListingInput input;
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = host;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/folder/";
    input.inodes.reset(nc::base::variable_container<>::type::sparse);
    size_t index = 0;
    for( const std::string_view filename : _filenames ) {
        input.filenames.emplace_back(filename);
        input.unix_modes.emplace_back(S_IFREG | S_IRUSR | S_IWUSR);
        input.unix_types.emplace_back(DT_REG);
        input.inodes.insert(index, 100 + index);
        host->entries.emplace("/folder/" + std::string{filename}, EntryStat(100 + index));
        ++index;
    }

    auto listing = VFSListing::Build(std::move(input));
    return Fixture{
        .host = std::move(host),
        .listing = listing,
        .live = InlineRenameLiveContext{
            .pane_available = true,
            .window_available = true,
            .loading = false,
            .listing_loaded = true,
            .uniform = true,
            .listing = std::move(listing),
            .generation = 41,
            .host = {},
            .directory = "/folder/",
        },
    };
}

void BindHost(Fixture &_fixture)
{
    _fixture.live.host = _fixture.host;
}

InlineRenamePlanningResult Plan(Fixture &_fixture, const std::string_view _name)
{
    BindHost(_fixture);
    return PlanInlineRename(_fixture.live, _fixture.listing->Item(0), _name);
}

} // namespace

#define PREFIX "nc::panel::actions::InlineRename "

TEST_CASE(PREFIX "binds a ready plan to the exact listing generation provider and path")
{
    auto fixture = MakeFixture();
    const auto result = Plan(fixture, "Renamed.txt");

    REQUIRE(result.status == InlineRenameStatus::Ready);
    REQUIRE(result.plan);
    CHECK(result.plan->Source() == fixture.listing->Item(0));
    CHECK(result.plan->Listing() == fixture.listing);
    CHECK(result.plan->Generation() == 41);
    CHECK(result.plan->Host() == fixture.host);
    CHECK(result.plan->Directory() == "/folder/");
    CHECK(result.plan->DestinationName() == "Renamed.txt");
    CHECK(result.plan->DestinationPath() == "/folder/Renamed.txt");
    CHECK(result.plan->CaseSensitive());
    CHECK_FALSE(result.plan->IsCaseOnlyRename());
    CHECK(RevalidateInlineRenameRuntime(*result.plan));
    CHECK(MakeInlineRenameOperation(*result.plan) != nullptr);
}

TEST_CASE(PREFIX "returns typed fail-closed states before planning")
{
    auto fixture = MakeFixture();
    BindHost(fixture);
    const VFSListingItem source = fixture.listing->Item(0);

    fixture.live.pane_available = false;
    CHECK(PlanInlineRename(fixture.live, source, "Renamed.txt").status == InlineRenameStatus::PaneUnavailable);
    fixture.live.pane_available = true;
    fixture.live.window_available = false;
    CHECK(PlanInlineRename(fixture.live, source, "Renamed.txt").status == InlineRenameStatus::WindowUnavailable);
    fixture.live.window_available = true;
    fixture.live.loading = true;
    CHECK(PlanInlineRename(fixture.live, source, "Renamed.txt").status == InlineRenameStatus::Loading);
    fixture.live.loading = false;
    fixture.live.listing_loaded = false;
    CHECK(PlanInlineRename(fixture.live, source, "Renamed.txt").status == InlineRenameStatus::ListingUnavailable);
    fixture.live.listing_loaded = true;

    CHECK(PlanInlineRename(fixture.live, source, "Report.txt").status == InlineRenameStatus::Unchanged);
    CHECK(PlanInlineRename(fixture.live, source, "").status == InlineRenameStatus::InvalidName);
    CHECK(PlanInlineRename(fixture.live, source, ".").status == InlineRenameStatus::InvalidName);
    CHECK(PlanInlineRename(fixture.live, source, "../escape").status == InlineRenameStatus::InvalidName);
    const std::string embedded_nul{"name\0hidden.txt", 15};
    CHECK(PlanInlineRename(fixture.live, source, embedded_nul).status == InlineRenameStatus::InvalidName);

    auto other = MakeFixture();
    CHECK(PlanInlineRename(fixture.live, other.listing->Item(0), "Renamed.txt").status ==
          InlineRenameStatus::StaleSource);
}

TEST_CASE(PREFIX "requires path writability rename capability and authoritative case sensitivity")
{
    auto read_only = MakeFixture();
    read_only.host->writable = false;
    CHECK(Plan(read_only, "Renamed.txt").status == InlineRenameStatus::DestinationReadOnly);

    auto unsupported = MakeFixture(false, false);
    CHECK(Plan(unsupported, "Renamed.txt").status == InlineRenameStatus::ProviderUnsupported);

    auto unknown_case = MakeFixture(false, true, std::nullopt);
    CHECK(Plan(unknown_case, "Renamed.txt").status == InlineRenameStatus::CaseSensitivityUnavailable);
}

TEST_CASE(PREFIX "uses case-aware collision admission and restricts case-only rename to Native")
{
    auto insensitive = MakeFixture(false, true, false, {"Report.txt", "Taken.TXT"});
    CHECK(Plan(insensitive, "taken.txt").status == InlineRenameStatus::DestinationExists);
    CHECK(Plan(insensitive, "REPORT.TXT").status == InlineRenameStatus::UnsafeCaseOnlyRename);

    auto native = MakeFixture(true, true, false, {"Report.txt", "Taken.TXT"});
    const auto native_case_only = Plan(native, "REPORT.TXT");
    REQUIRE(native_case_only.status == InlineRenameStatus::Ready);
    REQUIRE(native_case_only.plan);
    CHECK(native_case_only.plan->IsCaseOnlyRename());
    CHECK(RevalidateInlineRenameRuntime(*native_case_only.plan));

    auto sensitive = MakeFixture(false, true, true, {"Report.txt", "Taken.TXT"});
    CHECK(Plan(sensitive, "taken.txt").status == InlineRenameStatus::Ready);
    CHECK(Plan(sensitive, "Taken.TXT").status == InlineRenameStatus::DestinationExists);
}

TEST_CASE(PREFIX "runtime preflight rejects a late source replacement or removal")
{
    auto fixture = MakeFixture();
    const auto result = Plan(fixture, "Renamed.txt");
    REQUIRE(result.plan);
    REQUIRE(RevalidateInlineRenameRuntime(*result.plan));

    fixture.host->entries["/folder/Report.txt"] = EntryStat(777);
    CHECK_FALSE(RevalidateInlineRenameRuntime(*result.plan));

    fixture.host->entries.erase("/folder/Report.txt");
    CHECK_FALSE(RevalidateInlineRenameRuntime(*result.plan));
}

TEST_CASE(PREFIX "runtime preflight rejects late destination and provider changes")
{
    auto fixture = MakeFixture();
    const auto result = Plan(fixture, "Renamed.txt");
    REQUIRE(result.plan);
    REQUIRE(RevalidateInlineRenameRuntime(*result.plan));

    fixture.host->entries.emplace("/folder/Renamed.txt", EntryStat(777));
    CHECK_FALSE(RevalidateInlineRenameRuntime(*result.plan));
    fixture.host->entries.erase("/folder/Renamed.txt");

    fixture.host->writable = false;
    CHECK_FALSE(RevalidateInlineRenameRuntime(*result.plan));
    fixture.host->writable = true;
    fixture.host->SetCaseSensitivity(false);
    CHECK_FALSE(RevalidateInlineRenameRuntime(*result.plan));
}

TEST_CASE(PREFIX "Native case-only runtime requires the destination alias to identify the exact source")
{
    auto fixture = MakeFixture(true, true, false);
    const auto result = Plan(fixture, "REPORT.TXT");
    REQUIRE(result.plan);
    REQUIRE(RevalidateInlineRenameRuntime(*result.plan));

    fixture.host->entries.emplace("/folder/REPORT.TXT", EntryStat(999));
    CHECK_FALSE(RevalidateInlineRenameRuntime(*result.plan));
}

#undef PREFIX
