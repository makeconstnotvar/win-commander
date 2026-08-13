// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "OperationCenterCoordinator.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nc::ops {
namespace {

std::optional<OperationRecordState> OperationCenterStateFromJournal(const OperationJournalState _state) noexcept
{
    switch( _state ) {
        case OperationJournalState::Interrupted:
            return OperationRecordState::Interrupted;
        case OperationJournalState::Completed:
            return OperationRecordState::Completed;
        case OperationJournalState::Failed:
            return OperationRecordState::Failed;
        case OperationJournalState::Cancelled:
            return OperationRecordState::Cancelled;
        case OperationJournalState::Admitted:
        case OperationJournalState::Running:
            return std::nullopt;
    }
    return std::nullopt;
}

bool OperationCenterStateIsTerminal(const OperationRecordState _state) noexcept
{
    switch( _state ) {
        case OperationRecordState::Interrupted:
        case OperationRecordState::Cancelled:
        case OperationRecordState::Failed:
        case OperationRecordState::Completed:
        case OperationRecordState::CompletedWithWarnings:
            return true;
        case OperationRecordState::Queued:
        case OperationRecordState::Running:
        case OperationRecordState::Paused:
        case OperationRecordState::Cancelling:
        case OperationRecordState::Finalizing:
            return false;
    }
    return true;
}

struct OperationCenterSubmissionBinding final {
    std::mutex lock;
    std::weak_ptr<OperationCenterCoordinator> coordinator;
    std::optional<OperationId> operation_id;
    std::optional<OperationCenterCoordinator::LiveResidencyDraft> residency_draft;

    [[nodiscard]] std::optional<std::pair<std::shared_ptr<OperationCenterCoordinator>, OperationId>> Resolve() noexcept
    {
        const auto guard = std::lock_guard{lock};
        const auto locked_coordinator = coordinator.lock();
        if( !locked_coordinator || !operation_id )
            return std::nullopt;
        return std::pair{std::move(locked_coordinator), *operation_id};
    }
};

} // namespace

struct OperationCenterCoordinator::LiveResidency final {
    std::recursive_mutex cancel_gate;
    std::shared_ptr<Pool> pool;
    std::shared_ptr<Operation> operation;
    bool cancel_in_flight{false};
    std::optional<OperationJournalState> deferred_terminal;
};

struct OperationCenterCoordinator::LiveResidencyBinding final {
    OperationId operation_id;
    std::shared_ptr<LiveResidency> residency;
};

OperationCenterCoordinator::LiveResidencyDraft::LiveResidencyDraft(
    std::shared_ptr<LiveResidency> _residency,
    std::weak_ptr<OperationCenterCoordinator> _coordinator) noexcept
    : m_Residency{std::move(_residency)}, m_Coordinator{std::move(_coordinator)}
{
}

OperationCenterCoordinator::LiveResidencyDraft::LiveResidencyDraft(LiveResidencyDraft &&_other) noexcept
    : m_Residency{std::move(_other.m_Residency)},
      m_Coordinator{std::move(_other.m_Coordinator)},
      m_Consumed{std::exchange(_other.m_Consumed, true)}
{
}

OperationCenterCoordinator::LiveResidencyDraft &
OperationCenterCoordinator::LiveResidencyDraft::operator=(LiveResidencyDraft &&_other) noexcept
{
    if( this != &_other ) {
        Release();
        m_Residency = std::move(_other.m_Residency);
        m_Coordinator = std::move(_other.m_Coordinator);
        m_Consumed = std::exchange(_other.m_Consumed, true);
    }
    return *this;
}

OperationCenterCoordinator::LiveResidencyDraft::~LiveResidencyDraft()
{
    Release();
}

void OperationCenterCoordinator::LiveResidencyDraft::Release() noexcept
{
    if( m_Consumed )
        return;
    m_Consumed = true;
    if( const auto coordinator = m_Coordinator.lock() )
        coordinator->ReleaseLiveResidencyDraft();
    m_Residency.reset();
}

