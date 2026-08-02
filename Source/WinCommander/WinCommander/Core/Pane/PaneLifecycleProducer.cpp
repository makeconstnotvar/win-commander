// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PaneLifecycleProducer.h"

#include <Base/algo.h>
#include <Base/dispatch_cpp.h>
#include <cassert>
#include <list>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace nc::core {

class PaneLifecycleProducer::Impl final : private base::ScopedObservableBase,
                                          public std::enable_shared_from_this<PaneLifecycleProducer::Impl>
{
public:
    explicit Impl(const PaneId _pane_id, const detail::PaneLifecycleCounterLimits _counter_limits)
        : m_PaneId(_pane_id), m_CounterLimits(_counter_limits)
    {
        dispatch_assert_main_queue();
        if( m_CounterLimits.maximum_request_id == 0 || m_CounterLimits.maximum_event_sequence == 0 )
            throw std::invalid_argument("PaneLifecycleProducer counter limits must be non-zero");
    }

    ~Impl()
    {
        dispatch_assert_main_queue();
        assert(m_Shutdown);
        assert(!m_ActiveRequest);
    }

    PaneRequestId Start(PaneRequestDescriptor _descriptor)
    {
        dispatch_assert_main_queue();
        RejectFinishMutationReentry("PaneLifecycleProducer cannot start during a finish mutation");
        EnsureRunning();
        if( m_ActiveRequest )
            throw std::logic_error("PaneLifecycleProducer already has an active request");

        EnsureRequestIdCapacity(1);
        // Started consumes one sequence and an accepted request must retain one for its terminal.
        EnsureEventSequenceCapacity(2);
        auto next_active = std::make_unique<PaneActiveRequest>(
            PaneActiveRequest{PaneRequestId{m_NextRequestId}, std::move(_descriptor)});
        NotificationBatch batch;
        batch.emplace_back(PrepareEvent(*next_active, m_NextEventSequence, PaneLifecycleStarted{}));

        const PaneRequestId request_id = next_active->request_id;
        m_ActiveRequest = std::move(next_active);
        m_RetainedFailure.reset();
        AdvanceCounter(m_NextRequestId, m_CounterLimits.maximum_request_id, 1);
        AdvanceCounter(m_NextEventSequence, m_CounterLimits.maximum_event_sequence, 1);
        CommitBatch(batch);
        DrainNotifications();
        return request_id;
    }

    PaneRequestId Reject(PaneRequestDescriptor _descriptor, PaneLifecycleRejected _rejection)
    {
        dispatch_assert_main_queue();
        RejectFinishMutationReentry("PaneLifecycleProducer cannot reject during a finish mutation");
        EnsureRunning();

        EnsureRequestIdCapacity(1);
        // Rejections cannot consume the terminal sequence reserved by an active request.
        EnsureEventSequenceCapacity(m_ActiveRequest ? 2 : 1);
        const PaneActiveRequest attempt{PaneRequestId{m_NextRequestId}, std::move(_descriptor)};
        NotificationBatch batch;
        batch.emplace_back(PrepareEvent(attempt, m_NextEventSequence, std::move(_rejection)));

        AdvanceCounter(m_NextRequestId, m_CounterLimits.maximum_request_id, 1);
        AdvanceCounter(m_NextEventSequence, m_CounterLimits.maximum_event_sequence, 1);
        CommitBatch(batch);
        DrainNotifications();
        return attempt.request_id;
    }

    PaneRequestId SupersedeAndStart(PaneRequestDescriptor _descriptor)
    {
        dispatch_assert_main_queue();
        RejectFinishMutationReentry(
            "PaneLifecycleProducer cannot supersede during a finish mutation");
        EnsureRunning();
        if( !m_ActiveRequest )
            throw std::logic_error("PaneLifecycleProducer has no request to supersede");

        EnsureRequestIdCapacity(1);
        // Superseded + Started consume two; the replacement retains one for its terminal.
        EnsureEventSequenceCapacity(3);
        const PaneActiveRequest previous = *m_ActiveRequest;
        auto replacement = std::make_unique<PaneActiveRequest>(
            PaneActiveRequest{PaneRequestId{m_NextRequestId}, std::move(_descriptor)});
        NotificationBatch batch;
        batch.emplace_back(
            PrepareEvent(previous, m_NextEventSequence, PaneLifecycleSuperseded{replacement->request_id}));
        batch.emplace_back(PrepareEvent(*replacement, m_NextEventSequence + 1, PaneLifecycleStarted{}));

        const PaneRequestId replacement_id = replacement->request_id;
        m_ActiveRequest = std::move(replacement);
        m_RetainedFailure.reset();
        AdvanceCounter(m_NextRequestId, m_CounterLimits.maximum_request_id, 1);
        AdvanceCounter(m_NextEventSequence, m_CounterLimits.maximum_event_sequence, 2);
        CommitBatch(batch);
        DrainNotifications();
        return replacement_id;
    }

    FinishResult Finish(const PaneRequestId _request_id,
                        PaneLifecycleFinishPayload _terminal,
                        PaneLifecycleProducer::FinishMutation _mutation)
    {
        dispatch_assert_main_queue();
        RejectFinishMutationReentry("PaneLifecycleProducer cannot finish during a finish mutation");
        if( m_Shutdown )
            return FinishResult::ProducerShutdown;
        if( !m_ActiveRequest )
            return FinishResult::NoActiveRequest;
        if( m_ActiveRequest->request_id != _request_id )
            return FinishResult::StaleRequest;

        EnsureEventSequenceCapacity(1);
        const PaneActiveRequest completed = *m_ActiveRequest;
        NotificationBatch batch;
        std::visit(
            [this, &batch, &completed](auto &&_payload) {
                batch.emplace_back(PrepareEvent(completed, m_NextEventSequence, std::move(_payload)));
            },
            std::move(_terminal));

        if( _mutation ) {
            m_MutationViolated = false;
            m_DestructionRequestedDuringMutation = false;
            m_IsExecutingFinishMutation = true;
            try {
                _mutation();
            } catch( ... ) {
                m_IsExecutingFinishMutation = false;
                m_MutationViolated = false;
                if( m_DestructionRequestedDuringMutation )
                    CompleteDeferredDestruction();
                throw;
            }
            m_IsExecutingFinishMutation = false;
            if( m_MutationViolated ) {
                m_MutationViolated = false;
                if( m_DestructionRequestedDuringMutation )
                    CompleteDeferredDestruction();
                throw std::logic_error(
                    "PaneLifecycleProducer finish mutation attempted lifecycle reentry");
            }
        }

        m_ActiveRequest.reset();
        if( std::holds_alternative<PaneLifecycleFailed>(batch.front()->payload) )
            m_RetainedFailure = batch.front();
        else
            m_RetainedFailure.reset();
        AdvanceCounter(m_NextEventSequence, m_CounterLimits.maximum_event_sequence, 1);
        CommitBatch(batch);
        DrainNotifications();
        return FinishResult::Published;
    }

    void Shutdown()
    {
        dispatch_assert_main_queue();
        RejectFinishMutationReentry("PaneLifecycleProducer cannot shut down during a finish mutation");
        if( m_Shutdown )
            return;

        NotificationBatch batch;
        if( m_ActiveRequest ) {
            EnsureEventSequenceCapacity(1);
            const PaneActiveRequest cancelled = *m_ActiveRequest;
            batch.emplace_back(PrepareEvent(cancelled,
                                            m_NextEventSequence,
                                            PaneLifecycleCancelled{PaneCancellationReason::ProducerShutdown}));
        }

        m_Shutdown = true;
        if( m_ActiveRequest ) {
            m_ActiveRequest.reset();
            m_RetainedFailure.reset();
            AdvanceCounter(m_NextEventSequence, m_CounterLimits.maximum_event_sequence, 1);
            CommitBatch(batch);
            DrainNotifications();
        }
    }

    void ShutdownForDestruction() noexcept
    {
        if( m_IsExecutingFinishMutation ) {
            m_MutationViolated = true;
            m_DestructionRequestedDuringMutation = true;
            return;
        }
        try {
            Shutdown();
        } catch( ... ) {
            // Destruction cannot propagate allocation/overflow failures. Leave any in-flight event
            // alive until its owning drain unwinds, but discard unpublished work and ownership.
            m_Shutdown = true;
            m_ActiveRequest.reset();
            m_PendingNotifications.clear();
        }
    }

    std::optional<PaneActiveRequest> Active() const
    {
        dispatch_assert_main_queue();
        if( m_ActiveRequest )
            return *m_ActiveRequest;
        return std::nullopt;
    }

    PaneId Pane() const noexcept { return m_PaneId; }

    ObservationTicket Observe(PaneLifecycleProducer::Observer _observer)
    {
        dispatch_assert_main_queue();
        if( !_observer )
            return {};

        const std::weak_ptr<Impl> weak_self = shared_from_this();
        return AddTicketedObserver([weak_self, observer = std::move(_observer)] {
            if( const auto self = weak_self.lock() ) {
                assert(self->m_ActiveNotification);
                observer(*self->m_ActiveNotification);
            }
        });
    }

    PaneLifecycleProducer::Subscription Subscribe(PaneLifecycleProducer::Observer _observer)
    {
        dispatch_assert_main_queue();
        if( !_observer )
            return {};

        PaneLifecycleProducer::Subscription subscription;
        subscription.observation = Observe(std::move(_observer));

        // Pending notifications will be delivered to the newly registered observer. If their
        // first authoritative transition is a terminal, its Started boundary was already missed
        // and must be seeded. If it is Started, live delivery supplies the boundary exactly once.
        for( const auto &notification : m_PendingNotifications ) {
            if( std::holds_alternative<PaneLifecycleRejected>(notification->payload) )
                continue;
            if( std::holds_alternative<PaneLifecycleStarted>(notification->payload) )
                return subscription;
            if( IsPaneLifecycleAcceptedTerminal(notification->payload) ) {
                subscription.seed_request = PaneActiveRequest{
                    .request_id = notification->request_id,
                    .descriptor = notification->descriptor,
                };
                return subscription;
            }
        }

        if( m_ActiveRequest ) {
            subscription.seed_request = *m_ActiveRequest;
            return subscription;
        }

        if( m_RetainedFailure ) {
            subscription.seed_request = PaneActiveRequest{
                .request_id = m_RetainedFailure->request_id,
                .descriptor = m_RetainedFailure->descriptor,
            };
            subscription.retained_failure = *m_RetainedFailure;
            // A joining observer does not receive the notification currently being fired. Rebase
            // the retained failure to that observation checkpoint (or the last drained sequence)
            // so rejected attempts between the failure and subscription do not create a gap.
            const uint64_t checkpoint = m_ActiveNotification
                                          ? m_ActiveNotification->event_sequence
                                          : PreviousEventSequence();
            assert(checkpoint != 0);
            subscription.checkpoint_sequence = checkpoint;
        }
        return subscription;
    }

private:
    using Notification = std::shared_ptr<const PaneLifecycleEvent>;
    using NotificationBatch = std::list<Notification>;

    static_assert(std::is_nothrow_move_constructible_v<PaneActiveRequest>);
    static_assert(std::is_nothrow_move_assignable_v<PaneActiveRequest>);
    static_assert(std::is_nothrow_move_assignable_v<std::unique_ptr<PaneActiveRequest>>);

    void RejectFinishMutationReentry(const char *_message)
    {
        if( !m_IsExecutingFinishMutation )
            return;
        m_MutationViolated = true;
        throw std::logic_error(_message);
    }

    void CompleteDeferredDestruction() noexcept
    {
        m_DestructionRequestedDuringMutation = false;
        try {
            Shutdown();
        } catch( ... ) {
            m_Shutdown = true;
            m_ActiveRequest.reset();
            m_PendingNotifications.clear();
        }
    }

    void EnsureRunning() const
    {
        if( m_Shutdown )
            throw std::logic_error("PaneLifecycleProducer is shut down");
    }

    static void EnsureCounterCapacity(const uint64_t _next,
                                      const uint64_t _maximum,
                                      const uint64_t _count,
                                      const char *_message)
    {
        assert(_count > 0);
        if( _next == 0 || _next > _maximum || _count > _maximum - _next + 1 )
            throw std::overflow_error(_message);
    }

    void EnsureRequestIdCapacity(const uint64_t _count) const
    {
        EnsureCounterCapacity(m_NextRequestId,
                              m_CounterLimits.maximum_request_id,
                              _count,
                              "PaneLifecycleProducer request id overflow");
    }

    void EnsureEventSequenceCapacity(const uint64_t _count) const
    {
        EnsureCounterCapacity(m_NextEventSequence,
                              m_CounterLimits.maximum_event_sequence,
                              _count,
                              "PaneLifecycleProducer event sequence overflow");
    }

    [[nodiscard]] uint64_t PreviousEventSequence() const noexcept
    {
        if( m_NextEventSequence == 0 )
            return m_CounterLimits.maximum_event_sequence;
        assert(m_NextEventSequence > 1);
        return m_NextEventSequence - 1;
    }

    static void AdvanceCounter(uint64_t &_next, const uint64_t _maximum, const uint64_t _count) noexcept
    {
        assert(_next != 0);
        assert(_next <= _maximum);
        assert(_count > 0);
        assert(_count <= _maximum - _next + 1);
        if( _count == _maximum - _next + 1 )
            _next = 0;
        else
            _next += _count;
    }

    template <class Payload>
    Notification PrepareEvent(const PaneActiveRequest &_request,
                              const uint64_t _event_sequence,
                              Payload _payload) const
    {
        return std::make_shared<const PaneLifecycleEvent>(PaneLifecycleEvent{
            .pane_id = m_PaneId,
            .request_id = _request.request_id,
            .event_sequence = _event_sequence,
            .descriptor = _request.descriptor,
            .payload = PaneLifecycleEventPayload{std::move(_payload)},
        });
    }

    void CommitBatch(NotificationBatch &_batch) noexcept
    {
        m_PendingNotifications.splice(m_PendingNotifications.end(), _batch);
    }

    void DrainNotifications()
    {
        dispatch_assert_main_queue();
        [[maybe_unused]] const auto lifetime_guard = shared_from_this();
        if( m_IsNotifying )
            return;

        m_IsNotifying = true;
        const auto restore_notification_state = at_scope_end([this] {
            m_ActiveNotification.reset();
            m_IsNotifying = false;
        });

        while( !m_PendingNotifications.empty() ) {
            m_ActiveNotification = std::move(m_PendingNotifications.front());
            m_PendingNotifications.pop_front();
            FireObservers();
        }
    }

    PaneId m_PaneId;
    detail::PaneLifecycleCounterLimits m_CounterLimits;
    uint64_t m_NextRequestId = 1;
    uint64_t m_NextEventSequence = 1;
    std::unique_ptr<PaneActiveRequest> m_ActiveRequest;
    Notification m_RetainedFailure;
    NotificationBatch m_PendingNotifications;
    Notification m_ActiveNotification;
    bool m_IsNotifying = false;
    bool m_Shutdown = false;
    bool m_IsExecutingFinishMutation = false;
    bool m_MutationViolated = false;
    bool m_DestructionRequestedDuringMutation = false;
};

