// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Operations/CopyOperationRecoveryCoordinatorTesting.h>

#include <Operations/OperationCenterCoordinator.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace {

using nc::core::CopyOperationRecoveryCoordinator;
using nc::core::CopyOperationRecoveryCoordinatorTesting;
using nc::core::CopyOperationRecoveryHistoryRefreshResult;
using nc::core::CopyOperationRecoveryHistoryRefreshStatus;
using nc::core::CopyOperationRecoveryServiceError;
using nc::core::CopyOperationRecoveryServiceStep;
using nc::core::RetryDeferredHistoryProjection;
using nc::core::ServiceCopyRecoveryAndRefreshHistory;
using nc::ops::CopyOperationRunReceiptCustodyResult;
using nc::ops::CopyOperationRunReceiptCustodyStatus;
using nc::ops::CopyOperationRunReceiptPoolReleaseStatus;
using nc::ops::CopyOperationRunReceiptReconciliationResult;
using nc::ops::CopyOperationRunReceiptReconciliationStatus;
using nc::ops::OperationJournal;
using nc::ops::OperationJournalError;
using nc::ops::OperationJournalErrorCode;
using nc::ops::OperationJournalState;
using nc::ops::OperationCenterCoordinator;
using nc::ops::OperationCenterCoordinatorErrorCode;
using nc::ops::OperationRecordState;
using nc::ops::OperationPlan;
using nc::ops::OperationPlanConflictDecision;
using nc::ops::OperationPlanConflictPolicy;
using nc::ops::OperationPlanConflictScope;
using nc::ops::OperationPlanDestinationInput;
using nc::ops::OperationPlanDestinationKind;
using nc::ops::OperationPlanInput;
using nc::ops::OperationPlanSourceInput;
using nc::ops::OperationPlanType;
using namespace std::chrono_literals;

std::string CanonicalDirectory(const TempTestDir &_directory)
{
    return std::filesystem::canonical(_directory.directory).string();
}

OperationPlan Plan(std::string _id, const OperationPlanType _type = OperationPlanType::Copy)
{
    auto plan = OperationPlan::Create(OperationPlanInput{
        .plan_id = std::move(_id),
        .type = _type,
        .sources = {OperationPlanSourceInput{"native", "/source/item"}},
        .destination = OperationPlanDestinationInput{"native", "/destination", OperationPlanDestinationKind::Directory},
        .conflict_policy =
            OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem},
        .created_at = OperationPlan::TimePoint{1'700'000'000s},
    });
    REQUIRE(plan);
    return std::move(*plan);
}

std::shared_ptr<OperationJournal> OpenJournal(const std::string &_directory)
{
    auto opened = OperationJournal::Open(_directory);
    REQUIRE(opened);
    return std::make_shared<OperationJournal>(std::move(*opened));
}

CopyOperationRecoveryCoordinatorTesting::Services PassiveServices()
{
    return {
        .retry =
            [](std::string_view) {
                return CopyOperationRunReceiptCustodyResult{.status = CopyOperationRunReceiptCustodyStatus::NotFound};
            },
        .reopen = [](const std::string_view _directory) { return OperationJournal::Open(_directory); },
        .reconcile =
            [](std::string_view, const OperationJournal &) {
                return CopyOperationRunReceiptReconciliationResult{
                    .status = CopyOperationRunReceiptReconciliationStatus::NotFound};
            },
        .release_reconciled = [](std::string_view) { return CopyOperationRunReceiptPoolReleaseStatus::NotFound; },
        .pending_count = [] { return size_t{0}; },
    };
}

} // namespace

#define PREFIX "CopyOperationRecoveryCoordinator "