OperationCenterCoordinator::AdmissionStaging::AdmissionStaging(OperationJournal::AdmissionReservation _journal_reservation,
                                                                OperationCenterModel::AdmissionDraft _model_draft,
                                                                LiveResidencyDraft _residency_draft,
                                                                OperationPlan _plan) noexcept
    : m_JournalReservation{std::move(_journal_reservation)},
      m_ModelDraft{std::move(_model_draft)},
      m_ResidencyDraft{std::move(_residency_draft)},
      m_Plan{std::move(_plan)}
{
}

std::expected<std::shared_ptr<OperationCenterCoordinator>, OperationCenterCoordinatorError>
OperationCenterCoordinator::Create(const OperationJournal &_journal)
{
    const auto entries = _journal.Snapshot();
    std::vector<OperationRecord> records;
    records.reserve(entries.size());
    std::unordered_set<std::string> operation_ids;
    operation_ids.reserve(entries.size());
    for( const auto &entry : entries ) {
        const auto record_state = OperationCenterStateFromJournal(entry.state);
        if( !record_state ) {
            return std::unexpected(OperationCenterCoordinatorError{
                .code = OperationCenterCoordinatorErrorCode::ActiveJournalEntry,
                .operation_id = entry.operation_id,
            });
        }
        if( !operation_ids.emplace(entry.operation_id.ToString()).second ) {
            return std::unexpected(OperationCenterCoordinatorError{
                .code = OperationCenterCoordinatorErrorCode::DuplicateOperationId,
                .operation_id = entry.operation_id,
            });
        }
        records.emplace_back(OperationRecord{
            .operation_id = entry.operation_id,
            .plan_id = entry.plan.Id(),
            .operation_type = entry.plan.Type(),
            .state = *record_state,
            .revision = 1,
            .created_at = entry.plan.CreatedAt(),
            .started_at = std::nullopt,
            .finished_at = entry.updated_at,
            .controls = {},
        });
    }

    auto coordinator = std::shared_ptr<OperationCenterCoordinator>{new OperationCenterCoordinator};
    if( const auto hydrated = coordinator->m_Model.Hydrate(std::move(records)); !hydrated ) {
        return std::unexpected(OperationCenterCoordinatorError{
            .code = OperationCenterCoordinatorErrorCode::ModelHydrationFailed,
            .model_error = hydrated.error(),
        });
    }
    coordinator->m_JournalStorageIdentity = _journal.StorageIdentityForCustody();
    return coordinator;
}

std::expected<void, OperationCenterCoordinatorError>
OperationCenterCoordinator::RefreshColdHistory(const OperationJournal &_journal)
{
    if( _journal.StorageIdentityForCustody() != m_JournalStorageIdentity )
        return std::unexpected(OperationCenterCoordinatorError{
            .code = OperationCenterCoordinatorErrorCode::JournalStorageMismatch,
        });

    std::vector<OperationRecord> records;
    try {
        const auto entries = _journal.Snapshot();
        records.reserve(entries.size());
        std::unordered_set<std::string> operation_ids;
        operation_ids.reserve(entries.size());
        for( const auto &entry : entries ) {
            const auto record_state = OperationCenterStateFromJournal(entry.state);
            if( !record_state ) {
                return std::unexpected(OperationCenterCoordinatorError{
                    .code = OperationCenterCoordinatorErrorCode::ActiveJournalEntry,
                    .operation_id = entry.operation_id,
                });
            }
            if( !operation_ids.emplace(entry.operation_id.ToString()).second ) {
                return std::unexpected(OperationCenterCoordinatorError{
                    .code = OperationCenterCoordinatorErrorCode::DuplicateOperationId,
                    .operation_id = entry.operation_id,
                });
            }
            records.emplace_back(OperationRecord{
                .operation_id = entry.operation_id,
                .plan_id = entry.plan.Id(),
                .operation_type = entry.plan.Type(),
                .state = *record_state,
                .revision = 1,
                .created_at = entry.plan.CreatedAt(),
                .started_at = std::nullopt,
                .finished_at = entry.updated_at,
                .controls = {},
            });
        }
    } catch( ... ) {
        return std::unexpected(OperationCenterCoordinatorError{
            .code = OperationCenterCoordinatorErrorCode::ColdHistoryRefreshFailed,
        });
    }

    const auto history_guard = std::lock_guard{m_ColdHistoryGate};
    {
        const auto residency_guard = std::lock_guard{m_ResidencyLock};
        if( m_StagedLiveResidencies != 0 || !m_LiveResidencies.empty() ) {
            return std::unexpected(OperationCenterCoordinatorError{
                .code = OperationCenterCoordinatorErrorCode::ColdHistoryBusy,
            });
        }
    }
    const auto reduction_guard = std::lock_guard{m_ReductionLock};
    if( const auto refreshed = m_Model.RefreshColdHistory(std::move(records)); !refreshed ) {
        return std::unexpected(OperationCenterCoordinatorError{
            .code = refreshed.error().code == OperationCenterModelErrorCode::ColdHistoryActive
                        ? OperationCenterCoordinatorErrorCode::ColdHistoryBusy
                        : OperationCenterCoordinatorErrorCode::ColdHistoryRefreshFailed,
            .model_error = refreshed.error(),
        });
    }
    return {};
}

