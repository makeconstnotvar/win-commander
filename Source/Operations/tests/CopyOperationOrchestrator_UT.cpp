// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"

#include "../source/CopyOperationOrchestrator.h"
#include "../source/CopyOperationOrchestratorTesting.h"
#include "../source/Job.h"
#include "../source/OperationCenterCoordinator.h"
#include "../source/OperationJournalTesting.h"
#include "../../VFS/source/ProviderCapabilitiesTesting.h"

#include <VFS/Host.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#define PREFIX "CopyOperationOrchestrator: "

namespace nc::ops {
namespace {

using namespace std::chrono_literals;

struct CopyOrchestratorDirectory final {
    CopyOrchestratorDirectory()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "copy-orchestrator-ut-XXXXXX").string();
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        path = std::filesystem::canonical(pattern).string();
    }
    ~CopyOrchestratorDirectory() { std::filesystem::remove_all(path); }
    std::string path;
};

std::shared_ptr<OperationJournal> CopyOrchestratorJournal(
    const CopyOrchestratorDirectory &_directory,
    std::shared_ptr<OperationJournalSyscalls> _syscalls = OperationJournalTesting::DefaultSyscalls())
{
    auto opened = OperationJournalTesting::Open(
        _directory.path, std::move(_syscalls), [] { return OperationPlan::TimePoint{1'700'000'100s}; });
    REQUIRE(opened);
    return std::make_shared<OperationJournal>(std::move(*opened));
}

OperationPlan
CopyOrchestratorPlan(std::string _id,
                     const std::filesystem::path &_source,
                     const std::filesystem::path &_destination,
                     OperationPlanConflictDecision _conflict_decision = OperationPlanConflictDecision::Ask)
{
    auto plan = OperationPlan::Create({
        .plan_id = std::move(_id),
        .type = OperationPlanType::Copy,
        .sources = {OperationPlanSourceInput{"local", _source.string()}},
        .destination =
            OperationPlanDestinationInput{"local", _destination.string(), OperationPlanDestinationKind::Directory},
        .conflict_policy = OperationPlanConflictPolicy{_conflict_decision, OperationPlanConflictScope::ThisItem},
        .created_at = OperationPlan::TimePoint{1'700'000'000s},
    });
    REQUIRE(plan);
    return std::move(*plan);
}

ReviewedVFSOperationPreflight
CopyOrchestratorReview(std::string _id,
                       TempTestDir &_temporary,
                       OperationPlanConflictDecision _conflict_decision = OperationPlanConflictDecision::Ask,
                       bool _source_is_directory = false)
{
    const auto source = _temporary.directory / "source.txt";
    const auto destination = _temporary.directory / "destination";
    if( _source_is_directory ) {
        REQUIRE(std::filesystem::create_directory(source));
    }
    else {
        std::ofstream stream{source};
        REQUIRE(stream);
        stream << "source";
        REQUIRE(stream);
    }
    REQUIRE(std::filesystem::create_directory(destination));

    auto bindings = VFSOperationPlanningBindings::Create({{"local", TestEnv().vfs_native}});
    REQUIRE(bindings);
    auto probes = VFSOperationPlanningProbes::Create(
        *bindings,
        [](const OperationPlanningPath &,
           OperationPlanningRequiredAccess,
           nc::vfs::Host &) -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    REQUIRE(probes);
    auto reviewed = ReviewedVFSOperationPreflight::Review(
        probes->Preflight(CopyOrchestratorPlan(std::move(_id), source, destination, _conflict_decision)),
        VFSOperationPreflightReviewDecision::Approved);
    REQUIRE(reviewed);
    return std::move(*reviewed);
}

ReviewedVFSOperationPreflight CopyOrchestratorBatchReview(std::string _id, TempTestDir &_temporary)
{
    const auto first_source = _temporary.directory / "first-source.txt";
    const auto second_source = _temporary.directory / "second-source.txt";
    const auto destination = _temporary.directory / "destination";
    for( const auto *const source : {&first_source, &second_source} ) {
        std::ofstream stream{*source};
        REQUIRE(stream);
        stream << "source";
        REQUIRE(stream);
    }
    REQUIRE(std::filesystem::create_directory(destination));

    auto plan = OperationPlan::Create({
        .plan_id = std::move(_id),
        .type = OperationPlanType::Copy,
        .sources = {OperationPlanSourceInput{"local", first_source.string()},
                    OperationPlanSourceInput{"local", second_source.string()}},
        .destination =
            OperationPlanDestinationInput{"local", destination.string(), OperationPlanDestinationKind::Directory},
        .conflict_policy = OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask,
                                                        OperationPlanConflictScope::ThisItem},
        .created_at = OperationPlan::TimePoint{1'700'000'000s},
    });
    REQUIRE(plan);
    auto bindings = VFSOperationPlanningBindings::Create({{"local", TestEnv().vfs_native}});
    REQUIRE(bindings);
    auto probes = VFSOperationPlanningProbes::Create(
        *bindings,
        [](const OperationPlanningPath &,
           OperationPlanningRequiredAccess,
           nc::vfs::Host &) -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    REQUIRE(probes);
    auto reviewed = ReviewedVFSOperationPreflight::Review(
        probes->Preflight(std::move(*plan)), VFSOperationPreflightReviewDecision::Approved);
    REQUIRE(reviewed);
    return std::move(*reviewed);
}

OperationPlan CopyOrchestratorThreeSourcePlan(std::string _id)
{
    auto plan = OperationPlan::Create({
        .plan_id = std::move(_id),
        .type = OperationPlanType::Copy,
        .sources = {OperationPlanSourceInput{"local", "/first-source"},
                    OperationPlanSourceInput{"local", "/second-source"},
                    OperationPlanSourceInput{"local", "/third-source"}},
        .destination = OperationPlanDestinationInput{"local", "/destination", OperationPlanDestinationKind::Directory},
        .conflict_policy = OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask,
                                                        OperationPlanConflictScope::ThisItem},
        .created_at = OperationPlan::TimePoint{1'700'000'000s},
    });
    REQUIRE(plan);
    return std::move(*plan);
}

OperationJournalItemResult CopyOrchestratorSuccess()
{
    return OperationJournalItemResult{
        .item_index = 0,
        .status = OperationJournalItemStatus::Succeeded,
        .error = OperationJournalItemError::None,
        .system_error = 0,
        .prior_error = OperationJournalItemError::None,
        .prior_system_error = 0,
        .bytes = 6,
        .destination_publication = OperationJournalPublicationState::Published,
        .filesystem_sync_status = OperationJournalFilesystemSyncStatus::Confirmed,
        .filesystem_sync_system_error = 0,
        .recovery_action = OperationJournalRecoveryAction::None,
    };
}

OperationJournalItemResult CopyOrchestratorUnknownFailure()
{
    return OperationJournalItemResult{
        .item_index = 0,
        .status = OperationJournalItemStatus::Failed,
        .error = OperationJournalItemError::Commit,
        .system_error = EIO,
        .prior_error = OperationJournalItemError::None,
        .prior_system_error = 0,
        .bytes = 0,
        .destination_publication = OperationJournalPublicationState::Unknown,
        .filesystem_sync_status = OperationJournalFilesystemSyncStatus::NotAttempted,
        .filesystem_sync_system_error = 0,
        .recovery_action = OperationJournalRecoveryAction::InspectDestination,
    };
}

OperationJournalItemResult CopyOrchestratorCancelled()
{
    return OperationJournalItemResult{
        .item_index = 0,
        .status = OperationJournalItemStatus::Cancelled,
        .error = OperationJournalItemError::Cancelled,
        .system_error = 0,
        .prior_error = OperationJournalItemError::None,
        .prior_system_error = 0,
        .bytes = 0,
        .destination_publication = OperationJournalPublicationState::NotPublished,
        .filesystem_sync_status = OperationJournalFilesystemSyncStatus::NotAttempted,
        .filesystem_sync_system_error = 0,
        .recovery_action = OperationJournalRecoveryAction::None,
    };
}

std::vector<OperationJournalItemResult> CopyOrchestratorThreeItemSuccesses()
{
    auto first = CopyOrchestratorSuccess();
    auto second = CopyOrchestratorSuccess();
    auto third = CopyOrchestratorSuccess();
    second.item_index = 1;
    third.item_index = 2;
    return {std::move(first), std::move(second), std::move(third)};
}

CopyOperationOrchestratorTesting::ConditionalCommitTransactionResolver
CopyOrchestratorStrongConditionalCommitTransaction(std::atomic_int *_commit_calls = nullptr,
                                                   std::atomic_int *_abort_calls = nullptr,
                                                   std::atomic_bool *_cancel_after_mint = nullptr)
{
    return [_commit_calls, _abort_calls, _cancel_after_mint](
               nc::vfs::ProviderConditionalCopyReviewedAuthority _authority,
               const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &) {
        REQUIRE(_authority.HasReviewSeal());
        const auto source = _authority.Claims().source.absolute_path;
        const auto destination = _authority.Claims().destination.absolute_path;
        auto destination_host = _authority.Claims().destination_binding.host;
        auto transaction = nc::vfs::ProviderConditionalCopyTransactionTestAccess::Mint(
            *destination_host,
            std::move(_authority),
            [source, destination, _commit_calls](const auto &) {
                if( _commit_calls )
                    ++*_commit_calls;
                std::error_code error;
                std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error);
                if( error ) {
                    return nc::vfs::ProviderConditionalCopyCommitResult{
                        .publication = nc::vfs::ProviderConditionalCopyPublicationState::NotPublished,
                        .failure = nc::vfs::ProviderConditionalCopyCommitFailure::ProviderFailure,
                        .system_error = error.value(),
                    };
                }
                return nc::vfs::ProviderConditionalCopyCommitResult{
                    .publication = nc::vfs::ProviderConditionalCopyPublicationState::Published,
                    .failure = nc::vfs::ProviderConditionalCopyCommitFailure::None,
                    .filesystem_sync_status = nc::vfs::ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
                };
            },
            [_abort_calls] {
                if( _abort_calls )
                    ++*_abort_calls;
                return nc::vfs::ProviderConditionalCopyPublicationState::NotPublished;
            });
        if( _cancel_after_mint )
            *_cancel_after_mint = true;
        return transaction;
    };
}

CopyOperationOrchestratorTesting::ConditionalCommitTransactionResolver
CopyOrchestratorConditionalCommitFailure(nc::vfs::ProviderConditionalCopyTransactionBeginError _error,
                                         std::atomic_int *_calls = nullptr)
{
    return [_error, _calls](nc::vfs::ProviderConditionalCopyReviewedAuthority,
                            const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &)
               -> std::expected<std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction>,
                                nc::vfs::ProviderConditionalCopyTransactionBeginError> {
        if( _calls )
            ++*_calls;
        return std::unexpected(_error);
    };
}

std::string CopyOrchestratorReadFile(const std::filesystem::path &_path)
{
    std::ifstream stream{_path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

bool CopyOrchestratorCheckUntil(std::function<bool()> _predicate, std::chrono::steady_clock::duration _timeout = 1s)
{
    const auto deadline = std::chrono::steady_clock::now() + _timeout;
    while( std::chrono::steady_clock::now() < deadline ) {
        if( _predicate() )
            return true;
        std::this_thread::yield();
    }
    return _predicate();
}

struct CopyOrchestratorControlledJob final : Job {
    void Perform() override
    {
        if( report_host )
            TellItemReport({*report_host, "/source.txt", ItemStatus::Processed});
        while( !done && !IsStopped() )
            std::this_thread::yield();
        if( !IsStopped() )
            SetCompleted();
    }
    void OnStopped() override
    {
        if( on_stopped )
            on_stopped();
    }
    std::atomic_bool done{false};
    VFSHost *report_host{nullptr};
    std::function<void()> on_stopped;
};

struct CopyOrchestratorControlledOperation final : Operation {
    ~CopyOrchestratorControlledOperation() override { Wait(); }
    Job *GetJob() noexcept override { return &job; }
    CopyOrchestratorControlledJob job;
};

struct CopyOrchestratorEvents final {
    void Add(std::string _event)
    {
        const auto guard = std::lock_guard{lock};
        events.emplace_back(std::move(_event));
    }
    std::vector<std::string> Snapshot() const
    {
        const auto guard = std::lock_guard{lock};
        return events;
    }
    mutable std::mutex lock;
    std::vector<std::string> events;
};

struct CopyOrchestratorReleaseBarrier final {
    void EnterAndWait()
    {
        auto guard = std::unique_lock{lock};
        entered = true;
        condition.notify_all();
        condition.wait(guard, [&] { return released; });
    }
    bool WaitUntilEntered(std::chrono::steady_clock::duration _timeout)
    {
        auto guard = std::unique_lock{lock};
        return condition.wait_for(guard, _timeout, [&] { return entered; });
    }
    void Release()
    {
        const auto guard = std::lock_guard{lock};
        released = true;
        condition.notify_all();
    }

    std::mutex lock;
    std::condition_variable condition;
    bool entered{false};
    bool released{false};
};

} // namespace

static_assert(std::is_constructible_v<CopyOperationOrchestrator,
                                      std::shared_ptr<OperationJournal>,
                                      std::shared_ptr<Pool>,
                                      std::shared_ptr<CopyOperationRunReceiptCustodian>>);
static_assert(!std::is_constructible_v<CopyOperationOrchestrator,
                                       std::shared_ptr<OperationJournal>,
                                       std::shared_ptr<Pool>,
                                       CopyOperationOrchestratorTesting::ExecutionFactory,
                                       std::shared_ptr<CopyOperationRunReceiptCustodian>>);

TEST_CASE(PREFIX "production factory submits exact provider transaction product",
          "[copy-operation-orchestrator][copy-operation-orchestrator-production]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int commit_calls{0};
    std::atomic_int abort_calls{0};
    auto orchestrator = CopyOperationOrchestratorTesting::CreateProductionWithResolver(
        journal, pool, custodian, CopyOrchestratorStrongConditionalCommitTransaction(&commit_calls, &abort_calls));

    auto submitted = orchestrator.Submit(CopyOrchestratorReview("production-success", temporary));
    REQUIRE(submitted);
    REQUIRE((*submitted)->Wait(5s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->Empty(); }));

    CHECK(commit_calls == 1);
    CHECK(abort_calls == 0);
    CHECK(custodian->PendingCount() == 0);
    const auto source = temporary.directory / "source.txt";
    const auto destination = temporary.directory / "destination" / "source.txt";
    CHECK(CopyOrchestratorReadFile(destination) == CopyOrchestratorReadFile(source));
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Completed);
    REQUIRE(snapshot[0].item_results.size() == 1);
    CHECK(snapshot[0].item_results[0].item_index == 0);
    CHECK(snapshot[0].item_results[0].bytes == std::filesystem::file_size(source));
    CHECK(snapshot[0].item_results[0].destination_publication == OperationJournalPublicationState::Published);
    CHECK(snapshot[0].item_results[0].filesystem_sync_status == OperationJournalFilesystemSyncStatus::Confirmed);
}

TEST_CASE(PREFIX "production product owns the sanitized Submit cancel checker",
          "[copy-operation-orchestrator][copy-operation-orchestrator-production]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int commit_calls{0};
    std::atomic_int abort_calls{0};
    std::atomic_int cancel_calls{0};
    std::atomic_bool throw_after_mint{false};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { throw_after_mint = true; });
    auto orchestrator = CopyOperationOrchestratorTesting::CreateProductionWithResolver(
        journal, pool, custodian, CopyOrchestratorStrongConditionalCommitTransaction(&commit_calls, &abort_calls));

    auto submitted = orchestrator.Submit(CopyOrchestratorReview("production-cancel-owned", temporary), [&]() -> bool {
        ++cancel_calls;
        if( throw_after_mint )
            throw std::runtime_error{"late cancellation"};
        return false;
    });
    REQUIRE(submitted);
    REQUIRE((*submitted)->Wait(5s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->Empty(); }));

    CHECK(cancel_calls > 1);
    CHECK(commit_calls == 0);
    CHECK(abort_calls == 1);
    CHECK(custodian->PendingCount() == 0);
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Cancelled);
    REQUIRE(snapshot[0].item_results.size() == 1);
    CHECK(snapshot[0].item_results[0].status == OperationJournalItemStatus::Cancelled);
    CHECK(snapshot[0].item_results[0].destination_publication == OperationJournalPublicationState::NotPublished);
    CHECK_FALSE(std::filesystem::exists(temporary.directory / "destination" / "source.txt"));
}

TEST_CASE(PREFIX "production factory rejection preserves exact typed error and durable admission",
          "[copy-operation-orchestrator][copy-operation-orchestrator-production]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int additions{0};
    std::atomic_int resolver_calls{0};
    std::atomic_int cancel_calls{0};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });

    CopyOperationOrchestrator::CancelChecker cancel_checker;
    auto conflict_decision = OperationPlanConflictDecision::Ask;
    auto resolver = CopyOrchestratorConditionalCommitFailure(
        nc::vfs::ProviderConditionalCopyTransactionBeginError::SourceStale, &resolver_calls);
    auto expected_factory_error = ReviewedOperationFactoryErrorCode::StaleSource;
    auto expected_orchestrator_error = CopyOperationOrchestratorErrorCode::ExecutionFactoryFailed;
    auto expected_journal_state = OperationJournalState::Failed;
    bool expect_path = true;
    bool source_is_directory = false;
    int expected_resolver_calls = 1;
    SECTION("stale source")
    {
    }
    SECTION("cancelled during private factory validation")
    {
        cancel_checker = [&cancel_calls] { return ++cancel_calls >= 2; };
        resolver = CopyOrchestratorStrongConditionalCommitTransaction();
        expected_factory_error = ReviewedOperationFactoryErrorCode::Cancelled;
        expected_orchestrator_error = CopyOperationOrchestratorErrorCode::Cancelled;
        expected_journal_state = OperationJournalState::Cancelled;
        expect_path = false;
        expected_resolver_calls = 0;
    }
    SECTION("unsupported conditional transaction scope")
    {
        resolver = CopyOrchestratorConditionalCommitFailure(
            nc::vfs::ProviderConditionalCopyTransactionBeginError::Unsupported, &resolver_calls);
        expected_factory_error = ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable;
        expect_path = false;
    }
    SECTION("unsupported source kind")
    {
        resolver = CopyOrchestratorStrongConditionalCommitTransaction();
        expected_factory_error = ReviewedOperationFactoryErrorCode::UnsupportedSourceKind;
        source_is_directory = true;
        expected_resolver_calls = 0;
    }

    auto orchestrator =
        CopyOperationOrchestratorTesting::CreateProductionWithResolver(journal, pool, custodian, std::move(resolver));
    auto submitted = orchestrator.Submit(
        CopyOrchestratorReview("production-rejected", temporary, conflict_decision, source_is_directory),
        std::move(cancel_checker));
    REQUIRE_FALSE(submitted);
    CHECK(submitted.error().code == expected_orchestrator_error);
    REQUIRE(submitted.error().reviewed_factory_error);
    CHECK(submitted.error().reviewed_factory_error->code == expected_factory_error);
    CHECK(submitted.error().reviewed_factory_error->path.has_value() == expect_path);
    if( expect_path ) {
        const OperationPlanningPath expected_path{
            "local", std::filesystem::canonical(temporary.directory / "source.txt").string()};
        CHECK(submitted.error().reviewed_factory_error->path->provider_id == expected_path.provider_id);
        CHECK(submitted.error().reviewed_factory_error->path->absolute_path == expected_path.absolute_path);
    }
    CHECK(resolver_calls == expected_resolver_calls);
    CHECK(additions == 0);
    CHECK(pool->Empty());
    CHECK(custodian->PendingCount() == 0);
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == expected_journal_state);
    CHECK(snapshot[0].item_results.empty());
    CHECK_FALSE(std::filesystem::exists(temporary.directory / "destination" / "source.txt"));
}

