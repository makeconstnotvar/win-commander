// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "OperationCenterModel.h"
#include "OperationCenterModelTesting.h"
#include "OperationJournal.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace nc::ops {
namespace {

bool IsTerminal(const OperationRecordState _state) noexcept
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

bool IsAllowedTransition(const OperationRecordState _current, const OperationRecordState _next) noexcept
{
    switch( _current ) {
        case OperationRecordState::Queued:
            return _next == OperationRecordState::Running || _next == OperationRecordState::Cancelling ||
                   _next == OperationRecordState::Finalizing;
        case OperationRecordState::Running:
            return _next == OperationRecordState::Paused || _next == OperationRecordState::Cancelling ||
                   _next == OperationRecordState::Finalizing;
        case OperationRecordState::Paused:
            return _next == OperationRecordState::Running || _next == OperationRecordState::Cancelling ||
                   _next == OperationRecordState::Finalizing;
        case OperationRecordState::Cancelling:
            return _next == OperationRecordState::Finalizing;
        case OperationRecordState::Finalizing:
            return IsTerminal(_next);
        case OperationRecordState::Interrupted:
        case OperationRecordState::Cancelled:
        case OperationRecordState::Failed:
        case OperationRecordState::Completed:
        case OperationRecordState::CompletedWithWarnings:
            return false;
    }
    return false;
}

OperationControlAvailability ControlsFor(const OperationRecordState _state) noexcept
{
    switch( _state ) {
        case OperationRecordState::Queued:
            return {.can_pause = false, .can_resume = false, .can_cancel = true, .can_retry = false};
        case OperationRecordState::Running:
            return {.can_pause = true, .can_resume = false, .can_cancel = true, .can_retry = false};
        case OperationRecordState::Paused:
            return {.can_pause = false, .can_resume = true, .can_cancel = true, .can_retry = false};
        case OperationRecordState::Failed:
        case OperationRecordState::Interrupted:
            return {.can_pause = false, .can_resume = false, .can_cancel = false, .can_retry = true};
        case OperationRecordState::Cancelling:
        case OperationRecordState::Finalizing:
        case OperationRecordState::Cancelled:
        case OperationRecordState::Completed:
        case OperationRecordState::CompletedWithWarnings:
            return {};
    }
    return {};
}

} // namespace

class OperationCenterModel::Impl final
{
public:
    mutable std::mutex lock;
    uint64_t next_sequence{1};
    uint64_t next_reservation_nonce{1};
    std::unordered_map<uint64_t, uint64_t> reservations;
    uint64_t next_admission_draft_nonce{1};
    std::unordered_set<uint64_t> admission_drafts;
    std::vector<OperationRecord> records;
};

OperationCenterModel::Reservation::Reservation(OperationId _operation_id,
                                                const uint64_t _nonce,
                                                std::weak_ptr<Impl> _impl) noexcept
    : m_OperationId(std::move(_operation_id)), m_Nonce(_nonce), m_Impl(std::move(_impl))
{
}

OperationCenterModel::Reservation::Reservation(Reservation &&_other) noexcept
    : m_OperationId(std::move(_other.m_OperationId)),
      m_Nonce(_other.m_Nonce),
      m_Impl(std::move(_other.m_Impl)),
      m_Consumed(_other.m_Consumed)
{
    _other.m_Consumed = true;
}

OperationCenterModel::Reservation &OperationCenterModel::Reservation::operator=(Reservation &&_other) noexcept
{
    if( this == &_other )
        return *this;

    Release();
    m_OperationId = std::move(_other.m_OperationId);
    m_Nonce = _other.m_Nonce;
    m_Impl = std::move(_other.m_Impl);
    m_Consumed = _other.m_Consumed;
    _other.m_Consumed = true;
    return *this;
}

OperationCenterModel::Reservation::~Reservation()
{
    Release();
}

void OperationCenterModel::Reservation::Release() noexcept
{
    if( m_Consumed )
        return;

    if( const auto impl = m_Impl.lock() ) {
        const auto guard = std::lock_guard{impl->lock};
        const auto found = impl->reservations.find(m_OperationId.m_Sequence);
        if( found != impl->reservations.end() && found->second == m_Nonce )
            impl->reservations.erase(found);
    }
    m_Consumed = true;
    m_Impl.reset();
}

