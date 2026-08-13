// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CopyOperationOrchestrator.h"
#include "CopyOperationOrchestratorTesting.h"

#include <algorithm>
#include <cerrno>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace nc::ops {

struct CopyOperationRunReceiptCustodian::Slot final {
    enum class Phase : uint8_t {
        Reserved,
        Armed,
        EnqueueInProgress,
        PoolOwned,
        RetryRequired,
        ReconcileRequired,
        ContractViolation,
        Reconciled,
        ReleaseInProgress,
        Finalized,
        Released
    };

    Slot(OperationPlan _plan,
         std::string _plan_id,
         std::shared_ptr<OperationJournal> _journal,
         CopyOperationExecutionProduct::TerminalEvidenceAccessor _accessor,
         std::function<void(const CopyOperationDurableTerminalOutcome &)> _terminal_observer,
         std::weak_ptr<Impl> _owner,
         std::pair<uint64_t, uint64_t> _storage_identity)
        : plan{std::move(_plan)}, plan_id{_plan_id}, terminal_outcome_plan_id{std::move(_plan_id)},
          journal{std::move(_journal)}, accessor{std::move(_accessor)},
          terminal_observer{std::move(_terminal_observer)}, owner{std::move(_owner)},
          storage_identity{_storage_identity}
    {
        pre_running_terminal_evidence_storage.reserve(plan.Sources().size());
    }

    const OperationPlan plan;
    const std::string plan_id;
    std::string terminal_outcome_plan_id;
    std::shared_ptr<OperationJournal> journal;
    std::optional<OperationJournalRunReceipt> receipt;
    CopyOperationExecutionProduct::TerminalEvidenceAccessor accessor;
    bool terminal_evidence_acquired{false};
    std::optional<CopyOperationTerminalResultError> terminal_result_error;
    std::optional<CopyOperationTerminalEvidence> terminal_evidence;
    /**
     * Allocated with the bounded custody Slot before Running, one element per source the plan names,
     * for terminalization that happens before the operation ever runs.
     */
    std::vector<OperationJournalItemResult> pre_running_terminal_evidence_storage;
    std::function<void(const CopyOperationDurableTerminalOutcome &)> terminal_observer;
    bool terminal_observer_delivered{false};
    std::optional<OperationJournalError> last_journal_error;
    std::weak_ptr<Impl> owner;
    const std::pair<uint64_t, uint64_t> storage_identity;
    std::weak_ptr<Pool> pool;
    std::shared_ptr<Operation> operation;
    bool pool_owned{false};
    bool pool_release_latched{false};
    std::optional<CopyOperationRunReceiptReconciliationStatus> reconciliation_status;
    std::function<void()> testing_release_finalizer_barrier;
    Phase phase{Phase::Reserved};
    std::mutex lock;
};

struct CopyOperationRunReceiptCustodian::Impl final {
    struct TransparentStringHash final {
        using is_transparent = void;
        [[nodiscard]] size_t operator()(std::string_view _value) const noexcept
        {
            return std::hash<std::string_view>{}(_value);
        }
    };
    struct TransparentStringEqual final {
        using is_transparent = void;
        [[nodiscard]] bool operator()(std::string_view _lhs, std::string_view _rhs) const noexcept
        {
            return _lhs == _rhs;
        }
    };

    mutable std::mutex lock;
    std::unordered_map<std::string, std::shared_ptr<Slot>, TransparentStringHash, TransparentStringEqual> slots;
};

namespace {

static_assert(std::is_nothrow_move_constructible_v<OperationJournalItemResult>);
static_assert(std::is_nothrow_move_constructible_v<CopyOperationTerminalEvidence>);

CopyOperationOrchestratorError
CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode _code,
                        std::optional<OperationJournalError> _journal_error = std::nullopt,
                        std::optional<CopyOperationExecutionFactoryError> _factory_error = std::nullopt,
                        std::optional<PoolEnqueueResult> _enqueue_result = std::nullopt,
                        std::optional<CopyOperationRunReceiptRecoveryDisposition> _recovery_disposition = std::nullopt,
                        std::optional<ReviewedOperationFactoryError> _reviewed_factory_error = std::nullopt)
{
    return CopyOperationOrchestratorError{.code = _code,
                                          .journal_error = std::move(_journal_error),
                                          .factory_error = _factory_error,
                                          .enqueue_result = _enqueue_result,
                                          .recovery_disposition = _recovery_disposition,
                                          .reviewed_factory_error = std::move(_reviewed_factory_error)};
}

bool CopyOrchestratorIsCancelled(const CopyOperationOrchestrator::CancelChecker &_cancel_checker) noexcept
{
    if( !_cancel_checker )
        return false;
    try {
        return _cancel_checker();
    } catch( ... ) {
        return true;
    }
}

bool CopyOrchestratorValidColdObservation(const CopyOperationColdObservation &_observation) noexcept
{
    constexpr uint64_t known_notifications = Operation::NotifyAboutStart | Operation::NotifyAboutPause |
                                             Operation::NotifyAboutResume | Operation::NotifyAboutStop |
                                             Operation::NotifyAboutTitleChange;
    return _observation.notification_mask != 0 && (_observation.notification_mask & ~known_notifications) == 0 &&
           static_cast<bool>(_observation.callback);
}

OperationJournalItemResult CopyOrchestratorCancelledItemResult() noexcept
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

OperationJournalItemResult CopyOrchestratorEnqueueFailureItemResult(PoolEnqueueResult _result) noexcept
{
    if( _result == PoolEnqueueResult::ShuttingDown )
        return CopyOrchestratorCancelledItemResult();

    int system_error = EINVAL;
    switch( _result ) {
        case PoolEnqueueResult::ShuttingDown:
            break;
        case PoolEnqueueResult::Duplicate:
            system_error = EALREADY;
            break;
        case PoolEnqueueResult::NotCold:
        case PoolEnqueueResult::Accepted:
            system_error = EINVAL;
            break;
    }
    return OperationJournalItemResult{
        .item_index = 0,
        .status = OperationJournalItemStatus::Failed,
        .error = OperationJournalItemError::Unknown,
        .system_error = system_error,
        .prior_error = OperationJournalItemError::None,
        .prior_system_error = 0,
        .bytes = 0,
        .destination_publication = OperationJournalPublicationState::NotPublished,
        .filesystem_sync_status = OperationJournalFilesystemSyncStatus::NotAttempted,
        .filesystem_sync_system_error = 0,
        .recovery_action = OperationJournalRecoveryAction::Retry,
    };
}

std::optional<CopyOperationRunReceiptRecoveryDisposition>
CopyOrchestratorRecoveryDisposition(CopyOperationRunReceiptCustodyStatus _status) noexcept
{
    switch( _status ) {
        case CopyOperationRunReceiptCustodyStatus::RetryRequired:
            return CopyOperationRunReceiptRecoveryDisposition::RetryRequired;
        case CopyOperationRunReceiptCustodyStatus::ReconcileRequired:
            return CopyOperationRunReceiptRecoveryDisposition::ReconcileRequired;
        case CopyOperationRunReceiptCustodyStatus::Finalized:
        case CopyOperationRunReceiptCustodyStatus::ContractViolation:
        case CopyOperationRunReceiptCustodyStatus::NotFound:
        case CopyOperationRunReceiptCustodyStatus::Busy:
            return std::nullopt;
    }
    return std::nullopt;
}

