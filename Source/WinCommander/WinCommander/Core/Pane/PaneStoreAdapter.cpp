// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PaneStoreAdapter.h"
#include <Base/algo.h>
#include <Base/dispatch_cpp.h>
#include <atomic>
#include <cassert>
#include <limits>
#include <list>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace nc::core {

class PaneStoreAdapter::Impl final : private base::ScopedObservableBase,
                                     public std::enable_shared_from_this<PaneStoreAdapter::Impl>
{
public:
    Impl(PaneId _pane_id, StateReader _state_reader, Scheduler _scheduler)
        : m_StateReader(std::move(_state_reader)), m_Scheduler(std::move(_scheduler))
    {
        dispatch_assert_main_queue();
        if( !m_StateReader )
            throw std::invalid_argument("PaneStoreAdapter requires a state reader");
        if( !m_Scheduler )
            m_Scheduler = [](std::function<void()> _work) { dispatch_to_main_queue(std::move(_work)); };
        PaneState initial_state = m_StateReader();
        m_Reducer = std::make_unique<PaneLifecycleReducer>(_pane_id, std::move(initial_state));
        m_Snapshot.pane_id = _pane_id;
        m_Snapshot.state = m_Reducer->State();
    }

    [[nodiscard]] PaneSnapshot Snapshot() const
    {
        dispatch_assert_main_queue();
        return m_Snapshot;
    }

    [[nodiscard]] ObservationTicket Observe(PaneStoreAdapter::Observer _observer)
    {
        dispatch_assert_main_queue();
        if( !_observer )
            return {};

        const std::weak_ptr<Impl> weak_self = shared_from_this();
        return AddTicketedObserver([weak_self, observer = std::move(_observer)] {
            if( const auto self = weak_self.lock() ) {
                const auto snapshot = self->m_ActiveNotification;
                assert(snapshot != nullptr);
                observer(*snapshot);
            }
        });
    }

    void ScheduleRebuild()
    {
        const std::weak_ptr<Impl> weak_self = shared_from_this();
        std::function<void()> work = [weak_self] {
            if( const auto self = weak_self.lock() )
                self->RunScheduledRebuild();
        };
        if( m_RebuildScheduled.exchange(true) )
            return;

        bool submitted = false;
        const auto rollback = at_scope_end([&] {
            if( !submitted )
                m_RebuildScheduled.store(false);
        });
        m_Scheduler(std::move(work));
        submitted = true;
    }

    PaneLifecycleReducerResult ApplyLifecycleEvent(const PaneLifecycleEvent &_event)
    {
        dispatch_assert_main_queue();
        PaneLifecycleReducer candidate = *m_Reducer;
        std::optional<PaneState> committed_projection;
        if( std::holds_alternative<PaneLifecycleCommitted>(_event.payload) )
            committed_projection.emplace(m_StateReader());

        const auto result = candidate.Apply(
            _event,
            committed_projection ? std::addressof(*committed_projection) : nullptr);
        return Commit(std::move(candidate), result);
    }

    PaneLifecycleReducerResult SeedActiveLifecycle(PaneActiveRequest _active_request)
    {
        dispatch_assert_main_queue();
        PaneLifecycleReducer candidate = *m_Reducer;
        const auto result = candidate.SeedActive(std::move(_active_request));
        return Commit(std::move(candidate), result);
    }

    PaneLifecycleReducerResult SeedRetainedLifecycleFailure(
        PaneActiveRequest _failed_request,
        const PaneLifecycleEvent &_failure,
        const uint64_t _checkpoint_sequence)
    {
        dispatch_assert_main_queue();
        PaneLifecycleReducer candidate = *m_Reducer;
        const auto result = candidate.SeedRetainedFailure(
            std::move(_failed_request), _failure, _checkpoint_sequence);
        return Commit(std::move(candidate), result);
    }

private:
    void RunScheduledRebuild()
    {
        dispatch_assert_main_queue();
        if( !m_RebuildScheduled.exchange(false) )
            return;

        PaneLifecycleReducer candidate = *m_Reducer;
        const auto result = candidate.UpdateCommittedProjection(m_StateReader());
        [[maybe_unused]] const auto committed = Commit(std::move(candidate), result);
    }

    PaneLifecycleReducerResult Commit(PaneLifecycleReducer _candidate,
                                      PaneLifecycleReducerResult _result)
    {
        dispatch_assert_main_queue();
        static_assert(std::is_nothrow_move_assignable_v<PaneLifecycleReducer>);
        static_assert(std::is_nothrow_move_assignable_v<PaneSnapshot>);

        if( !_result.state_changed ) {
            *m_Reducer = std::move(_candidate);
            return _result;
        }

        if( m_Snapshot.revision == std::numeric_limits<uint64_t>::max() )
            throw std::overflow_error("PaneStoreAdapter revision exhausted");

        PaneSnapshot next_snapshot = m_Snapshot;
        next_snapshot.state = _candidate.State();
        ++next_snapshot.revision;
        if( next_snapshot.state.listing != m_Snapshot.state.listing ) {
            if( next_snapshot.listing_generation == std::numeric_limits<uint64_t>::max() )
                throw std::overflow_error("PaneStoreAdapter listing generation exhausted");
            ++next_snapshot.listing_generation;
        }

        auto notification = std::make_shared<const PaneSnapshot>(next_snapshot);
        std::list<std::shared_ptr<const PaneSnapshot>> prepared_notifications;
        prepared_notifications.emplace_back(std::move(notification));

        *m_Reducer = std::move(_candidate);
        m_Snapshot = std::move(next_snapshot);
        m_PendingNotifications.splice(m_PendingNotifications.end(), prepared_notifications);
        DrainNotifications();
        return _result;
    }

    void DrainNotifications()
    {
        dispatch_assert_main_queue();
        if( m_IsNotifying )
            return;

        m_IsNotifying = true;
        const auto restore_notification_state = at_scope_end([&] {
            m_ActiveNotification.reset();
            m_IsNotifying = false;
        });

        while( !m_PendingNotifications.empty() ) {
            m_ActiveNotification = std::move(m_PendingNotifications.front());
            m_PendingNotifications.pop_front();
            FireObservers();
        }
    }

    StateReader m_StateReader;
    Scheduler m_Scheduler;
    std::unique_ptr<PaneLifecycleReducer> m_Reducer;
    PaneSnapshot m_Snapshot;
    std::list<std::shared_ptr<const PaneSnapshot>> m_PendingNotifications;
    std::shared_ptr<const PaneSnapshot> m_ActiveNotification;
    bool m_IsNotifying = false;
    std::atomic_bool m_RebuildScheduled{false};
};