TEST_CASE(PREFIX "orders durable admission, construction, enqueue and terminal release",
          "[copy-operation-orchestrator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    auto events = std::make_shared<CopyOrchestratorEvents>();
    std::atomic_bool factory_saw_admitted{false};
    std::atomic_int item_status_calls{0};
    std::atomic_bool item_status_was_exact{false};
    std::atomic_bool terminal_observer_saw_durable_result{false};
    std::atomic_bool removal_saw_completed{false};

    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [events] { events->Add("addition"); });
    pool->ObserveUnticketed(Pool::NotifyAboutRemoval, [events, journal, &removal_saw_completed] {
        const auto snapshot = journal->Snapshot();
        removal_saw_completed = snapshot.size() == 1 && snapshot[0].state == OperationJournalState::Completed &&
                                snapshot[0].item_results == std::vector{CopyOrchestratorSuccess()};
        events->Add("removal");
    });

    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [journal, operation, events, &factory_saw_admitted](ReviewedVFSOperationPreflight,
                                                            CopyOperationOrchestrator::CancelChecker) {
            const auto snapshot = journal->Snapshot();
            factory_saw_admitted = snapshot.size() == 1 && snapshot[0].state == OperationJournalState::Admitted;
            events->Add("factory");
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [events] {
                events->Add("accessor");
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorSuccess()};
            });
        },
        custodian);
    operation->job.report_host = TestEnv().vfs_native.get();

    auto submitted = orchestrator.Submit(
        CopyOrchestratorReview("ordered", temporary),
        {},
        CopyOperationSubmissionHooks{
            .cold_operation_observations = {{.notification_mask =
                                                 Operation::NotifyAboutStart | Operation::NotifyAboutPause,
                                             .callback =
                                                 [events] {
                                                     events->Add("start");
                                                     throw std::runtime_error{"transport observer failure"};
                                                 }}},
            .item_status_observer =
                [&item_status_calls, &item_status_was_exact](ItemStateReport _report) {
                    item_status_was_exact = _report.path == "/source.txt" && _report.status == ItemStatus::Processed;
                    ++item_status_calls;
                },
            .durable_terminal_observer =
                [events, journal, &terminal_observer_saw_durable_result](
                    const CopyOperationDurableTerminalOutcome &_outcome) {
                    const auto snapshot = journal->Snapshot();
                    terminal_observer_saw_durable_result =
                        _outcome.plan_id == "ordered" && _outcome.state == OperationJournalState::Completed &&
                        _outcome.item_results == std::vector{CopyOrchestratorSuccess()} &&
                        _outcome.confirmation == CopyOperationDurableTerminalConfirmation::Finalized &&
                        snapshot.size() == 1 && snapshot[0].state == OperationJournalState::Completed &&
                        snapshot[0].item_results == std::vector{CopyOrchestratorSuccess()};
                    events->Add("terminal");
                }});
    REQUIRE(submitted);
    CHECK(*submitted == operation);
    CHECK(factory_saw_admitted);
    operation->job.done = true;
    REQUIRE(operation->Wait(1s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->Empty(); }));

    CHECK(events->Snapshot() ==
          std::vector<std::string>{"factory", "addition", "start", "accessor", "terminal", "removal"});
    CHECK(terminal_observer_saw_durable_result);
    CHECK(item_status_calls == 1);
    CHECK(item_status_was_exact);
    CHECK(removal_saw_completed);
}