bool CopyOrchestratorContractJournalError(OperationJournalErrorCode _code) noexcept
{
    switch( _code ) {
        case OperationJournalErrorCode::InvalidRunReceipt:
        case OperationJournalErrorCode::RunReceiptAlreadyConsumed:
        case OperationJournalErrorCode::InvalidItemResult:
        case OperationJournalErrorCode::InvalidTransition:
            return true;
        default:
            return false;
    }
}

} // namespace

CopyOperationRunReceiptCustodian::CopyOperationRunReceiptCustodian() : m_Impl{std::make_shared<Impl>()}
{
}

CopyOperationRunReceiptCustodian::~CopyOperationRunReceiptCustodian() = default;

void CopyOperationRunReceiptCustodian::DeliverTerminalOutcome(
    const std::shared_ptr<Slot> &_slot,
    CopyOperationDurableTerminalConfirmation _confirmation) noexcept
{
    if( !_slot )
        return;

    std::function<void(const CopyOperationDurableTerminalOutcome &)> observer;
    OperationJournalState state = OperationJournalState::Interrupted;
    CopyOperationTerminalEvidence *terminal_evidence = nullptr;
    std::optional<CopyOperationDurableTerminalOutcome> outcome;
    {
        const auto guard = std::lock_guard{_slot->lock};
        if( _slot->terminal_observer_delivered || !_slot->terminal_observer )
            return;

        switch( _confirmation ) {
            case CopyOperationDurableTerminalConfirmation::Finalized:
                if( _slot->phase != Slot::Phase::Finalized || !_slot->terminal_evidence )
                    return;
                state = _slot->terminal_evidence->state;
                terminal_evidence = &*_slot->terminal_evidence;
                break;
            case CopyOperationDurableTerminalConfirmation::ReconciledTerminal:
                if( (_slot->phase != Slot::Phase::Reconciled && _slot->phase != Slot::Phase::ReleaseInProgress) ||
                    _slot->reconciliation_status != CopyOperationRunReceiptReconciliationStatus::TerminalConfirmed ||
                    !_slot->terminal_evidence )
                    return;
                state = _slot->terminal_evidence->state;
                terminal_evidence = &*_slot->terminal_evidence;
                break;
            case CopyOperationDurableTerminalConfirmation::ReconciledInterrupted:
                if( (_slot->phase != Slot::Phase::Reconciled && _slot->phase != Slot::Phase::ReleaseInProgress) ||
                    _slot->reconciliation_status != CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed )
                    return;
                state = OperationJournalState::Interrupted;
                break;
        }

        try {
            outcome.emplace(CopyOperationDurableTerminalOutcome{.plan_id = std::move(_slot->terminal_outcome_plan_id),
                                                                .state = state,
                                                                .item_results = terminal_evidence
                                                                                    ? std::move(terminal_evidence->item_results)
                                                                                    : std::vector<OperationJournalItemResult>{},
                                                                .confirmation = _confirmation});
        } catch( ... ) {
            _slot->terminal_observer = {};
            _slot->terminal_observer_delivered = true;
            return;
        }
        observer = std::move(_slot->terminal_observer);
        _slot->terminal_observer_delivered = true;
    }

    try {
        observer(*outcome);
    } catch( ... ) {
    }
}

PoolTerminalFinalizationDecision
CopyOperationRunReceiptCustodian::ReleaseDecision(const std::shared_ptr<Slot> &_slot) noexcept
{
    if( !_slot )
        return PoolTerminalFinalizationDecision::Retain;
    const auto guard = std::lock_guard{_slot->lock};
    if( _slot->reconciliation_status == CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed )
        return PoolTerminalFinalizationDecision::ReleaseWithoutCompletion;
    if( !_slot->terminal_evidence )
        return PoolTerminalFinalizationDecision::Retain;
    return _slot->terminal_evidence->state == OperationJournalState::Completed
               ? PoolTerminalFinalizationDecision::Release
               : PoolTerminalFinalizationDecision::ReleaseWithoutCompletion;
}

std::expected<CopyOperationRunReceiptCustodian::Reservation, CopyOperationRunReceiptCustodianError>
CopyOperationRunReceiptCustodian::Reserve(
    OperationPlan _plan,
    std::shared_ptr<OperationJournal> _journal,
    CopyOperationExecutionProduct::TerminalEvidenceAccessor _accessor,
    std::function<void(const CopyOperationDurableTerminalOutcome &)> _terminal_observer)
{
    std::string plan_id;
    std::shared_ptr<Slot> slot;
    try {
        plan_id = std::string{_plan.Id().Value()};
        const auto storage_identity = _journal->StorageIdentityForCustody();
        slot = std::make_shared<Slot>(std::move(_plan),
                                      std::move(plan_id),
                                      std::move(_journal),
                                      std::move(_accessor),
                                      std::move(_terminal_observer),
                                      m_Impl,
                                      storage_identity);
    } catch( ... ) {
        return std::unexpected(CopyOperationRunReceiptCustodianError::ResourceLimitExceeded);
    }

    const auto guard = std::lock_guard{m_Impl->lock};
    if( m_Impl->slots.size() >= MaxPendingSlots )
        return std::unexpected(CopyOperationRunReceiptCustodianError::ResourceLimitExceeded);
    if( m_Impl->slots.contains(slot->plan_id) )
        return std::unexpected(CopyOperationRunReceiptCustodianError::DuplicatePlan);
    try {
        m_Impl->slots.emplace(slot->plan_id, slot);
    } catch( ... ) {
        return std::unexpected(CopyOperationRunReceiptCustodianError::ResourceLimitExceeded);
    }
    return Reservation{std::move(slot)};
}

bool CopyOperationRunReceiptCustodian::Arm(Reservation &_reservation, OperationJournalRunReceipt &&_receipt) noexcept
{
    const auto slot = _reservation.m_Slot;
    if( !slot )
        return false;
    const auto guard = std::lock_guard{slot->lock};
    if( slot->phase != Slot::Phase::Reserved || !slot->journal || _receipt.m_Consumed || _receipt.m_Plan != slot->plan )
        return false;
    const auto receipt_owner = _receipt.m_JournalInstance.lock();
    if( !receipt_owner || receipt_owner.get() != static_cast<const void *>(slot->journal->m_Impl.get()) )
        return false;
    slot->receipt.emplace(std::move(_receipt));
    slot->phase = Slot::Phase::Armed;
    return true;
}

void CopyOperationRunReceiptCustodian::Release(Reservation &_reservation) noexcept
{
    const auto slot = _reservation.m_Slot;
    if( !slot )
        return;
    {
        const auto guard = std::lock_guard{slot->lock};
        if( slot->phase != Slot::Phase::Reserved )
            return;
        slot->phase = Slot::Phase::Released;
    }
    const auto guard = std::lock_guard{m_Impl->lock};
    const auto found = m_Impl->slots.find(slot->plan_id);
    if( found != m_Impl->slots.end() && found->second == slot )
        m_Impl->slots.erase(found);
    _reservation.m_Slot.reset();
}

