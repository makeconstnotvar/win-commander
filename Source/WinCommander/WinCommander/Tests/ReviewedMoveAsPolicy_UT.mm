// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <Base/Error.h>
#include <Operations/CopyingOptions.h>
#include <Operations/OperationPlan.h>
#include <Operations/VFSOperationPlanningProbes.h>
#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/Core/Operations/ReviewedCopyAsApplicationBoundary.h>
#include <WinCommander/States/FilePanels/Actions/CopyFile.h>

#include <memory>
#include <sys/dirent.h>
#include <sys/stat.h>

namespace {

using nc::ops::CopyingOptions;
using nc::ops::OperationPlan;
using nc::ops::OperationPlanConflictDecision;
using nc::ops::OperationPlanConflictPolicy;
using nc::ops::OperationPlanConflictScope;
using nc::ops::OperationPlanDestinationInput;
using nc::ops::OperationPlanDestinationKind;
using nc::ops::OperationPlanInput;
using nc::ops::OperationPlanningAccessEvidence;
using nc::ops::OperationPlanningAccessState;
using nc::ops::OperationPlanningRequiredAccess;
using nc::ops::OperationPlanType;
using nc::ops::VFSBoundOperationPreflight;
using nc::ops::VFSOperationPlanningBindings;
using nc::ops::VFSOperationPlanningProbes;
using nc::panel::actions::reviewed_copy_as::PrepareReviewedCopyApplicationBoundary;
using nc::panel::actions::reviewed_move::Select;
using nc::panel::actions::reviewed_move::Selection;
using nc::vfs::ListingItem;

class ReviewedMoveAsTestHost final : public nc::vfs::Host
{
public:
    explicit ReviewedMoveAsTestHost(const bool _native,
                                    const nc::vfs::ProviderConditionalMovePathSupport _path_support =
                                        nc::vfs::ProviderConditionalMovePathSupport::SameVolumeRename)
        : Host("/", nullptr, _native ? "reviewed_move_as_native" : "reviewed_move_as_remote"), m_Native(_native),
          m_PathSupport(_path_support)
    {
        AddFeatures(nc::vfs::HostFeatures::Read | nc::vfs::HostFeatures::CreateFile |
                    nc::vfs::HostFeatures::CreateDirectory | nc::vfs::HostFeatures::Unlink |
                    nc::vfs::HostFeatures::RemoveDirectory | nc::vfs::HostFeatures::CreateSymlink |
                    nc::vfs::HostFeatures::Rename);
    }

    bool IsNativeFS() const noexcept override { return m_Native; }
    bool IsWritable() const override { return true; }
    bool IsWritableAtPath(std::string_view) const override { return true; }
    bool IsCaseSensitiveAtPath(std::string_view) const override { return true; }
    std::optional<bool> CaseSensitivityAtPath(std::string_view) const override { return true; }
    std::optional<std::string> SemanticNamespaceIdentity() const override
    {
        return "reviewed-move-as-policy-test-namespace";
    }
    nc::vfs::ProviderConditionalMovePathSupport ConditionalMovePathSupport(std::string_view,
                                                                           std::string_view) const noexcept override
    {
        return m_PathSupport;
    }
    std::expected<VFSStat, nc::Error> Stat(std::string_view _path, unsigned long, const VFSCancelChecker &) override
    {
        if( _path == "/source" || _path == "/destination" || _path == "/other" ) {
            VFSStat stat;
            stat.mode = S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR;
            stat.meaning.mode = 1;
            return stat;
        }
        if( _path == "/source/source.txt" ) {
            VFSStat stat;
            stat.mode = S_IFREG | S_IRUSR | S_IWUSR;
            stat.size = 42;
            stat.meaning.mode = 1;
            stat.meaning.size = 1;
            return stat;
        }
        return std::unexpected(nc::Error{nc::Error::POSIX, ENOENT});
    }
    std::expected<VFSStatFS, nc::Error> StatFS(std::string_view, const VFSCancelChecker &) override
    {
        return VFSStatFS{.total_bytes = 8'192, .free_bytes = 4'096, .avail_bytes = 4'096};
    }
    std::expected<void, nc::Error> IterateDirectoryListing(std::string_view,
                                                           const std::function<bool(const VFSDirEnt &)> &) override
    {
        return {};
    }

private:
    bool m_Native;
    nc::vfs::ProviderConditionalMovePathSupport m_PathSupport;
};

ListingItem Item(const std::shared_ptr<ReviewedMoveAsTestHost> &_host,
                 const mode_t _mode = S_IFREG | S_IRUSR | S_IWUSR,
                 const uint8_t _type = DT_REG)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/source/";
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = _host;
    input.filenames.emplace_back("source.txt");
    input.unix_modes.emplace_back(_mode);
    input.unix_types.emplace_back(_type);
    return VFSListing::Build(std::move(input))->Item(0);
}

CopyingOptions MoveOptions()
{
    CopyingOptions options;
    options.docopy = false;
    return options;
}

/** An accepted single-item Move, reviewed - the shape `Move As` produces. */
VFSBoundOperationPreflight MoveBoundaryPreflight()
{
    const auto host = std::make_shared<ReviewedMoveAsTestHost>(true);
    auto bindings = VFSOperationPlanningBindings::Create({{"native", host}});
    REQUIRE(bindings);
    auto probes = VFSOperationPlanningProbes::Create(
        *bindings,
        [](const nc::ops::OperationPlanningPath &,
           const OperationPlanningRequiredAccess,
           nc::vfs::Host &) -> nc::ops::OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    REQUIRE(probes);
    auto plan = OperationPlan::Create({
        .plan_id = "reviewed-move-as-boundary",
        .type = OperationPlanType::Move,
        .sources = {{"native", "/source/source.txt"}},
        .destination =
            OperationPlanDestinationInput{"native", "/source/moved.txt", OperationPlanDestinationKind::ExactItem},
        .conflict_policy =
            OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem},
        .created_at = OperationPlan::TimePoint{std::chrono::seconds{1}},
    });
    REQUIRE(plan);
    return probes->Preflight(std::move(*plan));
}

} // namespace

