// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

namespace nc::core {

/**
 * Window-scoped ownership barrier for operation admission work performed before Pool enqueue.
 *
 * A ticket keeps the submission visible to close/terminate checks. Cancellation is monotonic: after
 * CancelAndWait starts, no new ticket can be acquired and every existing ticket observes cancellation.
 */
class OperationSubmissionGate final
{
    struct State;

public:
    class Ticket final
    {
    public:
        Ticket(const Ticket &) = delete;
        Ticket &operator=(const Ticket &) = delete;
        Ticket(Ticket &&) = delete;
        Ticket &operator=(Ticket &&) = delete;
        ~Ticket();

        [[nodiscard]] bool IsCancelled() const noexcept;

    private:
        explicit Ticket(std::shared_ptr<State> _state) : m_State(std::move(_state)) {}
        std::shared_ptr<State> m_State;
        bool m_Accounted{false};
        friend class OperationSubmissionGate;
    };

    OperationSubmissionGate();
    ~OperationSubmissionGate();
    OperationSubmissionGate(const OperationSubmissionGate &) = delete;
    OperationSubmissionGate &operator=(const OperationSubmissionGate &) = delete;

    /** Returns empty after cancellation begins or when a ticket cannot be allocated safely. */
    [[nodiscard]] std::shared_ptr<Ticket> Acquire() noexcept;
    [[nodiscard]] bool HasPending() const noexcept;
    [[nodiscard]] size_t PendingCount() const noexcept;

    /** Monotonically cancels admission work and waits until every acquired ticket is released. */
    void CancelAndWait() noexcept;

private:
    struct State final {
        mutable std::mutex lock;
        std::condition_variable changed;
        size_t pending{0};
        bool cancelled{false};
    };

    std::shared_ptr<State> m_State;
};

inline OperationSubmissionGate::OperationSubmissionGate() : m_State(std::make_shared<State>())
{
}

inline OperationSubmissionGate::~OperationSubmissionGate()
{
    CancelAndWait();
}

inline OperationSubmissionGate::Ticket::~Ticket()
{
    const auto state = m_State;
    if( !state )
        return;

    if( !m_Accounted )
        return;

    const auto guard = std::lock_guard{state->lock};
    assert(state->pending > 0);
    --state->pending;
    state->changed.notify_all();
}

inline bool OperationSubmissionGate::Ticket::IsCancelled() const noexcept
{
    const auto state = m_State;
    if( !state )
        return true;
    const auto guard = std::lock_guard{state->lock};
    return state->cancelled;
}

inline std::shared_ptr<OperationSubmissionGate::Ticket> OperationSubmissionGate::Acquire() noexcept
{
    const auto state = m_State;
    if( !state )
        return {};

    try {
        auto ticket = std::shared_ptr<Ticket>{new Ticket{state}};
        const auto guard = std::lock_guard{state->lock};
        if( state->cancelled )
            return {};
        ++state->pending;
        ticket->m_Accounted = true;
        return ticket;
    } catch( ... ) {
        return {};
    }
}

inline bool OperationSubmissionGate::HasPending() const noexcept
{
    return PendingCount() != 0;
}

inline size_t OperationSubmissionGate::PendingCount() const noexcept
{
    const auto state = m_State;
    if( !state )
        return 0;
    const auto guard = std::lock_guard{state->lock};
    return state->pending;
}

inline void OperationSubmissionGate::CancelAndWait() noexcept
{
    const auto state = m_State;
    if( !state )
        return;

    auto guard = std::unique_lock{state->lock};
    state->cancelled = true;
    state->changed.wait(guard, [&] { return state->pending == 0; });
}

} // namespace nc::core
