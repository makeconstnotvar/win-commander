// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <Base/Error.h>
#include <Operations/CopyingOptions.h>
#include <Operations/Job.h>
#include <Operations/OperationPlan.h>
#include <Operations/Pool.h>
#include <Operations/VFSOperationPlanningProbes.h>
#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/Core/Operations/OperationSubmissionGate.h>
#include <WinCommander/Core/Operations/ReviewedCopyAsApplicationBoundary.h>
#include <WinCommander/States/FilePanels/Actions/CopyFile.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <thread>
#include <vector>

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
using nc::ops::OperationPlanningBlockerCode;
using nc::ops::OperationPlanningRequiredAccess;
using nc::ops::OperationPlanType;
using nc::ops::VFSBoundOperationPreflight;
using nc::ops::VFSOperationPlanningBindings;
using nc::ops::VFSOperationPlanningProbes;
using nc::panel::actions::reviewed_copy_as::ApprovalResult;
using nc::panel::actions::reviewed_copy_as::PreparationErrorCode;
using nc::panel::actions::reviewed_copy_as::PreparedReview;
using nc::panel::actions::reviewed_copy_as::PrepareReviewedCopyApplicationBoundary;
using nc::panel::actions::reviewed_copy_as::Select;
using nc::panel::actions::reviewed_copy_as::Selection;
using nc::vfs::ListingItem;

class ReviewedCopyAsTestHost final : public nc::vfs::Host
{
public:
    explicit ReviewedCopyAsTestHost(const bool _native,
                                    const nc::vfs::ProviderConditionalCopyPathSupport _path_support =
                                        nc::vfs::ProviderConditionalCopyPathSupport::SameVolumeClone)
        : Host("/", nullptr, _native ? "reviewed_copy_as_native" : "reviewed_copy_as_remote"), m_Native(_native),
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
        return "reviewed-copy-as-policy-test-namespace";
    }
    nc::vfs::ProviderConditionalCopyPathSupport ConditionalCopyPathSupport(std::string_view,
                                                                           std::string_view) const noexcept override
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
        if( m_SourceExists && _path == "/source/source.txt" ) {
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

    void SetSourceExists(const bool _exists) noexcept { m_SourceExists = _exists; }

private:
    bool m_Native;
    nc::vfs::ProviderConditionalCopyPathSupport m_PathSupport;
    bool m_SourceExists = true;
};

ListingItem Item(const std::shared_ptr<ReviewedCopyAsTestHost> &_host,
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

VFSBoundOperationPreflight BoundaryPreflight(const bool _source_exists = true)
{
    const auto host = std::make_shared<ReviewedCopyAsTestHost>(true);
    host->SetSourceExists(_source_exists);
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
        .plan_id = "reviewed-copy-as-boundary",
        .type = OperationPlanType::Copy,
        .sources = {{"native", "/source/source.txt"}},
        .destination =
            OperationPlanDestinationInput{"native", "/source/copy.txt", OperationPlanDestinationKind::ExactItem},
        .conflict_policy =
            OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem},
        .created_at = OperationPlan::TimePoint{std::chrono::seconds{1}},
    });
    REQUIRE(plan);
    return probes->Preflight(std::move(*plan));
}

PreparedReview::SubmissionPort CountingSubmissionPort(int &_submissions)
{
    return {
        .dispatch_to_ui = [](std::function<void()> _task) { _task(); },
        .present_durable_outcome = [](nc::ops::CopyOperationDurableTerminalOutcome) {},
        .submit = [&_submissions](nc::ops::ReviewedVFSOperationPreflight,
                                  std::shared_ptr<nc::core::OperationSubmissionGate::Ticket>,
                                  nc::ops::CopyOperationSubmissionHooks,
                                  std::shared_ptr<std::atomic_bool>) { ++_submissions; },
    };
}

} // namespace

#define PREFIX "reviewed CopyAs policy "

TEST_CASE(PREFIX "accepts only the exact default single-file Native shape")
{
    const auto host = std::make_shared<ReviewedCopyAsTestHost>(true);
    const auto item = Item(host);
    CopyingOptions options;

    CHECK(Select(item, "/source/copy.txt", host, options) == Selection::Reviewed);

    options.verification = CopyingOptions::ChecksumVerification::WhenMoves;
    CHECK(Select(item, "/source/copy.txt", host, options) == Selection::Reviewed);

    const auto other_native_host = std::make_shared<ReviewedCopyAsTestHost>(true);
    CHECK(Select(item, "/source/copy.txt", other_native_host, options) == Selection::Legacy);
    CHECK(Select(item, "/other/copy.txt", host, options) == Selection::Legacy);
    CHECK(Select(item, "copy.txt", host, options) == Selection::Legacy);
}