std::expected<OperationCenterCoordinator::AdmissionStaging, OperationCenterCoordinatorError>
OperationCenterCoordinator::StageAdmission(OperationJournal &_journal,
                                            const OperationPlan &_plan)
{
    static_assert(std::is_nothrow_move_constructible_v<OperationPlan>);
    const auto history_guard = std::lock_guard{m_ColdHistoryGate};
    try {
        OperationPlan plan{_plan};
        auto model_draft = m_Model.StageAdmission(plan, plan.CreatedAt());
        if( !model_draft ) {
            return std::unexpected(OperationCenterCoordinatorError{
                .code = OperationCenterCoordinatorErrorCode::ModelStagingFailed,
                .model_error = model_draft.error(),
            });
        }
        std::optional<LiveResidencyDraft> residency_draft;
        try {
            const auto residency_guard = std::lock_guard{m_ResidencyLock};
            m_LiveResidencies.reserve(m_LiveResidencies.size() + m_StagedLiveResidencies + 1);
            auto staged_draft = LiveResidencyDraft{std::make_shared<LiveResidency>(), weak_from_this()};
            residency_draft.emplace(std::move(staged_draft));
            ++m_StagedLiveResidencies;
        } catch( ... ) {
            return std::unexpected(OperationCenterCoordinatorError{
                .code = OperationCenterCoordinatorErrorCode::ResidencyStagingFailed,
            });
        }
        auto journal_reservation = _journal.ReserveOperationId();
        if( !journal_reservation ) {
            return std::unexpected(OperationCenterCoordinatorError{
                .code = OperationCenterCoordinatorErrorCode::JournalReservationFailed,
                .journal_error = journal_reservation.error(),
            });
        }
        return AdmissionStaging{
            std::move(*journal_reservation), std::move(*model_draft), std::move(*residency_draft), std::move(plan)};
    } catch( ... ) {
        return std::unexpected(OperationCenterCoordinatorError{
            .code = OperationCenterCoordinatorErrorCode::ModelStagingFailed,
        });
    }
}