CopyOperationRunReceiptCustodyResult
CopyOperationRunReceiptCustodian::FinalizeCustodiedSlot(const std::shared_ptr<Slot> &_slot) noexcept
{
    const auto guard = std::lock_guard{_slot->lock};
    if( _slot->phase != Slot::Phase::Armed && _slot->phase != Slot::Phase::RetryRequired )
        return {.status = CopyOperationRunReceiptCustodyStatus::Busy, .journal_error = std::nullopt};
    if( !_slot->journal || !_slot->receipt || !_slot->terminal_evidence_acquired || !_slot->terminal_evidence ) {
        _slot->phase = Slot::Phase::ContractViolation;
        return {.status = CopyOperationRunReceiptCustodyStatus::ContractViolation, .journal_error = std::nullopt};
    }

    std::expected<void, OperationJournalError> finalized =
        std::unexpected(OperationJournalError{.code = OperationJournalErrorCode::JournalUnusable});
    try {
        finalized = _slot->journal->Finalize(
            std::move(*_slot->receipt), _slot->terminal_evidence->item_results, _slot->terminal_evidence->state);
    } catch( ... ) {
        _slot->phase = Slot::Phase::RetryRequired;
        _slot->last_journal_error.reset();
        return {.status = CopyOperationRunReceiptCustodyStatus::RetryRequired, .journal_error = std::nullopt};
    }
    if( finalized ) {
        _slot->receipt.reset();
        _slot->phase = Slot::Phase::Finalized;
        _slot->last_journal_error.reset();
        return {.status = CopyOperationRunReceiptCustodyStatus::Finalized, .journal_error = std::nullopt};
    }

    _slot->last_journal_error = finalized.error();
    if( finalized.error().code == OperationJournalErrorCode::DurabilityUncertain ||
        finalized.error().code == OperationJournalErrorCode::JournalUnusable ) {
        _slot->receipt.reset();
        _slot->journal.reset();
        _slot->accessor = {};
        _slot->phase = Slot::Phase::ReconcileRequired;
        return {.status = CopyOperationRunReceiptCustodyStatus::ReconcileRequired, .journal_error = finalized.error()};
    }
    if( CopyOrchestratorContractJournalError(finalized.error().code) ) {
        _slot->phase = Slot::Phase::ContractViolation;
        return {.status = CopyOperationRunReceiptCustodyStatus::ContractViolation, .journal_error = finalized.error()};
    }

    _slot->phase = Slot::Phase::RetryRequired;
    return {.status = CopyOperationRunReceiptCustodyStatus::RetryRequired, .journal_error = finalized.error()};
}

CopyOperationRunReceiptCustodyResult
CopyOperationRunReceiptCustodian::FinalizeBeforeEnqueue(Reservation &_reservation,
                                                        OperationJournalItemResult _result,
                                                        OperationJournalState _terminal_state) noexcept
{
    const auto slot = _reservation.m_Slot;
    if( !slot )
        return {.status = CopyOperationRunReceiptCustodyStatus::NotFound, .journal_error = std::nullopt};
    {
        const auto guard = std::lock_guard{slot->lock};
        if( slot->phase != Slot::Phase::Armed || slot->terminal_evidence_acquired ) {
            slot->phase = Slot::Phase::ContractViolation;
            return {.status = CopyOperationRunReceiptCustodyStatus::ContractViolation, .journal_error = std::nullopt};
        }
        const auto sources = slot->plan.Sources().size();
        if( !slot->pre_running_terminal_evidence_storage.empty() ||
            slot->pre_running_terminal_evidence_storage.capacity() < sources ) {
            slot->phase = Slot::Phase::ContractViolation;
            return {.status = CopyOperationRunReceiptCustodyStatus::ContractViolation, .journal_error = std::nullopt};
        }
        // One statement per source, not one about the first of them. What the caller describes -
        // cancelled, or refused before it could be enqueued - happened to the whole plan before any
        // item ran, and recording it against source 0 alone would be a claim about that source in
        // particular that nothing supports. Slot construction reserved exactly this many elements
        // before the Running transition, so nothing here can allocate.
        for( size_t index = 0; index != sources; ++index ) {
            auto item_result = _result;
            item_result.item_index = index;
            slot->pre_running_terminal_evidence_storage.push_back(item_result);
        }
        slot->terminal_evidence.emplace(CopyOperationTerminalEvidence{
            .state = _terminal_state,
            .item_results = std::move(slot->pre_running_terminal_evidence_storage),
        });
        slot->terminal_evidence_acquired = true;
    }

    const auto result = FinalizeCustodiedSlot(slot);
    if( result.status == CopyOperationRunReceiptCustodyStatus::Finalized ) {
        DeliverTerminalOutcome(slot, CopyOperationDurableTerminalConfirmation::Finalized);
        const auto guard = std::lock_guard{m_Impl->lock};
        const auto found = m_Impl->slots.find(slot->plan_id);
        if( found != m_Impl->slots.end() && found->second == slot )
            m_Impl->slots.erase(found);
        _reservation.m_Slot.reset();
    }
    return result;
}

bool CopyOperationRunReceiptCustodian::BeginEnqueue(Reservation &_reservation,
                                                    const std::shared_ptr<Pool> &_pool,
                                                    const std::shared_ptr<Operation> &_operation) noexcept
{
    const auto slot = _reservation.m_Slot;
    if( !slot )
        return false;
    const auto guard = std::lock_guard{slot->lock};
    if( slot->phase != Slot::Phase::Armed || slot->terminal_evidence_acquired || !slot->receipt )
        return false;
    slot->pool = _pool;
    slot->operation = _operation;
    slot->phase = Slot::Phase::EnqueueInProgress;
    return true;
}

void CopyOperationRunReceiptCustodian::CommitEnqueue(Reservation &_reservation) noexcept
{
    const auto slot = _reservation.m_Slot;
    if( !slot )
        return;
    bool finalized = false;
    {
        const auto guard = std::lock_guard{slot->lock};
        if( slot->phase == Slot::Phase::EnqueueInProgress )
            slot->phase = Slot::Phase::PoolOwned;
        else if( slot->phase != Slot::Phase::Finalized && slot->phase != Slot::Phase::ReconcileRequired )
            return;
        slot->pool_owned = true;
        finalized = slot->phase == Slot::Phase::Finalized;
    }
    if( finalized ) {
        const auto guard = std::lock_guard{m_Impl->lock};
        const auto found = m_Impl->slots.find(slot->plan_id);
        if( found != m_Impl->slots.end() && found->second == slot )
            m_Impl->slots.erase(found);
    }
    _reservation.m_Slot.reset();
}

