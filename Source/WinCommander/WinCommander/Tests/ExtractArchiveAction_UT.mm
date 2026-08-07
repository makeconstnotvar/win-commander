// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <VFS/VFSListingInput.h>
#include <WinCommander/States/FilePanels/Actions/ExtractArchive.h>
#include <cerrno>
#include <sys/dirent.h>
#include <sys/stat.h>

namespace {

class ArchiveSourceIdentityHost final : public nc::vfs::Host
{
public:
    ArchiveSourceIdentityHost() : Host("/", nullptr, "archive_source_identity_test")
    {
        AddFeatures(nc::vfs::HostFeatures::Read);
        stat.mode = S_IFREG | S_IRUSR;
        stat.inode = 41;
        stat.size = 1024;
        stat.mtime = {.tv_sec = 1'700'000'000, .tv_nsec = 123};
        stat.meaning.mode = 1;
        stat.meaning.inode = 1;
        stat.meaning.size = 1;
        stat.meaning.mtime = 1;
    }

    std::expected<VFSStat, nc::Error> Stat(std::string_view,
                                           unsigned long,
                                           const VFSCancelChecker &) override
    {
        if( failure )
            return std::unexpected(*failure);
        return stat;
    }

    VFSStat stat;
    std::optional<nc::Error> failure;
};

VFSListingItem ArchiveItem(const std::shared_ptr<ArchiveSourceIdentityHost> &_host)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/source/";
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = _host;
    input.filenames.emplace_back("payload.zip");
    input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
    input.unix_types.emplace_back(DT_REG);
    return VFSListing::Build(std::move(input))->Item(0);
}

} // namespace

#define PREFIX "nc::panel::actions::ExtractArchive "

TEST_CASE(PREFIX "captures and revalidates exact inode size and modification time")
{
    const auto host = std::make_shared<ArchiveSourceIdentityHost>();
    const VFSListingItem source = ArchiveItem(host);
    const auto identity = nc::panel::actions::ArchiveExtractionSourceIdentity::Capture(source);
    REQUIRE(identity);
    CHECK(identity->inode == 41);
    CHECK(identity->size == 1024);
    CHECK(identity->modification_seconds == 1'700'000'000);
    CHECK(identity->modification_nanoseconds == 123);
    CHECK(identity->Matches(source));

    ++host->stat.inode;
    CHECK_FALSE(identity->Matches(source));
    --host->stat.inode;
    ++host->stat.size;
    CHECK_FALSE(identity->Matches(source));
    --host->stat.size;
    ++host->stat.mtime.tv_nsec;
    CHECK_FALSE(identity->Matches(source));
}

TEST_CASE(PREFIX "fails closed when source identity evidence is incomplete or not a regular file")
{
    const auto host = std::make_shared<ArchiveSourceIdentityHost>();
    const VFSListingItem source = ArchiveItem(host);

    host->stat.meaning.inode = 0;
    CHECK_FALSE(nc::panel::actions::ArchiveExtractionSourceIdentity::Capture(source));
    host->stat.meaning.inode = 1;
    host->stat.mode = S_IFLNK | S_IRUSR;
    CHECK_FALSE(nc::panel::actions::ArchiveExtractionSourceIdentity::Capture(source));
    host->stat.mode = S_IFREG | S_IRUSR;
    host->failure = nc::Error{nc::Error::POSIX, EACCES};
    CHECK_FALSE(nc::panel::actions::ArchiveExtractionSourceIdentity::Capture(source));
}

TEST_CASE(PREFIX "rejects depth 128 before copying its ancestor vector")
{
    using Budget = nc::panel::actions::ArchiveExtractionTraversalBudget;
    Budget budget;
    CHECK(budget.Admit(Budget::MaximumDepth - 1, 126, 1) == Budget::Admission::Accepted);
    CHECK(budget.Entries() == 1);
    CHECK(budget.Components() == Budget::MaximumDepth);
    CHECK(budget.NameBytes() == Budget::MaximumDepth);

    CHECK(budget.Admit(Budget::MaximumDepth, 127, 1) == Budget::Admission::DepthExceeded);
    CHECK(budget.Entries() == 1);
    CHECK(budget.Components() == Budget::MaximumDepth);
    CHECK(budget.NameBytes() == Budget::MaximumDepth);
}

TEST_CASE(PREFIX "bounds aggregate entries components and path bytes without partial reservation")
{
    using Budget = nc::panel::actions::ArchiveExtractionTraversalBudget;

    Budget entries;
    bool accepted = true;
    for( size_t index = 0; index < Budget::MaximumEntries; ++index )
        accepted = accepted && entries.Admit(0, 0, 0) == Budget::Admission::Accepted;
    REQUIRE(accepted);
    CHECK(entries.Admit(0, 0, 0) == Budget::Admission::CapacityExceeded);
    CHECK(entries.Entries() == Budget::MaximumEntries);

    Budget components;
    accepted = true;
    constexpr size_t parent_depth = 4;
    constexpr size_t components_per_entry = parent_depth + 1;
    constexpr size_t component_entries = Budget::MaximumComponents / components_per_entry;
    for( size_t index = 0; index < component_entries; ++index )
        accepted = accepted && components.Admit(parent_depth, 0, 0) == Budget::Admission::Accepted;
    REQUIRE(accepted);
    CHECK(components.Admit(parent_depth, 0, 0) == Budget::Admission::CapacityExceeded);
    CHECK(components.Components() == component_entries * components_per_entry);

    Budget bytes;
    CHECK(bytes.Admit(0, 0, Budget::MaximumNameBytes) == Budget::Admission::Accepted);
    CHECK(bytes.Admit(0, 0, 1) == Budget::Admission::CapacityExceeded);
    CHECK(bytes.NameBytes() == Budget::MaximumNameBytes);
}

#undef PREFIX
