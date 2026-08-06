// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "OperationId.h"
#include "OperationPlan.h"
#include "OperationPlanCodec.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nc::ops {

class CopyOperationOrchestrator;
class OperationCenterCoordinator;

enum class OperationJournalState : uint8_t {
    Admitted,
    Running,
    Interrupted,
    Completed,
    Failed,
    Cancelled
};

enum class OperationJournalItemStatus : uint8_t {
    Succeeded,
    Failed,
    Cancelled,
    Skipped
};

enum class OperationJournalItemError : uint8_t {
    None,
    SourceChanged,
    DestinationChanged,
    PermissionDenied,
    Read,
    Write,
    Metadata,
    Commit,
    Cleanup,
    Cancelled,
    Unknown
};

enum class OperationJournalRecoveryAction : uint8_t {
    None,
    Retry,
    InspectDestination,
    RemoveTemporaryItem,
    RestoreSource
};

enum class OperationJournalFilesystemSyncStatus : uint8_t {
    NotAttempted,
    Confirmed,
    Failed
};

enum class OperationJournalPublicationState : uint8_t {
    NotPublished,
    Published,
    Unknown
};

struct OperationJournalItemResult final {
    size_t item_index{0};
    OperationJournalItemStatus status{OperationJournalItemStatus::Failed};
    OperationJournalItemError error{OperationJournalItemError::Unknown};
    int system_error{0};
    OperationJournalItemError prior_error{OperationJournalItemError::None};
    int prior_system_error{0};
    uint64_t bytes{0};
    OperationJournalPublicationState destination_publication{
        OperationJournalPublicationState::NotPublished};
    OperationJournalFilesystemSyncStatus filesystem_sync_status{
        OperationJournalFilesystemSyncStatus::NotAttempted};
    int filesystem_sync_system_error{0};
    OperationJournalRecoveryAction recovery_action{OperationJournalRecoveryAction::None};

    bool operator==(const OperationJournalItemResult &) const = default;
};

struct OperationJournalEntry final {
    OperationId operation_id;
    OperationPlan plan;
    OperationJournalState state;
    OperationPlan::TimePoint updated_at;
    std::vector<OperationJournalItemResult> item_results;

    bool operator==(const OperationJournalEntry &) const = default;
};

enum class OperationJournalErrorCode : uint8_t {
    InvalidParentPath,
    ParentOpenFailed,
    LockOpenFailed,
    JournalAlreadyOpen,
    JournalOpenFailed,
    JournalReadFailed,
    JournalTooLarge,
    MalformedJournal,
    UnsupportedSchemaVersion,
    CorruptJournal,
    DuplicateOperationId,
    DuplicatePlanId,
    PlanCodecFailed,
    PlanAlreadyAdmitted,
    PlanNotFound,
    InvalidTransition,
    InvalidAdmissionReceipt,
    AdmissionReceiptAlreadyConsumed,
    InvalidRunReceipt,
    RunReceiptAlreadyConsumed,
    InvalidItemResult,
    ResourceLimitExceeded,
    TemporaryCreateFailed,
    WriteFailed,
    FileSyncFailed,
    CloseFailed,
    RenameFailed,
    DurabilityUncertain,
    JournalUnusable
};

struct OperationJournalError final {
    OperationJournalErrorCode code;
    int system_error = 0;
    std::optional<OperationPlanCodecError> plan_codec_error;

    bool operator==(const OperationJournalError &) const = default;
};

/**
 * Proof that a plan was durably admitted to a journal.
 *
 * The receipt carries no execution, enqueue, provider, or mutation authority.
 */
class OperationJournalAdmissionReceipt final
{
public:
    OperationJournalAdmissionReceipt(const OperationJournalAdmissionReceipt &) = delete;
    OperationJournalAdmissionReceipt &operator=(const OperationJournalAdmissionReceipt &) = delete;
    OperationJournalAdmissionReceipt(OperationJournalAdmissionReceipt &&_other) noexcept;
    OperationJournalAdmissionReceipt &operator=(OperationJournalAdmissionReceipt &&) = delete;

