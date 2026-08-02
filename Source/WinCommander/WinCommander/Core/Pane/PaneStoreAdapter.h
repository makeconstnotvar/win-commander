// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "PaneLifecycleReducer.h"
#include <Base/ScopedObservable.h>
#include <functional>
#include <memory>

namespace nc::core {

/**
 * An observable read-model boundary over an injected pane state reader.
 *
 * The reader is invoked only on the main queue. ScheduleRebuild may be called from any queue and
 * coalesces pending requests until the injected scheduler runs the rebuild on the main queue. The
 * default scheduler dispatches asynchronously to the main queue. Rebuilds suppress
 * semantically identical states.
 */
class PaneStoreAdapter
{
public:
    /** Main-queue reader. Implementations must not throw. */
    using StateReader = std::function<PaneState()>;
    /** Thread-safe, non-throwing scheduler that executes submitted work on the main queue. */
    using Scheduler = std::function<void(std::function<void()>)>;
    /** Synchronous main-queue callback. Implementations must not throw. */
    using Observer = std::function<void(const PaneSnapshot &)>;
    using ObservationTicket = base::ScopedObservableBase::ObservationTicket;

    PaneStoreAdapter(PaneId _pane_id, StateReader _state_reader, Scheduler _scheduler = {});
    PaneStoreAdapter(const PaneStoreAdapter &) = delete;
    PaneStoreAdapter(PaneStoreAdapter &&) = delete;
    ~PaneStoreAdapter();

    PaneStoreAdapter &operator=(const PaneStoreAdapter &) = delete;
    PaneStoreAdapter &operator=(PaneStoreAdapter &&) = delete;

    /** Returns an immutable copy of the current snapshot. Must be called on the main queue. */
    [[nodiscard]] PaneSnapshot Snapshot() const;

    /**
     * Registers a synchronous main-queue observer. The callback reference is valid only for that
     * invocation; the initial snapshot is read separately via Snapshot(). Observer exceptions are
     * a contract violation.
     */
    [[nodiscard]] ObservationTicket Observe(Observer _observer);

    /** Schedules one coalesced reader invocation and publication on the main queue. Thread-safe. */
    void ScheduleRebuild();

    /** Reduces one ordered controller lifecycle event. Must be called on the main queue. */
    [[nodiscard]] PaneLifecycleReducerResult ApplyLifecycleEvent(const PaneLifecycleEvent &_event);

    /** Seeds an accepted request that was already active when observation began. Main queue only. */
    [[nodiscard]] PaneLifecycleReducerResult SeedActiveLifecycle(PaneActiveRequest _active_request);

    /** Seeds an accepted historical failure and its observation checkpoint. Main queue only. */
    [[nodiscard]] PaneLifecycleReducerResult SeedRetainedLifecycleFailure(
        PaneActiveRequest _failed_request,
        const PaneLifecycleEvent &_failure,
        uint64_t _checkpoint_sequence);

private:
    class Impl;
    std::shared_ptr<Impl> m_Impl;
};

} // namespace nc::core