OperationCenterModel::AdmissionDraft::AdmissionDraft(OperationRecord _record,
                                                      const uint64_t _nonce,
                                                      std::weak_ptr<Impl> _impl) noexcept
    : m_Record{std::move(_record)}, m_Nonce{_nonce}, m_Impl{std::move(_impl)}
{
}

OperationCenterModel::AdmissionDraft::AdmissionDraft(AdmissionDraft &&_other) noexcept
    : m_Record{std::move(_other.m_Record)},
      m_Nonce{_other.m_Nonce},
      m_Impl{std::move(_other.m_Impl)},
      m_Consumed{std::exchange(_other.m_Consumed, true)}
{
}

OperationCenterModel::AdmissionDraft &OperationCenterModel::AdmissionDraft::operator=(AdmissionDraft &&_other) noexcept
{
    if( this == &_other )
        return *this;
    Release();
    m_Record = std::move(_other.m_Record);
    m_Nonce = _other.m_Nonce;
    m_Impl = std::move(_other.m_Impl);
    m_Consumed = std::exchange(_other.m_Consumed, true);
    return *this;
}

OperationCenterModel::AdmissionDraft::~AdmissionDraft()
{
    Release();
}

void OperationCenterModel::AdmissionDraft::Release() noexcept
{
    if( m_Consumed )
        return;
    if( const auto impl = m_Impl.lock() ) {
        const auto guard = std::lock_guard{impl->lock};
        impl->admission_drafts.erase(m_Nonce);
    }
    m_Consumed = true;
    m_Impl.reset();
}

OperationCenterModel::OperationCenterModel() : m_Impl(std::make_shared<Impl>()) {}

OperationCenterModel::~OperationCenterModel() = default;

OperationCenterModel::ObservationTicket OperationCenterModel::ObserveChanges(std::function<void()> _callback)
{
    return AddTicketedObserver(std::move(_callback));
}

std::expected<OperationCenterModel::Reservation, OperationCenterModelError> OperationCenterModel::Reserve()
{
    const auto guard = std::lock_guard{m_Impl->lock};
    if( m_Impl->next_sequence == 0 || m_Impl->next_sequence == std::numeric_limits<uint64_t>::max() ||
        m_Impl->next_reservation_nonce == 0 )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::IdExhausted});

    const auto sequence = m_Impl->next_sequence++;
    const auto nonce = m_Impl->next_reservation_nonce++;
    m_Impl->reservations.emplace(sequence, nonce);
    return Reservation{OperationId{sequence}, nonce, m_Impl};
}

std::expected<OperationRecord, OperationCenterModelError>
OperationCenterModel::Admit(Reservation &&_reservation,
                            const OperationPlan &_plan,
                            const OperationPlan::TimePoint _created_at)
{
    if( _reservation.m_Consumed )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::UnreservedOperationId});
    const auto reservation_impl = _reservation.m_Impl.lock();
    if( !reservation_impl || reservation_impl.get() != m_Impl.get() )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::UnreservedOperationId});

    DeferredNotification notify{*this};
    const auto guard = std::lock_guard{m_Impl->lock};
    const auto reservation = m_Impl->reservations.find(_reservation.m_OperationId.m_Sequence);
    if( reservation == m_Impl->reservations.end() || reservation->second != _reservation.m_Nonce )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::UnreservedOperationId});

    OperationRecord record{
        .operation_id = _reservation.m_OperationId,
        .plan_id = _plan.Id(),
        .operation_type = _plan.Type(),
        .state = OperationRecordState::Queued,
        .revision = 1,
        .created_at = _created_at,
        .started_at = std::nullopt,
        .finished_at = std::nullopt,
        .controls = ControlsFor(OperationRecordState::Queued),
    };
    m_Impl->records.emplace_back(record);
    m_Impl->reservations.erase(reservation);
    _reservation.m_Consumed = true;
    _reservation.m_Impl.reset();
    notify.Arm();
    return record;
}