TEST_CASE(PREFIX "rejects provider and item shapes outside the reviewed lifecycle")
{
    const auto native = std::make_shared<ReviewedCopyAsTestHost>(true);
    const auto remote = std::make_shared<ReviewedCopyAsTestHost>(false);
    CopyingOptions options;

    CHECK(Select(Item(remote), "/source/copy.txt", remote, options) == Selection::Legacy);
    CHECK(Select(Item(native, S_IFDIR | S_IRUSR | S_IWUSR, DT_DIR), "/source/copy.txt", native, options) ==
          Selection::Legacy);
    CHECK(Select(Item(native), "/source/copy.txt", {}, options) == Selection::Legacy);
}

TEST_CASE(PREFIX "rejects copy preferences that the clone-only product does not consume")
{
    const auto host = std::make_shared<ReviewedCopyAsTestHost>(true);
    const auto item = Item(host);
    CopyingOptions options;

    options.verification = CopyingOptions::ChecksumVerification::Always;
    CHECK(Select(item, "/source/copy.txt", host, options) == Selection::Legacy);

    options = {};
    options.disable_system_caches = true;
    CHECK(Select(item, "/source/copy.txt", host, options) == Selection::Legacy);

    options = {};
    options.copy_xattrs = false;
    CHECK(Select(item, "/source/copy.txt", host, options) == Selection::Legacy);

    options = {};
    options.exist_behavior = CopyingOptions::ExistBehavior::KeepBoth;
    CHECK(Select(item, "/source/copy.txt", host, options) == Selection::Legacy);
}

TEST_CASE(PREFIX "distinguishes known unsupported storage from unavailable eligibility evidence")
{
    using nc::vfs::ProviderConditionalCopyPathSupport;
    const auto unsupported =
        std::make_shared<ReviewedCopyAsTestHost>(true, ProviderConditionalCopyPathSupport::Unsupported);
    const auto unavailable =
        std::make_shared<ReviewedCopyAsTestHost>(true, ProviderConditionalCopyPathSupport::Unavailable);
    CopyingOptions options;

    CHECK(Select(Item(unsupported), "/source/copy.txt", unsupported, options) == Selection::Legacy);
    CHECK(Select(Item(unavailable), "/source/copy.txt", unavailable, options) == Selection::Reject);
}

TEST_CASE(PREFIX "keeps clone and staged reviewed Copy scopes distinct")
{
    using nc::vfs::ProviderConditionalCopyPathSupport;
    const auto clone =
        std::make_shared<ReviewedCopyAsTestHost>(true, ProviderConditionalCopyPathSupport::SameVolumeClone);
    const auto staged =
        std::make_shared<ReviewedCopyAsTestHost>(true, ProviderConditionalCopyPathSupport::CrossVolumeStaged);
    CopyingOptions options;

    CHECK(Select(Item(clone), "/source/copy.txt", clone, options) == Selection::Reviewed);
    CHECK(Select(Item(clone), "/different-volume/copy.txt", clone, options) == Selection::Legacy);
    CHECK(Select(Item(staged), "/different-volume/copy.txt", staged, options) == Selection::Reviewed);
}

TEST_CASE(PREFIX "submission gate accounts for every admission ticket")
{
    nc::core::OperationSubmissionGate gate;
    CHECK_FALSE(gate.HasPending());
    CHECK(gate.PendingCount() == 0);

    auto first = gate.Acquire();
    auto second = gate.Acquire();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(gate.HasPending());
    CHECK(gate.PendingCount() == 2);

    first.reset();
    CHECK(gate.PendingCount() == 1);
    second.reset();
    CHECK_FALSE(gate.HasPending());
}

TEST_CASE(PREFIX "submission gate cancels monotonically and waits for owners")
{
    using namespace std::chrono_literals;

    nc::core::OperationSubmissionGate gate;
    auto ticket = gate.Acquire();
    REQUIRE(ticket);

    auto cancellation = std::async(std::launch::async, [&gate] { gate.CancelAndWait(); });
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while( !ticket->IsCancelled() && std::chrono::steady_clock::now() < deadline )
        std::this_thread::yield();

    CHECK(ticket->IsCancelled());
    CHECK_FALSE(gate.Acquire());
    CHECK(cancellation.wait_for(0ms) == std::future_status::timeout);

    ticket.reset();
    CHECK(cancellation.wait_for(1s) == std::future_status::ready);
    cancellation.get();
    CHECK_FALSE(gate.Acquire());
}

