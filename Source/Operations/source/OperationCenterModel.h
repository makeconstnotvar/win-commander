// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "OperationId.h"
#include "OperationPlan.h"

#include <Base/ScopedObservable.h>

#include <cstdint>
#include <functional>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nc::ops {

class OperationCenterCoordinator;
class OperationCenterModelTesting;
class OperationJournalAdmissionReceipt;

enum class OperationRecordState : uint8_t {
    Queued,
    Running,
    Paused,
    Cancelling,
    Finalizing,
    Interrupted,
    Cancelled,
    Failed,
    Completed,
    CompletedWithWarnings
};

struct OperationControlAvailability final {
    bool can_pause{false};
    bool can_resume{false};
    bool can_cancel{false};
    bool can_retry{false};

    bool operator==(const OperationControlAvailability &) const noexcept = default;
};

/** Value-only read record. It deliberately owns no executor, callback, view or Pool reference. */
struct OperationRecord final {
    OperationId operation_id;
    OperationPlanId plan_id;
    OperationPlanType operation_type;
    OperationRecordState state;
    uint64_t revision{0};
    OperationPlan::TimePoint created_at;
    std::optional<OperationPlan::TimePoint> started_at;
    std::optional<OperationPlan::TimePoint> finished_at;
    OperationControlAvailability controls;

    bool operator==(const OperationRecord &) const = default;
};

enum class OperationCenterModelErrorCode : uint8_t {
    IdExhausted,
    UnreservedOperationId,
    UnstagedAdmission,
    ReceiptPlanMismatch,
    HydrationAlreadyInitialized,
    InvalidHydratedRecord,
    DuplicateOperationId,
    ColdHistoryActive,
    ColdHistoryConflict,
    ColdHistoryRefreshFailed,
    OperationNotFound,
    StaleRevision,
    InvalidTransition
};

struct OperationCenterModelError final {
    OperationCenterModelErrorCode code;
    std::optional<uint64_t> current_revision;

    bool operator==(const OperationCenterModelError &) const = default;
};

/**
 * Thread-safe value model for active operation records and future durable history.
 *
 * A coordinator reserves an ID before durable admission, publishes it through Admit only after
 * durable and Pool admission succeed, and subsequently applies serialized lifecycle evidence.
 * The model has no authority to enqueue, stop, pause or retain an Operation. A later control port
 * resolves such authority separately and feeds its committed transitions back into this model.
 */
class OperationCenterModel final : private base::ScopedObservableBase
{
private:
    class Impl;

    /**
     * Fires model observers once, after the caller's lock guard has been released.
     *
     * Declared *before* the guard in each mutator so that reverse destruction order runs it after
     * the unlock. Observers are synchronous and may re-enter the model - a snapshot read is the
     * whole point of being notified - so firing under the lock would deadlock the first observer
     * that did the obvious thing.
     */
    class DeferredNotification final
    {
    public:
        explicit DeferredNotification(const OperationCenterModel &_model) noexcept : m_Model(&_model) {}
        DeferredNotification(const DeferredNotification &) = delete;
        DeferredNotification &operator=(const DeferredNotification &) = delete;
        ~DeferredNotification()
        {
            if( m_Armed )
                m_Model->FireObservers();
        }
        /** Call only once the mutation has actually succeeded. */
        void Arm() noexcept { m_Armed = true; }

    private:
        const OperationCenterModel *m_Model;
        bool m_Armed{false};
    };

public:
    /**
     * Legacy move-only model reservation, retained solely for isolated Model tests.
     *
     * Production IDs and durable admission belong to OperationJournal; only
     * OperationCenterModelTesting may obtain this token.
     */
    class Reservation final
    {
    public:
        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;
        Reservation(Reservation &&_other) noexcept;
        Reservation &operator=(Reservation &&_other) noexcept;
        ~Reservation();

        [[nodiscard]] OperationId Id() const noexcept { return m_OperationId; }

    private:
        Reservation(OperationId _operation_id, uint64_t _nonce, std::weak_ptr<Impl> _impl) noexcept;
        void Release() noexcept;

