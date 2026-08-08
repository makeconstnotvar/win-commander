// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include <Operations/OperationCenterCoordinator.h>

#include "../source/OperationJournalTesting.h"

#include <catch2/catch_all.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace nc::ops;
using namespace std::chrono_literals;

namespace {

struct OperationCenterCoordinatorUTDirectory final {
    OperationCenterCoordinatorUTDirectory()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "operation-center-coordinator-ut-XXXXXX").string();
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        path = std::filesystem::canonical(pattern).string();
    }
    ~OperationCenterCoordinatorUTDirectory() { std::filesystem::remove_all(path); }
    std::string path;
};

OperationPlan OperationCenterCoordinatorUTPlan(std::string _id)
{
    auto plan = OperationPlan::Create({.plan_id = std::move(_id),
                                       .type = OperationPlanType::Copy,
                                       .sources = {OperationPlanSourceInput{"native", "/source"}},
                                       .destination = OperationPlanDestinationInput{
                                           "native", "/destination", OperationPlanDestinationKind::Directory},
                                       .conflict_policy = OperationPlanConflictPolicy{
                                           OperationPlanConflictDecision::Ask,
                                           OperationPlanConflictScope::ThisItem},
                                       .created_at = OperationPlan::TimePoint{1'700'000'000s}});
    REQUIRE(plan);
    return std::move(*plan);
}

OperationJournalItemResult OperationCenterCoordinatorUTSuccess()
{
    return {.item_index = 0,
            .status = OperationJournalItemStatus::Succeeded,
            .error = OperationJournalItemError::None,
            .system_error = 0,
            .prior_error = OperationJournalItemError::None,
            .prior_system_error = 0,
            .bytes = 42,
            .destination_publication = OperationJournalPublicationState::Published,
            .filesystem_sync_status = OperationJournalFilesystemSyncStatus::Confirmed,
            .filesystem_sync_system_error = 0,
            .recovery_action = OperationJournalRecoveryAction::None};
}

void FinalizeAdmission(OperationJournal &_journal, OperationPlan _plan, const OperationJournalState _terminal_state)
{
    auto admission = _journal.Admit(_plan);
    REQUIRE(admission);
    if( _terminal_state == OperationJournalState::Failed || _terminal_state == OperationJournalState::Cancelled ) {
        REQUIRE(_journal.FinalizeAdmission(std::move(*admission), _terminal_state));
        return;
    }

    REQUIRE(_terminal_state == OperationJournalState::Completed);
    auto running = _journal.TransitionToRunning(std::move(*admission));
    REQUIRE(running);
    REQUIRE(_journal.Finalize(std::move(*running), OperationCenterCoordinatorUTSuccess(), _terminal_state));
}

} // namespace

TEST_CASE("OperationCenterCoordinator: hydrates terminal and interrupted journal history without journal ownership",
          "[operation-center-coordinator]")
{
    OperationCenterCoordinatorUTDirectory directory;
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        auto completed = journal->Admit(OperationCenterCoordinatorUTPlan("completed"));
        REQUIRE(completed);
        auto completed_run = journal->TransitionToRunning(std::move(*completed));
        REQUIRE(completed_run);
        REQUIRE(journal->Finalize(
            std::move(*completed_run), OperationCenterCoordinatorUTSuccess(), OperationJournalState::Completed));

        auto running = journal->Admit(OperationCenterCoordinatorUTPlan("running"));
        REQUIRE(running);
        REQUIRE(journal->TransitionToRunning(std::move(*running)));
    }

    auto journal = OperationJournal::Open(directory.path);
    REQUIRE(journal);
    auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);
    const auto snapshot = (*coordinator)->Model().Snapshot();
    REQUIRE(snapshot.size() == 2);
    CHECK(snapshot[0].operation_id.ToString() == "op-1");
    CHECK(snapshot[0].plan_id.Value() == "completed");
    CHECK(snapshot[0].state == OperationRecordState::Completed);
    CHECK(snapshot[0].revision == 1);
    CHECK(snapshot[0].finished_at);
    CHECK_FALSE(snapshot[0].started_at);
    CHECK(snapshot[1].operation_id.ToString() == "op-2");
    CHECK(snapshot[1].plan_id.Value() == "running");
    CHECK(snapshot[1].state == OperationRecordState::Interrupted);
    CHECK(snapshot[1].controls.can_retry);

}