    [[nodiscard]] std::string_view PlanId() const noexcept { return m_Plan.Id().Value(); }
    [[nodiscard]] const nc::ops::OperationId &OperationId() const noexcept { return m_OperationId; }

private:
    OperationJournalAdmissionReceipt(std::weak_ptr<const void> _journal_instance,
                                    nc::ops::OperationId _operation_id,
                                    OperationPlan _plan)
        : m_JournalInstance(std::move(_journal_instance)),
          m_OperationId(std::move(_operation_id)),
          m_Plan(std::move(_plan))
    {
    }

    std::weak_ptr<const void> m_JournalInstance;
    nc::ops::OperationId m_OperationId;
    OperationPlan m_Plan;
    bool m_Consumed{false};
    friend class OperationJournal;
    friend class CopyOperationOrchestrator;
    friend class OperationJournalTesting;
};

/**
 * Exact authority for publishing one running plan's durable terminal result.
 *
 * The receipt is journal- and plan-bound, move-only, and consumed only after a durable terminal snapshot.
 */
class OperationJournalRunReceipt final
{
public:
    OperationJournalRunReceipt(const OperationJournalRunReceipt &) = delete;
    OperationJournalRunReceipt &operator=(const OperationJournalRunReceipt &) = delete;
    OperationJournalRunReceipt(OperationJournalRunReceipt &&_other) noexcept;
    OperationJournalRunReceipt &operator=(OperationJournalRunReceipt &&) = delete;

    [[nodiscard]] std::string_view PlanId() const noexcept { return m_Plan.Id().Value(); }
    [[nodiscard]] const nc::ops::OperationId &OperationId() const noexcept { return m_OperationId; }

private:
    OperationJournalRunReceipt(std::weak_ptr<const void> _journal_instance,
                               nc::ops::OperationId _operation_id,
                               OperationPlan _plan)
        : m_JournalInstance(std::move(_journal_instance)),
          m_OperationId(std::move(_operation_id)),
          m_Plan(std::move(_plan))
    {
    }

    std::weak_ptr<const void> m_JournalInstance;
    nc::ops::OperationId m_OperationId;
    OperationPlan m_Plan;
    bool m_Consumed{false};
    friend class OperationJournal;
    friend class CopyOperationOrchestrator;
    friend class OperationJournalTesting;
    friend class CopyOperationRunReceiptCustodian;
};

/**
 * File-backed durable lifecycle ledger for immutable operation plans.
 *
 * Open() anchors all file access to an existing absolute directory. Every mutation writes and fsyncs a
 * complete candidate snapshot before atomically publishing it and fsyncing the parent directory. Startup
 * converts persisted Admitted/Running entries to Interrupted and never resumes execution.
 */
class OperationJournal final
{
private:
    struct Impl;

public:
    /**
     * Move-only reservation for one journal-owned operation ID.
     *
     * It is ephemeral process state only. A successful admission persists the journal high-water
     * mark together with the entry; destroying an unused reservation releases only its in-memory
     * exclusion and grants no mutation authority.
     */
    class AdmissionReservation final
    {
    public:
        AdmissionReservation(const AdmissionReservation &) = delete;
        AdmissionReservation &operator=(const AdmissionReservation &) = delete;
        AdmissionReservation(AdmissionReservation &&_other) noexcept;
        AdmissionReservation &operator=(AdmissionReservation &&_other) noexcept;
        ~AdmissionReservation();

        [[nodiscard]] const nc::ops::OperationId &Id() const noexcept { return m_OperationId; }

    private:
        AdmissionReservation(nc::ops::OperationId _operation_id, uint64_t _nonce, std::weak_ptr<Impl> _impl) noexcept;
        void Release() noexcept;

        nc::ops::OperationId m_OperationId;
        uint64_t m_Nonce;
        std::weak_ptr<Impl> m_Impl;
        bool m_Consumed{false};

        friend class OperationJournal;
        friend class OperationJournalTesting;
    };

    static constexpr uint32_t SchemaVersion = 3;
    static constexpr std::string_view Filename = "operation-journal-v1.json";
    static constexpr size_t MaxEntries = 10'000;
    static constexpr size_t MaxItemResults = 100'000;
    static constexpr size_t MaxJournalBytes = 128 * 1024 * 1024;