TEST_CASE(PREFIX "publishes only Copy entries observed as Interrupted at startup")
{
    TempTestDir temporary;
    const auto directory = CanonicalDirectory(temporary);
    {
        auto journal = OpenJournal(directory);
        auto copy = journal->Admit(Plan("copy-interrupted"));
        REQUIRE(copy);
        auto move = journal->Admit(Plan("move-interrupted", OperationPlanType::Move));
        REQUIRE(move);
        auto terminal = journal->Admit(Plan("copy-terminal"));
        REQUIRE(terminal);
        REQUIRE(journal->FinalizeAdmission(std::move(*terminal), OperationJournalState::Failed));
    }

    auto startup_journal = OpenJournal(directory);
    int service_calls = 0;
    auto services = PassiveServices();
    services.retry = [&](std::string_view) {
        ++service_calls;
        return CopyOperationRunReceiptCustodyResult{.status = CopyOperationRunReceiptCustodyStatus::NotFound};
    };
    auto coordinator =
        CopyOperationRecoveryCoordinatorTesting::Make(std::move(startup_journal), directory, std::move(services));

    const auto &history = coordinator->StartupInterruptedHistory();
    REQUIRE(history.size() == 1);
    CHECK(history[0].plan.Id().Value() == "copy-interrupted");
    CHECK(history[0].state == OperationJournalState::Interrupted);
    CHECK(history[0].item_results.empty());
    CHECK(service_calls == 0);
}

TEST_CASE(PREFIX "stops after one successful receipt retry")
{
    TempTestDir temporary;
    const auto directory = CanonicalDirectory(temporary);
    auto journal = OpenJournal(directory);
    const auto original = std::weak_ptr<OperationJournal>{journal};
    int retries = 0;
    int reopens = 0;
    int reconciliations = 0;
    int releases = 0;
    auto services = PassiveServices();
    services.retry = [&](std::string_view _plan_id) {
        ++retries;
        CHECK(_plan_id == "retry-only");
        return CopyOperationRunReceiptCustodyResult{.status = CopyOperationRunReceiptCustodyStatus::Finalized};
    };
    services.reopen = [&](std::string_view) {
        ++reopens;
        return OperationJournal::Open(directory);
    };
    services.reconcile = [&](std::string_view, const OperationJournal &) {
        ++reconciliations;
        return CopyOperationRunReceiptReconciliationResult{.status =
                                                               CopyOperationRunReceiptReconciliationStatus::NotFound};
    };
    services.release_reconciled = [&](std::string_view) {
        ++releases;
        return CopyOperationRunReceiptPoolReleaseStatus::NotFound;
    };
    auto coordinator =
        CopyOperationRecoveryCoordinatorTesting::Make(std::move(journal), directory, std::move(services));

    const auto result = coordinator->Service("retry-only");

    CHECK(result.error == CopyOperationRecoveryServiceError::None);
    CHECK(result.last_step == CopyOperationRecoveryServiceStep::Retry);
    REQUIRE(result.retry);
    CHECK(result.retry->status == CopyOperationRunReceiptCustodyStatus::Finalized);
    CHECK_FALSE(result.reconciliation);
    CHECK_FALSE(result.release);
    CHECK(retries == 1);
    CHECK(reopens == 0);
    CHECK(reconciliations == 0);
    CHECK(releases == 0);
    CHECK(coordinator->CurrentJournal() == original.lock());
}

TEST_CASE(PREFIX "reopens the exact state directory and releases only confirmed custody")
{
    TempTestDir temporary;
    const auto directory = CanonicalDirectory(temporary);
    auto journal = OpenJournal(directory);
    int retries = 0;
    int reopens = 0;
    int reconciliations = 0;
    int releases = 0;
    std::string reopened_directory;
    auto services = PassiveServices();
    services.retry = [&](std::string_view) {
        ++retries;
        return CopyOperationRunReceiptCustodyResult{.status = CopyOperationRunReceiptCustodyStatus::ReconcileRequired};
    };
    services.reopen = [&](const std::string_view _directory) {
        ++reopens;
        reopened_directory = _directory;
        return OperationJournal::Open(_directory);
    };
    services.reconcile = [&](const std::string_view _plan_id, const OperationJournal &) {
        ++reconciliations;
        CHECK(_plan_id == "reconcile-release");
        return CopyOperationRunReceiptReconciliationResult{
            .status = CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed, .pool_release_required = true};
    };
    services.release_reconciled = [&](const std::string_view _plan_id) {
        ++releases;
        CHECK(_plan_id == "reconcile-release");
        return CopyOperationRunReceiptPoolReleaseStatus::Released;
    };
    auto coordinator =
        CopyOperationRecoveryCoordinatorTesting::Make(std::move(journal), directory, std::move(services));

    const auto result = coordinator->Service("reconcile-release");

    CHECK(result.error == CopyOperationRecoveryServiceError::None);
    CHECK(result.last_step == CopyOperationRecoveryServiceStep::ReleaseReconciled);
    REQUIRE(result.retry);
    CHECK(result.retry->status == CopyOperationRunReceiptCustodyStatus::ReconcileRequired);
    REQUIRE(result.reconciliation);
    CHECK(result.reconciliation->status == CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed);
    REQUIRE(result.release);
    CHECK(*result.release == CopyOperationRunReceiptPoolReleaseStatus::Released);
    CHECK(reopened_directory == directory);
    CHECK(retries == 1);
    CHECK(reopens == 1);
    CHECK(reconciliations == 1);
    CHECK(releases == 1);
    CHECK(coordinator->CurrentJournal());
}