std::expected<OperationCenterCoordinator::CommittedAdmission, OperationCenterCoordinatorError>
OperationCenterCoordinator::CommitAdmission(OperationJournal &_journal, AdmissionStaging &&_staging)
{
    const auto history_guard = std::lock_guard{m_ColdHistoryGate};
    auto receipt = _journal.Admit(std::move(_staging.m_JournalReservation), _staging.m_Plan);
    if( !receipt ) {
        return std::unexpected(OperationCenterCoordinatorError{
            .code = OperationCenterCoordinatorErrorCode::JournalAdmissionFailed,
            .journal_error = receipt.error(),
        });
    }
    const auto operation_id = receipt->OperationId();
    const auto published = m_Model.Publish(std::move(_staging.m_ModelDraft), *receipt);
    if( published )
        return CommittedAdmission{operation_id, std::move(*receipt), std::move(_staging.m_ResidencyDraft)};

    auto finalization = _journal.FinalizeAdmission(std::move(*receipt), OperationJournalState::Failed);
    if( !finalization ) {
        return std::unexpected(OperationCenterCoordinatorError{
            .code = OperationCenterCoordinatorErrorCode::AdmissionFinalizationFailed,
            .operation_id = operation_id,
            .model_error = published.error(),
            .journal_error = finalization.error(),
        });
    }
    return std::unexpected(OperationCenterCoordinatorError{
        .code = OperationCenterCoordinatorErrorCode::ModelPublicationFailed,
        .operation_id = operation_id,
        .model_error = published.error(),
    });
}

void OperationCenterCoordinator::ReduceStarted(const OperationId _operation_id) noexcept
{
    const auto reduction_guard = std::lock_guard{m_ReductionLock};
    const auto record = m_Model.Find(_operation_id);
    if( !record || record->state != OperationRecordState::Queued )
        return;
    try {
        (void)m_Model.Transition(
            _operation_id, record->revision, OperationRecordState::Running, std::chrono::system_clock::now());
    } catch( ... ) {
    }
}

void OperationCenterCoordinator::ReduceDurableTerminal(const OperationId _operation_id,
                                                        const OperationJournalState _journal_state) noexcept
{
    const auto terminal_state = OperationCenterStateFromJournal(_journal_state);
    if( !terminal_state )
        return;

    const auto residency = FindLiveResidency(_operation_id);
    if( residency ) {
        const auto cancel_guard = std::lock_guard{residency->cancel_gate};
        if( residency->cancel_in_flight ) {
            residency->deferred_terminal = _journal_state;
            return;
        }
        // Keep the control gate through both model terminalization and residency retirement. A
        // concurrent Cancel must not acquire an executor after its durable terminal outcome exists.
        ApplyDurableTerminal(_operation_id, _journal_state);
        RetireLiveResidency(_operation_id, residency);
        return;
    }
    ApplyDurableTerminal(_operation_id, _journal_state);
}

void OperationCenterCoordinator::ApplyDurableTerminal(const OperationId _operation_id,
                                                       const OperationJournalState _journal_state) noexcept
{
    const auto terminal_state = OperationCenterStateFromJournal(_journal_state);
    if( !terminal_state )
        return;

    const auto reduction_guard = std::lock_guard{m_ReductionLock};
    const auto current = m_Model.Find(_operation_id);
    if( !current || OperationCenterStateIsTerminal(current->state) )
        return;
    auto record = *current;
    try {
        if( record.state != OperationRecordState::Finalizing ) {
            const auto finalizing = m_Model.Transition(
                _operation_id, record.revision, OperationRecordState::Finalizing, std::chrono::system_clock::now());
            if( !finalizing )
                return;
            record = *finalizing;
        }
        (void)m_Model.Transition(
            _operation_id, record.revision, *terminal_state, std::chrono::system_clock::now());
    } catch( ... ) {
    }
}