TEST_CASE(PREFIX "app boundary keeps blocked stale and unpersisted preflight out of the submission port",
          "[reviewed-copy-as-app-boundary]")
{
    int submissions = 0;

    const auto blocked = PrepareReviewedCopyApplicationBoundary(BoundaryPreflight(false), true, true);
    REQUIRE_FALSE(blocked);
    CHECK(blocked.error().code == PreparationErrorCode::BlockedPreflight);
    REQUIRE(blocked.error().blocker);
    CHECK(*blocked.error().blocker == OperationPlanningBlockerCode::SourceMissing);

    const auto stale = PrepareReviewedCopyApplicationBoundary(BoundaryPreflight(), true, false);
    REQUIRE_FALSE(stale);
    CHECK(stale.error().code == PreparationErrorCode::StaleIntent);

    const auto unpersisted = PrepareReviewedCopyApplicationBoundary(BoundaryPreflight(), false, true);
    REQUIRE_FALSE(unpersisted);
    CHECK(unpersisted.error().code == PreparationErrorCode::UnpersistedRuntime);

    CHECK(submissions == 0);
}

TEST_CASE(PREFIX "app boundary rechecks approval intent and cancellation before its sole submission port",
          "[reviewed-copy-as-app-boundary]")
{
    int submissions = 0;

    SECTION("review decline")
    {
        auto prepared = PrepareReviewedCopyApplicationBoundary(BoundaryPreflight(), true, true);
        REQUIRE(prepared);
        nc::core::OperationSubmissionGate gate;
        CHECK(prepared->Approve(
                  false, [] { return true; }, gate, CountingSubmissionPort(submissions)) == ApprovalResult::Declined);
    }

    SECTION("intent changed after the review was shown")
    {
        auto prepared = PrepareReviewedCopyApplicationBoundary(BoundaryPreflight(), true, true);
        REQUIRE(prepared);
        nc::core::OperationSubmissionGate gate;
        CHECK(prepared->Approve(
                  true, [] { return false; }, gate, CountingSubmissionPort(submissions)) ==
              ApprovalResult::StaleIntent);
    }

    SECTION("window submission gate was cancelled")
    {
        auto prepared = PrepareReviewedCopyApplicationBoundary(BoundaryPreflight(), true, true);
        REQUIRE(prepared);
        nc::core::OperationSubmissionGate gate;
        gate.CancelAndWait();
        CHECK(prepared->Approve(
                  true, [] { return true; }, gate, CountingSubmissionPort(submissions)) == ApprovalResult::Cancelled);
    }

    CHECK(submissions == 0);
}