CopyOperationRunReceiptCustodyResult
CopyOperationRunReceiptCustodian::RejectEnqueue(Reservation &_reservation,
                                                OperationJournalItemResult _result,
                                                OperationJournalState _terminal_state) noexcept
{
    const auto slot = _reservation.m_Slot;
    if( !slot )
        return {.status = CopyOperationRunReceiptCustodyStatus::NotFound, .journal_error = std::nullopt};
    {
        const auto guard = std::lock_guard{slot->lock};
        if( slot->phase != Slot::Phase::EnqueueInProgress || slot->terminal_evidence_acquired ) {
            slot->phase = Slot::Phase::ContractViolation;
            return {.status = CopyOperationRunReceiptCustodyStatus::ContractViolation, .journal_error = std::nullopt};
        }
        const auto sources = slot->plan.Sources().size();
        if( !slot->pre_running_terminal_evidence_storage.empty() ||
            slot->pre_running_terminal_evidence_storage.capacity() < sources ) {
            slot->phase = Slot::Phase::ContractViolation;
            return {.status = CopyOperationRunReceiptCustodyStatus::ContractViolation, .journal_error = std::nullopt};
        }
        // One statement per source, not one about the first of them. What the caller describes -
        // cancelled, or refused before it could be enqueued - happened to the whole plan before any
        // item ran, and recording it against source 0 alone would be a claim about that source in
        // particular that nothing supports. Slot construction reserved exactly this many elements
        // before the Running transition, so nothing here can allocate.
        for( size_t index = 0; index != sources; ++index ) {
            auto item_result = _result;
            item_result.item_index = index;
            slot->pre_running_terminal_evidence_storage.push_back(item_result);
        }
        slot->terminal_evidence.emplace(CopyOperationTerminalEvidence{
            .state = _terminal_state,
            .item_results = std::move(slot->pre_running_terminal_evidence_storage),
        });
        slot->terminal_evidence_acquired = true;
        slot->phase = Slot::Phase::Armed;
    }

    const auto result = FinalizeCustodiedSlot(slot);
    if( result.status == CopyOperationRunReceiptCustodyStatus::Finalized ) {
        DeliverTerminalOutcome(slot, CopyOperationDurableTerminalConfirmation::Finalized);
        const auto guard = std::lock_guard{m_Impl->lock};
        const auto found = m_Impl->slots.find(slot->plan_id);
        if( found != m_Impl->slots.end() && found->second == slot )
            m_Impl->slots.erase(found);
        _reservation.m_Slot.reset();
    }
    return result;
}

PoolTerminalFinalizationDecision
CopyOperationRunReceiptCustodian::FinalizePoolSlot(const std::shared_ptr<Slot> &_slot) noexcept
{
    auto guard = std::unique_lock{_slot->lock};
    if( _slot->phase == Slot::Phase::Reconciled || _slot->phase == Slot::Phase::ReleaseInProgress ) {
        std::function<void()> testing_release_finalizer_barrier;
        try {
            testing_release_finalizer_barrier = _slot->testing_release_finalizer_barrier;
        } catch( ... ) {
            return PoolTerminalFinalizationDecision::Retain;
        }
        const auto confirmation =
            _slot->reconciliation_status == CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed
                ? CopyOperationDurableTerminalConfirmation::ReconciledInterrupted
                : CopyOperationDurableTerminalConfirmation::ReconciledTerminal;
        _slot->pool_release_latched = true;
        guard.unlock();
        if( testing_release_finalizer_barrier ) {
            try {
                testing_release_finalizer_barrier();
            } catch( ... ) {
                return PoolTerminalFinalizationDecision::Retain;
            }
        }
        DeliverTerminalOutcome(_slot, confirmation);
        return ReleaseDecision(_slot);
    }
    if( _slot->phase == Slot::Phase::Finalized ) {
        guard.unlock();
        DeliverTerminalOutcome(_slot, CopyOperationDurableTerminalConfirmation::Finalized);
        return ReleaseDecision(_slot);
    }
    if( _slot->phase != Slot::Phase::EnqueueInProgress && _slot->phase != Slot::Phase::PoolOwned )
        return PoolTerminalFinalizationDecision::Retain;
    if( !_slot->receipt || !_slot->journal )
        return PoolTerminalFinalizationDecision::Retain;

    if( !_slot->terminal_evidence_acquired ) {
        std::expected<CopyOperationTerminalEvidence, CopyOperationTerminalResultError> result =
            std::unexpected(CopyOperationTerminalResultError::Inconsistent);
        try {
            result = _slot->accessor();
        } catch( ... ) {
            return PoolTerminalFinalizationDecision::Retain;
        }
        if( !result ) {
            if( result.error() == CopyOperationTerminalResultError::Inconsistent ) {
                _slot->terminal_result_error = result.error();
                _slot->terminal_evidence_acquired = true;
                _slot->phase = Slot::Phase::ContractViolation;
            }
            return PoolTerminalFinalizationDecision::Retain;
        }
        try {
            _slot->terminal_evidence.emplace(std::move(*result));
        } catch( ... ) {
            _slot->terminal_result_error = CopyOperationTerminalResultError::Inconsistent;
            _slot->terminal_evidence_acquired = true;
            _slot->phase = Slot::Phase::ContractViolation;
            return PoolTerminalFinalizationDecision::Retain;
        }
        _slot->terminal_evidence_acquired = true;
    }
    if( !_slot->terminal_evidence ) {
        _slot->phase = Slot::Phase::ContractViolation;
        return PoolTerminalFinalizationDecision::Retain;
    }

    std::expected<void, OperationJournalError> finalized =
        std::unexpected(OperationJournalError{.code = OperationJournalErrorCode::JournalUnusable});
    try {
        finalized = _slot->journal->Finalize(
            std::move(*_slot->receipt), _slot->terminal_evidence->item_results, _slot->terminal_evidence->state);
    } catch( ... ) {
        return PoolTerminalFinalizationDecision::Retain;
    }
    if( !finalized ) {
        _slot->last_journal_error = finalized.error();
        if( finalized.error().code == OperationJournalErrorCode::DurabilityUncertain ||
            finalized.error().code == OperationJournalErrorCode::JournalUnusable ) {
            _slot->receipt.reset();
            _slot->journal.reset();
            _slot->accessor = {};
            _slot->phase = Slot::Phase::ReconcileRequired;
        }
        else if( CopyOrchestratorContractJournalError(finalized.error().code) ) {
            _slot->phase = Slot::Phase::ContractViolation;
        }
        return PoolTerminalFinalizationDecision::Retain;
    }

    _slot->receipt.reset();
    _slot->last_journal_error.reset();
    _slot->phase = Slot::Phase::Finalized;
    guard.unlock();
    DeliverTerminalOutcome(_slot, CopyOperationDurableTerminalConfirmation::Finalized);
    const auto decision = ReleaseDecision(_slot);
    if( const auto owner = _slot->owner.lock() ) {
        const auto owner_guard = std::lock_guard{owner->lock};
        const auto found = owner->slots.find(_slot->plan_id);
        if( found != owner->slots.end() && found->second == _slot )
            owner->slots.erase(found);
    }
    return decision;
}

CopyOperationRunReceiptCustodyResult CopyOperationRunReceiptCustodian::Retry(std::string_view _plan_id) noexcept
{
    std::shared_ptr<Slot> slot;
    {
        const auto guard = std::lock_guard{m_Impl->lock};
        const auto found = m_Impl->slots.find(_plan_id);
        if( found == m_Impl->slots.end() )
            return {.status = CopyOperationRunReceiptCustodyStatus::NotFound, .journal_error = std::nullopt};
        slot = found->second;
    }

    {
        const auto guard = std::lock_guard{slot->lock};
        switch( slot->phase ) {
            case Slot::Phase::RetryRequired:
                break;
            case Slot::Phase::ReconcileRequired:
                return {.status = CopyOperationRunReceiptCustodyStatus::ReconcileRequired,
                        .journal_error = slot->last_journal_error};
            case Slot::Phase::ContractViolation:
                return {.status = CopyOperationRunReceiptCustodyStatus::ContractViolation,
                        .journal_error = slot->last_journal_error};
            case Slot::Phase::Reserved:
            case Slot::Phase::Armed:
            case Slot::Phase::EnqueueInProgress:
            case Slot::Phase::PoolOwned:
            case Slot::Phase::Reconciled:
            case Slot::Phase::ReleaseInProgress:
            case Slot::Phase::Finalized:
            case Slot::Phase::Released:
                return {.status = CopyOperationRunReceiptCustodyStatus::Busy,
                        .journal_error = slot->last_journal_error};
        }
    }

    const auto result = FinalizeCustodiedSlot(slot);
    if( result.status == CopyOperationRunReceiptCustodyStatus::Finalized ) {
        DeliverTerminalOutcome(slot, CopyOperationDurableTerminalConfirmation::Finalized);
        const auto guard = std::lock_guard{m_Impl->lock};
        const auto found = m_Impl->slots.find(slot->plan_id);
        if( found != m_Impl->slots.end() && found->second == slot )
            m_Impl->slots.erase(found);
    }
    return result;
}