PaneStoreAdapter::PaneStoreAdapter(PaneId _pane_id, StateReader _state_reader, Scheduler _scheduler)
    : m_Impl(std::make_shared<Impl>(_pane_id, std::move(_state_reader), std::move(_scheduler)))
{
}

PaneStoreAdapter::~PaneStoreAdapter() = default;

PaneSnapshot PaneStoreAdapter::Snapshot() const
{
    const auto impl = m_Impl;
    return impl->Snapshot();
}

PaneStoreAdapter::ObservationTicket PaneStoreAdapter::Observe(Observer _observer)
{
    const auto impl = m_Impl;
    return impl->Observe(std::move(_observer));
}

void PaneStoreAdapter::ScheduleRebuild()
{
    const auto impl = m_Impl;
    impl->ScheduleRebuild();
}

PaneLifecycleReducerResult PaneStoreAdapter::ApplyLifecycleEvent(const PaneLifecycleEvent &_event)
{
    const auto impl = m_Impl;
    return impl->ApplyLifecycleEvent(_event);
}

PaneLifecycleReducerResult PaneStoreAdapter::SeedActiveLifecycle(PaneActiveRequest _active_request)
{
    const auto impl = m_Impl;
    return impl->SeedActiveLifecycle(std::move(_active_request));
}

PaneLifecycleReducerResult PaneStoreAdapter::SeedRetainedLifecycleFailure(
    PaneActiveRequest _failed_request,
    const PaneLifecycleEvent &_failure,
    const uint64_t _checkpoint_sequence)
{
    const auto impl = m_Impl;
    return impl->SeedRetainedLifecycleFailure(
        std::move(_failed_request), _failure, _checkpoint_sequence);
}

} // namespace nc::core