TEST_CASE(PREFIX "persists an atomic empty cancellation evidence snapshot", "[copy-operation-orchestrator][batch-durable-terminal]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    operation->job.done = true;
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::optional<CopyOperationDurableTerminalOutcome> terminal_outcome;
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [] {
                return std::expected<CopyOperationTerminalEvidence, CopyOperationTerminalResultError>{
                    CopyOperationTerminalEvidence{.state = OperationJournalState::Cancelled, .item_results = {}}};
            });
        },
        custodian);

    const auto submitted = orchestrator.Submit(
        CopyOrchestratorReview("empty-cancellation-evidence", temporary),
        {},
        CopyOperationSubmissionHooks{
            .durable_terminal_observer = [&terminal_outcome](const CopyOperationDurableTerminalOutcome &_outcome) {
                terminal_outcome = _outcome;
            }});
    REQUIRE(submitted);
    REQUIRE((*submitted)->Wait(1s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->Empty(); }));

    REQUIRE(terminal_outcome);
    CHECK(terminal_outcome->plan_id == "empty-cancellation-evidence");
    CHECK(terminal_outcome->state == OperationJournalState::Cancelled);
    CHECK(terminal_outcome->item_results.empty());
    CHECK(terminal_outcome->SingleItemResult() == nullptr);
    CHECK(terminal_outcome->confirmation == CopyOperationDurableTerminalConfirmation::Finalized);
    CHECK(custodian->PendingCount() == 0);
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Cancelled);
    CHECK(snapshot[0].item_results.empty());
}

TEST_CASE(PREFIX "custodian retries an exact three-item terminal evidence snapshot without reaccessing",
          "[copy-operation-orchestrator][batch-durable-terminal]")
{
    CopyOrchestratorDirectory directory;
    auto syscalls = OperationJournalTesting::DefaultSyscalls();
    auto journal = CopyOrchestratorJournal(directory, syscalls);
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    const auto plan = CopyOrchestratorThreeSourcePlan("three-item-retry");
    const auto evidence = CopyOrchestratorThreeItemSuccesses();
    std::atomic_int accessor_calls{0};
    std::optional<CopyOperationDurableTerminalOutcome> terminal_outcome;

    auto admission = journal->Admit(plan);
    REQUIRE(admission);
    auto run = journal->TransitionToRunning(std::move(*admission));
    REQUIRE(run);
    REQUIRE(CopyOperationRunReceiptCustodianTesting::EnqueueExactTerminalEvidence(
        *custodian,
        plan,
        journal,
        std::move(*run),
        [&accessor_calls, evidence] {
            ++accessor_calls;
            return std::expected<CopyOperationTerminalEvidence, CopyOperationTerminalResultError>{
                CopyOperationTerminalEvidence{.state = OperationJournalState::Completed, .item_results = evidence}};
        },
        pool,
        operation,
        [&terminal_outcome](const CopyOperationDurableTerminalOutcome &_outcome) { terminal_outcome = _outcome; }));

    const auto real_open_at = syscalls->open_at;
    std::atomic_bool fail_once{true};
    syscalls->open_at = [real_open_at, &fail_once](int _directory, const char *_path, int _flags, mode_t _mode) {
        if( fail_once.exchange(false) ) {
            errno = EIO;
            return -1;
        }
        return real_open_at(_directory, _path, _flags, _mode);
    };
    operation->job.done = true;
    REQUIRE(operation->Wait(1s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->FinalizingOperationsCount() == 1; }));
    CHECK(accessor_calls == 1);
    const auto running = journal->Snapshot();
    REQUIRE(running.size() == 1);
    CHECK(running[0].state == OperationJournalState::Running);
    CHECK(running[0].item_results.empty());
    CHECK_FALSE(terminal_outcome);

    CHECK(pool->RetryFinalization(operation) == PoolRetryFinalizationResult::Released);
    CHECK(accessor_calls == 1);
    CHECK(pool->Empty());
    CHECK(custodian->PendingCount() == 0);
    REQUIRE(terminal_outcome);
    CHECK(terminal_outcome->state == OperationJournalState::Completed);
    CHECK(terminal_outcome->item_results == evidence);
    CHECK(terminal_outcome->SingleItemResult() == nullptr);
    CHECK(terminal_outcome->confirmation == CopyOperationDurableTerminalConfirmation::Finalized);
    const auto finalized = journal->Snapshot();
    REQUIRE(finalized.size() == 1);
    CHECK(finalized[0].state == OperationJournalState::Completed);
    CHECK(finalized[0].item_results == evidence);
}