OperationCenterPauseResult OperationCenterCoordinator::SetPaused(const OperationId _operation_id,
                                                                 const uint64_t _expected_revision,
                                                                 const OperationCenterPauseIntent _intent) noexcept
{
    const bool pausing = _intent == OperationCenterPauseIntent::Pause;

    const auto validate = [&](const OperationRecord &_record) {
        if( _record.revision != _expected_revision )
            return OperationCenterPauseResult{
                .code = OperationCenterPauseResultCode::StaleRevision,
                .current_record = _record,
            };
        // The record's own projection is the authority for which direction is offered, so a resume
        // on a running operation and a pause on a paused one are both refused here rather than
        // being passed to the executor to sort out.
        const bool available = pausing ? _record.controls.can_pause : _record.controls.can_resume;
        if( !available )
            return OperationCenterPauseResult{
                .code = OperationCenterPauseResultCode::ControlUnavailable,
                .current_record = _record,
            };
        return OperationCenterPauseResult{
            .code = OperationCenterPauseResultCode::Accepted,
            .current_record = _record,
        };
    };

    try {
        const auto initial = m_Model.Find(_operation_id);
        if( !initial )
            return {.code = OperationCenterPauseResultCode::OperationNotFound};
        if( const auto validation = validate(*initial);
            validation.code != OperationCenterPauseResultCode::Accepted )
            return validation;

        const auto residency = FindLiveResidency(_operation_id);
        if( !residency || !residency->operation )
            return {.code = OperationCenterPauseResultCode::ResidencyUnavailable, .current_record = initial};

        // Revalidate under the same gate Cancel uses, so a cancellation landing concurrently cannot
        // be overtaken by a pause that was authorised against the pre-cancel record.
        const auto cancel_guard = std::lock_guard{residency->cancel_gate};
        const auto current = m_Model.Find(_operation_id);
        if( !current )
            return {.code = OperationCenterPauseResultCode::OperationNotFound};
        if( const auto validation = validate(*current);
            validation.code != OperationCenterPauseResultCode::Accepted )
            return validation;

        const auto pool_operations =
            residency->pool ? residency->pool->Operations() : std::vector<std::shared_ptr<Operation>>{};
        const bool is_resident = std::ranges::any_of(pool_operations, [&](const auto &_operation) {
            return _operation == residency->operation;
        });
        if( !is_resident )
            return {.code = OperationCenterPauseResultCode::ResidencyUnavailable, .current_record = current};

        if( pausing )
            residency->operation->Pause();
        else
            residency->operation->Resume();

        ReducePaused(_operation_id, pausing);
        return {.code = OperationCenterPauseResultCode::Accepted, .current_record = m_Model.Find(_operation_id)};
    } catch( ... ) {
        return {.code = OperationCenterPauseResultCode::ResidencyUnavailable};
    }
}

void OperationCenterCoordinator::ReducePaused(const OperationId _operation_id, const bool _paused) noexcept
{
    const auto reduction_guard = std::lock_guard{m_ReductionLock};
    const auto current = m_Model.Find(_operation_id);
    if( !current )
        return;
    const bool available = _paused ? current->controls.can_pause : current->controls.can_resume;
    if( !available )
        return;
    try {
        (void)m_Model.Transition(_operation_id,
                                 current->revision,
                                 _paused ? OperationRecordState::Paused : OperationRecordState::Running,
                                 std::chrono::system_clock::now());
    } catch( ... ) {
    }
}

void OperationCenterCoordinator::ReduceCancelling(const OperationId _operation_id) noexcept
{
    const auto reduction_guard = std::lock_guard{m_ReductionLock};
    const auto current = m_Model.Find(_operation_id);
    if( !current || !current->controls.can_cancel )
        return;
    try {
        (void)m_Model.Transition(
            _operation_id, current->revision, OperationRecordState::Cancelling, std::chrono::system_clock::now());
    } catch( ... ) {
    }
}

void OperationCenterCoordinator::ReleaseLiveResidencyDraft() noexcept
{
    const auto residency_guard = std::lock_guard{m_ResidencyLock};
    if( m_StagedLiveResidencies > 0 )
        --m_StagedLiveResidencies;
}