CopyOperationRunReceiptReconciliationResult
CopyOperationRunReceiptCustodian::Reconcile(std::string_view _plan_id,
                                            const OperationJournal &_reopened_journal) noexcept
{
    std::shared_ptr<Slot> slot;
    {
        const auto guard = std::lock_guard{m_Impl->lock};
        const auto found = m_Impl->slots.find(_plan_id);
        if( found == m_Impl->slots.end() )
            return {.status = CopyOperationRunReceiptReconciliationStatus::NotFound};
        slot = found->second;
    }

    auto result = CopyOperationRunReceiptReconciliationStatus::Mismatch;
    bool pool_release_required = false;
    {
        const auto guard = std::lock_guard{slot->lock};
        switch( slot->phase ) {
            case Slot::Phase::ReconcileRequired:
                break;
            case Slot::Phase::RetryRequired:
                return {.status = CopyOperationRunReceiptReconciliationStatus::RetryRequired};
            case Slot::Phase::ContractViolation:
                return {.status = CopyOperationRunReceiptReconciliationStatus::ContractViolation};
            case Slot::Phase::Reconciled:
                if( !slot->reconciliation_status )
                    return {.status = CopyOperationRunReceiptReconciliationStatus::ContractViolation};
                return {.status = *slot->reconciliation_status, .pool_release_required = slot->pool_owned};
            case Slot::Phase::Reserved:
            case Slot::Phase::Armed:
            case Slot::Phase::EnqueueInProgress:
            case Slot::Phase::PoolOwned:
            case Slot::Phase::ReleaseInProgress:
            case Slot::Phase::Finalized:
            case Slot::Phase::Released:
                return {.status = CopyOperationRunReceiptReconciliationStatus::Busy};
        }
        if( !slot->terminal_evidence_acquired || !slot->terminal_evidence )
            return {.status = CopyOperationRunReceiptReconciliationStatus::ContractViolation};
        if( _reopened_journal.StorageIdentityForCustody() != slot->storage_identity )
            return {.status = CopyOperationRunReceiptReconciliationStatus::Mismatch};

        try {
            const auto snapshot = _reopened_journal.Snapshot();
            const auto entry = std::ranges::find_if(
                snapshot, [&](const auto &_entry) { return _entry.plan.Id().Value() == slot->plan.Id().Value(); });
            if( entry == snapshot.end() || entry->plan != slot->plan )
                return {.status = CopyOperationRunReceiptReconciliationStatus::Mismatch};
            if( entry->state == slot->terminal_evidence->state &&
                entry->item_results == slot->terminal_evidence->item_results ) {
                result = CopyOperationRunReceiptReconciliationStatus::TerminalConfirmed;
            }
            else if( entry->state == OperationJournalState::Interrupted && entry->item_results.empty() ) {
                result = CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed;
            }
            else {
                return {.status = CopyOperationRunReceiptReconciliationStatus::Mismatch};
            }
            slot->reconciliation_status = result;
            slot->phase = Slot::Phase::Reconciled;
        } catch( ... ) {
            return {.status = CopyOperationRunReceiptReconciliationStatus::Mismatch};
        }
        pool_release_required = slot->pool_owned;
    }

    if( pool_release_required )
        return {.status = result, .pool_release_required = true};

    DeliverTerminalOutcome(slot,
                           result == CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed
                               ? CopyOperationDurableTerminalConfirmation::ReconciledInterrupted
                               : CopyOperationDurableTerminalConfirmation::ReconciledTerminal);
    {
        const auto guard = std::lock_guard{slot->lock};
        if( slot->phase != Slot::Phase::Reconciled )
            return {.status = CopyOperationRunReceiptReconciliationStatus::ContractViolation};
        slot->phase = Slot::Phase::Released;
    }

    const auto guard = std::lock_guard{m_Impl->lock};
    const auto found = m_Impl->slots.find(slot->plan_id);
    if( found != m_Impl->slots.end() && found->second == slot )
        m_Impl->slots.erase(found);
    return {.status = result, .pool_release_required = false};
}

CopyOperationRunReceiptPoolReleaseStatus
CopyOperationRunReceiptCustodian::ReleaseReconciled(std::string_view _plan_id) noexcept
{
    std::shared_ptr<Slot> slot;
    {
        const auto guard = std::lock_guard{m_Impl->lock};
        const auto found = m_Impl->slots.find(_plan_id);
        if( found == m_Impl->slots.end() )
            return CopyOperationRunReceiptPoolReleaseStatus::NotFound;
        slot = found->second;
    }

    std::shared_ptr<Pool> pool;
    std::shared_ptr<Operation> operation;
    bool release_without_live_pool = false;
    auto release_confirmation = CopyOperationDurableTerminalConfirmation::ReconciledTerminal;
    {
        const auto guard = std::lock_guard{slot->lock};
        if( slot->phase == Slot::Phase::ReleaseInProgress )
            return CopyOperationRunReceiptPoolReleaseStatus::InProgress;
        if( slot->phase == Slot::Phase::ContractViolation )
            return CopyOperationRunReceiptPoolReleaseStatus::ContractViolation;
        if( slot->phase != Slot::Phase::Reconciled || !slot->pool_owned || !slot->reconciliation_status )
            return CopyOperationRunReceiptPoolReleaseStatus::Busy;
        if( *slot->reconciliation_status == CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed )
            release_confirmation = CopyOperationDurableTerminalConfirmation::ReconciledInterrupted;
        pool = slot->pool.lock();
        operation = slot->operation;
        if( !operation )
            return CopyOperationRunReceiptPoolReleaseStatus::ContractViolation;
        if( !pool ) {
            slot->phase = Slot::Phase::ReleaseInProgress;
            release_without_live_pool = true;
        }
        else {
            slot->phase = Slot::Phase::ReleaseInProgress;
        }
    }

    if( release_without_live_pool ) {
        DeliverTerminalOutcome(slot, release_confirmation);
        {
            const auto guard = std::lock_guard{slot->lock};
            slot->phase = Slot::Phase::Released;
        }
        const auto guard = std::lock_guard{m_Impl->lock};
        const auto found = m_Impl->slots.find(slot->plan_id);
        if( found != m_Impl->slots.end() && found->second == slot )
            m_Impl->slots.erase(found);
        return CopyOperationRunReceiptPoolReleaseStatus::Released;
    }

    PoolRetryFinalizationResult released = PoolRetryFinalizationResult::NotFinalizing;
    bool threw = false;
    try {
        released = pool->RetryFinalization(operation);
    } catch( ... ) {
        threw = true;
    }
    bool exact_release_confirmed = false;
    {
        const auto guard = std::lock_guard{slot->lock};
        exact_release_confirmed = slot->pool_release_latched;
    }
    const bool still_in_progress = released == PoolRetryFinalizationResult::InProgress;
    const bool retained = released == PoolRetryFinalizationResult::Retained;
    const bool confirmed = exact_release_confirmed && !still_in_progress && !retained &&
                           (threw || released == PoolRetryFinalizationResult::Released ||
                            released == PoolRetryFinalizationResult::NotFinalizing);
    if( !confirmed ) {
        const auto guard = std::lock_guard{slot->lock};
        if( slot->phase == Slot::Phase::ReleaseInProgress )
            slot->phase = Slot::Phase::Reconciled;
        if( still_in_progress )
            return CopyOperationRunReceiptPoolReleaseStatus::InProgress;
        if( released == PoolRetryFinalizationResult::NotFinalizing )
            return CopyOperationRunReceiptPoolReleaseStatus::ContractViolation;
        return threw || retained ? CopyOperationRunReceiptPoolReleaseStatus::Retained
                                 : CopyOperationRunReceiptPoolReleaseStatus::ContractViolation;
    }

    {
        const auto guard = std::lock_guard{slot->lock};
        slot->phase = Slot::Phase::Released;
    }
    const auto guard = std::lock_guard{m_Impl->lock};
    const auto found = m_Impl->slots.find(slot->plan_id);
    if( found != m_Impl->slots.end() && found->second == slot )
        m_Impl->slots.erase(found);
    return CopyOperationRunReceiptPoolReleaseStatus::Released;
}