TEST_CASE(PREFIX "custodian reconciles and releases an exact three-item terminal evidence snapshot",
          "[copy-operation-orchestrator][batch-durable-terminal]")
{
    CopyOrchestratorDirectory directory;
    CopyOrchestratorDirectory wrong_directory;
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    const auto evidence = CopyOrchestratorThreeItemSuccesses();
    std::atomic_int accessor_calls{0};
    std::atomic_int removals{0};
    std::optional<CopyOperationDurableTerminalOutcome> terminal_outcome;
    pool->ObserveUnticketed(Pool::NotifyAboutRemoval, [&] { ++removals; });

    {
        auto syscalls = OperationJournalTesting::DefaultSyscalls();
        auto journal = CopyOrchestratorJournal(directory, syscalls);
        const auto plan = CopyOrchestratorThreeSourcePlan("three-item-reconcile");
        auto admission = journal->Admit(plan);
        REQUIRE(admission);
        auto run = journal->TransitionToRunning(std::move(*admission));
        REQUIRE(run);
        const auto real_rename_at = syscalls->rename_at;
        std::atomic_bool inject_failure{false};
        syscalls->rename_at = [real_rename_at, &inject_failure](
                                  int _from_directory, const char *_from, int _to_directory, const char *_to) {
            if( !inject_failure.exchange(false) )
                return real_rename_at(_from_directory, _from, _to_directory, _to);
            const int renamed = real_rename_at(_from_directory, _from, _to_directory, _to);
            if( renamed != 0 )
                return renamed;
            errno = EIO;
            return -1;
        };
        REQUIRE(CopyOperationRunReceiptCustodianTesting::EnqueueExactTerminalEvidence(
            *custodian,
            plan,
            journal,
            std::move(*run),
            [&accessor_calls, evidence] {
                ++accessor_calls;
                return std::expected<CopyOperationTerminalEvidence, CopyOperationTerminalResultError>{
                    CopyOperationTerminalEvidence{.state = OperationJournalState::Completed, .item_results = evidence}};
            },
            pool,
            operation,
            [&terminal_outcome](const CopyOperationDurableTerminalOutcome &_outcome) {
                terminal_outcome = _outcome;
            }));

        inject_failure = true;
        operation->job.done = true;
        REQUIRE(operation->Wait(1s));
        REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->FinalizingOperationsCount() == 1; }));
        CHECK(accessor_calls == 1);
        CHECK(custodian->Retry("three-item-reconcile").status ==
              CopyOperationRunReceiptCustodyStatus::ReconcileRequired);
        CHECK_FALSE(terminal_outcome);
    }

    auto wrong = CopyOrchestratorJournal(wrong_directory);
    CHECK(custodian->Reconcile("three-item-reconcile", *wrong).status ==
          CopyOperationRunReceiptReconciliationStatus::Mismatch);
    CHECK(pool->FinalizingOperationsCount() == 1);
    auto reopened = CopyOrchestratorJournal(directory);
    const auto reconciled = custodian->Reconcile("three-item-reconcile", *reopened);
    CHECK(reconciled.status == CopyOperationRunReceiptReconciliationStatus::TerminalConfirmed);
    CHECK(reconciled.pool_release_required);
    CHECK(custodian->ReleaseReconciled("three-item-reconcile") == CopyOperationRunReceiptPoolReleaseStatus::Released);

    CHECK(accessor_calls == 1);
    CHECK(removals == 1);
    CHECK(pool->Empty());
    CHECK(custodian->PendingCount() == 0);
    REQUIRE(terminal_outcome);
    CHECK(terminal_outcome->state == OperationJournalState::Completed);
    CHECK(terminal_outcome->item_results == evidence);
    CHECK(terminal_outcome->SingleItemResult() == nullptr);
    CHECK(terminal_outcome->confirmation == CopyOperationDurableTerminalConfirmation::ReconciledTerminal);
    const auto snapshot = reopened->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Completed);
    CHECK(snapshot[0].item_results == evidence);
}

TEST_CASE(PREFIX "carries a whole reviewed batch through one operation and one journal entry",
          "[copy-operation-orchestrator][copy-operation-orchestrator-production][batch-durable-terminal]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int commit_calls{0};
    std::atomic_int abort_calls{0};
    std::atomic_int additions{0};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });
    auto orchestrator = CopyOperationOrchestratorTesting::CreateProductionWithResolver(
        journal, pool, custodian, CopyOrchestratorStrongConditionalCommitTransaction(&commit_calls, &abort_calls));

    auto reviewed = CopyOrchestratorBatchReview("direct-batch", temporary);
    REQUIRE(reviewed.AcceptedPlan().Plan().Sources().size() == 2);
    REQUIRE(reviewed.AcceptedPlan().Report().items.size() == 2);
    auto submitted = orchestrator.Submit(std::move(reviewed));

    REQUIRE(submitted);
    REQUIRE((*submitted)->Wait(5s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->Empty(); }));

    CHECK(commit_calls == 2);
    CHECK(abort_calls == 0);
    // One addition, not two: the user asked for one copy of two files, and the Operation Center is
    // shown exactly that.
    CHECK(additions == 1);
    CHECK(custodian->PendingCount() == 0);
    const auto first_source = temporary.directory / "first-source.txt";
    const auto second_source = temporary.directory / "second-source.txt";
    CHECK(CopyOrchestratorReadFile(temporary.directory / "destination" / "first-source.txt") ==
          CopyOrchestratorReadFile(first_source));
    CHECK(CopyOrchestratorReadFile(temporary.directory / "destination" / "second-source.txt") ==
          CopyOrchestratorReadFile(second_source));

    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Completed);
    REQUIRE(snapshot[0].item_results.size() == 2);
    CHECK(snapshot[0].item_results[0].item_index == 0);
    CHECK(snapshot[0].item_results[1].item_index == 1);
    CHECK(snapshot[0].item_results[0].bytes == std::filesystem::file_size(first_source));
    CHECK(snapshot[0].item_results[1].bytes == std::filesystem::file_size(second_source));
    for( const auto &result : snapshot[0].item_results ) {
        CHECK(result.status == OperationJournalItemStatus::Succeeded);
        CHECK(result.destination_publication == OperationJournalPublicationState::Published);
        CHECK(result.filesystem_sync_status == OperationJournalFilesystemSyncStatus::Confirmed);
    }
}

TEST_CASE(PREFIX "coordinator admits a batch review as one operation in its model",
          "[copy-operation-orchestrator][operation-center-coordinator][batch-durable-terminal]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int commit_calls{0};
    std::atomic_int abort_calls{0};
    std::atomic_int additions{0};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });
    auto orchestrator = CopyOperationOrchestratorTesting::CreateProductionWithResolver(
        journal, pool, custodian, CopyOrchestratorStrongConditionalCommitTransaction(&commit_calls, &abort_calls));

    auto reviewed = CopyOrchestratorBatchReview("coordinator-batch", temporary);
    REQUIRE(reviewed.AcceptedPlan().Plan().Sources().size() == 2);
    REQUIRE(reviewed.AcceptedPlan().Report().items.size() == 2);
    auto submitted = (*coordinator)->SubmitReviewedCopy(*journal, orchestrator, std::move(reviewed), {}, {});

    REQUIRE(submitted);
    REQUIRE(*submitted);
    REQUIRE((*submitted)->Wait(5s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->Empty(); }));

    CHECK(commit_calls == 2);
    CHECK(abort_calls == 0);
    CHECK(additions == 1);
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Completed);
    CHECK(snapshot[0].item_results.size() == 2);
    // One record in the Operation Center for one batch, which is the whole point of executing it as
    // one operation rather than as a loop of single-item ones.
    CHECK((*coordinator)->Model().Snapshot().size() == 1);
}

TEST_CASE(PREFIX "consumes coordinator admission without a second journal entry and sees queued model before Pool start",
          "[copy-operation-orchestrator][operation-center-coordinator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);
    auto reviewed = CopyOrchestratorReview("coordinator-admission", temporary);
    auto staging = (*coordinator)->StageAdmission(*journal, reviewed.AcceptedPlan().Plan());
    REQUIRE(staging);
    auto committed = (*coordinator)->CommitAdmission(*journal, std::move(*staging));
    REQUIRE(committed);
    const auto operation_id = committed->operation_id;
    std::atomic_bool pool_addition_saw_queued_model{false};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&coordinator, operation_id, &pool_addition_saw_queued_model] {
        const auto record = (*coordinator)->Model().Find(operation_id);
        pool_addition_saw_queued_model = record && record->state == OperationRecordState::Queued && record->revision == 1;
    });

    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    operation->job.done = true;
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [] {
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorSuccess()};
            });
        },
        custodian);

    const auto submitted = CopyOperationOrchestratorTesting::SubmitAdmitted(
        orchestrator, std::move(reviewed), std::move(committed->journal_receipt));
    REQUIRE(submitted);
    REQUIRE((*submitted)->Wait(1s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->Empty(); }));
    CHECK(pool_addition_saw_queued_model);
    const auto journal_snapshot = journal->Snapshot();
    REQUIRE(journal_snapshot.size() == 1);
    CHECK(journal_snapshot.front().operation_id == operation_id);
    CHECK(journal_snapshot.front().state == OperationJournalState::Completed);
}

TEST_CASE(PREFIX "coordinator joins receipt admission to lifecycle reduction without executor ownership",
          "[copy-operation-orchestrator][operation-center-coordinator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    operation->job.done = true;
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [] {
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorSuccess()};
            });
        },
        custodian);

    std::atomic_bool pool_addition_saw_queued{false};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&coordinator, &pool_addition_saw_queued] {
        const auto snapshot = (*coordinator)->Model().Snapshot();
        pool_addition_saw_queued = snapshot.size() == 1 && snapshot.front().state == OperationRecordState::Queued &&
                                  snapshot.front().revision == 1;
    });

    const auto submitted = (*coordinator)->SubmitReviewedCopy(
        *journal, orchestrator, CopyOrchestratorReview("coordinator-lifecycle", temporary), {}, {});
    REQUIRE(submitted);
    REQUIRE((*submitted)->Wait(1s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->Empty(); }));
    CHECK(pool_addition_saw_queued);
    const auto model_snapshot = (*coordinator)->Model().Snapshot();
    REQUIRE(model_snapshot.size() == 1);
    CHECK(model_snapshot.front().state == OperationRecordState::Completed);
    CHECK(model_snapshot.front().revision >= 3);
    const auto journal_snapshot = journal->Snapshot();
    REQUIRE(journal_snapshot.size() == 1);
    CHECK(journal_snapshot.front().operation_id == model_snapshot.front().operation_id);
    CHECK(journal_snapshot.front().state == OperationJournalState::Completed);
}