        OperationId m_OperationId;
        uint64_t m_Nonce;
        std::weak_ptr<Impl> m_Impl;
        bool m_Consumed{false};

        friend class OperationCenterModel;
        friend class OperationCenterModelTesting;
    };

    /**
     * Move-only unpublished record prepared before durable journal admission.
     * It has no externally visible operation ID; Publish binds it only to the exact journal receipt.
     */
    class AdmissionDraft final
    {
    public:
        AdmissionDraft(const AdmissionDraft &) = delete;
        AdmissionDraft &operator=(const AdmissionDraft &) = delete;
        AdmissionDraft(AdmissionDraft &&_other) noexcept;
        AdmissionDraft &operator=(AdmissionDraft &&_other) noexcept;
        ~AdmissionDraft();

    private:
        AdmissionDraft(OperationRecord _record, uint64_t _nonce, std::weak_ptr<Impl> _impl) noexcept;
        void Release() noexcept;

        OperationRecord m_Record;
        uint64_t m_Nonce;
        std::weak_ptr<Impl> m_Impl;
        bool m_Consumed{false};

        friend class OperationCenterModel;
    };

    OperationCenterModel();
    OperationCenterModel(const OperationCenterModel &) = delete;
    OperationCenterModel(OperationCenterModel &&) = delete;
    ~OperationCenterModel();

    OperationCenterModel &operator=(const OperationCenterModel &) = delete;
    OperationCenterModel &operator=(OperationCenterModel &&) = delete;

    /**
     * Applies one lifecycle state only when the caller still owns the exact record revision.
     * Terminal state is accepted only from Finalizing, preserving journal-before-release ordering.
     */
    [[nodiscard]] std::expected<OperationRecord, OperationCenterModelError>
    Transition(OperationId _operation_id,
               uint64_t _expected_revision,
               OperationRecordState _next_state,
               OperationPlan::TimePoint _observed_at);

    using ObservationTicket = ScopedObservableBase::ObservationTicket;

    /**
     * Observes every accepted change to the model: publication, lifecycle transition, startup
     * hydration and cold-history refresh. This is what makes a live Operation Center possible
     * without polling.
     *
     * The callback carries no payload by design. It is fired synchronously, after the model's
     * lock is released, and the consumer answers it with Snapshot() - so a consumer always
     * renders a self-consistent set of records and can never assemble a view from two different
     * generations. A rejected mutation fires nothing.
     *
     * Observers may be invoked on any thread, including the one that made the change.
     */
    [[nodiscard]] ObservationTicket ObserveChanges(std::function<void()> _callback);

    /** Immutable copies; callers never receive mutable storage or executor authority. */
    [[nodiscard]] std::optional<OperationRecord> Find(OperationId _operation_id) const;
    [[nodiscard]] std::vector<OperationRecord> Snapshot() const;

private:
    /** Test-only legacy foundation surface; production allocation belongs to OperationJournal. */
    [[nodiscard]] std::expected<Reservation, OperationCenterModelError> Reserve();
    [[nodiscard]] std::expected<OperationRecord, OperationCenterModelError>
    Admit(Reservation &&_reservation, const OperationPlan &_plan, OperationPlan::TimePoint _created_at);

    /** Coordinator-only prepare/commit path; Stage performs every allocation before journal admission. */
    [[nodiscard]] std::expected<AdmissionDraft, OperationCenterModelError>
    StageAdmission(const OperationPlan &_plan, OperationPlan::TimePoint _created_at);
    [[nodiscard]] std::expected<void, OperationCenterModelError>
    Publish(AdmissionDraft &&_draft, const OperationJournalAdmissionReceipt &_receipt);
    [[nodiscard]] std::expected<void, OperationCenterModelError> Hydrate(std::vector<OperationRecord> _records);
    [[nodiscard]] std::expected<void, OperationCenterModelError>
    RefreshColdHistory(std::vector<OperationRecord> _records);

    std::shared_ptr<Impl> m_Impl;

    friend class OperationCenterCoordinator;
    friend class OperationCenterModelTesting;
};

} // namespace nc::ops