bool CopyOperationRunReceiptCustodianTesting::EnqueueExactTerminalEvidence(
    CopyOperationRunReceiptCustodian &_custodian,
    OperationPlan _plan,
    std::shared_ptr<OperationJournal> _journal,
    OperationJournalRunReceipt &&_receipt,
    CopyOperationExecutionProduct::TerminalEvidenceAccessor _accessor,
    const std::shared_ptr<Pool> &_pool,
    const std::shared_ptr<Operation> &_operation,
    std::function<void(const CopyOperationDurableTerminalOutcome &)> _terminal_observer)
{
    if( !_journal || !_pool || !_operation || !_accessor )
        return false;
    auto reservation = _custodian.Reserve(
        std::move(_plan), std::move(_journal), std::move(_accessor), std::move(_terminal_observer));
    if( !reservation )
        return false;
    if( !_custodian.Arm(*reservation, std::move(_receipt)) ) {
        _custodian.Release(*reservation);
        return false;
    }
    if( !_custodian.BeginEnqueue(*reservation, _pool, _operation) ) {
        (void)_custodian.FinalizeBeforeEnqueue(
            *reservation, CopyOrchestratorEnqueueFailureItemResult(PoolEnqueueResult::NotCold), OperationJournalState::Failed);
        return false;
    }

    const auto finalization_slot = reservation->m_Slot;
    PoolEnqueueResult enqueue_result = PoolEnqueueResult::NotCold;
    try {
        enqueue_result = _pool->TryEnqueue(_operation, [finalization_slot](const std::shared_ptr<Operation> &) {
            return CopyOperationRunReceiptCustodian::FinalizePoolSlot(finalization_slot);
        });
    } catch( ... ) {
        enqueue_result = PoolEnqueueResult::NotCold;
    }
    if( enqueue_result != PoolEnqueueResult::Accepted ) {
        const auto terminal_state = enqueue_result == PoolEnqueueResult::ShuttingDown ? OperationJournalState::Cancelled
                                                                                      : OperationJournalState::Failed;
        (void)_custodian.RejectEnqueue(
            *reservation, CopyOrchestratorEnqueueFailureItemResult(enqueue_result), terminal_state);
        return false;
    }
    _custodian.CommitEnqueue(*reservation);
    return true;
}

bool CopyOperationRunReceiptCustodianTesting::SetReleaseFinalizerBarrier(CopyOperationRunReceiptCustodian &_custodian,
                                                                         std::string_view _plan_id,
                                                                         std::function<void()> _barrier)
{
    std::shared_ptr<CopyOperationRunReceiptCustodian::Slot> slot;
    {
        const auto guard = std::lock_guard{_custodian.m_Impl->lock};
        const auto found = _custodian.m_Impl->slots.find(_plan_id);
        if( found == _custodian.m_Impl->slots.end() )
            return false;
        slot = found->second;
    }
    const auto guard = std::lock_guard{slot->lock};
    if( slot->phase != CopyOperationRunReceiptCustodian::Slot::Phase::Reconciled )
        return false;
    slot->testing_release_finalizer_barrier = std::move(_barrier);
    return true;
}

size_t CopyOperationRunReceiptCustodian::PendingCount() const noexcept
{
    const auto guard = std::lock_guard{m_Impl->lock};
    return m_Impl->slots.size();
}

CopyOperationOrchestrator::CopyOperationOrchestrator(
    std::shared_ptr<OperationJournal> _journal,
    std::shared_ptr<Pool> _pool,
    std::shared_ptr<CopyOperationRunReceiptCustodian> _run_receipt_custodian)
    : m_Journal{std::move(_journal)}, m_Pool{std::move(_pool)}, m_RunReceiptCustodian{std::move(_run_receipt_custodian)}
{
}

CopyOperationOrchestrator::CopyOperationOrchestrator(
    std::shared_ptr<OperationJournal> _journal,
    std::shared_ptr<Pool> _pool,
    ExecutionFactory _execution_factory,
    std::shared_ptr<CopyOperationRunReceiptCustodian> _run_receipt_custodian)
    : m_Journal{std::move(_journal)}, m_Pool{std::move(_pool)}, m_ExecutionFactory{std::move(_execution_factory)},
      m_UseInjectedExecutionFactory{true}, m_RunReceiptCustodian{std::move(_run_receipt_custodian)}
{
}

std::expected<std::shared_ptr<Operation>, CopyOperationOrchestratorError>
CopyOperationOrchestrator::Submit(ReviewedVFSOperationPreflight _reviewed,
                                  CancelChecker _cancel_checker,
                                  CopyOperationSubmissionHooks _hooks)
{
    if( !m_Journal )
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::MissingJournal));
    if( !m_Pool )
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::MissingPool));
    if( m_UseInjectedExecutionFactory && !m_ExecutionFactory )
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::MissingExecutionFactory));
    if( !m_RunReceiptCustodian )
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::MissingRunReceiptCustodian));

    const auto &accepted = _reviewed.AcceptedPlan();
    const OperationPlan plan = accepted.Plan();
    // A batch is admitted like any other reviewed Copy now, and a Move is admitted the same way a
    // Copy is: both publish their destination and both are what the journal already knows how to
    // record. What is still refused is a report that does not cover the plan's sources one item
    // each: this boundary is the one that writes to the journal, and the journal numbers results in
    // the source space and will not record a completed entry missing one. Refusing before admission
    // keeps an unexecutable plan out of the record rather than leaving a failed entry behind it.
    if( (plan.Type() != OperationPlanType::Copy && plan.Type() != OperationPlanType::Move) ||
        accepted.Report().items.size() != plan.Sources().size() ) {
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::UnsupportedReviewedPlan));
    }

    auto admission = [&]() -> std::expected<OperationJournalAdmissionReceipt, OperationJournalError> {
        try {
            return m_Journal->Admit(plan);
        } catch( ... ) {
            return std::unexpected(OperationJournalError{.code = OperationJournalErrorCode::JournalUnusable});
        }
    }();
    if( !admission ) {
        return std::unexpected(
            CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::JournalAdmissionFailed, admission.error()));
    }

    return SubmitAdmitted(
        std::move(_reviewed), std::move(*admission), std::move(_cancel_checker), std::move(_hooks), {});
}