void OperationCenterCoordinator::RegisterLiveResidency(LiveResidencyDraft &&_draft,
                                                        const OperationId _operation_id,
                                                        const std::shared_ptr<Pool> &_pool,
                                                        const std::shared_ptr<Operation> &_operation)
{
    if( !_draft.m_Residency || !_pool || !_operation )
        throw std::logic_error{"invalid live residency handoff"};
    const auto draft_coordinator = _draft.m_Coordinator.lock();
    if( !draft_coordinator || draft_coordinator.get() != this )
        throw std::logic_error{"foreign live residency handoff"};

    const auto residency_guard = std::lock_guard{m_ResidencyLock};
    if( std::ranges::any_of(m_LiveResidencies, [&](const auto &_binding) {
            return _binding.operation_id == _operation_id;
        }) )
        throw std::logic_error{"duplicate live operation residency"};
    if( m_LiveResidencies.size() == m_LiveResidencies.capacity() )
        throw std::logic_error{"unprepared live operation residency"};

    _draft.m_Residency->pool = _pool;
    _draft.m_Residency->operation = _operation;
    m_LiveResidencies.emplace_back(LiveResidencyBinding{_operation_id, std::move(_draft.m_Residency)});
    _draft.m_Consumed = true;
    if( m_StagedLiveResidencies > 0 )
        --m_StagedLiveResidencies;
}

std::shared_ptr<OperationCenterCoordinator::LiveResidency>
OperationCenterCoordinator::FindLiveResidency(const OperationId _operation_id) const noexcept
{
    try {
        const auto residency_guard = std::lock_guard{m_ResidencyLock};
        const auto found = std::ranges::find_if(m_LiveResidencies, [&](const auto &_binding) {
            return _binding.operation_id == _operation_id;
        });
        return found == m_LiveResidencies.end() ? nullptr : found->residency;
    } catch( ... ) {
        return nullptr;
    }
}

void OperationCenterCoordinator::RetireLiveResidency(const OperationId _operation_id,
                                                      const std::shared_ptr<LiveResidency> &_residency) noexcept
{
    try {
        const auto residency_guard = std::lock_guard{m_ResidencyLock};
        std::erase_if(m_LiveResidencies, [&](const auto &_binding) {
            return _binding.operation_id == _operation_id && (!_residency || _binding.residency == _residency);
        });
    } catch( ... ) {
    }
}

void OperationCenterCoordinator::ReduceDurableTerminalFromJournal(const OperationId _operation_id,
                                                                   const OperationJournal &_journal) noexcept
{
    try {
        const auto entries = _journal.Snapshot();
        const auto entry = std::ranges::find_if(entries, [&](const OperationJournalEntry &_entry) {
            return _entry.operation_id == _operation_id;
        });
        if( entry != entries.end() )
            ReduceDurableTerminal(_operation_id, entry->state);
    } catch( ... ) {
    }
}