PaneLifecycleProducer::PaneLifecycleProducer(const PaneId _pane_id)
    : PaneLifecycleProducer(_pane_id, detail::PaneLifecycleCounterLimits{})
{
}

PaneLifecycleProducer::PaneLifecycleProducer(const PaneId _pane_id,
                                             const detail::PaneLifecycleCounterLimits _counter_limits)
    : m_Impl(std::make_shared<Impl>(_pane_id, _counter_limits))
{
}

PaneLifecycleProducer::~PaneLifecycleProducer()
{
    m_Impl->ShutdownForDestruction();
}

PaneRequestId PaneLifecycleProducer::Start(PaneRequestDescriptor _descriptor)
{
    const auto lifetime_guard = m_Impl;
    return lifetime_guard->Start(std::move(_descriptor));
}

PaneRequestId PaneLifecycleProducer::Reject(PaneRequestDescriptor _descriptor, PaneLifecycleRejected _rejection)
{
    const auto lifetime_guard = m_Impl;
    return lifetime_guard->Reject(std::move(_descriptor), std::move(_rejection));
}

PaneRequestId PaneLifecycleProducer::SupersedeAndStart(PaneRequestDescriptor _descriptor)
{
    const auto lifetime_guard = m_Impl;
    return lifetime_guard->SupersedeAndStart(std::move(_descriptor));
}

