// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CopyOperationExecutionProduct.h"
#include "OperationJournal.h"
#include "Pool.h"
#include "ReviewedOperationFactory.h"
#include "VFSOperationPlanningProbes.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nc::ops {

class CopyOperationOrchestratorTesting;
class OperationCenterCoordinator;

enum class CopyOperationExecutionFactoryError : uint8_t {
    Cancelled,
    Rejected
};

enum class CopyOperationRunReceiptRecoveryDisposition : uint8_t {
    RetryRequired,
    ReconcileRequired
};

enum class CopyOperationRunReceiptCustodyStatus : uint8_t {
    Finalized,
    RetryRequired,
    ReconcileRequired,
    ContractViolation,
    NotFound,
    Busy
};

struct CopyOperationRunReceiptCustodyResult final {
    CopyOperationRunReceiptCustodyStatus status;
    std::optional<OperationJournalError> journal_error;

    bool operator==(const CopyOperationRunReceiptCustodyResult &) const = default;
};

enum class CopyOperationRunReceiptReconciliationStatus : uint8_t {
    TerminalConfirmed,
    InterruptedConfirmed,
    Mismatch,
    RetryRequired,
    ContractViolation,
    NotFound,
    Busy
};

struct CopyOperationRunReceiptReconciliationResult final {
    CopyOperationRunReceiptReconciliationStatus status;
    bool pool_release_required{false};

    bool operator==(const CopyOperationRunReceiptReconciliationResult &) const = default;
};

enum class CopyOperationDurableTerminalConfirmation : uint8_t {
    Finalized,
    ReconciledTerminal,
    ReconciledInterrupted
};

/**
 * Exact durable outcome delivered after journal finalization or read-only reopen reconciliation.
 * The value owns all data required for safe dispatch to another executor.
 */
struct CopyOperationDurableTerminalOutcome final {
    std::string plan_id;
    OperationJournalState state;
    std::vector<OperationJournalItemResult> item_results;
    CopyOperationDurableTerminalConfirmation confirmation;

    /** Compatibility projection for consumers that can present exactly one source item. */
    [[nodiscard]] const OperationJournalItemResult *SingleItemResult() const noexcept
    {
        return item_results.size() == 1 ? &item_results.front() : nullptr;
    }

    bool operator==(const CopyOperationDurableTerminalOutcome &) const = default;
};

struct CopyOperationColdObservation final {
    uint64_t notification_mask;
    std::function<void()> callback;
};

/**
 * Private admission lock retained from the coordinator's pre-enqueue handoff until Pool accepts or
 * rejects the exact operation. It intentionally exposes neither the lock nor executor authority.
 */
class CopyOperationPreEnqueueLease final
{
public:
    CopyOperationPreEnqueueLease(const CopyOperationPreEnqueueLease &) = delete;
    CopyOperationPreEnqueueLease &operator=(const CopyOperationPreEnqueueLease &) = delete;
    CopyOperationPreEnqueueLease(CopyOperationPreEnqueueLease &&) noexcept = default;
    CopyOperationPreEnqueueLease &operator=(CopyOperationPreEnqueueLease &&) noexcept = default;

private:
    explicit CopyOperationPreEnqueueLease(std::unique_lock<std::recursive_mutex> _gate) noexcept
        : m_Gate{std::move(_gate)}
    {
    }

    std::unique_lock<std::recursive_mutex> m_Gate;

    friend class CopyOperationOrchestrator;
    friend class OperationCenterCoordinator;
};

struct CopyOperationSubmissionHooks final {
    /**
     * Transport lifecycle observers installed before Running and Pool admission.
     * Completion/finish observations are rejected; durable_terminal_observer is the terminal authority.
     */
    std::vector<CopyOperationColdObservation> cold_operation_observations;
    /** Installed on the cold operation before execution; invoked by its background Job thread. */
    ItemStateReportCallback item_status_observer;
    /**
     * Once Submit advances into run-receipt custody, runs at most once after exact durable terminal
     * confirmation and before Pool removal/success reporting. Earlier synchronous Submit failures are
     * reported only by Submit's typed error.
     * Delivery is synchronous on the submitting, Pool-finalizer, or recovery caller thread; UI consumers
     * dispatch the owning outcome to their executor. Observer exceptions are contained.
     */
    std::function<void(const CopyOperationDurableTerminalOutcome &)> durable_terminal_observer;
};