#define PREFIX "reviewed MoveAs policy "

TEST_CASE(PREFIX "accepts only the exact default single-file rename-within-directory shape",
          "[reviewed-move-to-policy]")
{
    const auto host = std::make_shared<ReviewedMoveAsTestHost>(true);
    const auto item = Item(host);
    const auto options = MoveOptions();

    CHECK(Select(item, "/source/moved.txt", host, options) == Selection::Reviewed);

    const auto other_native_host = std::make_shared<ReviewedMoveAsTestHost>(true);
    CHECK(Select(item, "/source/moved.txt", other_native_host, options) == Selection::Legacy);
    CHECK(Select(item, "moved.txt", host, options) == Selection::Legacy);
}

TEST_CASE(PREFIX "reviews only a move that stays within the item's own directory", "[reviewed-move-to-policy]")
{
    // `Move As` keeps the same narrower-than-the-provider scope `Copy As` keeps: `Move To`, which would
    // need a folder destination, does not exist yet, so nothing here should claim more than the exact
    // same-directory rename `Move As` actually offers.
    const auto host = std::make_shared<ReviewedMoveAsTestHost>(true);
    const auto item = Item(host);
    const auto options = MoveOptions();

    CHECK(Select(item, "/other/moved.txt", host, options) == Selection::Legacy);
}

TEST_CASE(PREFIX "rejects provider and item shapes outside the reviewed lifecycle", "[reviewed-move-to-policy]")
{
    const auto native = std::make_shared<ReviewedMoveAsTestHost>(true);
    const auto remote = std::make_shared<ReviewedMoveAsTestHost>(false);
    const auto options = MoveOptions();

    CHECK(Select(Item(remote), "/source/moved.txt", remote, options) == Selection::Legacy);
    CHECK(Select(Item(native, S_IFDIR | S_IRUSR | S_IWUSR, DT_DIR), "/source/moved.txt", native, options) ==
          Selection::Legacy);
    CHECK(Select(Item(native), "/source/moved.txt", {}, options) == Selection::Legacy);
}

TEST_CASE(PREFIX "rejects a Copy-shaped options object, and a Move-shaped one Copy As would refuse in turn",
          "[reviewed-move-to-policy]")
{
    const auto host = std::make_shared<ReviewedMoveAsTestHost>(true);
    const auto item = Item(host);

    CopyingOptions copy_shaped;
    copy_shaped.docopy = true;
    CHECK(Select(item, "/source/moved.txt", host, copy_shaped) == Selection::Legacy);

    auto move_shaped = MoveOptions();
    move_shaped.disable_system_caches = true;
    CHECK(Select(item, "/source/moved.txt", host, move_shaped) == Selection::Legacy);
}

TEST_CASE(PREFIX "distinguishes known unsupported storage from unavailable eligibility evidence",
          "[reviewed-move-to-policy]")
{
    using nc::vfs::ProviderConditionalMovePathSupport;
    const auto unsupported =
        std::make_shared<ReviewedMoveAsTestHost>(true, ProviderConditionalMovePathSupport::Unsupported);
    const auto unavailable =
        std::make_shared<ReviewedMoveAsTestHost>(true, ProviderConditionalMovePathSupport::Unavailable);
    const auto options = MoveOptions();

    CHECK(Select(Item(unsupported), "/source/moved.txt", unsupported, options) == Selection::Legacy);
    CHECK(Select(Item(unavailable), "/source/moved.txt", unavailable, options) == Selection::Reject);
}

TEST_CASE(PREFIX "app boundary accepts an accepted Move plan the same way it accepts a Copy one",
          "[reviewed-move-as-app-boundary]")
{
    auto prepared = PrepareReviewedCopyApplicationBoundary(MoveBoundaryPreflight(), true, true);
    REQUIRE(prepared);
    CHECK(prepared->Presentation().items.size() == 1);
    CHECK(prepared->Presentation().items.front().source_path == "/source/source.txt");
    CHECK(prepared->Presentation().items.front().destination_path == "/source/moved.txt");
}
