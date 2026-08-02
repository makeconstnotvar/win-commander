// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CopyOperationRecoveryCoordinator.h"

#include <algorithm>
#include <utility>

namespace nc::core {

CopyOperationRecoveryCoordinator::Services CopyOperationRecoveryCoordinator::MakeProductionServices(
    const std::shared_ptr<nc::ops::CopyOperationRunReceiptCustodian> &_custodian)
{
    if( !_custodian )
        return {};

    return Services{
        .retry = [_custodian](const std::string_view _plan_id) { return _custodian->Retry(_plan_id); },
        .reopen =
            [](const std::string_view _state_directory) { return nc::ops::OperationJournal::Open(_state_directory); },
        .reconcile =
            [_custodian](const std::string_view _plan_id, const nc::ops::OperationJournal &_journal) {
                return _custodian->Reconcile(_plan_id, _journal);
            },
        .release_reconciled =
            [_custodian](const std::string_view _plan_id) { return _custodian->ReleaseReconciled(_plan_id); },
        .pending_count = [_custodian] { return _custodian->PendingCount(); },
    };
}

CopyOperationRecoveryCoordinator::CopyOperationRecoveryCoordinator(
    std::shared_ptr<nc::ops::OperationJournal> _journal,
    std::shared_ptr<nc::ops::CopyOperationRunReceiptCustodian> _custodian,
    std::string _state_directory)
    : CopyOperationRecoveryCoordinator(_journal,
                                       _custodian,
                                       std::move(_state_directory),
                                       MakeProductionServices(_custodian))
{
}

CopyOperationRecoveryCoordinator::CopyOperationRecoveryCoordinator(
    std::shared_ptr<nc::ops::OperationJournal> _journal,
    std::shared_ptr<nc::ops::CopyOperationRunReceiptCustodian> _custodian,
    std::string _state_directory,
    Services _services)
    : m_Journal{std::move(_journal)}, m_Custodian{std::move(_custodian)}, m_StateDirectory{std::move(_state_directory)},
      m_Services{std::move(_services)}
{
    if( !m_Journal )
        return;

    const auto snapshot = m_Journal->Snapshot();
    std::ranges::copy_if(
        snapshot, std::back_inserter(m_StartupInterruptedHistory), [](const nc::ops::OperationJournalEntry &_entry) {
            return _entry.plan.Type() == nc::ops::OperationPlanType::Copy &&
                   _entry.state == nc::ops::OperationJournalState::Interrupted;
        });
}

const std::vector<nc::ops::OperationJournalEntry> &
CopyOperationRecoveryCoordinator::StartupInterruptedHistory() const noexcept
{
    return m_StartupInterruptedHistory;
}

std::optional<size_t> CopyOperationRecoveryCoordinator::PendingRecoveryCount() const noexcept
{
    const auto lock = std::lock_guard{m_Mutex};
    if( !m_Services.pending_count )
        return std::nullopt;
    try {
        return m_Services.pending_count();
    } catch( ... ) {
        return std::nullopt;
    }
}

CopyOperationRecoveryServiceResult CopyOperationRecoveryCoordinator::Service(const std::string_view _plan_id) noexcept
{
    auto result = CopyOperationRecoveryServiceResult{};
    auto lock = std::unique_lock{m_Mutex, std::try_to_lock};
    if( !lock.owns_lock() ) {
        result.error = CopyOperationRecoveryServiceError::CoordinatorBusy;
        return result;
    }
    if( !m_Services.retry ) {
        result.error = CopyOperationRecoveryServiceError::RuntimeUnavailable;
        return result;
    }

    try {
        result.retry = m_Services.retry(_plan_id);
        result.last_step = CopyOperationRecoveryServiceStep::Retry;
        if( result.retry->status != nc::ops::CopyOperationRunReceiptCustodyStatus::ReconcileRequired )
            return result;

        if( !m_Services.reopen || !m_Services.reconcile ) {
            result.error = CopyOperationRecoveryServiceError::RuntimeUnavailable;
            return result;
        }

        // A live external journal holder can represent admission or an operation still reaching Pool.
        // Reopening would otherwise convert its persisted Running state to Interrupted.
        if( m_Journal && m_Journal.use_count() != 1 ) {
            result.error = CopyOperationRecoveryServiceError::JournalInUse;
            return result;
        }

        m_Journal.reset();
        result.last_step = CopyOperationRecoveryServiceStep::Reopen;
        auto reopened = m_Services.reopen(m_StateDirectory);
        if( !reopened ) {
            result.error = CopyOperationRecoveryServiceError::JournalReopenFailed;
            result.reopen_error = reopened.error();
            return result;
        }

        m_Journal = std::make_shared<nc::ops::OperationJournal>(std::move(*reopened));
        result.reconciliation = m_Services.reconcile(_plan_id, *m_Journal);
        result.last_step = CopyOperationRecoveryServiceStep::Reconcile;

        const bool confirmed =
            result.reconciliation->status == nc::ops::CopyOperationRunReceiptReconciliationStatus::TerminalConfirmed ||
            result.reconciliation->status == nc::ops::CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed;
        if( !confirmed || !result.reconciliation->pool_release_required )
            return result;

        if( !m_Services.release_reconciled ) {
            result.error = CopyOperationRecoveryServiceError::RuntimeUnavailable;
            return result;
        }
        result.release = m_Services.release_reconciled(_plan_id);
        result.last_step = CopyOperationRecoveryServiceStep::ReleaseReconciled;
        return result;
    } catch( ... ) {
        result.error = CopyOperationRecoveryServiceError::UnexpectedFailure;
        return result;
    }
}

std::shared_ptr<nc::ops::OperationJournal> CopyOperationRecoveryCoordinator::CurrentJournal() const noexcept
{
    const auto lock = std::lock_guard{m_Mutex};
    return m_Journal;
}

std::shared_ptr<nc::ops::CopyOperationRunReceiptCustodian>
CopyOperationRecoveryCoordinator::CurrentRunReceiptCustodian() const noexcept
{
    const auto lock = std::lock_guard{m_Mutex};
    return m_Custodian;
}

} // namespace nc::core