enum class CopyOperationRunReceiptPoolReleaseStatus : uint8_t {
    Released,
    Retained,
    InProgress,
    NotFound,
    Busy,
    ContractViolation
};

enum class CopyOperationRunReceiptCustodianError : uint8_t {
    DuplicatePlan,
    ResourceLimitExceeded,
    InvalidReservation
};

/**
 * Retains exact pre-enqueue run authority when terminal persistence cannot be confirmed.
 *
 * A slot is allocated and bound to the immutable plan before the Running transition. Runtime retry invokes
 * only OperationJournal::Finalize with the retained receipt and terminal evidence. A durability-uncertain
 * publication drops live journal authority and can only be reconciled against an independently reopened
 * journal snapshot.
 */
class CopyOperationRunReceiptCustodian final
{
public:
    static constexpr size_t MaxPendingSlots = OperationJournal::MaxEntries;

    CopyOperationRunReceiptCustodian();
    ~CopyOperationRunReceiptCustodian();

    CopyOperationRunReceiptCustodian(const CopyOperationRunReceiptCustodian &) = delete;
    CopyOperationRunReceiptCustodian &operator=(const CopyOperationRunReceiptCustodian &) = delete;

    [[nodiscard]] CopyOperationRunReceiptCustodyResult Retry(std::string_view _plan_id) noexcept;
    [[nodiscard]] CopyOperationRunReceiptReconciliationResult
    Reconcile(std::string_view _plan_id, const OperationJournal &_reopened_journal) noexcept;
    [[nodiscard]] CopyOperationRunReceiptPoolReleaseStatus ReleaseReconciled(std::string_view _plan_id) noexcept;
    [[nodiscard]] size_t PendingCount() const noexcept;

private:
    struct Slot;
    struct Impl;
    class Reservation final
    {
    public:
        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;
        Reservation(Reservation &&) noexcept = default;
        Reservation &operator=(Reservation &&) = delete;

    private:
        explicit Reservation(std::shared_ptr<Slot> _slot) : m_Slot{std::move(_slot)} {}
        std::shared_ptr<Slot> m_Slot;
        friend class CopyOperationRunReceiptCustodian;
        friend class CopyOperationOrchestrator;
        friend class CopyOperationRunReceiptCustodianTesting;
    };

    [[nodiscard]] std::expected<Reservation, CopyOperationRunReceiptCustodianError>
    Reserve(OperationPlan _plan,
            std::shared_ptr<OperationJournal> _journal,
            CopyOperationExecutionProduct::TerminalEvidenceAccessor _accessor,
            std::function<void(const CopyOperationDurableTerminalOutcome &)> _terminal_observer);
    [[nodiscard]] bool Arm(Reservation &_reservation, OperationJournalRunReceipt &&_receipt) noexcept;
    void Release(Reservation &_reservation) noexcept;
    [[nodiscard]] CopyOperationRunReceiptCustodyResult
    FinalizeBeforeEnqueue(Reservation &_reservation,
                          OperationJournalItemResult _result,
                          OperationJournalState _terminal_state) noexcept;
    [[nodiscard]] bool BeginEnqueue(Reservation &_reservation,
                                    const std::shared_ptr<Pool> &_pool,
                                    const std::shared_ptr<Operation> &_operation) noexcept;
    void CommitEnqueue(Reservation &_reservation) noexcept;
    [[nodiscard]] CopyOperationRunReceiptCustodyResult RejectEnqueue(Reservation &_reservation,
                                                                     OperationJournalItemResult _result,
                                                                     OperationJournalState _terminal_state) noexcept;
    [[nodiscard]] static PoolTerminalFinalizationDecision FinalizePoolSlot(const std::shared_ptr<Slot> &_slot) noexcept;
    static void DeliverTerminalOutcome(const std::shared_ptr<Slot> &_slot,
                                       CopyOperationDurableTerminalConfirmation _confirmation) noexcept;
    [[nodiscard]] static PoolTerminalFinalizationDecision ReleaseDecision(const std::shared_ptr<Slot> &_slot) noexcept;
    [[nodiscard]] static CopyOperationRunReceiptCustodyResult
    FinalizeCustodiedSlot(const std::shared_ptr<Slot> &_slot) noexcept;

