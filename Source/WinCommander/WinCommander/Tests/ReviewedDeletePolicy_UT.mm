// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <Base/Error.h>
#include <Operations/OperationPlan.h>
#include <Operations/VFSOperationPlanningProbes.h>
#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/Core/Operations/ReviewedDeleteApplicationBoundary.h>
#include <WinCommander/States/FilePanels/Actions/Delete.h>

#include <memory>
#include <sys/dirent.h>
#include <sys/stat.h>

namespace {

using nc::ops::OperationPlan;
using nc::ops::OperationPlanInput;
using nc::ops::OperationPlanningAccessEvidence;
using nc::ops::OperationPlanningAccessState;
using nc::ops::OperationPlanningRequiredAccess;
using nc::ops::OperationPlanType;
using nc::ops::VFSBoundOperationPreflight;
using nc::ops::VFSOperationPlanningBindings;
using nc::ops::VFSOperationPlanningProbes;
using nc::panel::actions::reviewed_delete::PrepareReviewedDeleteApplicationBoundary;
using nc::panel::actions::reviewed_delete::Select;
using nc::panel::actions::reviewed_delete::SelectBatch;
using nc::panel::actions::reviewed_delete::Selection;
using nc::vfs::ListingItem;

class ReviewedDeleteTestHost final : public nc::vfs::Host
{
public:
    explicit ReviewedDeleteTestHost(const bool _native,
                                    const nc::vfs::ProviderConditionalDeletePathSupport _path_support =
                                        nc::vfs::ProviderConditionalDeletePathSupport::SameVolumeUnlink)
        : Host("/", nullptr, _native ? "reviewed_delete_native" : "reviewed_delete_remote"), m_Native(_native),
          m_PathSupport(_path_support)
    {
        AddFeatures(nc::vfs::HostFeatures::Read | nc::vfs::HostFeatures::CreateFile |
                    nc::vfs::HostFeatures::CreateDirectory | nc::vfs::HostFeatures::Unlink |
                    nc::vfs::HostFeatures::RemoveDirectory | nc::vfs::HostFeatures::CreateSymlink);
    }

    bool IsNativeFS() const noexcept override { return m_Native; }
    bool IsWritable() const override { return true; }
    bool IsWritableAtPath(std::string_view) const override { return true; }
    bool IsCaseSensitiveAtPath(std::string_view) const override { return true; }
    std::optional<bool> CaseSensitivityAtPath(std::string_view) const override { return true; }
    std::optional<std::string> SemanticNamespaceIdentity() const override
    {
        return "reviewed-delete-policy-test-namespace";
    }
    nc::vfs::ProviderConditionalDeletePathSupport ConditionalDeletePathSupport(std::string_view) const
        noexcept override
    {
        return m_PathSupport;
    }
    std::expected<VFSStat, nc::Error> Stat(std::string_view _path, unsigned long, const VFSCancelChecker &) override
    {
        if( _path == "/source" ) {
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
    nc::vfs::ProviderConditionalDeletePathSupport m_PathSupport;
};

ListingItem NamedItem(const std::shared_ptr<ReviewedDeleteTestHost> &_host,
                      const std::string &_directory,
                      const std::string &_filename,
                      const mode_t _mode = S_IFREG | S_IRUSR | S_IWUSR,
                      const uint8_t _type = DT_REG)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = _directory;
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = _host;
    input.filenames.emplace_back(_filename);
    input.unix_modes.emplace_back(_mode);
    input.unix_types.emplace_back(_type);
    return VFSListing::Build(std::move(input))->Item(0);
}

ListingItem Item(const std::shared_ptr<ReviewedDeleteTestHost> &_host,
                 const mode_t _mode = S_IFREG | S_IRUSR | S_IWUSR,
                 const uint8_t _type = DT_REG)
{
    return NamedItem(_host, "/source/", "source.txt", _mode, _type);
}

/** An accepted single-item Delete, reviewed - the shape `Delete Permanently` produces. */
VFSBoundOperationPreflight DeleteBoundaryPreflight()
{
    const auto host = std::make_shared<ReviewedDeleteTestHost>(true);
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
        .plan_id = "reviewed-delete-boundary",
        .type = OperationPlanType::PermanentDelete,
        .sources = {{"native", "/source/source.txt"}},
        .destination = std::nullopt,
        .conflict_policy = std::nullopt,
        .created_at = OperationPlan::TimePoint{std::chrono::seconds{1}},
    });
    REQUIRE(plan);
    return probes->Preflight(std::move(*plan));
}

} // namespace

#define PREFIX "reviewed Delete policy "

TEST_CASE(PREFIX "accepts a native regular file whose provider supports a conditional unlink",
          "[reviewed-delete-policy]")
{
    const auto host = std::make_shared<ReviewedDeleteTestHost>(true);
    const auto item = Item(host);

    CHECK(Select(item) == Selection::Reviewed);
}