std::expected<OperationCenterModel::AdmissionDraft, OperationCenterModelError>
OperationCenterModel::StageAdmission(const OperationPlan &_plan, const OperationPlan::TimePoint _created_at)
{
    static_assert(std::is_nothrow_move_constructible_v<OperationRecord>);
    static_assert(std::is_nothrow_move_assignable_v<OperationRecord>);
    OperationRecord record{
        .operation_id = OperationId{1},
        .plan_id = _plan.Id(),
        .operation_type = _plan.Type(),
        .state = OperationRecordState::Queued,
        .revision = 1,
        .created_at = _created_at,
        .started_at = std::nullopt,
        .finished_at = std::nullopt,
        .controls = ControlsFor(OperationRecordState::Queued),
    };
    const auto guard = std::lock_guard{m_Impl->lock};
    if( m_Impl->next_admission_draft_nonce == 0 )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::IdExhausted});
    // Every outstanding draft needs its own post-journal publication slot. Reserve all of them
    // while allocation is still reversible, so concurrently staged admissions cannot strand a
    // later durable receipt behind a full vector.
    m_Impl->records.reserve(m_Impl->records.size() + m_Impl->admission_drafts.size() + 1);
    const auto nonce = m_Impl->next_admission_draft_nonce++;
    const auto [_, inserted] = m_Impl->admission_drafts.emplace(nonce);
    if( !inserted )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::UnstagedAdmission});
    return AdmissionDraft{std::move(record), nonce, m_Impl};
}

std::expected<void, OperationCenterModelError>
OperationCenterModel::Publish(AdmissionDraft &&_draft, const OperationJournalAdmissionReceipt &_receipt)
{
    if( _draft.m_Consumed )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::UnstagedAdmission});
    const auto draft_impl = _draft.m_Impl.lock();
    if( !draft_impl || draft_impl.get() != m_Impl.get() )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::UnstagedAdmission});

    DeferredNotification notify{*this};
    const auto guard = std::lock_guard{m_Impl->lock};
    if( !m_Impl->admission_drafts.contains(_draft.m_Nonce) )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::UnstagedAdmission});
    if( _draft.m_Record.plan_id.Value() != _receipt.PlanId() )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::ReceiptPlanMismatch});
    if( std::ranges::any_of(m_Impl->records,
                            [&](const OperationRecord &_record) { return _record.operation_id == _receipt.OperationId(); }) )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::DuplicateOperationId});
    if( m_Impl->records.size() == m_Impl->records.capacity() )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::UnstagedAdmission});

    _draft.m_Record.operation_id = _receipt.OperationId();
    m_Impl->records.emplace_back(std::move(_draft.m_Record));
    m_Impl->admission_drafts.erase(_draft.m_Nonce);
    _draft.m_Consumed = true;
    _draft.m_Impl.reset();
    notify.Arm();
    return {};
}

std::expected<OperationRecord, OperationCenterModelError>
OperationCenterModel::Transition(const OperationId _operation_id,
                                 const uint64_t _expected_revision,
                                 const OperationRecordState _next_state,
                                 const OperationPlan::TimePoint _observed_at)
{
    DeferredNotification notify{*this};
    const auto guard = std::lock_guard{m_Impl->lock};
    const auto found = std::ranges::find_if(m_Impl->records, [&](const OperationRecord &_record) {
        return _record.operation_id == _operation_id;
    });
    if( found == m_Impl->records.end() )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::OperationNotFound});
    if( found->revision != _expected_revision ) {
        return std::unexpected(OperationCenterModelError{
            .code = OperationCenterModelErrorCode::StaleRevision,
            .current_revision = found->revision,
        });
    }
    if( !IsAllowedTransition(found->state, _next_state) )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::InvalidTransition});

    found->state = _next_state;
    ++found->revision;
    found->controls = ControlsFor(_next_state);
    if( _next_state == OperationRecordState::Running && !found->started_at )
        found->started_at = _observed_at;
    if( IsTerminal(_next_state) )
        found->finished_at = _observed_at;
    notify.Arm();
    return *found;
}

std::optional<OperationRecord> OperationCenterModel::Find(const OperationId _operation_id) const
{
    const auto guard = std::lock_guard{m_Impl->lock};
    const auto found = std::ranges::find_if(m_Impl->records, [&](const OperationRecord &_record) {
        return _record.operation_id == _operation_id;
    });
    if( found == m_Impl->records.end() )
        return std::nullopt;
    return *found;
}

std::vector<OperationRecord> OperationCenterModel::Snapshot() const
{
    const auto guard = std::lock_guard{m_Impl->lock};
    return m_Impl->records;
}

