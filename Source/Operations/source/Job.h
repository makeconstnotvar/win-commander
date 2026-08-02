// Copyright (C) 2017-2020 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <Base/spinlock.h>
#include "Statistics.h"
#include "ItemStateReport.h"

namespace nc::ops {

class Job
{
public:
    virtual ~Job();

    void Run(std::shared_ptr<void> _worker_keep_alive = {});
    void Pause();
    void Resume();
    /** Returns true only when this call wins the terminal stop transition. */
    bool Stop();

    bool IsRunning() const noexcept;
    bool IsPaused() const noexcept;
    bool IsStopped() const noexcept;
    bool IsCompleted() const noexcept;

    /**
     * Waits until the worker has finished all callbacks that may access this Job or its owning Operation.
     * A cold Job has no worker and therefore returns immediately.
     */
    void Wait() const;
    bool Wait(std::chrono::nanoseconds _wait_for_time) const;

    void SetFinishCallback(std::function<void()> _callback);
    void SetPauseCallback(std::function<void()> _callback);
    void SetResumeCallback(std::function<void()> _callback);
    void SetItemStateReportCallback(ItemStateReportCallback _callback);

    class Statistics &Statistics();
    const class Statistics &Statistics() const;

protected:
    Job();
    /** Production launches Execute() on one detached worker; tests may fail this boundary deterministically. */
    virtual void LaunchWorker(std::shared_ptr<void> _worker_keep_alive);
    virtual void Perform();
    /**
     * Serializes operation-specific commit intent with cancellation. Returning false means that
     * an irreversible commit already owns the terminal decision and the stop request is ignored.
     */
    virtual bool OnStopRequested() noexcept;
    virtual void OnStopped();

    void SetCompleted();
    void Execute() noexcept;
    void FinishExecution() noexcept;
    void BlockIfPaused();
    void TellItemReport(ItemStateReport _report);

private:
    void NotifyResumed() noexcept;

    std::atomic_bool m_IsRunning;
    std::atomic_bool m_IsPaused;
    std::atomic_bool m_IsCompleted;
    std::atomic_bool m_IsStopped;
    std::recursive_mutex m_TransitionMutex;
    mutable std::mutex m_StateMutex;
    std::condition_variable m_PauseCV;

    mutable std::mutex m_FinishMutex;
    mutable std::condition_variable m_FinishCV;
    bool m_WasStarted{false};
    bool m_ExecutionFinished{false};
    std::thread::id m_WorkerThreadId;

    std::function<void()> m_OnFinish;
    std::function<void()> m_OnPause;
    std::function<void()> m_OnResume;
    ItemStateReportCallback m_OnItemStateReport;
    spinlock m_CallbackLock;

    class Statistics m_Stats;
};

} // namespace nc::ops