TEST_CASE("OperationCenterCoordinator: rejects a journal that still has active execution", "[operation-center-coordinator]")
{
    OperationCenterCoordinatorUTDirectory directory;
    auto journal = OperationJournal::Open(directory.path);
    REQUIRE(journal);
    auto admission = journal->Admit(OperationCenterCoordinatorUTPlan("admitted"));
    REQUIRE(admission);

    const auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE_FALSE(coordinator);
    CHECK(coordinator.error().code == OperationCenterCoordinatorErrorCode::ActiveJournalEntry);
    REQUIRE(coordinator.error().operation_id);
    CHECK(coordinator.error().operation_id->ToString() == "op-1");
}

TEST_CASE("OperationCenterCoordinator: stages a model draft before exact journal admission", "[operation-center-coordinator]")
{
    OperationCenterCoordinatorUTDirectory directory;
    auto journal = OperationJournal::Open(directory.path);
    REQUIRE(journal);
    auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);

    const auto plan = OperationCenterCoordinatorUTPlan("staged");
    auto staging = (*coordinator)->StageAdmission(*journal, plan);
    REQUIRE(staging);
    CHECK((*coordinator)->Model().Snapshot().empty());
    CHECK(journal->Snapshot().empty());

    auto committed = (*coordinator)->CommitAdmission(*journal, std::move(*staging));
    REQUIRE(committed);
    CHECK(committed->operation_id == committed->journal_receipt.OperationId());
    CHECK(committed->operation_id.ToString() == "op-1");
    const auto snapshot = (*coordinator)->Model().Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot.front().operation_id == committed->operation_id);
    CHECK(snapshot.front().plan_id == plan.Id());
    CHECK(snapshot.front().state == OperationRecordState::Queued);
    CHECK(snapshot.front().revision == 1);
    const auto journal_snapshot = journal->Snapshot();
    REQUIRE(journal_snapshot.size() == 1);
    CHECK(journal_snapshot.front().operation_id == committed->operation_id);
    CHECK(journal_snapshot.front().state == OperationJournalState::Admitted);
}

TEST_CASE("OperationCenterCoordinator: reserves publication capacity for every concurrent staging", "[operation-center-coordinator]")
{
    OperationCenterCoordinatorUTDirectory directory;
    auto journal = OperationJournal::Open(directory.path);
    REQUIRE(journal);
    auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);

    const auto first_plan = OperationCenterCoordinatorUTPlan("first-staged");
    const auto second_plan = OperationCenterCoordinatorUTPlan("second-staged");
    auto first_staging = (*coordinator)->StageAdmission(*journal, first_plan);
    REQUIRE(first_staging);
    auto second_staging = (*coordinator)->StageAdmission(*journal, second_plan);
    REQUIRE(second_staging);
    CHECK((*coordinator)->Model().Snapshot().empty());

    auto first = (*coordinator)->CommitAdmission(*journal, std::move(*first_staging));
    REQUIRE(first);
    auto second = (*coordinator)->CommitAdmission(*journal, std::move(*second_staging));
    REQUIRE(second);

    const auto model_snapshot = (*coordinator)->Model().Snapshot();
    REQUIRE(model_snapshot.size() == 2);
    CHECK(model_snapshot[0].operation_id == first->operation_id);
    CHECK(model_snapshot[0].state == OperationRecordState::Queued);
    CHECK(model_snapshot[1].operation_id == second->operation_id);
    CHECK(model_snapshot[1].state == OperationRecordState::Queued);
    const auto journal_snapshot = journal->Snapshot();
    REQUIRE(journal_snapshot.size() == 2);
    CHECK(journal_snapshot[0].operation_id == first->operation_id);
    CHECK(journal_snapshot[1].operation_id == second->operation_id);
}

TEST_CASE("OperationCenterCoordinator: journal admission failure releases an invisible model draft",
          "[operation-center-coordinator]")
{
    OperationCenterCoordinatorUTDirectory directory;
    auto journal = OperationJournal::Open(directory.path);
    REQUIRE(journal);
    auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);

    const auto plan = OperationCenterCoordinatorUTPlan("duplicate");
    auto staging = (*coordinator)->StageAdmission(*journal, plan);
    REQUIRE(staging);
    REQUIRE(journal->Admit(plan));

    const auto committed = (*coordinator)->CommitAdmission(*journal, std::move(*staging));
    REQUIRE_FALSE(committed);
    CHECK(committed.error().code == OperationCenterCoordinatorErrorCode::JournalAdmissionFailed);
    REQUIRE(committed.error().journal_error);
    CHECK(committed.error().journal_error->code == OperationJournalErrorCode::PlanAlreadyAdmitted);
    CHECK((*coordinator)->Model().Snapshot().empty());
}