TEST_CASE(PREFIX "rejects provider and item shapes outside the reviewed lifecycle", "[reviewed-delete-policy]")
{
    const auto native = std::make_shared<ReviewedDeleteTestHost>(true);
    const auto remote = std::make_shared<ReviewedDeleteTestHost>(false);

    CHECK(Select(Item(remote)) == Selection::Legacy);
    CHECK(Select(Item(native, S_IFDIR | S_IRUSR | S_IWUSR, DT_DIR)) == Selection::Legacy);
    CHECK(Select({}) == Selection::Legacy);
}

TEST_CASE(PREFIX "distinguishes known unsupported storage from unavailable eligibility evidence",
          "[reviewed-delete-policy]")
{
    using nc::vfs::ProviderConditionalDeletePathSupport;
    const auto unsupported =
        std::make_shared<ReviewedDeleteTestHost>(true, ProviderConditionalDeletePathSupport::Unsupported);
    const auto unavailable =
        std::make_shared<ReviewedDeleteTestHost>(true, ProviderConditionalDeletePathSupport::Unavailable);

    CHECK(Select(Item(unsupported)) == Selection::Legacy);
    CHECK(Select(Item(unavailable)) == Selection::Reject);
}

TEST_CASE(PREFIX "answers for a whole selection at once, and lets no refusal be downgraded",
          "[reviewed-delete-policy]")
{
    const auto host = std::make_shared<ReviewedDeleteTestHost>(true);
    const auto unavailable_host =
        std::make_shared<ReviewedDeleteTestHost>(true, nc::vfs::ProviderConditionalDeletePathSupport::Unavailable);

    // The common shape: several siblings out of the same source folder, deleted as one batch.
    const std::vector<ListingItem> two{NamedItem(host, "/source/", "first.txt"),
                                       NamedItem(host, "/source/", "second.txt")};
    CHECK(SelectBatch(two) == Selection::Reviewed);

    // One item the reviewed engine cannot take sends the whole set to the legacy operation.
    const std::vector<ListingItem> mixed{NamedItem(host, "/source/", "first.txt"),
                                         Item(host, S_IFDIR | S_IRUSR | S_IWUSR, DT_DIR)};
    CHECK(SelectBatch(mixed) == Selection::Legacy);

    // A provider that cannot answer the eligibility question outranks a legacy sibling, and is found
    // even when it comes after one.
    const std::vector<ListingItem> legacy_then_reject{Item(host, S_IFDIR | S_IRUSR | S_IWUSR, DT_DIR),
                                                       NamedItem(unavailable_host, "/source/", "second.txt")};
    CHECK(SelectBatch(legacy_then_reject) == Selection::Reject);

    CHECK(SelectBatch({}) == Selection::Legacy);
}

TEST_CASE(PREFIX "app boundary accepts an accepted Delete plan, with no destination to name",
          "[reviewed-delete-app-boundary]")
{
    auto prepared = PrepareReviewedDeleteApplicationBoundary(DeleteBoundaryPreflight(), true, true);
    REQUIRE(prepared);
    CHECK(prepared->Presentation().plan_id == "reviewed-delete-boundary");
    CHECK(prepared->Presentation().items.size() == 1);
    CHECK(prepared->Presentation().items.front().source_path == "/source/source.txt");
}

TEST_CASE(PREFIX "app boundary refuses without a durable runtime or a current intent",
          "[reviewed-delete-app-boundary]")
{
    using nc::panel::actions::reviewed_delete::PreparationErrorCode;

    const auto unpersisted = PrepareReviewedDeleteApplicationBoundary(DeleteBoundaryPreflight(), false, true);
    REQUIRE_FALSE(unpersisted);
    CHECK(unpersisted.error().code == PreparationErrorCode::UnpersistedRuntime);

    const auto stale = PrepareReviewedDeleteApplicationBoundary(DeleteBoundaryPreflight(), true, false);
    REQUIRE_FALSE(stale);
    CHECK(stale.error().code == PreparationErrorCode::StaleIntent);
}

TEST_CASE(PREFIX "app boundary is single-use - a second Approve after the first cannot mint a second authority",
          "[reviewed-delete-app-boundary]")
{
    using nc::panel::actions::reviewed_delete::ApprovalResult;
    using nc::panel::actions::reviewed_delete::PreparedReview;

    auto prepared = PrepareReviewedDeleteApplicationBoundary(DeleteBoundaryPreflight(), true, true);
    REQUIRE(prepared);

    nc::core::OperationSubmissionGate gate;
    const auto declined = prepared->Approve(
        false, [] { return true; }, gate, {});
    CHECK(declined == ApprovalResult::Declined);

    // Consumed by the first call regardless of its outcome - a duplicate UI callback must not be able
    // to mint a second authority from the same bound preflight.
    const auto second = prepared->Approve(
        true, [] { return true; }, gate, {});
    CHECK(second == ApprovalResult::AlreadyConsumed);
}