TEST_CASE(PREFIX "fails closed while another owner can still use the current journal")
{
    TempTestDir temporary;
    const auto directory = CanonicalDirectory(temporary);
    auto services = PassiveServices();
    int reopens = 0;
    services.retry = [](std::string_view) {
        return CopyOperationRunReceiptCustodyResult{.status = CopyOperationRunReceiptCustodyStatus::ReconcileRequired};
    };
    services.reopen = [&](const std::string_view _directory) {
        ++reopens;
        return OperationJournal::Open(_directory);
    };
    auto coordinator =
        CopyOperationRecoveryCoordinatorTesting::Make(OpenJournal(directory), directory, std::move(services));
    auto outstanding_authority = coordinator->CurrentJournal();

    const auto blocked = coordinator->Service("still-owned");

    CHECK(blocked.error == CopyOperationRecoveryServiceError::JournalInUse);
    CHECK(blocked.last_step == CopyOperationRecoveryServiceStep::Retry);
    CHECK(reopens == 0);
    CHECK(coordinator->CurrentJournal() == outstanding_authority);
}

TEST_CASE(PREFIX "reports the exact reopen failure and retains no stale journal authority")
{
    TempTestDir temporary;
    const auto directory = CanonicalDirectory(temporary);
    auto services = PassiveServices();
    int reconciliations = 0;
    int releases = 0;
    services.retry = [](std::string_view) {
        return CopyOperationRunReceiptCustodyResult{.status = CopyOperationRunReceiptCustodyStatus::ReconcileRequired};
    };
    services.reopen = [](std::string_view) -> std::expected<OperationJournal, OperationJournalError> {
        return std::unexpected(
            OperationJournalError{.code = OperationJournalErrorCode::JournalAlreadyOpen, .system_error = EWOULDBLOCK});
    };
    services.reconcile = [&](std::string_view, const OperationJournal &) {
        ++reconciliations;
        return CopyOperationRunReceiptReconciliationResult{.status =
                                                               CopyOperationRunReceiptReconciliationStatus::Mismatch};
    };
    services.release_reconciled = [&](std::string_view) {
        ++releases;
        return CopyOperationRunReceiptPoolReleaseStatus::Retained;
    };
    auto coordinator =
        CopyOperationRecoveryCoordinatorTesting::Make(OpenJournal(directory), directory, std::move(services));

    const auto result = coordinator->Service("reopen-failure");

    CHECK(result.error == CopyOperationRecoveryServiceError::JournalReopenFailed);
    CHECK(result.last_step == CopyOperationRecoveryServiceStep::Reopen);
    REQUIRE(result.reopen_error);
    CHECK(result.reopen_error->code == OperationJournalErrorCode::JournalAlreadyOpen);
    CHECK(result.reopen_error->system_error == EWOULDBLOCK);
    CHECK_FALSE(result.reconciliation);
    CHECK_FALSE(result.release);
    CHECK(reconciliations == 0);
    CHECK(releases == 0);
    CHECK_FALSE(coordinator->CurrentJournal());
}