TEST_CASE("OperationCenterCoordinator: foreign model draft is finalized without publication", "[operation-center-coordinator]")
{
    OperationCenterCoordinatorUTDirectory directory;
    auto journal = OperationJournal::Open(directory.path);
    REQUIRE(journal);
    auto staging_coordinator = OperationCenterCoordinator::Create(*journal);
    auto committing_coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(staging_coordinator);
    REQUIRE(committing_coordinator);

    auto staging = (*staging_coordinator)->StageAdmission(*journal, OperationCenterCoordinatorUTPlan("foreign"));
    REQUIRE(staging);
    const auto committed = (*committing_coordinator)->CommitAdmission(*journal, std::move(*staging));
    REQUIRE_FALSE(committed);
    CHECK(committed.error().code == OperationCenterCoordinatorErrorCode::ModelPublicationFailed);
    REQUIRE(committed.error().model_error);
    CHECK(committed.error().model_error->code == OperationCenterModelErrorCode::UnstagedAdmission);
    CHECK((*staging_coordinator)->Model().Snapshot().empty());
    CHECK((*committing_coordinator)->Model().Snapshot().empty());
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot.front().operation_id.ToString() == "op-1");
    CHECK(snapshot.front().state == OperationJournalState::Failed);
}

TEST_CASE("OperationCenterCoordinator: refreshes only absent terminal history from its exact cold journal",
          "[operation-center-coordinator]")
{
    OperationCenterCoordinatorUTDirectory directory;
    std::shared_ptr<OperationCenterCoordinator> coordinator;
    std::vector<OperationRecord> initial;
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        FinalizeAdmission(*journal, OperationCenterCoordinatorUTPlan("first-history"), OperationJournalState::Completed);

        auto created = OperationCenterCoordinator::Create(*journal);
        REQUIRE(created);
        coordinator = std::move(*created);
        initial = coordinator->Model().Snapshot();
        REQUIRE(initial.size() == 1);
        CHECK(initial.front().operation_id.ToString() == "op-1");
        CHECK(initial.front().state == OperationRecordState::Completed);

        auto second_admission = journal->Admit(OperationCenterCoordinatorUTPlan("second-history"));
        REQUIRE(second_admission);
        REQUIRE(journal->TransitionToRunning(std::move(*second_admission)));
    }

    auto journal = OperationJournal::Open(directory.path);
    REQUIRE(journal);
    REQUIRE(coordinator->RefreshColdHistory(*journal));
    const auto refreshed = coordinator->Model().Snapshot();
    REQUIRE(refreshed.size() == 2);
    CHECK(refreshed[0] == initial[0]);
    CHECK(refreshed[1].operation_id.ToString() == "op-2");
    CHECK(refreshed[1].plan_id.Value() == "second-history");
    CHECK(refreshed[1].state == OperationRecordState::Interrupted);
    CHECK(refreshed[1].revision == 1);
    CHECK(refreshed[1].finished_at);
    CHECK_FALSE(refreshed[1].started_at);
    CHECK(refreshed[1].controls.can_retry);

    REQUIRE(coordinator->RefreshColdHistory(*journal));
    CHECK(coordinator->Model().Snapshot() == refreshed);
}

TEST_CASE("OperationCenterCoordinator: rejects foreign or active journal history without changing its cold model",
          "[operation-center-coordinator]")
{
    OperationCenterCoordinatorUTDirectory first_directory;
    auto first_journal = OperationJournal::Open(first_directory.path);
    REQUIRE(first_journal);
    FinalizeAdmission(*first_journal, OperationCenterCoordinatorUTPlan("first-history"), OperationJournalState::Failed);
    auto coordinator = OperationCenterCoordinator::Create(*first_journal);
    REQUIRE(coordinator);
    const auto before = (*coordinator)->Model().Snapshot();

    OperationCenterCoordinatorUTDirectory foreign_directory;
    auto foreign_journal = OperationJournal::Open(foreign_directory.path);
    REQUIRE(foreign_journal);
    FinalizeAdmission(*foreign_journal, OperationCenterCoordinatorUTPlan("foreign-history"), OperationJournalState::Completed);
    const auto foreign = (*coordinator)->RefreshColdHistory(*foreign_journal);
    REQUIRE_FALSE(foreign);
    CHECK(foreign.error().code == OperationCenterCoordinatorErrorCode::JournalStorageMismatch);
    CHECK((*coordinator)->Model().Snapshot() == before);

    REQUIRE(first_journal->Admit(OperationCenterCoordinatorUTPlan("active-history")));
    const auto active = (*coordinator)->RefreshColdHistory(*first_journal);
    REQUIRE_FALSE(active);
    CHECK(active.error().code == OperationCenterCoordinatorErrorCode::ActiveJournalEntry);
    CHECK((*coordinator)->Model().Snapshot() == before);
}