PaneLifecycleProducer::FinishResult PaneLifecycleProducer::Finish(const PaneRequestId _request_id,
                                                                  PaneLifecycleFinishPayload _terminal)
{
    const auto lifetime_guard = m_Impl;
    return lifetime_guard->Finish(_request_id, std::move(_terminal), {});
}

PaneLifecycleProducer::FinishResult PaneLifecycleProducer::Finish(const PaneRequestId _request_id,
                                                                  PaneLifecycleFinishPayload _terminal,
                                                                  FinishMutation _mutation)
{
    const auto lifetime_guard = m_Impl;
    return lifetime_guard->Finish(_request_id, std::move(_terminal), std::move(_mutation));
}

void PaneLifecycleProducer::Shutdown()
{
    const auto lifetime_guard = m_Impl;
    lifetime_guard->Shutdown();
}

std::optional<PaneActiveRequest> PaneLifecycleProducer::Active() const
{
    return m_Impl->Active();
}

PaneId PaneLifecycleProducer::Pane() const noexcept
{
    return m_Impl->Pane();
}

PaneLifecycleProducer::ObservationTicket PaneLifecycleProducer::Observe(Observer _observer)
{
    return m_Impl->Observe(std::move(_observer));
}

PaneLifecycleProducer::Subscription PaneLifecycleProducer::Subscribe(Observer _observer)
{
    const auto lifetime_guard = m_Impl;
    return lifetime_guard->Subscribe(std::move(_observer));
}

std::unique_ptr<PaneLifecycleProducer> detail::PaneLifecycleProducerTestAccess::Make(
    const PaneId _pane_id,
    const PaneLifecycleCounterLimits _counter_limits)
{
    return std::unique_ptr<PaneLifecycleProducer>(new PaneLifecycleProducer(_pane_id, _counter_limits));
}

} // namespace nc::core