TEST_CASE(PREFIX "coordinator reduces a durably rejected pre-enqueue admission without Pool side effects",
          "[copy-operation-orchestrator][operation-center-coordinator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int factory_calls{0};
    std::atomic_int additions{0};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [&factory_calls](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            ++factory_calls;
            return std::unexpected(CopyOperationExecutionFactoryError::Rejected);
        },
        custodian);

    const auto submitted = (*coordinator)->SubmitReviewedCopy(
        *journal, orchestrator, CopyOrchestratorReview("coordinator-rejection", temporary), {}, {});
    REQUIRE_FALSE(submitted);
    CHECK(submitted.error().code == OperationCenterSubmissionErrorCode::OrchestratorRejected);
    REQUIRE(submitted.error().orchestrator_error);
    CHECK(submitted.error().orchestrator_error->code == CopyOperationOrchestratorErrorCode::ExecutionFactoryFailed);
    CHECK(factory_calls == 1);
    CHECK(additions == 0);
    CHECK(pool->Empty());
    CHECK(custodian->PendingCount() == 0);
    const auto model_snapshot = (*coordinator)->Model().Snapshot();
    REQUIRE(model_snapshot.size() == 1);
    CHECK(model_snapshot.front().state == OperationRecordState::Failed);
    CHECK(model_snapshot.front().revision == 3);
    const auto journal_snapshot = journal->Snapshot();
    REQUIRE(journal_snapshot.size() == 1);
    CHECK(journal_snapshot.front().state == OperationJournalState::Failed);
}

TEST_CASE(PREFIX "coordinator cancels exact live residency by model revision",
          "[copy-operation-orchestrator][operation-center-coordinator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [operation] {
                if( operation->job.IsStopped() )
                    return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                        CopyOrchestratorCancelled()};
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorSuccess()};
            });
        },
        custodian);

    const auto submitted = (*coordinator)->SubmitReviewedCopy(
        *journal, orchestrator, CopyOrchestratorReview("coordinator-cancel", temporary), {}, {});
    REQUIRE(submitted);
    REQUIRE(CopyOrchestratorCheckUntil([&] {
        const auto snapshot = (*coordinator)->Model().Snapshot();
        return snapshot.size() == 1 && snapshot.front().state == OperationRecordState::Running;
    }));

    const auto running = (*coordinator)->Model().Snapshot().front();
    const auto stale = (*coordinator)->Cancel(running.operation_id, running.revision - 1);
    CHECK(stale.code == OperationCenterCancelResultCode::StaleRevision);
    CHECK(operation->State() == OperationState::Running);

    const auto accepted = (*coordinator)->Cancel(running.operation_id, running.revision);
    CHECK(accepted.code == OperationCenterCancelResultCode::Accepted);
    REQUIRE(operation->Wait(1s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->Empty(); }));

    const auto terminal = (*coordinator)->Model().Find(running.operation_id);
    REQUIRE(terminal);
    CHECK(terminal->state == OperationRecordState::Cancelled);
    CHECK(terminal->revision >= running.revision + 2);
    const auto repeated = (*coordinator)->Cancel(running.operation_id, terminal->revision);
    CHECK(repeated.code == OperationCenterCancelResultCode::CancelUnavailable);
}

TEST_CASE(PREFIX "pre-enqueue preparation failure finalizes without Pool admission", "[copy-operation-orchestrator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int additions{0};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [] {
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorSuccess()};
            });
        },
        custodian);

    auto reviewed = CopyOrchestratorReview("pre-enqueue-failure", temporary);
    auto admission_reservation = journal->ReserveOperationId();
    REQUIRE(admission_reservation);
    auto admission = journal->Admit(std::move(*admission_reservation), reviewed.AcceptedPlan().Plan());
    REQUIRE(admission);
    auto submitted = CopyOperationOrchestratorTesting::SubmitAdmitted(
        orchestrator,
        std::move(reviewed),
        std::move(*admission),
        {},
        {},
        [](const std::shared_ptr<Pool> &, const std::shared_ptr<Operation> &) -> CopyOperationPreEnqueueLease {
            throw std::runtime_error{"intentional pre-enqueue failure"};
        });
    REQUIRE_FALSE(submitted);
    CHECK(submitted.error().code == CopyOperationOrchestratorErrorCode::PreEnqueuePreparationFailed);
    CHECK(additions == 0);
    CHECK(pool->Empty());
    CHECK(custodian->PendingCount() == 0);
    const auto journal_snapshot = journal->Snapshot();
    REQUIRE(journal_snapshot.size() == 1);
    CHECK(journal_snapshot.front().state == OperationJournalState::Failed);
}

TEST_CASE(PREFIX "coordinator rejects reentrant cancellation while a stop request is in flight",
          "[copy-operation-orchestrator][operation-center-coordinator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [operation] {
                if( operation->job.IsStopped() )
                    return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                        CopyOrchestratorCancelled()};
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorSuccess()};
            });
        },
        custodian);

    std::optional<OperationId> cancellation_id;
    std::optional<uint64_t> cancellation_revision;
    std::optional<OperationCenterCancelResult> reentrant_cancel;
    auto submitted = (*coordinator)->SubmitReviewedCopy(
        *journal,
        orchestrator,
        CopyOrchestratorReview("coordinator-reentrant-cancel", temporary),
        {},
        {});
    REQUIRE(submitted);
    REQUIRE(CopyOrchestratorCheckUntil([&] {
        const auto snapshot = (*coordinator)->Model().Snapshot();
        return snapshot.size() == 1 && snapshot.front().state == OperationRecordState::Running;
    }));

    const auto running = (*coordinator)->Model().Snapshot().front();
    cancellation_id = running.operation_id;
    cancellation_revision = running.revision;
    operation->job.on_stopped = [&] {
        reentrant_cancel = (*coordinator)->Cancel(*cancellation_id, *cancellation_revision);
    };
    const auto accepted = (*coordinator)->Cancel(*cancellation_id, *cancellation_revision);
    CHECK(accepted.code == OperationCenterCancelResultCode::Accepted);
    REQUIRE(reentrant_cancel);
    CHECK(reentrant_cancel->code == OperationCenterCancelResultCode::CancelInProgress);
    REQUIRE(operation->Wait(1s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->Empty(); }));
    const auto terminal = (*coordinator)->Model().Find(*cancellation_id);
    REQUIRE(terminal);
    CHECK(terminal->state == OperationRecordState::Cancelled);
}

TEST_CASE(PREFIX "rejects a coordinator receipt paired with another reviewed plan without mutation",
          "[copy-operation-orchestrator][operation-center-coordinator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir admitted_temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);
    auto admitted_review = CopyOrchestratorReview("receipt-plan", admitted_temporary);
    auto staging = (*coordinator)->StageAdmission(*journal, admitted_review.AcceptedPlan().Plan());
    REQUIRE(staging);
    auto committed = (*coordinator)->CommitAdmission(*journal, std::move(*staging));
    REQUIRE(committed);
    std::atomic_int factory_calls{0};
    std::atomic_int additions{0};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [&factory_calls](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            ++factory_calls;
            return std::unexpected(CopyOperationExecutionFactoryError::Rejected);
        },
        custodian);
    REQUIRE(std::filesystem::remove_all(admitted_temporary.directory / "destination") == 1);

    const auto submitted = CopyOperationOrchestratorTesting::SubmitAdmitted(
        orchestrator,
        CopyOrchestratorReview("foreign-plan", admitted_temporary),
        std::move(committed->journal_receipt));
    REQUIRE_FALSE(submitted);
    CHECK(submitted.error().code == CopyOperationOrchestratorErrorCode::InvalidJournalAdmissionReceipt);
    CHECK(factory_calls == 0);
    CHECK(additions == 0);
    CHECK(pool->Empty());
    CHECK(custodian->PendingCount() == 0);
    const auto journal_snapshot = journal->Snapshot();
    REQUIRE(journal_snapshot.size() == 1);
    CHECK(journal_snapshot.front().state == OperationJournalState::Admitted);
    CHECK(journal_snapshot.front().plan.Id().Value() == "receipt-plan");
}

TEST_CASE(PREFIX "configuration failure aborts the cold product and durably fails admission",
          "[copy-operation-orchestrator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    std::weak_ptr<Operation> weak_operation;
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int additions{0};
    std::atomic_int terminal_calls{0};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });
    uint64_t invalid_notification_mask = uint64_t{1} << 63;
    SECTION("unknown notification")
    {
    }
    SECTION("zero notification")
    {
        invalid_notification_mask = 0;
    }
    SECTION("generic completion")
    {
        invalid_notification_mask = Operation::NotifyAboutCompletion;
    }
    SECTION("generic finish")
    {
        invalid_notification_mask = Operation::NotifyAboutFinish;
    }
    bool empty_callback = false;
    SECTION("empty callback")
    {
        invalid_notification_mask = Operation::NotifyAboutStart;
        empty_callback = true;
    }

    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [&weak_operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
            weak_operation = operation;
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [] {
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorSuccess()};
            });
        },
        custodian);

    const auto submitted = orchestrator.Submit(
        CopyOrchestratorReview("configure-failure", temporary),
        {},
        CopyOperationSubmissionHooks{
            .cold_operation_observations = {{.notification_mask = invalid_notification_mask,
                                             .callback = empty_callback ? std::function<void()>{}
                                                                        : std::function<void()>{[] {}}}},
            .durable_terminal_observer = [&terminal_calls](const CopyOperationDurableTerminalOutcome &) {
                ++terminal_calls;
            }});

    REQUIRE_FALSE(submitted);
    CHECK(submitted.error().code == CopyOperationOrchestratorErrorCode::OperationConfigurationFailed);
    CHECK(additions == 0);
    CHECK(terminal_calls == 0);
    CHECK(pool->Empty());
    CHECK(custodian->PendingCount() == 0);
    CHECK(weak_operation.expired());
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Failed);
    CHECK(snapshot[0].item_results.empty());
}