TEST_CASE(PREFIX "does not release Pool authority after an unconfirmed reconciliation")
{
    TempTestDir temporary;
    const auto directory = CanonicalDirectory(temporary);
    auto services = PassiveServices();
    int releases = 0;
    services.retry = [](std::string_view) {
        return CopyOperationRunReceiptCustodyResult{.status = CopyOperationRunReceiptCustodyStatus::ReconcileRequired};
    };
    services.reconcile = [](std::string_view, const OperationJournal &) {
        return CopyOperationRunReceiptReconciliationResult{
            .status = CopyOperationRunReceiptReconciliationStatus::Mismatch, .pool_release_required = true};
    };
    services.release_reconciled = [&](std::string_view) {
        ++releases;
        return CopyOperationRunReceiptPoolReleaseStatus::Released;
    };
    auto coordinator =
        CopyOperationRecoveryCoordinatorTesting::Make(OpenJournal(directory), directory, std::move(services));

    const auto result = coordinator->Service("mismatch");

    CHECK(result.error == CopyOperationRecoveryServiceError::None);
    CHECK(result.last_step == CopyOperationRecoveryServiceStep::Reconcile);
    REQUIRE(result.reconciliation);
    CHECK(result.reconciliation->status == CopyOperationRunReceiptReconciliationStatus::Mismatch);
    CHECK_FALSE(result.release);
    CHECK(releases == 0);
}

TEST_CASE(PREFIX "projects confirmed reopened history into an exact cold Operation Center")
{
    TempTestDir temporary;
    const auto directory = CanonicalDirectory(temporary);
    auto journal = OpenJournal(directory);
    auto initial = journal->Admit(Plan("initial-history"));
    REQUIRE(initial);
    REQUIRE(journal->FinalizeAdmission(std::move(*initial), OperationJournalState::Failed));

    auto operation_center = OperationCenterCoordinator::Create(*journal);
    REQUIRE(operation_center);
    const auto before = (*operation_center)->Model().Snapshot();
    REQUIRE(before.size() == 1);

    auto recovered = journal->Admit(Plan("recovered-history"));
    REQUIRE(recovered);
    REQUIRE(journal->TransitionToRunning(std::move(*recovered)));

    auto services = PassiveServices();
    services.retry = [](std::string_view) {
        return CopyOperationRunReceiptCustodyResult{.status = CopyOperationRunReceiptCustodyStatus::ReconcileRequired};
    };
    services.reconcile = [](std::string_view, const OperationJournal &) {
        return CopyOperationRunReceiptReconciliationResult{
            .status = CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed};
    };
    auto recovery = CopyOperationRecoveryCoordinatorTesting::Make(std::move(journal), directory, std::move(services));

    const auto result = ServiceCopyRecoveryAndRefreshHistory(recovery, *operation_center, "recovered-history");

    CHECK(result.recovery.error == CopyOperationRecoveryServiceError::None);
    REQUIRE(result.recovery.reconciliation);
    CHECK(result.recovery.reconciliation->status == CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed);
    CHECK(result.history_refresh == CopyOperationRecoveryHistoryRefreshStatus::Refreshed);
    CHECK_FALSE(result.history_refresh_error);
    const auto refreshed = (*operation_center)->Model().Snapshot();
    REQUIRE(refreshed.size() == 2);
    CHECK(refreshed[0] == before[0]);
    CHECK(refreshed[1].operation_id.ToString() == "op-2");
    CHECK(refreshed[1].plan_id.Value() == "recovered-history");
    CHECK(refreshed[1].state == OperationRecordState::Interrupted);
    CHECK(refreshed[1].revision == 1);
    CHECK(refreshed[1].finished_at);
    CHECK_FALSE(refreshed[1].started_at);
    CHECK(refreshed[1].controls.can_retry);
    CHECK_FALSE(refreshed[1].controls.can_cancel);
}