std::expected<std::shared_ptr<Operation>, CopyOperationOrchestratorError>
CopyOperationOrchestrator::SubmitAdmitted(ReviewedVFSOperationPreflight _reviewed,
                                          OperationJournalAdmissionReceipt _admission,
                                          CancelChecker _cancel_checker,
                                          CopyOperationSubmissionHooks _hooks,
                                          PreEnqueueHandoff _pre_enqueue_handoff)
{
    if( !m_Journal )
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::MissingJournal));
    if( !m_Pool )
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::MissingPool));
    if( m_UseInjectedExecutionFactory && !m_ExecutionFactory )
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::MissingExecutionFactory));
    if( !m_RunReceiptCustodian )
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::MissingRunReceiptCustodian));

    const auto &accepted = _reviewed.AcceptedPlan();
    const OperationPlan plan = accepted.Plan();
    // Same rule as the direct entry point, asked again because this one is reached with an admission
    // already in hand: what the journal cannot record must not get as far as running.
    if( (plan.Type() != OperationPlanType::Copy && plan.Type() != OperationPlanType::Move) ||
        accepted.Report().items.size() != plan.Sources().size() )
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::UnsupportedReviewedPlan));

    std::expected<void, OperationJournalError> validated =
        std::unexpected(OperationJournalError{.code = OperationJournalErrorCode::JournalUnusable});
    try {
        validated = m_Journal->ValidateAdmissionReceiptForOrchestration(_admission);
    } catch( ... ) {
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::InvalidJournalAdmissionReceipt));
    }
    if( !validated )
        return std::unexpected(
            CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::InvalidJournalAdmissionReceipt, validated.error()));
    if( _admission.m_Plan != plan )
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::InvalidJournalAdmissionReceipt));
    std::expected<OperationJournalAdmissionReceipt, OperationJournalError> admission{std::move(_admission)};

    const auto finalize_admission = [&](OperationJournalState _state) -> std::optional<CopyOperationOrchestratorError> {
        try {
            const auto finalized = m_Journal->FinalizeAdmission(std::move(*admission), _state);
            if( !finalized ) {
                return CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::AdmissionFinalizationFailed,
                                               finalized.error());
            }
        } catch( ... ) {
            return CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::AdmissionFinalizationFailed);
        }
        return std::nullopt;
    };

    const auto sanitized_cancel_checker = [_cancel_checker = std::move(_cancel_checker)]() noexcept {
        return CopyOrchestratorIsCancelled(_cancel_checker);
    };
    if( sanitized_cancel_checker() ) {
        if( const auto finalization_error = finalize_admission(OperationJournalState::Cancelled) )
            return std::unexpected(*finalization_error);
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::Cancelled));
    }

    std::expected<CopyOperationExecutionProduct, CopyOperationExecutionFactoryError> product =
        std::unexpected(CopyOperationExecutionFactoryError::Rejected);
    if( m_UseInjectedExecutionFactory ) {
        try {
            product = m_ExecutionFactory(std::move(_reviewed), sanitized_cancel_checker);
        } catch( ... ) {
            product = std::unexpected(CopyOperationExecutionFactoryError::Rejected);
        }
        if( !product ) {
            const auto terminal_state = product.error() == CopyOperationExecutionFactoryError::Cancelled
                                            ? OperationJournalState::Cancelled
                                            : OperationJournalState::Failed;
            if( const auto finalization_error = finalize_admission(terminal_state) )
                return std::unexpected(*finalization_error);
            if( product.error() == CopyOperationExecutionFactoryError::Cancelled )
                return std::unexpected(CopyOrchestratorFailure(
                    CopyOperationOrchestratorErrorCode::Cancelled, std::nullopt, product.error()));
            return std::unexpected(CopyOrchestratorFailure(
                CopyOperationOrchestratorErrorCode::ExecutionFactoryFailed, std::nullopt, product.error()));
        }
    }
    else {
        std::expected<CopyOperationExecutionProduct, ReviewedOperationFactoryError> reviewed_product = std::unexpected(
            ReviewedOperationFactoryError{.code = ReviewedOperationFactoryErrorCode::ConstructionFailed});
        if( m_ConditionalCommitTransactionResolver ) {
            reviewed_product = ReviewedOperationFactory::CreateExecutionProductWithDependencies(
                std::move(_reviewed), sanitized_cancel_checker, {}, {}, m_ConditionalCommitTransactionResolver);
        }
        else {
            reviewed_product =
                ReviewedOperationFactory::CreateExecutionProduct(std::move(_reviewed), sanitized_cancel_checker);
        }
        if( !reviewed_product ) {
            auto factory_failure = std::move(reviewed_product.error());
            const bool cancelled = factory_failure.code == ReviewedOperationFactoryErrorCode::Cancelled;
            if( const auto finalization_error =
                    finalize_admission(cancelled ? OperationJournalState::Cancelled : OperationJournalState::Failed) )
                return std::unexpected(*finalization_error);
            return std::unexpected(
                CopyOrchestratorFailure(cancelled ? CopyOperationOrchestratorErrorCode::Cancelled
                                                  : CopyOperationOrchestratorErrorCode::ExecutionFactoryFailed,
                                        std::nullopt,
                                        std::nullopt,
                                        std::nullopt,
                                        std::nullopt,
                                        std::move(factory_failure)));
        }
        product = std::move(*reviewed_product);
    }
    if( !product->m_Operation || !product->m_TerminalEvidence ||
        product->m_Operation->State() != OperationState::Cold ) {
        if( const auto finalization_error = finalize_admission(OperationJournalState::Failed) )
            return std::unexpected(*finalization_error);
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::InvalidExecutionProduct));
    }

    const auto operation = product->m_Operation;
    bool valid_configuration = true;
    for( const auto &observation : _hooks.cold_operation_observations )
        valid_configuration = valid_configuration && CopyOrchestratorValidColdObservation(observation);
    try {
        if( !valid_configuration )
            throw std::invalid_argument{"invalid cold operation observation"};
        for( auto &observation : _hooks.cold_operation_observations ) {
            operation->ObserveUnticketed(observation.notification_mask, std::move(observation.callback));
        }
        if( _hooks.item_status_observer )
            operation->SetItemStatusCallback(std::move(_hooks.item_status_observer));
    } catch( ... ) {
        product->m_Operation.reset();
        product->m_TerminalEvidence = {};
        product->m_TerminalItemResult = {};
        if( const auto finalization_error = finalize_admission(OperationJournalState::Failed) )
            return std::unexpected(*finalization_error);
        return std::unexpected(
            CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::OperationConfigurationFailed));
    }
    auto reservation = m_RunReceiptCustodian->Reserve(
        plan, m_Journal, std::move(product->m_TerminalEvidence), std::move(_hooks.durable_terminal_observer));
    if( !reservation ) {
        if( const auto finalization_error = finalize_admission(OperationJournalState::Failed) )
            return std::unexpected(*finalization_error);
        return std::unexpected(
            CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::RunReceiptReservationFailed));
    }

    auto run = [&]() -> std::expected<OperationJournalRunReceipt, OperationJournalError> {
        try {
            return m_Journal->TransitionToRunning(std::move(*admission));
        } catch( ... ) {
            return std::unexpected(OperationJournalError{.code = OperationJournalErrorCode::JournalUnusable});
        }
    }();
    if( !run ) {
        m_RunReceiptCustodian->Release(*reservation);
        const auto transition_error = run.error();
        if( const auto finalization_error = finalize_admission(OperationJournalState::Failed) )
            return std::unexpected(*finalization_error);
        return std::unexpected(
            CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::RunningTransitionFailed, transition_error));
    }

    if( !m_RunReceiptCustodian->Arm(*reservation, std::move(*run)) ) {
        m_RunReceiptCustodian->Release(*reservation);
        try {
            // Every source, for the same reason the custodian gives one statement per source: the arm
            // failed before any item ran, which is true of all of them.
            std::vector<OperationJournalItemResult> item_results;
            item_results.reserve(plan.Sources().size());
            for( size_t index = 0; index != plan.Sources().size(); ++index ) {
                auto item_result = CopyOrchestratorEnqueueFailureItemResult(PoolEnqueueResult::NotCold);
                item_result.item_index = index;
                item_results.push_back(item_result);
            }
            const auto finalized =
                m_Journal->Finalize(std::move(*run), item_results, OperationJournalState::Failed);
            if( !finalized ) {
                return std::unexpected(CopyOrchestratorFailure(
                    CopyOperationOrchestratorErrorCode::RunningFinalizationFailed, finalized.error()));
            }
        } catch( ... ) {
            return std::unexpected(
                CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::RunningFinalizationFailed));
        }
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::RunReceiptArmFailed));
    }

    if( sanitized_cancel_checker() ) {
        const auto finalized = m_RunReceiptCustodian->FinalizeBeforeEnqueue(
            *reservation, CopyOrchestratorCancelledItemResult(), OperationJournalState::Cancelled);
        if( finalized.status != CopyOperationRunReceiptCustodyStatus::Finalized ) {
            return std::unexpected(
                CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::RunningFinalizationFailed,
                                        finalized.journal_error,
                                        std::nullopt,
                                        std::nullopt,
                                        CopyOrchestratorRecoveryDisposition(finalized.status)));
        }
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::Cancelled));
    }

    std::optional<CopyOperationPreEnqueueLease> pre_enqueue_lease;
    try {
        if( _pre_enqueue_handoff )
            pre_enqueue_lease.emplace(_pre_enqueue_handoff(m_Pool, operation));
    } catch( ... ) {
        const auto finalized = m_RunReceiptCustodian->FinalizeBeforeEnqueue(
            *reservation, CopyOrchestratorEnqueueFailureItemResult(PoolEnqueueResult::NotCold), OperationJournalState::Failed);
        if( finalized.status != CopyOperationRunReceiptCustodyStatus::Finalized ) {
            return std::unexpected(
                CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::RunningFinalizationFailed,
                                        finalized.journal_error,
                                        std::nullopt,
                                        std::nullopt,
                                        CopyOrchestratorRecoveryDisposition(finalized.status)));
        }
        return std::unexpected(
            CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::PreEnqueuePreparationFailed));
    }

    // The private lease serializes this final intent check with coordinator cancellation. A cancel
    // accepted before custody opens Pool enqueue is durably terminalized without Pool admission.
    if( sanitized_cancel_checker() ) {
        const auto finalized = m_RunReceiptCustodian->FinalizeBeforeEnqueue(
            *reservation, CopyOrchestratorCancelledItemResult(), OperationJournalState::Cancelled);
        if( finalized.status != CopyOperationRunReceiptCustodyStatus::Finalized ) {
            return std::unexpected(
                CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::RunningFinalizationFailed,
                                        finalized.journal_error,
                                        std::nullopt,
                                        std::nullopt,
                                        CopyOrchestratorRecoveryDisposition(finalized.status)));
        }
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::Cancelled));
    }

    if( !m_RunReceiptCustodian->BeginEnqueue(*reservation, m_Pool, operation) ) {
        const auto finalized = m_RunReceiptCustodian->FinalizeBeforeEnqueue(
            *reservation, CopyOrchestratorEnqueueFailureItemResult(PoolEnqueueResult::NotCold), OperationJournalState::Failed);
        if( finalized.status != CopyOperationRunReceiptCustodyStatus::Finalized ) {
            return std::unexpected(
                CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::RunningFinalizationFailed,
                                        finalized.journal_error,
                                        std::nullopt,
                                        std::nullopt,
                                        CopyOrchestratorRecoveryDisposition(finalized.status)));
        }
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::RunReceiptArmFailed));
    }
    const auto finalization_slot = reservation->m_Slot;

    PoolEnqueueResult enqueue_result = PoolEnqueueResult::NotCold;
    try {
        enqueue_result = m_Pool->TryEnqueue(operation, [finalization_slot](const std::shared_ptr<Operation> &) {
            return CopyOperationRunReceiptCustodian::FinalizePoolSlot(finalization_slot);
        });
    } catch( ... ) {
        const auto finalized =
            m_RunReceiptCustodian->RejectEnqueue(*reservation,
                                                 CopyOrchestratorEnqueueFailureItemResult(PoolEnqueueResult::NotCold),
                                                 OperationJournalState::Failed);
        if( finalized.status != CopyOperationRunReceiptCustodyStatus::Finalized ) {
            return std::unexpected(
                CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::RunningFinalizationFailed,
                                        finalized.journal_error,
                                        std::nullopt,
                                        std::nullopt,
                                        CopyOrchestratorRecoveryDisposition(finalized.status)));
        }
        return std::unexpected(CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::EnqueueRejected));
    }
    if( enqueue_result != PoolEnqueueResult::Accepted ) {
        const auto terminal_state = enqueue_result == PoolEnqueueResult::ShuttingDown ? OperationJournalState::Cancelled
                                                                                      : OperationJournalState::Failed;
        const auto finalized = m_RunReceiptCustodian->RejectEnqueue(
            *reservation, CopyOrchestratorEnqueueFailureItemResult(enqueue_result), terminal_state);
        if( finalized.status != CopyOperationRunReceiptCustodyStatus::Finalized ) {
            return std::unexpected(
                CopyOrchestratorFailure(CopyOperationOrchestratorErrorCode::RunningFinalizationFailed,
                                        finalized.journal_error,
                                        std::nullopt,
                                        enqueue_result,
                                        CopyOrchestratorRecoveryDisposition(finalized.status)));
        }
        return std::unexpected(CopyOrchestratorFailure(
            CopyOperationOrchestratorErrorCode::EnqueueRejected, std::nullopt, std::nullopt, enqueue_result));
    }

    m_RunReceiptCustodian->CommitEnqueue(*reservation);
    return operation;
}

} // namespace nc::ops
