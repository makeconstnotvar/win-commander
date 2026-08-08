// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CopyOperationOrchestrator.h"
#include "OperationCenterModel.h"
#include "OperationJournal.h"

#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace nc::ops {

enum class OperationCenterCoordinatorErrorCode : uint8_t {
    ActiveJournalEntry,
    DuplicateOperationId,
    ModelHydrationFailed,
    JournalReservationFailed,
    ModelStagingFailed,
    ResidencyStagingFailed,
    JournalAdmissionFailed,
    ModelPublicationFailed,
    AdmissionFinalizationFailed,
    JournalStorageMismatch,
    ColdHistoryBusy,
    ColdHistoryRefreshFailed
};

struct OperationCenterCoordinatorError final {
    OperationCenterCoordinatorErrorCode code;
    std::optional<OperationId> operation_id;
    std::optional<OperationCenterModelError> model_error;
    std::optional<OperationJournalError> journal_error;

    bool operator==(const OperationCenterCoordinatorError &) const = default;
};

enum class OperationCenterSubmissionErrorCode : uint8_t {
    HookPreparationFailed,
    AdmissionStagingFailed,
    AdmissionCommitFailed,
    OrchestratorRejected
};

struct OperationCenterSubmissionError final {
    OperationCenterSubmissionErrorCode code;
    std::optional<OperationCenterCoordinatorError> coordinator_error;
    std::optional<CopyOperationOrchestratorError> orchestrator_error;

    bool operator==(const OperationCenterSubmissionError &) const = default;
};

enum class OperationCenterCancelResultCode : uint8_t {
    Accepted,
    OperationNotFound,
    StaleRevision,
    CancelUnavailable,
    ResidencyUnavailable,
    CancelInProgress,
    StopRejected
};

/** Value-only result for a revalidated cancel request; it exposes no executor reference. */
struct OperationCenterCancelResult final {
    OperationCenterCancelResultCode code;
    std::optional<OperationRecord> current_record;

    bool operator==(const OperationCenterCancelResult &) const = default;
};

/**
 * Value-only startup projection from a durable journal into OperationCenterModel.
 *
 * It intentionally retains neither the journal nor an executor. The caller owns journal lifetime and
 * recovery/reopen policy; this seam imports only the post-Open terminal/interrupted history and seeds
 * the model above its exact persisted IDs. It also owns the preallocated model-draft to durable-admission
 * join. For a submitted live operation, it owns a private exact Pool/executor residency solely
 * to revalidate engine control requests; journal recovery and UI remain separate authorities.
 */
class OperationCenterCoordinator final : public std::enable_shared_from_this<OperationCenterCoordinator>
{
    struct LiveResidency;
    struct LiveResidencyBinding;

public:
    /**
     * Move-only preallocated live-residency slot. It is created before durable admission and can be
     * consumed only by the coordinator's private pre-enqueue handoff.
     */
    class LiveResidencyDraft final
    {
    public:
        LiveResidencyDraft(const LiveResidencyDraft &) = delete;
        LiveResidencyDraft &operator=(const LiveResidencyDraft &) = delete;
        LiveResidencyDraft(LiveResidencyDraft &&_other) noexcept;
        LiveResidencyDraft &operator=(LiveResidencyDraft &&_other) noexcept;
        ~LiveResidencyDraft();

    private:
        LiveResidencyDraft(std::shared_ptr<LiveResidency> _residency,
                           std::weak_ptr<OperationCenterCoordinator> _coordinator) noexcept;
        void Release() noexcept;

        std::shared_ptr<LiveResidency> m_Residency;
        std::weak_ptr<OperationCenterCoordinator> m_Coordinator;
        bool m_Consumed{false};

        friend class OperationCenterCoordinator;
    };

    /**
     * Move-only, unpublished join between a prepared model record and one journal-owned ID
     * reservation. Destruction leaves neither a model record nor a durable journal entry.
     */
    class AdmissionStaging final
    {
    public:
        AdmissionStaging(const AdmissionStaging &) = delete;
        AdmissionStaging &operator=(const AdmissionStaging &) = delete;
        AdmissionStaging(AdmissionStaging &&) noexcept = default;
        AdmissionStaging &operator=(AdmissionStaging &&) noexcept = default;

    private:
        AdmissionStaging(OperationJournal::AdmissionReservation _journal_reservation,
                         OperationCenterModel::AdmissionDraft _model_draft,
                         LiveResidencyDraft _residency_draft,
                         OperationPlan _plan) noexcept;

        OperationJournal::AdmissionReservation m_JournalReservation;
        OperationCenterModel::AdmissionDraft m_ModelDraft;
        LiveResidencyDraft m_ResidencyDraft;
        OperationPlan m_Plan;

        friend class OperationCenterCoordinator;
    };

    /** Exact durable receipt and value-model record produced by a committed staging token. */
    struct CommittedAdmission final {
        OperationId operation_id;
        OperationJournalAdmissionReceipt journal_receipt;