OperationCenterCancelResult OperationCenterCoordinator::Cancel(const OperationId _operation_id,
                                                                const uint64_t _expected_revision) noexcept
{
    const auto result_for_current = [&](const OperationRecord &_record) {
        if( _record.revision != _expected_revision )
            return OperationCenterCancelResult{
                .code = OperationCenterCancelResultCode::StaleRevision,
                .current_record = _record,
            };
        if( !_record.controls.can_cancel )
            return OperationCenterCancelResult{
                .code = OperationCenterCancelResultCode::CancelUnavailable,
                .current_record = _record,
            };
        return OperationCenterCancelResult{
            .code = OperationCenterCancelResultCode::Accepted,
            .current_record = _record,
        };
    };

    try {
        const auto initial = m_Model.Find(_operation_id);
        if( !initial )
            return {.code = OperationCenterCancelResultCode::OperationNotFound};
        if( const auto validation = result_for_current(*initial);
            validation.code != OperationCenterCancelResultCode::Accepted )
            return validation;

        const auto residency = FindLiveResidency(_operation_id);
        if( !residency )
            return {.code = OperationCenterCancelResultCode::ResidencyUnavailable, .current_record = initial};

        const auto cancel_guard = std::lock_guard{residency->cancel_gate};
        const auto current = m_Model.Find(_operation_id);
        if( !current )
            return {.code = OperationCenterCancelResultCode::OperationNotFound};
        if( const auto validation = result_for_current(*current);
            validation.code != OperationCenterCancelResultCode::Accepted )
            return validation;
        if( residency->cancel_in_flight ) {
            return {
                .code = OperationCenterCancelResultCode::CancelInProgress,
                .current_record = current,
            };
        }

        const auto pool_operations = residency->pool ? residency->pool->Operations() : std::vector<std::shared_ptr<Operation>>{};
        const bool is_resident = std::ranges::any_of(pool_operations, [&](const auto &_operation) {
            return _operation == residency->operation;
        });
        if( !is_resident )
            return {.code = OperationCenterCancelResultCode::ResidencyUnavailable, .current_record = current};

        residency->cancel_in_flight = true;
        bool accepted = false;
        try {
            accepted = residency->operation->Stop();
        } catch( ... ) {
            residency->cancel_in_flight = false;
            throw;
        }
        if( accepted )
            ReduceCancelling(_operation_id);
        residency->cancel_in_flight = false;
        const auto deferred_terminal = std::exchange(residency->deferred_terminal, std::nullopt);
        if( deferred_terminal ) {
            ApplyDurableTerminal(_operation_id, *deferred_terminal);
            RetireLiveResidency(_operation_id, residency);
        }
        return {
            .code = accepted ? OperationCenterCancelResultCode::Accepted : OperationCenterCancelResultCode::StopRejected,
            .current_record = m_Model.Find(_operation_id),
        };
    } catch( ... ) {
        return {.code = OperationCenterCancelResultCode::ResidencyUnavailable};
    }
}