TEST_CASE(PREFIX "keeps confirmed custody separate when Operation Center history is busy")
{
    TempTestDir temporary;
    const auto directory = CanonicalDirectory(temporary);
    auto journal = OpenJournal(directory);
    auto initial = journal->Admit(Plan("initial-history"));
    REQUIRE(initial);
    REQUIRE(journal->FinalizeAdmission(std::move(*initial), OperationJournalState::Failed));

    auto operation_center = OperationCenterCoordinator::Create(*journal);
    REQUIRE(operation_center);
    auto staging = (*operation_center)->StageAdmission(*journal, Plan("staged-history"));
    REQUIRE(staging);
    const auto before = (*operation_center)->Model().Snapshot();

    auto recovered = journal->Admit(Plan("recovered-history"));
    REQUIRE(recovered);
    REQUIRE(journal->TransitionToRunning(std::move(*recovered)));

    auto services = PassiveServices();
    services.retry = [](std::string_view) {
        return CopyOperationRunReceiptCustodyResult{.status = CopyOperationRunReceiptCustodyStatus::ReconcileRequired};
    };
    services.reconcile = [](std::string_view, const OperationJournal &) {
        return CopyOperationRunReceiptReconciliationResult{
            .status = CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed};
    };
    auto recovery = CopyOperationRecoveryCoordinatorTesting::Make(std::move(journal), directory, std::move(services));

    const auto result = ServiceCopyRecoveryAndRefreshHistory(recovery, *operation_center, "recovered-history");

    CHECK(result.recovery.error == CopyOperationRecoveryServiceError::None);
    REQUIRE(result.recovery.reconciliation);
    CHECK(result.history_refresh == CopyOperationRecoveryHistoryRefreshStatus::Deferred);
    REQUIRE(result.history_refresh_error);
    CHECK(result.history_refresh_error->code == OperationCenterCoordinatorErrorCode::ColdHistoryBusy);
    CHECK(result.HasDeferredHistoryProjection());
    CHECK((*operation_center)->Model().Snapshot() == before);
}

TEST_CASE(PREFIX "retries one deferred cold-history projection without servicing custody again")
{
    TempTestDir temporary;
    const auto directory = CanonicalDirectory(temporary);
    auto journal = OpenJournal(directory);
    auto initial = journal->Admit(Plan("initial-history"));
    REQUIRE(initial);
    REQUIRE(journal->FinalizeAdmission(std::move(*initial), OperationJournalState::Failed));

    auto operation_center = OperationCenterCoordinator::Create(*journal);
    REQUIRE(operation_center);
    const auto before = (*operation_center)->Model().Snapshot();

    auto recovered = journal->Admit(Plan("recovered-history"));
    REQUIRE(recovered);
    REQUIRE(journal->TransitionToRunning(std::move(*recovered)));

    int retries = 0;
    int reopens = 0;
    int reconciliations = 0;
    int releases = 0;
    auto services = PassiveServices();
    services.retry = [&](std::string_view) {
        ++retries;
        return CopyOperationRunReceiptCustodyResult{.status = CopyOperationRunReceiptCustodyStatus::ReconcileRequired};
    };
    services.reopen = [&](const std::string_view _directory) {
        ++reopens;
        return OperationJournal::Open(_directory);
    };
    services.reconcile = [&](std::string_view, const OperationJournal &) {
        ++reconciliations;
        return CopyOperationRunReceiptReconciliationResult{
            .status = CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed, .pool_release_required = true};
    };
    services.release_reconciled = [&](std::string_view) {
        ++releases;
        return CopyOperationRunReceiptPoolReleaseStatus::Released;
    };
    auto recovery = CopyOperationRecoveryCoordinatorTesting::Make(std::move(journal), directory, std::move(services));

    std::optional<CopyOperationRecoveryHistoryRefreshResult> deferred;
    {
        auto staging = (*operation_center)->StageAdmission(*recovery->CurrentJournal(), Plan("staged-history"));
        REQUIRE(staging);
        deferred.emplace(ServiceCopyRecoveryAndRefreshHistory(recovery, *operation_center, "recovered-history"));
    }

    REQUIRE(deferred);
    CHECK(deferred->history_refresh == CopyOperationRecoveryHistoryRefreshStatus::Deferred);
    CHECK(deferred->HasDeferredHistoryProjection());
    CHECK(deferred->recovery.last_step == CopyOperationRecoveryServiceStep::ReleaseReconciled);
    CHECK(retries == 1);
    CHECK(reopens == 1);
    CHECK(reconciliations == 1);
    CHECK(releases == 1);

    const auto refreshed = RetryDeferredHistoryProjection(recovery, *operation_center, *deferred);

    CHECK(refreshed.history_refresh == CopyOperationRecoveryHistoryRefreshStatus::Refreshed);
    CHECK_FALSE(refreshed.history_refresh_error);
    CHECK_FALSE(refreshed.HasDeferredHistoryProjection());
    CHECK(retries == 1);
    CHECK(reopens == 1);
    CHECK(reconciliations == 1);
    CHECK(releases == 1);
    const auto records = (*operation_center)->Model().Snapshot();
    REQUIRE(records.size() == 2);
    CHECK(records[0] == before[0]);
    CHECK(records[1].plan_id.Value() == "recovered-history");
    CHECK(records[1].state == OperationRecordState::Interrupted);
}