    private:
        CommittedAdmission(OperationId _operation_id,
                           OperationJournalAdmissionReceipt _journal_receipt,
                           LiveResidencyDraft _residency_draft) noexcept
            : operation_id{_operation_id},
              journal_receipt{std::move(_journal_receipt)},
              m_ResidencyDraft{std::move(_residency_draft)}
        {
        }

        LiveResidencyDraft m_ResidencyDraft;

        friend class OperationCenterCoordinator;
    };

    [[nodiscard]] static std::expected<std::shared_ptr<OperationCenterCoordinator>, OperationCenterCoordinatorError>
    Create(const OperationJournal &_journal);

    OperationCenterCoordinator(const OperationCenterCoordinator &) = delete;
    OperationCenterCoordinator &operator=(const OperationCenterCoordinator &) = delete;

    [[nodiscard]] const OperationCenterModel &Model() const noexcept { return m_Model; }

    /**
     * Observes every accepted model change, so a consumer can stay live instead of re-reading on
     * open. Forwarded here rather than taken from Model() because observation registers with the
     * model and Model() is deliberately a const read handle.
     *
     * The callback may arrive on any thread - Pool threads publish terminal outcomes - so a UI
     * consumer must hop to its own queue before touching views. It carries no payload; answer it
     * with Model().Snapshot().
     */
    [[nodiscard]] OperationCenterModel::ObservationTicket ObserveChanges(std::function<void()> _callback)
    {
        return m_Model.ObserveChanges(std::move(_callback));
    }

    /**
     * Value-only storage identity captured during cold-history hydration. It binds an app-owned deferred
     * history-projection continuation without exposing or retaining the journal authority.
     */
    [[nodiscard]] std::pair<uint64_t, uint64_t> HistoryProjectionStorageIdentity() const noexcept
    {
        return m_JournalStorageIdentity;
    }

    /** Preallocates the model draft, then reserves one durable journal ID; neither is visible yet. */
    [[nodiscard]] std::expected<AdmissionStaging, OperationCenterCoordinatorError>
    StageAdmission(OperationJournal &_journal, const OperationPlan &_plan);

    /** Durably admits then publishes `Queued`; it never enqueues and retains no executor until pre-enqueue handoff. */
    [[nodiscard]] std::expected<CommittedAdmission, OperationCenterCoordinatorError>
    CommitAdmission(OperationJournal &_journal, AdmissionStaging &&_staging);

    /**
     * The only production join from receipt-bound model admission to the private Orchestrator handoff.
     * Callbacks hold only a weak coordinator and reduce durable state into the value model before
     * forwarding application hooks. Before Pool admission it installs one private strong residency
     * for exact engine control; no raw executor leaves this boundary.
     */
    [[nodiscard]] std::expected<std::shared_ptr<Operation>, OperationCenterSubmissionError>
    SubmitReviewedCopy(OperationJournal &_journal,
                       CopyOperationOrchestrator &_orchestrator,
                       ReviewedVFSOperationPreflight _reviewed,
                       std::function<bool()> _cancel_checker,
                       CopyOperationSubmissionHooks _hooks);

    /**
     * Revalidates an exact model revision, retains its private Pool residency, releases coordinator
     * locks, and submits a stop request. It is the engine control port; callers receive no raw
     * executor reference.
     */
    [[nodiscard]] OperationCenterCancelResult Cancel(OperationId _operation_id,
                                                      uint64_t _expected_revision) noexcept;

    /**
     * Imports only absent terminal/Interrupted history from an exact reopened journal storage.
     * The coordinator must be cold: active model records, staged admissions and live residencies
     * fail closed without changing the model or retaining the journal.
     */
    [[nodiscard]] std::expected<void, OperationCenterCoordinatorError>
    RefreshColdHistory(const OperationJournal &_journal);

private:
    OperationCenterCoordinator() = default;

    void ReduceStarted(OperationId _operation_id) noexcept;
    void ReduceDurableTerminal(OperationId _operation_id, OperationJournalState _state) noexcept;
    void ReduceDurableTerminalFromJournal(OperationId _operation_id, const OperationJournal &_journal) noexcept;
    void ReduceCancelling(OperationId _operation_id) noexcept;
    void ApplyDurableTerminal(OperationId _operation_id, OperationJournalState _state) noexcept;
    void ReleaseLiveResidencyDraft() noexcept;
    void RegisterLiveResidency(LiveResidencyDraft &&_draft,
                               OperationId _operation_id,
                               const std::shared_ptr<Pool> &_pool,
                               const std::shared_ptr<Operation> &_operation);
    [[nodiscard]] std::shared_ptr<LiveResidency> FindLiveResidency(OperationId _operation_id) const noexcept;
    void RetireLiveResidency(OperationId _operation_id, const std::shared_ptr<LiveResidency> &_residency) noexcept;

    OperationCenterModel m_Model;
    std::mutex m_AdmissionLock;
    std::mutex m_ColdHistoryGate;
    std::mutex m_ReductionLock;
    mutable std::mutex m_ResidencyLock;
    std::vector<LiveResidencyBinding> m_LiveResidencies;
    size_t m_StagedLiveResidencies{0};
    std::pair<uint64_t, uint64_t> m_JournalStorageIdentity{};
};

} // namespace nc::ops