    std::shared_ptr<Impl> m_Impl;

    friend class CopyOperationOrchestrator;
    friend class CopyOperationRunReceiptCustodianTesting;
};

enum class CopyOperationOrchestratorErrorCode : uint8_t {
    MissingJournal,
    MissingPool,
    MissingExecutionFactory,
    MissingRunReceiptCustodian,
    UnsupportedReviewedPlan,
    Cancelled,
    JournalAdmissionFailed,
    InvalidJournalAdmissionReceipt,
    ExecutionFactoryFailed,
    InvalidExecutionProduct,
    OperationConfigurationFailed,
    AdmissionFinalizationFailed,
    RunReceiptReservationFailed,
    RunningTransitionFailed,
    RunReceiptArmFailed,
    PreEnqueuePreparationFailed,
    RunningFinalizationFailed,
    EnqueueRejected
};

struct CopyOperationOrchestratorError final {
    CopyOperationOrchestratorErrorCode code;
    std::optional<OperationJournalError> journal_error;
    std::optional<CopyOperationExecutionFactoryError> factory_error;
    std::optional<PoolEnqueueResult> enqueue_result;
    std::optional<CopyOperationRunReceiptRecoveryDisposition> recovery_disposition;
    std::optional<ReviewedOperationFactoryError> reviewed_factory_error;

    bool operator==(const CopyOperationOrchestratorError &) const = default;
};

/**
 * Composes one reviewed single-item Copy through durable admission, typed construction and Pool.
 * Production construction is available only through the private reviewed factory execution-product authority.
 */
class CopyOperationOrchestrator final
{
public:
    using CancelChecker = std::function<bool()>;

    CopyOperationOrchestrator(std::shared_ptr<OperationJournal> _journal,
                              std::shared_ptr<Pool> _pool,
                              std::shared_ptr<CopyOperationRunReceiptCustodian> _run_receipt_custodian);

    [[nodiscard]] std::expected<std::shared_ptr<Operation>, CopyOperationOrchestratorError>
    Submit(ReviewedVFSOperationPreflight _reviewed,
           CancelChecker _cancel_checker = {},
           CopyOperationSubmissionHooks _hooks = {});

private:
    using ExecutionFactory =
        std::function<std::expected<CopyOperationExecutionProduct,
                                    CopyOperationExecutionFactoryError>(ReviewedVFSOperationPreflight, CancelChecker)>;
    using PreEnqueueHandoff = std::function<CopyOperationPreEnqueueLease(const std::shared_ptr<Pool> &,
                                                                           const std::shared_ptr<Operation> &)>;
    using ConditionalCommitTransactionResolver =
        std::function<std::expected<std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction>,
                                    nc::vfs::ProviderConditionalCopyTransactionBeginError>(
            nc::vfs::ProviderConditionalCopyReviewedAuthority,
            const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &)>;

    CopyOperationOrchestrator(std::shared_ptr<OperationJournal> _journal,
                              std::shared_ptr<Pool> _pool,
                              ExecutionFactory _execution_factory,
                              std::shared_ptr<CopyOperationRunReceiptCustodian> _run_receipt_custodian);

    [[nodiscard]] std::expected<std::shared_ptr<Operation>, CopyOperationOrchestratorError>
    SubmitAdmitted(ReviewedVFSOperationPreflight _reviewed,
                   OperationJournalAdmissionReceipt _admission,
                   CancelChecker _cancel_checker,
                   CopyOperationSubmissionHooks _hooks,
                   PreEnqueueHandoff _pre_enqueue_handoff = {});

    std::shared_ptr<OperationJournal> m_Journal;
    std::shared_ptr<Pool> m_Pool;
    ExecutionFactory m_ExecutionFactory;
    ConditionalCommitTransactionResolver m_ConditionalCommitTransactionResolver;
    bool m_UseInjectedExecutionFactory{false};
    std::shared_ptr<CopyOperationRunReceiptCustodian> m_RunReceiptCustodian;

    friend class CopyOperationOrchestratorTesting;
    friend class OperationCenterCoordinator;
};

} // namespace nc::ops
