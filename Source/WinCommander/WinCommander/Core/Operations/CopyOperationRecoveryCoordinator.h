// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Operations/CopyOperationOrchestrator.h>
#include <Operations/OperationCenterCoordinator.h>
#include <Operations/OperationJournal.h>

#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nc::core {

class CopyOperationRecoveryCoordinatorTesting;

enum class CopyOperationRecoveryServiceStep : uint8_t {
    None,
    Retry,
    Reopen,
    Reconcile,
    ReleaseReconciled
};

enum class CopyOperationRecoveryServiceError : uint8_t {
    None,
    RuntimeUnavailable,
    CoordinatorBusy,
    JournalInUse,
    JournalReopenFailed,
    UnexpectedFailure
};

/**
 * Exact outcome of one bounded, explicitly requested custody service pass.
 *
 * Raw custody statuses remain visible so callers do not infer publication or durable terminal state from
 * a coordinator-level success value.
 */
struct CopyOperationRecoveryServiceResult final {
    CopyOperationRecoveryServiceStep last_step{CopyOperationRecoveryServiceStep::None};
    CopyOperationRecoveryServiceError error{CopyOperationRecoveryServiceError::None};
    std::optional<nc::ops::CopyOperationRunReceiptCustodyResult> retry;
    std::optional<nc::ops::OperationJournalError> reopen_error;
    std::optional<nc::ops::CopyOperationRunReceiptReconciliationResult> reconciliation;
    std::optional<nc::ops::CopyOperationRunReceiptPoolReleaseStatus> release;

    bool operator==(const CopyOperationRecoveryServiceResult &) const = default;
};

enum class CopyOperationRecoveryHistoryRefreshStatus : uint8_t {
    NotRequired,
    CoordinatorUnavailable,
    JournalUnavailable,
    Refreshed,
    Deferred
};

/**
 * App-core composition result: durable custody recovery stays authoritative; history refresh is
 * a separate cold value projection and never triggers a further recovery pass.
 */
struct CopyOperationRecoveryHistoryRefreshResult final {
    CopyOperationRecoveryServiceResult recovery;
    CopyOperationRecoveryHistoryRefreshStatus history_refresh{CopyOperationRecoveryHistoryRefreshStatus::NotRequired};
    std::optional<nc::ops::OperationCenterCoordinatorError> history_refresh_error;
};

/**
 * Process-owned boundary for cold Interrupted history and retained Copy run-receipt custody.
 *
 * Construction publishes an immutable projection of Copy entries observed as Interrupted by the startup
 * journal snapshot. It never resumes provider work. Service() performs at most one Retry, one same-directory
 * journal reopen, one Reconcile and one ReleaseReconciled, and only when called explicitly.
 */
class CopyOperationRecoveryCoordinator final
{
public:
    CopyOperationRecoveryCoordinator(std::shared_ptr<nc::ops::OperationJournal> _journal,
                                     std::shared_ptr<nc::ops::CopyOperationRunReceiptCustodian> _custodian,
                                     std::string _state_directory);

    CopyOperationRecoveryCoordinator(const CopyOperationRecoveryCoordinator &) = delete;
    CopyOperationRecoveryCoordinator &operator=(const CopyOperationRecoveryCoordinator &) = delete;

    [[nodiscard]] const std::vector<nc::ops::OperationJournalEntry> &StartupInterruptedHistory() const noexcept;
    [[nodiscard]] std::optional<size_t> PendingRecoveryCount() const noexcept;

    /** Runs one bounded recovery pass. Provider execution is never created or resumed here. */
    [[nodiscard]] CopyOperationRecoveryServiceResult Service(std::string_view _plan_id) noexcept;

    /** Returns a stable copy of the current process journal authority, or empty after reopen loss. */
    [[nodiscard]] std::shared_ptr<nc::ops::OperationJournal> CurrentJournal() const noexcept;
    [[nodiscard]] std::shared_ptr<nc::ops::CopyOperationRunReceiptCustodian>
    CurrentRunReceiptCustodian() const noexcept;

private:
    struct Services final {
        std::function<nc::ops::CopyOperationRunReceiptCustodyResult(std::string_view)> retry;
        std::function<std::expected<nc::ops::OperationJournal, nc::ops::OperationJournalError>(std::string_view)>
            reopen;
        std::function<nc::ops::CopyOperationRunReceiptReconciliationResult(std::string_view,
                                                                           const nc::ops::OperationJournal &)>
            reconcile;
        std::function<nc::ops::CopyOperationRunReceiptPoolReleaseStatus(std::string_view)> release_reconciled;
        std::function<size_t()> pending_count;
    };

    CopyOperationRecoveryCoordinator(std::shared_ptr<nc::ops::OperationJournal> _journal,
                                     std::shared_ptr<nc::ops::CopyOperationRunReceiptCustodian> _custodian,
                                     std::string _state_directory,
                                     Services _services);

    [[nodiscard]] static Services
    MakeProductionServices(const std::shared_ptr<nc::ops::CopyOperationRunReceiptCustodian> &_custodian);

    mutable std::mutex m_Mutex;
    std::shared_ptr<nc::ops::OperationJournal> m_Journal;
    std::shared_ptr<nc::ops::CopyOperationRunReceiptCustodian> m_Custodian;
    std::string m_StateDirectory;
    Services m_Services;
    std::vector<nc::ops::OperationJournalEntry> m_StartupInterruptedHistory;

    friend class CopyOperationRecoveryCoordinatorTesting;
};

/**
 * Runs one explicit custody service pass, then synchronously refreshes cold Operation Center
 * history from the post-reopen journal only after confirmed reconciliation and required Pool release.
 * Neither authority is retained beyond this call.
 */
[[nodiscard]] CopyOperationRecoveryHistoryRefreshResult
ServiceCopyRecoveryAndRefreshHistory(const std::shared_ptr<CopyOperationRecoveryCoordinator> &_recovery_coordinator,
                                     const std::shared_ptr<nc::ops::OperationCenterCoordinator> &_operation_center,
                                     std::string_view _plan_id) noexcept;

} // namespace nc::core