TEST_CASE(PREFIX "durable failure suppresses the generic success callback and contains observer exceptions",
          "[copy-operation-orchestrator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int completions{0};
    std::atomic_int removals{0};
    std::atomic_int terminal_calls{0};
    std::atomic_bool terminal_saw_exact_failure{false};
    pool->SetOperationCompletionCallback([&](const std::shared_ptr<Operation> &) { ++completions; });
    pool->ObserveUnticketed(Pool::NotifyAboutRemoval, [&] { ++removals; });

    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [] {
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorUnknownFailure()};
            });
        },
        custodian);

    REQUIRE(orchestrator.Submit(
        CopyOrchestratorReview("unknown-failure", temporary),
        {},
        CopyOperationSubmissionHooks{
            .durable_terminal_observer = [journal, &terminal_calls, &terminal_saw_exact_failure](
                                             const CopyOperationDurableTerminalOutcome &_outcome) {
                ++terminal_calls;
                const auto snapshot = journal->Snapshot();
                terminal_saw_exact_failure =
                    _outcome.plan_id == "unknown-failure" && _outcome.state == OperationJournalState::Failed &&
                    _outcome.item_results == std::vector{CopyOrchestratorUnknownFailure()} &&
                    _outcome.confirmation == CopyOperationDurableTerminalConfirmation::Finalized &&
                    snapshot.size() == 1 && snapshot[0].state == OperationJournalState::Failed &&
                    snapshot[0].item_results == std::vector{CopyOrchestratorUnknownFailure()};
                throw std::runtime_error{"presentation failure"};
            }}));

    operation->job.done = true;
    REQUIRE(operation->Wait(1s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->Empty(); }));
    CHECK(terminal_calls == 1);
    CHECK(terminal_saw_exact_failure);
    CHECK(removals == 1);
    CHECK(completions == 0);
    CHECK(custodian->PendingCount() == 0);
}

TEST_CASE(PREFIX "durably terminates every pre-running rejection without enqueue", "[copy-operation-orchestrator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int additions{0};
    std::atomic_int factory_calls{0};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });

    CopyOperationExecutionFactoryError factory_error = CopyOperationExecutionFactoryError::Rejected;
    bool valid_product = true;
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [&](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            ++factory_calls;
            if( !valid_product )
                return std::expected<CopyOperationExecutionProduct, CopyOperationExecutionFactoryError>{
                    CopyOperationOrchestratorTesting::MakeExecutionProduct({}, {})};
            return std::expected<CopyOperationExecutionProduct, CopyOperationExecutionFactoryError>{
                std::unexpected(factory_error)};
        },
        custodian);

    CopyOperationOrchestrator::CancelChecker cancel;
    CopyOperationOrchestratorErrorCode expected_error = CopyOperationOrchestratorErrorCode::ExecutionFactoryFailed;
    OperationJournalState expected_state = OperationJournalState::Failed;
    SECTION("initial cancellation")
    {
        cancel = [] { return true; };
        expected_error = CopyOperationOrchestratorErrorCode::Cancelled;
        expected_state = OperationJournalState::Cancelled;
    }
    SECTION("factory cancellation")
    {
        factory_error = CopyOperationExecutionFactoryError::Cancelled;
        expected_error = CopyOperationOrchestratorErrorCode::Cancelled;
        expected_state = OperationJournalState::Cancelled;
    }
    SECTION("factory rejection")
    {
    }
    SECTION("invalid product")
    {
        valid_product = false;
        expected_error = CopyOperationOrchestratorErrorCode::InvalidExecutionProduct;
    }

    const int expected_factory_calls = cancel ? 0 : 1;
    const auto submitted =
        orchestrator.Submit(CopyOrchestratorReview("pre-running-rejection", temporary), std::move(cancel));
    REQUIRE_FALSE(submitted);
    CHECK(submitted.error().code == expected_error);
    CHECK(additions == 0);
    CHECK(factory_calls == expected_factory_calls);
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == expected_state);
    CHECK(snapshot[0].item_results.empty());
    CHECK(pool->Empty());
}

TEST_CASE(PREFIX "final cancellation atomically records an item and never enqueues", "[copy-operation-orchestrator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int additions{0};
    std::atomic_int cancel_calls{0};
    std::atomic_int terminal_calls{0};
    std::optional<CopyOperationDurableTerminalOutcome> terminal_outcome;
    std::atomic_bool second_cancel_saw_running{false};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [] {
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorSuccess()};
            });
        },
        custodian);

    auto result = orchestrator.Submit(
        CopyOrchestratorReview("final-cancel", temporary),
        [&] {
            const int call = ++cancel_calls;
            if( call == 2 ) {
                const auto snapshot = journal->Snapshot();
                second_cancel_saw_running = snapshot.size() == 1 && snapshot[0].state == OperationJournalState::Running;
            }
            return call == 2;
        },
        CopyOperationSubmissionHooks{
            .durable_terminal_observer = [&terminal_calls,
                                          &terminal_outcome](const CopyOperationDurableTerminalOutcome &_outcome) {
                ++terminal_calls;
                terminal_outcome = _outcome;
            }});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == CopyOperationOrchestratorErrorCode::Cancelled);
    CHECK(second_cancel_saw_running);
    CHECK(additions == 0);
    CHECK(operation->State() == OperationState::Cold);
    CHECK(terminal_calls == 1);
    REQUIRE(terminal_outcome);
    CHECK(terminal_outcome->plan_id == "final-cancel");
    CHECK(terminal_outcome->state == OperationJournalState::Cancelled);
    const auto *const terminal_item_result = terminal_outcome->SingleItemResult();
    REQUIRE(terminal_item_result);
    CHECK(terminal_item_result->status == OperationJournalItemStatus::Cancelled);
    CHECK(terminal_outcome->confirmation == CopyOperationDurableTerminalConfirmation::Finalized);
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Cancelled);
    REQUIRE(snapshot[0].item_results.size() == 1);
    CHECK(snapshot[0].item_results[0].status == OperationJournalItemStatus::Cancelled);
    CHECK(pool->Empty());
}

TEST_CASE(PREFIX "records a cancelled batch against every source it named",
          "[copy-operation-orchestrator][batch-durable-terminal]")
{
    // The cancellation happens before any item runs, which is true of the whole plan. Recording it
    // against the first source alone would be a statement about that source in particular - legal for
    // the journal, which lets a cancelled entry omit items, and wrong.
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int additions{0};
    std::atomic_int cancel_calls{0};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [] {
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorSuccess()};
            });
        },
        custodian);

    const auto result = orchestrator.Submit(CopyOrchestratorBatchReview("batch-final-cancel", temporary), [&] {
        return ++cancel_calls == 2;
    });

    REQUIRE_FALSE(result);
    CHECK(result.error().code == CopyOperationOrchestratorErrorCode::Cancelled);
    CHECK(additions == 0);
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Cancelled);
    REQUIRE(snapshot[0].item_results.size() == 2);
    CHECK(snapshot[0].item_results[0].item_index == 0);
    CHECK(snapshot[0].item_results[1].item_index == 1);
    for( const auto &item_result : snapshot[0].item_results ) {
        CHECK(item_result.status == OperationJournalItemStatus::Cancelled);
        CHECK(item_result.destination_publication == OperationJournalPublicationState::NotPublished);
    }
    CHECK(pool->Empty());
}

TEST_CASE(PREFIX "maps Pool shutdown rejection to durable cancellation", "[copy-operation-orchestrator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    pool->StopAndWaitForShutdown();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int additions{0};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [] {
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorSuccess()};
            });
        },
        custodian);

    const auto result = orchestrator.Submit(CopyOrchestratorReview("shutdown-rejection", temporary));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == CopyOperationOrchestratorErrorCode::EnqueueRejected);
    CHECK(result.error().enqueue_result == PoolEnqueueResult::ShuttingDown);
    CHECK(additions == 0);
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Cancelled);
    REQUIRE(snapshot[0].item_results.size() == 1);
    CHECK(snapshot[0].item_results[0].status == OperationJournalItemStatus::Cancelled);
}

TEST_CASE(PREFIX "retains terminal residency while typed outcome is unavailable", "[copy-operation-orchestrator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_bool ready{false};
    std::atomic_int accessor_calls{0};
    std::atomic_int removals{0};
    pool->ObserveUnticketed(Pool::NotifyAboutRemoval, [&] { ++removals; });

    CopyOperationTerminalResultError unavailable = CopyOperationTerminalResultError::Pending;
    bool inconsistent = false;
    SECTION("pending")
    {
    }
    SECTION("inconsistent")
    {
        unavailable = CopyOperationTerminalResultError::Inconsistent;
        inconsistent = true;
    }
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation, &ready, &accessor_calls, unavailable](ReviewedVFSOperationPreflight,
                                                          CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(
                operation, [&ready, &accessor_calls, unavailable] {
                    ++accessor_calls;
                    if( !ready )
                        return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                            std::unexpected(unavailable)};
                    return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                        CopyOrchestratorSuccess()};
                });
        },
        custodian);

    REQUIRE(orchestrator.Submit(CopyOrchestratorReview("typed-outcome", temporary)));
    operation->job.done = true;
    REQUIRE(operation->Wait(1s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->FinalizingOperationsCount() == 1; }));
    CHECK(pool->FinalizingOperations() == std::vector<std::shared_ptr<Operation>>{operation});
    CHECK(pool->RunningOperations().empty());
    CHECK(journal->Snapshot()[0].state == OperationJournalState::Running);
    CHECK(removals == 0);
    CHECK(accessor_calls == 1);

    ready = true;
    if( inconsistent ) {
        CHECK(pool->RetryFinalization(operation) == PoolRetryFinalizationResult::Retained);
        CHECK(journal->Snapshot()[0].state == OperationJournalState::Running);
        CHECK(accessor_calls == 1);
        CHECK(removals == 0);
        CHECK(custodian->Retry("typed-outcome").status == CopyOperationRunReceiptCustodyStatus::ContractViolation);
        CHECK_FALSE(pool->Empty());
    }
    else {
        CHECK(pool->RetryFinalization(operation) == PoolRetryFinalizationResult::Released);
        CHECK(journal->Snapshot()[0].state == OperationJournalState::Completed);
        CHECK(accessor_calls == 2);
        CHECK(removals == 1);
        CHECK(pool->Empty());
    }
}