TEST_CASE(PREFIX "app boundary presents the exact accepted plan before approval", "[reviewed-copy-as-app-boundary]")
{
    auto prepared = PrepareReviewedCopyApplicationBoundary(BoundaryPreflight(), true, true);
    REQUIRE(prepared);

    const auto &presentation = prepared->Presentation();
    CHECK(presentation.plan_id == "reviewed-copy-as-boundary");
    CHECK(presentation.source_path == "/source/source.txt");
    CHECK(presentation.destination_path == "/source/copy.txt");
    CHECK(presentation.conflict_decision == OperationPlanConflictDecision::Ask);
    CHECK(presentation.conflict_scope == OperationPlanConflictScope::ThisItem);
    CHECK(presentation.estimated_files == 1);
    CHECK(presentation.estimated_bytes == 42);
    CHECK(presentation.available_bytes == 4'096);
    CHECK(presentation.provider_evidence_count == 2);
    CHECK(presentation.item_evidence_count == 3);
    CHECK(presentation.name_evidence_count == 1);
    CHECK(presentation.access_evidence_count == 2);
    CHECK_FALSE(presentation.warnings.empty());
}

TEST_CASE(PREFIX "app boundary dispatches one owning durable failure before non-success Pool release",
          "[reviewed-copy-as-app-boundary]")
{
    struct ImmediateJob final : nc::ops::Job {
        void Perform() override { SetCompleted(); }
    };
    struct ImmediateOperation final : nc::ops::Operation {
        ~ImmediateOperation() override { Wait(); }
        nc::ops::Job *GetJob() noexcept override { return &job; }
        ImmediateJob job;
    };
    struct Evidence final {
        std::mutex lock;
        std::vector<std::string> events;
        std::vector<std::function<void()>> ui_tasks;
        std::optional<nc::ops::CopyOperationDurableTerminalOutcome> presented_outcome;
        bool durable_outcome_delivered = false;
        int generic_completion_calls = 0;
    };

    auto prepared = PrepareReviewedCopyApplicationBoundary(BoundaryPreflight(), true, true);
    REQUIRE(prepared);
    nc::core::OperationSubmissionGate gate;
    const auto pool = nc::ops::Pool::Make();
    const auto operation = std::make_shared<ImmediateOperation>();
    const auto evidence = std::make_shared<Evidence>();

    const nc::ops::CopyOperationDurableTerminalOutcome expected_outcome{
        .plan_id = "reviewed-copy-as-boundary",
        .state = nc::ops::OperationJournalState::Failed,
        .item_results = {
            nc::ops::OperationJournalItemResult{
                .item_index = 0,
                .status = nc::ops::OperationJournalItemStatus::Failed,
                .error = nc::ops::OperationJournalItemError::Commit,
                .system_error = EIO,
                .prior_error = nc::ops::OperationJournalItemError::None,
                .prior_system_error = 0,
                .bytes = 0,
                .destination_publication = nc::ops::OperationJournalPublicationState::Unknown,
                .filesystem_sync_status = nc::ops::OperationJournalFilesystemSyncStatus::NotAttempted,
                .filesystem_sync_system_error = 0,
                .recovery_action = nc::ops::OperationJournalRecoveryAction::InspectDestination,
            }},
        .confirmation = nc::ops::CopyOperationDurableTerminalConfirmation::Finalized,
    };

    int submissions = 0;
    pool->ObserveUnticketed(nc::ops::Pool::NotifyAboutRemoval, [evidence] {
        const auto guard = std::lock_guard{evidence->lock};
        evidence->events.emplace_back("pool-removal");
    });
    pool->SetOperationCompletionCallback([evidence](const std::shared_ptr<nc::ops::Operation> &) {
        const auto guard = std::lock_guard{evidence->lock};
        ++evidence->generic_completion_calls;
    });

    const auto result = prepared->Approve(
        true,
        [] { return true; },
        gate,
        {
            .dispatch_to_ui =
                [&](std::function<void()> _task) {
                    const auto guard = std::lock_guard{evidence->lock};
                    evidence->events.emplace_back("ui-dispatch");
                    evidence->ui_tasks.emplace_back(std::move(_task));
                },
            .present_durable_outcome =
                [&](nc::ops::CopyOperationDurableTerminalOutcome _outcome) {
                    const auto guard = std::lock_guard{evidence->lock};
                    evidence->events.emplace_back("durable-presentation");
                    evidence->presented_outcome.emplace(std::move(_outcome));
                },
            .submit =
                [&, pool, operation, evidence](nc::ops::ReviewedVFSOperationPreflight,
                                               std::shared_ptr<nc::core::OperationSubmissionGate::Ticket>,
                                               nc::ops::CopyOperationSubmissionHooks _hooks,
                                               std::shared_ptr<std::atomic_bool> _durable_outcome_delivered) {
                    ++submissions;
                    REQUIRE(_hooks.durable_terminal_observer);
                    CHECK(_hooks.cold_operation_observations.empty());
                    REQUIRE(
                        pool->TryEnqueue(operation,
                                         [evidence,
                                          hooks = std::move(_hooks),
                                          durable_outcome_delivered = std::move(_durable_outcome_delivered),
                                          expected_outcome](const std::shared_ptr<nc::ops::Operation> &) mutable {
                                             {
                                                 const auto guard = std::lock_guard{evidence->lock};
                                                 evidence->events.emplace_back("pool-finalizer");
                                             }
                                             hooks.durable_terminal_observer(expected_outcome);
                                             {
                                                 const auto guard = std::lock_guard{evidence->lock};
                                                 evidence->durable_outcome_delivered =
                                                     durable_outcome_delivered->load(std::memory_order_acquire);
                                             }
                                             return nc::ops::PoolTerminalFinalizationDecision::ReleaseWithoutCompletion;
                                         }) == nc::ops::PoolEnqueueResult::Accepted);
                },
        });

    CHECK(result == ApprovalResult::Submitted);
    CHECK(submissions == 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while( !pool->Empty() && std::chrono::steady_clock::now() < deadline )
        std::this_thread::yield();
    REQUIRE(pool->Empty());

    std::function<void()> ui_task;
    {
        const auto guard = std::lock_guard{evidence->lock};
        CHECK(evidence->durable_outcome_delivered);
        CHECK(evidence->generic_completion_calls == 0);
        CHECK(evidence->events == std::vector<std::string>{"pool-finalizer", "ui-dispatch", "pool-removal"});
        REQUIRE(evidence->ui_tasks.size() == 1);
        ui_task = std::move(evidence->ui_tasks.front());
    }
    ui_task();
    {
        const auto guard = std::lock_guard{evidence->lock};
        REQUIRE(evidence->presented_outcome);
        CHECK(*evidence->presented_outcome == expected_outcome);
        CHECK(evidence->events ==
              std::vector<std::string>{"pool-finalizer", "ui-dispatch", "pool-removal", "durable-presentation"});
    }
}