    OperationJournal() = delete;

    [[nodiscard]] static std::expected<OperationJournal, OperationJournalError>
    Open(std::string_view _absolute_existing_parent);

    /** Reserves one non-durable ID from the journal-owned allocator. */
    [[nodiscard]] std::expected<AdmissionReservation, OperationJournalError> ReserveOperationId();

    /** Compatibility path for existing orchestration; it reserves and consumes a journal ID atomically. */
    [[nodiscard]] std::expected<OperationJournalAdmissionReceipt, OperationJournalError>
    Admit(const OperationPlan &_plan);

    /**
     * Consumes the exact journal-issued reservation while atomically persisting the entry and
     * durable high-water mark. A process coordinator later binds this receipt to a model record.
     */
    [[nodiscard]] std::expected<OperationJournalAdmissionReceipt, OperationJournalError>
    Admit(AdmissionReservation &&_reservation, const OperationPlan &_plan);

    /** Consumes an exact admission receipt and returns exact authority for durable finalization. */
    [[nodiscard]] std::expected<OperationJournalRunReceipt, OperationJournalError>
    TransitionToRunning(OperationJournalAdmissionReceipt &&_receipt);

    /** Durably terminates an exact admitted plan before execution starts. */
    [[nodiscard]] std::expected<void, OperationJournalError>
    FinalizeAdmission(OperationJournalAdmissionReceipt &&_receipt, OperationJournalState _terminal_state);

    /**
     * Atomically persists one complete canonical terminal evidence snapshot and its matching terminal state.
     *
     * The supplied results must be strictly ordered by source index. Completed requires one valid result for every
     * source; Failed and Cancelled may carry an empty snapshot. This authority finalizes only a previously
     * evidence-empty running entry.
     */
    [[nodiscard]] std::expected<void, OperationJournalError>
    Finalize(OperationJournalRunReceipt &&_receipt,
             std::span<const OperationJournalItemResult> _results,
             OperationJournalState _terminal_state);

    /** Convenience overload for an owning terminal evidence snapshot. */
    [[nodiscard]] std::expected<void, OperationJournalError>
    Finalize(OperationJournalRunReceipt &&_receipt,
             const std::vector<OperationJournalItemResult> &_results,
             OperationJournalState _terminal_state);

    /** Compatibility wrapper for a one-item terminal evidence snapshot. */
    [[nodiscard]] std::expected<void, OperationJournalError>
    Finalize(OperationJournalRunReceipt &&_receipt,
             OperationJournalItemResult _result,
             OperationJournalState _terminal_state);

    [[nodiscard]] std::vector<OperationJournalEntry> Snapshot() const;

private:
    explicit OperationJournal(std::shared_ptr<Impl> _impl) : m_Impl(std::move(_impl)) {}

    /** Test-only compatibility surface for the superseded ID-addressed lifecycle API. */
    [[nodiscard]] std::expected<void, OperationJournalError>
    Transition(std::string_view _plan_id, OperationJournalState _state);
    [[nodiscard]] std::expected<void, OperationJournalError>
    RecordItemResult(std::string_view _plan_id, OperationJournalItemResult _result);
    [[nodiscard]] std::pair<uint64_t, uint64_t> StorageIdentityForCustody() const noexcept;
    [[nodiscard]] std::expected<OperationJournalAdmissionReceipt, OperationJournalError>
    AdmitLocked(AdmissionReservation &_reservation, const OperationPlan &_plan);
    [[nodiscard]] std::expected<OperationJournalAdmissionReceipt, OperationJournalError>
    AdmitWithOperationIdForTesting(OperationId _operation_id, const OperationPlan &_plan);
    [[nodiscard]] std::expected<void, OperationJournalError>
    ValidateAdmissionReceiptForOrchestration(const OperationJournalAdmissionReceipt &_receipt) const;

    std::shared_ptr<Impl> m_Impl;

    friend class CopyOperationRunReceiptCustodian;
    friend class CopyOperationOrchestrator;
    friend class OperationCenterCoordinator;
    friend class OperationJournalTesting;
};

} // namespace nc::ops