TEST_CASE(PREFIX "retains and retries the exact terminal result after persistence failure",
          "[copy-operation-orchestrator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto syscalls = OperationJournalTesting::DefaultSyscalls();
    auto journal = CopyOrchestratorJournal(directory, syscalls);
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int accessor_calls{0};
    std::atomic_int removals{0};
    std::atomic_int terminal_calls{0};
    std::optional<CopyOperationDurableTerminalOutcome> terminal_outcome;
    pool->ObserveUnticketed(Pool::NotifyAboutRemoval, [&] { ++removals; });
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation, &accessor_calls](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [&accessor_calls] {
                ++accessor_calls;
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorSuccess()};
            });
        },
        custodian);
    REQUIRE(
        orchestrator.Submit(CopyOrchestratorReview("persist-retry", temporary),
                            {},
                            CopyOperationSubmissionHooks{
                                .durable_terminal_observer = [&terminal_calls, &terminal_outcome](
                                                                 const CopyOperationDurableTerminalOutcome &_outcome) {
                                    ++terminal_calls;
                                    terminal_outcome = _outcome;
                                }}));

    const auto real_open_at = syscalls->open_at;
    std::atomic_bool fail_once{true};
    syscalls->open_at = [real_open_at, &fail_once](int _directory, const char *_path, int _flags, mode_t _mode) {
        if( fail_once.exchange(false) ) {
            errno = EIO;
            return -1;
        }
        return real_open_at(_directory, _path, _flags, _mode);
    };
    operation->job.done = true;
    REQUIRE(operation->Wait(1s));
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->FinalizingOperationsCount() == 1; }));
    CHECK(journal->Snapshot()[0].state == OperationJournalState::Running);
    CHECK(accessor_calls == 1);
    CHECK(removals == 0);
    CHECK(terminal_calls == 0);

    CHECK(pool->RetryFinalization(operation) == PoolRetryFinalizationResult::Released);
    CHECK(journal->Snapshot()[0].state == OperationJournalState::Completed);
    CHECK(accessor_calls == 1);
    CHECK(removals == 1);
    CHECK(terminal_calls == 1);
    REQUIRE(terminal_outcome);
    CHECK(terminal_outcome->plan_id == "persist-retry");
    CHECK(terminal_outcome->state == OperationJournalState::Completed);
    CHECK(terminal_outcome->item_results == std::vector{CopyOrchestratorSuccess()});
    CHECK(terminal_outcome->confirmation == CopyOperationDurableTerminalConfirmation::Finalized);
    CHECK(pool->Empty());
}

TEST_CASE(PREFIX "retains post-running cancellation authority for exact retry without enqueue",
          "[copy-operation-orchestrator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto syscalls = OperationJournalTesting::DefaultSyscalls();
    auto journal = CopyOrchestratorJournal(directory, syscalls);
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int additions{0};
    std::atomic_int terminal_calls{0};
    std::optional<CopyOperationDurableTerminalOutcome> terminal_outcome;
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });
    const auto real_open_at = syscalls->open_at;
    std::atomic_bool inject_failure{false};
    syscalls->open_at = [real_open_at, &inject_failure](int _directory, const char *_path, int _flags, mode_t _mode) {
        if( inject_failure.exchange(false) ) {
            errno = EIO;
            return -1;
        }
        return real_open_at(_directory, _path, _flags, _mode);
    };
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [] {
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorSuccess()};
            });
        },
        custodian);
    std::atomic_int cancel_calls{0};
    const auto result = orchestrator.Submit(
        CopyOrchestratorReview("cancel-persist-blocker", temporary),
        [&] {
            if( ++cancel_calls != 2 )
                return false;
            inject_failure = true;
            return true;
        },
        CopyOperationSubmissionHooks{
            .durable_terminal_observer = [&terminal_calls,
                                          &terminal_outcome](const CopyOperationDurableTerminalOutcome &_outcome) {
                ++terminal_calls;
                terminal_outcome = _outcome;
            }});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == CopyOperationOrchestratorErrorCode::RunningFinalizationFailed);
    CHECK(result.error().recovery_disposition == CopyOperationRunReceiptRecoveryDisposition::RetryRequired);
    CHECK(additions == 0);
    CHECK(pool->Empty());
    auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Running);
    CHECK(snapshot[0].item_results.empty());
    CHECK(custodian->PendingCount() == 1);
    CHECK(terminal_calls == 0);

    const auto retry = custodian->Retry("cancel-persist-blocker");
    CHECK(retry.status == CopyOperationRunReceiptCustodyStatus::Finalized);
    CHECK_FALSE(retry.journal_error);
    CHECK(custodian->PendingCount() == 0);
    CHECK(terminal_calls == 1);
    REQUIRE(terminal_outcome);
    CHECK(terminal_outcome->plan_id == "cancel-persist-blocker");
    CHECK(terminal_outcome->state == OperationJournalState::Cancelled);
    CHECK(terminal_outcome->confirmation == CopyOperationDurableTerminalConfirmation::Finalized);
    CHECK(additions == 0);
    CHECK(pool->Empty());
    snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Cancelled);
    REQUIRE(snapshot[0].item_results.size() == 1);
    CHECK(snapshot[0].item_results[0].status == OperationJournalItemStatus::Cancelled);
    CHECK(snapshot[0].item_results[0].destination_publication == OperationJournalPublicationState::NotPublished);
}

TEST_CASE(PREFIX "reconciles exact post-rename cancellation evidence against the original storage",
          "[copy-operation-orchestrator]")
{
    CopyOrchestratorDirectory directory;
    CopyOrchestratorDirectory wrong_directory;
    TempTestDir temporary;
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int terminal_calls{0};
    std::optional<CopyOperationDurableTerminalOutcome> terminal_outcome;
    bool publish_candidate = false;
    CopyOperationRunReceiptReconciliationStatus expected_reconciliation =
        CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed;
    SECTION("terminal candidate won the rename")
    {
        publish_candidate = true;
        expected_reconciliation = CopyOperationRunReceiptReconciliationStatus::TerminalConfirmed;
    }
    SECTION("running snapshot remained durable")
    {
    }

    {
        auto syscalls = OperationJournalTesting::DefaultSyscalls();
        auto journal = CopyOrchestratorJournal(directory, syscalls);
        auto pool = Pool::Make();
        auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
        const auto real_rename_at = syscalls->rename_at;
        std::atomic_bool inject_failure{false};
        syscalls->rename_at = [real_rename_at, &inject_failure, publish_candidate](
                                  int _from_directory, const char *_from, int _to_directory, const char *_to) {
            if( !inject_failure.exchange(false) )
                return real_rename_at(_from_directory, _from, _to_directory, _to);
            if( publish_candidate ) {
                const int renamed = real_rename_at(_from_directory, _from, _to_directory, _to);
                if( renamed != 0 )
                    return renamed;
            }
            errno = EIO;
            return -1;
        };
        auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
            journal,
            pool,
            [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
                return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [] {
                    return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                        CopyOrchestratorSuccess()};
                });
            },
            custodian);
        std::atomic_int cancel_calls{0};
        const auto submitted = orchestrator.Submit(
            CopyOrchestratorReview("cancel-reconcile", temporary),
            [&] {
                if( ++cancel_calls != 2 )
                    return false;
                inject_failure = true;
                return true;
            },
            CopyOperationSubmissionHooks{
                .durable_terminal_observer = [&terminal_calls,
                                              &terminal_outcome](const CopyOperationDurableTerminalOutcome &_outcome) {
                    ++terminal_calls;
                    terminal_outcome = _outcome;
                }});
        REQUIRE_FALSE(submitted);
        CHECK(submitted.error().code == CopyOperationOrchestratorErrorCode::RunningFinalizationFailed);
        CHECK(submitted.error().recovery_disposition == CopyOperationRunReceiptRecoveryDisposition::ReconcileRequired);
        CHECK(pool->Empty());
        CHECK(custodian->PendingCount() == 1);
        CHECK(custodian->Retry("cancel-reconcile").status == CopyOperationRunReceiptCustodyStatus::ReconcileRequired);
        CHECK(terminal_calls == 0);
    }

    auto wrong = CopyOrchestratorJournal(wrong_directory);
    CHECK(custodian->Reconcile("cancel-reconcile", *wrong).status ==
          CopyOperationRunReceiptReconciliationStatus::Mismatch);
    CHECK(custodian->PendingCount() == 1);
    CHECK(terminal_calls == 0);

    auto reopened = CopyOrchestratorJournal(directory);
    const auto reconciled = custodian->Reconcile("cancel-reconcile", *reopened);
    CHECK(reconciled.status == expected_reconciliation);
    CHECK_FALSE(reconciled.pool_release_required);
    CHECK(custodian->PendingCount() == 0);
    CHECK(terminal_calls == 1);
    REQUIRE(terminal_outcome);
    CHECK(terminal_outcome->plan_id == "cancel-reconcile");
    CHECK(terminal_outcome->confirmation == (publish_candidate
                                                 ? CopyOperationDurableTerminalConfirmation::ReconciledTerminal
                                                 : CopyOperationDurableTerminalConfirmation::ReconciledInterrupted));
    CHECK(custodian->Retry("cancel-reconcile").status == CopyOperationRunReceiptCustodyStatus::NotFound);
    const auto snapshot = reopened->Snapshot();
    REQUIRE(snapshot.size() == 1);
    if( publish_candidate ) {
        CHECK(snapshot[0].state == OperationJournalState::Cancelled);
        REQUIRE(snapshot[0].item_results.size() == 1);
        CHECK(snapshot[0].item_results[0].status == OperationJournalItemStatus::Cancelled);
    }
    else {
        CHECK(snapshot[0].state == OperationJournalState::Interrupted);
        CHECK(snapshot[0].item_results.empty());
    }
}