std::expected<std::shared_ptr<Operation>, OperationCenterSubmissionError>
OperationCenterCoordinator::SubmitReviewedCopy(OperationJournal &_journal,
                                                CopyOperationOrchestrator &_orchestrator,
                                                ReviewedVFSOperationPreflight _reviewed,
                                                std::function<bool()> _cancel_checker,
                                                CopyOperationSubmissionHooks _hooks)
{
    const auto orchestrator_failure = [](const CopyOperationOrchestratorError _error) {
        return std::unexpected(OperationCenterSubmissionError{
            .code = OperationCenterSubmissionErrorCode::OrchestratorRejected,
            .orchestrator_error = _error,
        });
    };
    if( !_orchestrator.m_Journal || _orchestrator.m_Journal.get() != &_journal )
        return orchestrator_failure(
            CopyOperationOrchestratorError{.code = CopyOperationOrchestratorErrorCode::InvalidJournalAdmissionReceipt});
    if( !_orchestrator.m_Pool )
        return orchestrator_failure(CopyOperationOrchestratorError{.code = CopyOperationOrchestratorErrorCode::MissingPool});
    if( _orchestrator.m_UseInjectedExecutionFactory && !_orchestrator.m_ExecutionFactory ) {
        return orchestrator_failure(
            CopyOperationOrchestratorError{.code = CopyOperationOrchestratorErrorCode::MissingExecutionFactory});
    }
    if( !_orchestrator.m_RunReceiptCustodian ) {
        return orchestrator_failure(
            CopyOperationOrchestratorError{.code = CopyOperationOrchestratorErrorCode::MissingRunReceiptCustodian});
    }

    const auto &accepted = _reviewed.AcceptedPlan();
    const auto &plan = accepted.Plan();
    // A batch is one operation here as everywhere else, so the count is no longer the question, and a
    // Move is admitted on the same terms as a Copy - both publish a destination the journal already
    // knows how to record. What remains is the journal's rule, asked before this path admits
    // anything: a report that does not cover its plan's sources one item each cannot be recorded as
    // completed, so it must not run.
    if( (plan.Type() != OperationPlanType::Copy && plan.Type() != OperationPlanType::Move) ||
        accepted.Report().items.size() != plan.Sources().size() ) {
        return orchestrator_failure(
            CopyOperationOrchestratorError{.code = CopyOperationOrchestratorErrorCode::UnsupportedReviewedPlan});
    }

    std::shared_ptr<OperationCenterSubmissionBinding> binding;
    CopyOperationOrchestrator::PreEnqueueHandoff pre_enqueue_handoff;
    try {
        binding = std::make_shared<OperationCenterSubmissionBinding>();
        binding->coordinator = weak_from_this();
        pre_enqueue_handoff = [binding](const std::shared_ptr<Pool> &_pool,
                                        const std::shared_ptr<Operation> &_operation) {
                const auto resolved = binding->Resolve();
                if( !resolved )
                    throw std::logic_error{"missing coordinator admission binding"};
                std::optional<LiveResidencyDraft> residency_draft;
                {
                    const auto binding_guard = std::lock_guard{binding->lock};
                    if( !binding->residency_draft )
                        throw std::logic_error{"missing coordinator live residency"};
                    residency_draft.emplace(std::move(*binding->residency_draft));
                    binding->residency_draft.reset();
                }
                if( !residency_draft->m_Residency )
                    throw std::logic_error{"missing coordinator live residency"};
                auto cancel_gate = std::unique_lock{residency_draft->m_Residency->cancel_gate};
                resolved->first->RegisterLiveResidency(
                    std::move(*residency_draft), resolved->second, _pool, _operation);
                return CopyOperationPreEnqueueLease{std::move(cancel_gate)};
            };
        auto durable_terminal_observer = std::move(_hooks.durable_terminal_observer);
        _hooks.durable_terminal_observer =
            [binding, durable_terminal_observer = std::move(durable_terminal_observer)](
                const CopyOperationDurableTerminalOutcome &_outcome) mutable {
                if( const auto resolved = binding->Resolve() )
                    resolved->first->ReduceDurableTerminal(resolved->second, _outcome.state);
                if( durable_terminal_observer ) {
                    try {
                        durable_terminal_observer(_outcome);
                    } catch( ... ) {
                    }
                }
            };
        _hooks.cold_operation_observations.reserve(_hooks.cold_operation_observations.size() + 1);
        _hooks.cold_operation_observations.emplace_back(CopyOperationColdObservation{
            .notification_mask = Operation::NotifyAboutStart,
            .callback = [binding] {
                if( const auto resolved = binding->Resolve() )
                    resolved->first->ReduceStarted(resolved->second);
            },
        });
    } catch( ... ) {
        return std::unexpected(OperationCenterSubmissionError{
            .code = OperationCenterSubmissionErrorCode::HookPreparationFailed,
        });
    }

    std::optional<CommittedAdmission> committed;
    {
        const auto admission_guard = std::lock_guard{m_AdmissionLock};
        auto staging = StageAdmission(_journal, plan);
        if( !staging ) {
            return std::unexpected(OperationCenterSubmissionError{
                .code = OperationCenterSubmissionErrorCode::AdmissionStagingFailed,
                .coordinator_error = staging.error(),
            });
        }
        auto admission = CommitAdmission(_journal, std::move(*staging));
        if( !admission ) {
            return std::unexpected(OperationCenterSubmissionError{
                .code = OperationCenterSubmissionErrorCode::AdmissionCommitFailed,
                .coordinator_error = admission.error(),
            });
        }
        committed.emplace(std::move(*admission));
        const auto binding_guard = std::lock_guard{binding->lock};
        binding->operation_id = committed->operation_id;
        binding->residency_draft.emplace(std::move(committed->m_ResidencyDraft));
    }

    auto submitted = _orchestrator.SubmitAdmitted(std::move(_reviewed),
                                                  std::move(committed->journal_receipt),
                                                  std::move(_cancel_checker),
                                                  std::move(_hooks),
                                                  std::move(pre_enqueue_handoff));
    if( !submitted ) {
        ReduceDurableTerminalFromJournal(committed->operation_id, _journal);
        RetireLiveResidency(committed->operation_id, {});
        return orchestrator_failure(submitted.error());
    }
    return std::move(*submitted);
}

} // namespace nc::ops