TEST_CASE("OperationCenterCoordinator: refresh rejects a staged admission without publishing partial history",
          "[operation-center-coordinator]")
{
    OperationCenterCoordinatorUTDirectory directory;
    auto journal = OperationJournal::Open(directory.path);
    REQUIRE(journal);
    FinalizeAdmission(*journal, OperationCenterCoordinatorUTPlan("history"), OperationJournalState::Completed);
    auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);
    const auto before = (*coordinator)->Model().Snapshot();

    auto staging = (*coordinator)->StageAdmission(*journal, OperationCenterCoordinatorUTPlan("staged"));
    REQUIRE(staging);
    const auto refreshed = (*coordinator)->RefreshColdHistory(*journal);
    REQUIRE_FALSE(refreshed);
    CHECK(refreshed.error().code == OperationCenterCoordinatorErrorCode::ColdHistoryBusy);
    CHECK((*coordinator)->Model().Snapshot() == before);
}

TEST_CASE("OperationCenterCoordinator: pause and resume revalidate before reaching any executor",
          "[operation-center-coordinator]")
{
    OperationCenterCoordinatorUTDirectory directory;
    auto journal = OperationJournal::Open(directory.path);
    REQUIRE(journal);
    auto coordinator = OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);

    auto staging = (*coordinator)->StageAdmission(*journal, OperationCenterCoordinatorUTPlan("pausable"));
    REQUIRE(staging);
    auto committed = (*coordinator)->CommitAdmission(*journal, std::move(*staging));
    REQUIRE(committed);
    const auto id = committed->operation_id;
    const auto queued = (*coordinator)->Model().Find(id);
    REQUIRE(queued);
    REQUIRE(queued->state == OperationRecordState::Queued);

    SECTION("an unknown operation is refused without inventing a record")
    {
        // A second coordinator allocates its own sequence; its op-2 is unknown to the first, which
        // only holds op-1.
        OperationCenterCoordinatorUTDirectory other_directory;
        auto other_journal = OperationJournal::Open(other_directory.path);
        REQUIRE(other_journal);
        auto other = OperationCenterCoordinator::Create(*other_journal);
        REQUIRE(other);
        OperationId foreign = id;
        for( int index = 0; index < 2; ++index ) {
            auto other_staging = (*other)->StageAdmission(
                *other_journal, OperationCenterCoordinatorUTPlan("foreign-" + std::to_string(index)));
            REQUIRE(other_staging);
            auto other_committed = (*other)->CommitAdmission(*other_journal, std::move(*other_staging));
            REQUIRE(other_committed);
            foreign = other_committed->operation_id;
        }
        REQUIRE_FALSE(foreign == id);

        const auto result = (*coordinator)->SetPaused(foreign, 1, OperationCenterPauseIntent::Pause);
        CHECK(result.code == OperationCenterPauseResultCode::OperationNotFound);
        CHECK_FALSE(result.current_record);
    }
    SECTION("a stale revision is refused and reports what the record actually is now")
    {
        const auto result = (*coordinator)->SetPaused(id, queued->revision + 1, OperationCenterPauseIntent::Pause);
        CHECK(result.code == OperationCenterPauseResultCode::StaleRevision);
        REQUIRE(result.current_record);
        CHECK(result.current_record->revision == queued->revision);
        CHECK(result.current_record->state == OperationRecordState::Queued);
    }
    SECTION("the record's own control projection decides which direction is offered")
    {
        // A Queued operation has not started, so neither direction applies. The port refuses both
        // here rather than passing them to an executor to sort out - and, importantly, refuses
        // before it ever looks for a live residency.
        const auto paused = (*coordinator)->SetPaused(id, queued->revision, OperationCenterPauseIntent::Pause);
        CHECK(paused.code == OperationCenterPauseResultCode::ControlUnavailable);
        const auto resumed = (*coordinator)->SetPaused(id, queued->revision, OperationCenterPauseIntent::Resume);
        CHECK(resumed.code == OperationCenterPauseResultCode::ControlUnavailable);

        // A refused request leaves the record exactly as it was, so the panel cannot start claiming
        // a state the executor never entered.
        const auto current = (*coordinator)->Model().Find(id);
        REQUIRE(current);
        CHECK(current->state == OperationRecordState::Queued);
        CHECK(current->revision == queued->revision);
    }
}