TEST_CASE(PREFIX "retains accepted Pool residency through exact reopen reconciliation", "[copy-operation-orchestrator]")
{
    CopyOrchestratorDirectory directory;
    CopyOrchestratorDirectory wrong_directory;
    TempTestDir temporary;
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    std::atomic_int terminal_calls{0};
    std::optional<CopyOperationDurableTerminalOutcome> terminal_outcome;
    std::atomic_int removals{0};
    std::atomic_int completions{0};
    std::atomic_bool removal_saw_terminal{false};
    std::atomic_bool completion_saw_terminal{false};
    pool->ObserveUnticketed(Pool::NotifyAboutRemoval, [&] {
        ++removals;
        removal_saw_terminal = terminal_calls == 1;
        throw std::runtime_error{"intentional reconciled removal observer failure"};
    });
    pool->SetOperationCompletionCallback([&](const std::shared_ptr<Operation> &) {
        ++completions;
        completion_saw_terminal = terminal_calls == 1;
    });
    bool publish_candidate = false;
    bool drop_pool_before_release = false;
    bool concurrent_release = false;
    CopyOperationRunReceiptReconciliationStatus expected_reconciliation =
        CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed;
    SECTION("terminal candidate won the rename")
    {
        publish_candidate = true;
        expected_reconciliation = CopyOperationRunReceiptReconciliationStatus::TerminalConfirmed;
    }
    SECTION("running snapshot remained durable")
    {
    }
    SECTION("pool owner disappears after interrupted reconciliation")
    {
        drop_pool_before_release = true;
    }
    SECTION("concurrent release waits for the exact Pool erase")
    {
        concurrent_release = true;
    }

    {
        auto syscalls = OperationJournalTesting::DefaultSyscalls();
        auto journal = CopyOrchestratorJournal(directory, syscalls);
        const auto real_rename_at = syscalls->rename_at;
        std::atomic_bool inject_failure{false};
        syscalls->rename_at = [real_rename_at, &inject_failure, publish_candidate](
                                  int _from_directory, const char *_from, int _to_directory, const char *_to) {
            if( !inject_failure.exchange(false) )
                return real_rename_at(_from_directory, _from, _to_directory, _to);
            if( publish_candidate ) {
                const int renamed = real_rename_at(_from_directory, _from, _to_directory, _to);
                if( renamed != 0 )
                    return renamed;
            }
            errno = EIO;
            return -1;
        };
        auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
            journal,
            pool,
            [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
                return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [] {
                    return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                        CopyOrchestratorSuccess()};
                });
            },
            custodian);
        REQUIRE(orchestrator.Submit(
            CopyOrchestratorReview("pool-reconcile", temporary),
            {},
            CopyOperationSubmissionHooks{
                .durable_terminal_observer = [&terminal_calls,
                                              &terminal_outcome](const CopyOperationDurableTerminalOutcome &_outcome) {
                    ++terminal_calls;
                    terminal_outcome = _outcome;
                }}));
        CHECK(custodian->Retry("pool-reconcile").status == CopyOperationRunReceiptCustodyStatus::Busy);
        inject_failure = true;
        operation->job.done = true;
        REQUIRE(operation->Wait(1s));
        REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->FinalizingOperationsCount() == 1; }));
        CHECK_FALSE(pool->Empty());
        CHECK(custodian->PendingCount() == 1);
        CHECK(custodian->Retry("pool-reconcile").status == CopyOperationRunReceiptCustodyStatus::ReconcileRequired);
    }

    auto wrong = CopyOrchestratorJournal(wrong_directory);
    CHECK(custodian->Reconcile("pool-reconcile", *wrong).status ==
          CopyOperationRunReceiptReconciliationStatus::Mismatch);
    CHECK(pool->FinalizingOperationsCount() == 1);

    auto reopened = CopyOrchestratorJournal(directory);
    const auto reconciled = custodian->Reconcile("pool-reconcile", *reopened);
    CHECK(reconciled.status == expected_reconciliation);
    CHECK(reconciled.pool_release_required);
    CHECK(custodian->PendingCount() == 1);
    CHECK(pool->FinalizingOperationsCount() == 1);
    CHECK(terminal_calls == 0);
    if( concurrent_release ) {
        auto barrier = std::make_shared<CopyOrchestratorReleaseBarrier>();
        REQUIRE(CopyOperationRunReceiptCustodianTesting::SetReleaseFinalizerBarrier(
            *custodian, "pool-reconcile", [barrier] { barrier->EnterAndWait(); }));
        std::optional<PoolRetryFinalizationResult> external_release;
        std::exception_ptr external_error;
        std::thread release_thread{[&] {
            try {
                external_release = pool->RetryFinalization(operation);
            } catch( ... ) {
                external_error = std::current_exception();
            }
        }};
        const bool entered = barrier->WaitUntilEntered(1s);
        if( !entered ) {
            barrier->Release();
            release_thread.join();
            REQUIRE(entered);
        }
        CHECK(pool->FinalizingOperationsCount() == 1);
        CHECK(custodian->ReleaseReconciled("pool-reconcile") == CopyOperationRunReceiptPoolReleaseStatus::InProgress);
        CHECK(custodian->PendingCount() == 1);
        barrier->Release();
        release_thread.join();
        CHECK_FALSE(external_release.has_value());
        CHECK(external_error != nullptr);
        if( external_error )
            CHECK_THROWS_AS(std::rethrow_exception(external_error), std::runtime_error);
        CHECK(pool->Empty());
        CHECK(custodian->PendingCount() == 1);
        CHECK(custodian->ReleaseReconciled("pool-reconcile") == CopyOperationRunReceiptPoolReleaseStatus::Released);
    }
    else if( drop_pool_before_release ) {
        const auto weak_pool = std::weak_ptr<Pool>{pool};
        pool.reset();
        CHECK(weak_pool.expired());
        CHECK(custodian->ReleaseReconciled("pool-reconcile") == CopyOperationRunReceiptPoolReleaseStatus::Released);
    }
    else if( publish_candidate ) {
        CHECK_THROWS_AS(pool->RetryFinalization(operation), std::runtime_error);
        CHECK(pool->Empty());
        CHECK(custodian->PendingCount() == 1);
        operation.reset();
        CHECK(custodian->ReleaseReconciled("pool-reconcile") == CopyOperationRunReceiptPoolReleaseStatus::Released);
    }
    else {
        CHECK(custodian->ReleaseReconciled("pool-reconcile") == CopyOperationRunReceiptPoolReleaseStatus::Released);
    }
    CHECK(custodian->PendingCount() == 0);
    CHECK(terminal_calls == 1);
    REQUIRE(terminal_outcome);
    CHECK(terminal_outcome->plan_id == "pool-reconcile");
    CHECK(terminal_outcome->state ==
          (publish_candidate ? OperationJournalState::Completed : OperationJournalState::Interrupted));
    CHECK(terminal_outcome->confirmation == (publish_candidate
                                                 ? CopyOperationDurableTerminalConfirmation::ReconciledTerminal
                                                 : CopyOperationDurableTerminalConfirmation::ReconciledInterrupted));
    const auto *const reconciled_item_result = terminal_outcome->SingleItemResult();
    CHECK(static_cast<bool>(reconciled_item_result) == publish_candidate);
    if( publish_candidate )
        CHECK(*reconciled_item_result == CopyOrchestratorSuccess());
    CHECK(removals == (drop_pool_before_release ? 0 : 1));
    CHECK(removal_saw_terminal == !drop_pool_before_release);
    CHECK(completions == (publish_candidate ? 1 : 0));
    CHECK(completion_saw_terminal == publish_candidate);
    CHECK(custodian->ReleaseReconciled("pool-reconcile") == CopyOperationRunReceiptPoolReleaseStatus::NotFound);
    CHECK(terminal_calls == 1);
    if( pool )
        CHECK(pool->Empty());
    const auto snapshot = reopened->Snapshot();
    REQUIRE(snapshot.size() == 1);
    if( publish_candidate ) {
        CHECK(snapshot[0].state == OperationJournalState::Completed);
        CHECK(snapshot[0].item_results == std::vector{CopyOrchestratorSuccess()});
    }
    else {
        CHECK(snapshot[0].state == OperationJournalState::Interrupted);
        CHECK(snapshot[0].item_results.empty());
    }
}

TEST_CASE(PREFIX "handles terminal Pool callback before accepted enqueue returns", "[copy-operation-orchestrator]")
{
    CopyOrchestratorDirectory directory;
    TempTestDir temporary;
    auto journal = CopyOrchestratorJournal(directory);
    auto pool = Pool::Make();
    auto operation = std::make_shared<CopyOrchestratorControlledOperation>();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_bool start_observer_saw_finished{false};
    operation->job.done = true;
    operation->ObserveUnticketed(Operation::NotifyAboutStart, [operation, &start_observer_saw_finished] {
        start_observer_saw_finished = operation->Wait(1s);
    });
    auto orchestrator = CopyOperationOrchestratorTesting::CreateInjected(
        journal,
        pool,
        [operation](ReviewedVFSOperationPreflight, CopyOperationOrchestrator::CancelChecker) {
            return CopyOperationOrchestratorTesting::MakeExecutionProduct(operation, [] {
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{
                    CopyOrchestratorSuccess()};
            });
        },
        custodian);

    REQUIRE(orchestrator.Submit(CopyOrchestratorReview("synchronous-finish", temporary)));
    CHECK(start_observer_saw_finished);
    REQUIRE(CopyOrchestratorCheckUntil([&] { return pool->Empty(); }));
    CHECK(custodian->PendingCount() == 0);
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Completed);
    CHECK(snapshot[0].item_results == std::vector{CopyOrchestratorSuccess()});
}

} // namespace nc::ops

#undef PREFIX