TEST_CASE(PREFIX "consumes its sole deferred projection retry while cold history remains busy")
{
    TempTestDir temporary;
    const auto directory = CanonicalDirectory(temporary);
    auto journal = OpenJournal(directory);
    auto initial = journal->Admit(Plan("initial-history"));
    REQUIRE(initial);
    REQUIRE(journal->FinalizeAdmission(std::move(*initial), OperationJournalState::Failed));

    auto operation_center = OperationCenterCoordinator::Create(*journal);
    REQUIRE(operation_center);
    const auto before = (*operation_center)->Model().Snapshot();
    auto staging = (*operation_center)->StageAdmission(*journal, Plan("staged-history"));
    REQUIRE(staging);

    auto recovered = journal->Admit(Plan("recovered-history"));
    REQUIRE(recovered);
    REQUIRE(journal->TransitionToRunning(std::move(*recovered)));

    int retries = 0;
    int reopens = 0;
    int reconciliations = 0;
    auto services = PassiveServices();
    services.retry = [&](std::string_view) {
        ++retries;
        return CopyOperationRunReceiptCustodyResult{.status = CopyOperationRunReceiptCustodyStatus::ReconcileRequired};
    };
    services.reopen = [&](const std::string_view _directory) {
        ++reopens;
        return OperationJournal::Open(_directory);
    };
    services.reconcile = [&](std::string_view, const OperationJournal &) {
        ++reconciliations;
        return CopyOperationRunReceiptReconciliationResult{
            .status = CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed};
    };
    auto recovery = CopyOperationRecoveryCoordinatorTesting::Make(std::move(journal), directory, std::move(services));

    const auto deferred = ServiceCopyRecoveryAndRefreshHistory(recovery, *operation_center, "recovered-history");
    REQUIRE(deferred.history_refresh == CopyOperationRecoveryHistoryRefreshStatus::Deferred);
    REQUIRE(deferred.HasDeferredHistoryProjection());

    const auto exhausted = RetryDeferredHistoryProjection(recovery, *operation_center, deferred);
    CHECK(exhausted.history_refresh == CopyOperationRecoveryHistoryRefreshStatus::RetryExhausted);
    REQUIRE(exhausted.history_refresh_error);
    CHECK(exhausted.history_refresh_error->code == OperationCenterCoordinatorErrorCode::ColdHistoryBusy);
    CHECK_FALSE(exhausted.HasDeferredHistoryProjection());
    CHECK_FALSE(deferred.HasDeferredHistoryProjection());
    CHECK((*operation_center)->Model().Snapshot() == before);

    const auto reused = RetryDeferredHistoryProjection(recovery, *operation_center, deferred);
    CHECK(reused.history_refresh == CopyOperationRecoveryHistoryRefreshStatus::RetryExhausted);
    CHECK_FALSE(reused.history_refresh_error);
    CHECK_FALSE(reused.HasDeferredHistoryProjection());
    CHECK(retries == 1);
    CHECK(reopens == 1);
    CHECK(reconciliations == 1);
}