std::expected<void, OperationCenterModelError> OperationCenterModel::Hydrate(std::vector<OperationRecord> _records)
{
    DeferredNotification notify{*this};
    const auto guard = std::lock_guard{m_Impl->lock};
    if( !m_Impl->records.empty() || !m_Impl->reservations.empty() || !m_Impl->admission_drafts.empty() ||
        m_Impl->next_sequence != 1 || m_Impl->next_admission_draft_nonce != 1 )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::HydrationAlreadyInitialized});

    uint64_t maximum_sequence = 0;
    std::unordered_set<uint64_t> operation_sequences;
    operation_sequences.reserve(_records.size());
    for( auto &record : _records ) {
        if( !IsTerminal(record.state) || record.revision != 1 || !record.finished_at || record.started_at )
            return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::InvalidHydratedRecord});
        if( !operation_sequences.emplace(record.operation_id.m_Sequence).second )
            return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::InvalidHydratedRecord});
        maximum_sequence = std::max(maximum_sequence, record.operation_id.m_Sequence);
        record.controls = ControlsFor(record.state);
    }
    if( maximum_sequence == std::numeric_limits<uint64_t>::max() )
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::IdExhausted});

    m_Impl->next_sequence = maximum_sequence + 1;
    m_Impl->records = std::move(_records);
    notify.Arm();
    return {};
}

std::expected<void, OperationCenterModelError>
OperationCenterModel::RefreshColdHistory(std::vector<OperationRecord> _records)
{
    try {
        DeferredNotification notify{*this};
        const auto guard = std::lock_guard{m_Impl->lock};
        if( !m_Impl->reservations.empty() || !m_Impl->admission_drafts.empty() ||
            std::ranges::any_of(m_Impl->records, [](const OperationRecord &_record) {
                return !IsTerminal(_record.state);
            }) ) {
            return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::ColdHistoryActive});
        }

        uint64_t maximum_sequence = 0;
        std::unordered_set<uint64_t> operation_sequences;
        operation_sequences.reserve(_records.size());
        std::vector<OperationRecord> additions;
        additions.reserve(_records.size());
        for( auto &record : _records ) {
            if( !IsTerminal(record.state) || record.revision != 1 || !record.finished_at || record.started_at ) {
                return std::unexpected(
                    OperationCenterModelError{.code = OperationCenterModelErrorCode::InvalidHydratedRecord});
            }
            if( !operation_sequences.emplace(record.operation_id.m_Sequence).second ) {
                return std::unexpected(
                    OperationCenterModelError{.code = OperationCenterModelErrorCode::InvalidHydratedRecord});
            }

            maximum_sequence = std::max(maximum_sequence, record.operation_id.m_Sequence);
            record.controls = ControlsFor(record.state);
            const auto current = std::ranges::find_if(m_Impl->records, [&](const OperationRecord &_current) {
                return _current.operation_id == record.operation_id;
            });
            if( current == m_Impl->records.end() ) {
                additions.emplace_back(std::move(record));
                continue;
            }
            if( current->plan_id != record.plan_id || current->operation_type != record.operation_type ||
                current->state != record.state ) {
                return std::unexpected(
                    OperationCenterModelError{.code = OperationCenterModelErrorCode::ColdHistoryConflict});
            }
        }

        if( maximum_sequence == std::numeric_limits<uint64_t>::max() )
            return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::IdExhausted});
        if( additions.empty() )
            return {};

        std::vector<OperationRecord> candidate = m_Impl->records;
        candidate.reserve(candidate.size() + additions.size());
        for( auto &record : additions )
            candidate.emplace_back(std::move(record));

        m_Impl->records.swap(candidate);
        m_Impl->next_sequence = std::max(m_Impl->next_sequence, maximum_sequence + 1);
        notify.Arm();
        return {};
    } catch( ... ) {
        return std::unexpected(OperationCenterModelError{.code = OperationCenterModelErrorCode::ColdHistoryRefreshFailed});
    }
}

std::expected<OperationCenterModel::Reservation, OperationCenterModelError>
OperationCenterModelTesting::Reserve(OperationCenterModel &_model)
{
    return _model.Reserve();
}

std::expected<OperationRecord, OperationCenterModelError>
OperationCenterModelTesting::Admit(OperationCenterModel &_model,
                                   OperationCenterModel::Reservation &&_reservation,
                                   const OperationPlan &_plan,
                                   const OperationPlan::TimePoint _created_at)
{
    return _model.Admit(std::move(_reservation), _plan, _created_at);
}

} // namespace nc::ops