TEST_CASE(PREFIX "does not mint a retry for a nonbusy history projection failure")
{
    TempTestDir temporary;
    const auto directory = CanonicalDirectory(temporary);
    const auto foreign_path = temporary.directory / "foreign";
    REQUIRE(std::filesystem::create_directory(foreign_path));
    const auto foreign_directory = std::filesystem::canonical(foreign_path).string();
    auto journal = OpenJournal(directory);
    auto initial = journal->Admit(Plan("initial-history"));
    REQUIRE(initial);
    REQUIRE(journal->FinalizeAdmission(std::move(*initial), OperationJournalState::Failed));

    auto foreign_journal = OpenJournal(foreign_directory);
    auto foreign_operation_center = OperationCenterCoordinator::Create(*foreign_journal);
    REQUIRE(foreign_operation_center);
    const auto before = (*foreign_operation_center)->Model().Snapshot();

    auto recovered = journal->Admit(Plan("recovered-history"));
    REQUIRE(recovered);
    REQUIRE(journal->TransitionToRunning(std::move(*recovered)));

    auto services = PassiveServices();
    services.retry = [](std::string_view) {
        return CopyOperationRunReceiptCustodyResult{.status = CopyOperationRunReceiptCustodyStatus::ReconcileRequired};
    };
    services.reconcile = [](std::string_view, const OperationJournal &) {
        return CopyOperationRunReceiptReconciliationResult{
            .status = CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed};
    };
    auto recovery = CopyOperationRecoveryCoordinatorTesting::Make(std::move(journal), directory, std::move(services));

    const auto result = ServiceCopyRecoveryAndRefreshHistory(recovery, *foreign_operation_center, "recovered-history");

    CHECK(result.recovery.error == CopyOperationRecoveryServiceError::None);
    REQUIRE(result.recovery.reconciliation);
    CHECK(result.history_refresh == CopyOperationRecoveryHistoryRefreshStatus::ProjectionFailed);
    REQUIRE(result.history_refresh_error);
    CHECK(result.history_refresh_error->code == OperationCenterCoordinatorErrorCode::JournalStorageMismatch);
    CHECK_FALSE(result.HasDeferredHistoryProjection());
    CHECK((*foreign_operation_center)->Model().Snapshot() == before);
}

TEST_CASE(PREFIX "rejects a deferred projection when its recovery journal identity changes")
{
    TempTestDir temporary;
    const auto directory = CanonicalDirectory(temporary);
    const auto foreign_path = temporary.directory / "foreign";
    REQUIRE(std::filesystem::create_directory(foreign_path));
    const auto foreign_directory = std::filesystem::canonical(foreign_path).string();
    auto journal = OpenJournal(directory);
    auto initial = journal->Admit(Plan("initial-history"));
    REQUIRE(initial);
    REQUIRE(journal->FinalizeAdmission(std::move(*initial), OperationJournalState::Failed));

    auto operation_center = OperationCenterCoordinator::Create(*journal);
    REQUIRE(operation_center);
    const auto before = (*operation_center)->Model().Snapshot();
    auto staging = (*operation_center)->StageAdmission(*journal, Plan("staged-history"));
    REQUIRE(staging);

    auto recovered = journal->Admit(Plan("recovered-history"));
    REQUIRE(recovered);
    REQUIRE(journal->TransitionToRunning(std::move(*recovered)));

    auto services = PassiveServices();
    services.retry = [](std::string_view) {
        return CopyOperationRunReceiptCustodyResult{.status = CopyOperationRunReceiptCustodyStatus::ReconcileRequired};
    };
    services.reconcile = [](std::string_view, const OperationJournal &) {
        return CopyOperationRunReceiptReconciliationResult{
            .status = CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed};
    };
    auto recovery = CopyOperationRecoveryCoordinatorTesting::Make(std::move(journal), directory, std::move(services));
    const auto deferred = ServiceCopyRecoveryAndRefreshHistory(recovery, *operation_center, "recovered-history");
    REQUIRE(deferred.history_refresh == CopyOperationRecoveryHistoryRefreshStatus::Deferred);
    REQUIRE(deferred.HasDeferredHistoryProjection());

    auto foreign_recovery =
        CopyOperationRecoveryCoordinatorTesting::Make(OpenJournal(foreign_directory), foreign_directory, PassiveServices());
    const auto rejected = RetryDeferredHistoryProjection(foreign_recovery, *operation_center, deferred);

    CHECK(rejected.history_refresh == CopyOperationRecoveryHistoryRefreshStatus::ProjectionFailed);
    REQUIRE(rejected.history_refresh_error);
    CHECK(rejected.history_refresh_error->code == OperationCenterCoordinatorErrorCode::JournalStorageMismatch);
    CHECK_FALSE(rejected.HasDeferredHistoryProjection());
    CHECK((*operation_center)->Model().Snapshot() == before);
}
