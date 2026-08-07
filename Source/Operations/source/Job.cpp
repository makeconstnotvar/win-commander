// Copyright (C) 2017-2023 Michael Kazakov. Subject to GNU General Public License version 3.
#include "../include/Operations/Job.h"
#include <Base/IdleSleepPreventer.h>
#include <boost/core/demangle.hpp>
#include <thread>
#include <cassert>
#include <iostream>

namespace nc::ops {

Job::Job() : m_IsRunning{false}, m_IsPaused{false}, m_IsCompleted{false}, m_IsStopped{false}
{
}

Job::~Job() = default;

void Job::Perform()
{
}

void Job::LaunchWorker(std::shared_ptr<void> _worker_keep_alive)
{
    std::thread{[this, worker_keep_alive = std::move(_worker_keep_alive)] {
        Execute();
        (void)worker_keep_alive;
    }}.detach();
}

void Job::Run(std::shared_ptr<void> _worker_keep_alive)
{
    {
        const auto guard = std::lock_guard{m_FinishMutex};
        if( m_WasStarted || m_IsRunning || m_IsStopped )
            return;
        m_WasStarted = true;
        m_ExecutionFinished = false;
        m_IsRunning = true;
    }

    try {
        LaunchWorker(std::move(_worker_keep_alive));
    }
    catch( ... ) {
        {
            const auto guard = std::lock_guard{m_FinishMutex};
            m_WorkerThreadId = std::this_thread::get_id();
        }
        const auto transition_guard = std::lock_guard{m_TransitionMutex};
        bool was_paused = false;
        {
            const auto guard = std::lock_guard{m_StateMutex};
            was_paused = m_IsPaused.exchange(false);
            m_IsStopped = true;
        }
        if( was_paused ) {
            m_PauseCV.notify_all();
            NotifyResumed();
        }
        try {
            OnStopped();
        }
        catch( ... ) {
            std::cerr << "Error: operation " << typeid(*this).name()
                      << " launch-failure stop callback has thrown." << '\n';
        }
        FinishExecution();
        throw;
    }
}

void Job::Execute() noexcept
{
    try {
        {
            const auto guard = std::lock_guard{m_FinishMutex};
            m_WorkerThreadId = std::this_thread::get_id();
        }

        const auto thread_title = "com.wincommander." + boost::core::demangle(typeid(*this).name());
        pthread_setname_np(thread_title.c_str());

        const auto sleep_preventer = base::IdleSleepPreventer::GetPromise();
        m_Stats.StartTiming();

        try {
            if( !IsStopped() )
                Perform();
        }
        catch( const std::exception &e ) {
            std::cerr << "Error: operation " << typeid(*this).name() << " has thrown an exception: " << e.what()
                      << "." << '\n';
            (void)Stop();
        }
        catch( ... ) {
            std::cerr << "Error: operation " << typeid(*this).name() << " has thrown an unknown exception." << '\n';
            (void)Stop();
        }
    } catch( const std::exception &e ) {
        std::cerr << "Error: worker setup for operation " << typeid(*this).name()
                  << " has thrown an exception: " << e.what() << "." << '\n';
        (void)Stop();
    } catch( ... ) {
        std::cerr << "Error: worker setup for operation " << typeid(*this).name()
                  << " has thrown an unknown exception." << '\n';
        (void)Stop();
    }

    FinishExecution();
}

void Job::FinishExecution() noexcept
{
    {
        const auto guard = std::lock_guard{m_CurrentItemPathMutex};
        m_CurrentItemPath.reset();
    }

    if( !IsStopped() )
        SetCompleted();

    m_IsRunning = false;

    try {
        m_Stats.StopTiming();
    }
    catch( ... ) {
        std::cerr << "Error: operation " << typeid(*this).name() << " failed to stop statistics timing." << '\n';
    }

    std::function<void()> callback;
    {
        const auto guard = std::lock_guard{m_CallbackLock};
        callback = std::move(m_OnFinish);
    }
    if( callback ) {
        try {
            callback();
        }
        catch( const std::exception &e ) {
            std::cerr << "Error: operation " << typeid(*this).name()
                      << " finish callback has thrown an exception: " << e.what() << "." << '\n';
        }
        catch( ... ) {
            std::cerr << "Error: operation " << typeid(*this).name()
                      << " finish callback has thrown an unknown exception." << '\n';
        }
    }

    {
        const auto guard = std::lock_guard{m_FinishMutex};
        m_ExecutionFinished = true;
        m_WorkerThreadId = {};
    }
    m_FinishCV.notify_all();
}

void Job::Wait() const
{
    (void)Wait(std::chrono::nanoseconds::max());
}

bool Job::Wait(std::chrono::nanoseconds _wait_for_time) const
{
    std::unique_lock lock{m_FinishMutex};
    if( !m_WasStarted )
        return true;
    if( !m_ExecutionFinished && m_WorkerThreadId == std::this_thread::get_id() )
        return false;
    const auto finished = [this] { return m_ExecutionFinished; };
    if( _wait_for_time == std::chrono::nanoseconds::max() ) {
        m_FinishCV.wait(lock, finished);
        return true;
    }
    return m_FinishCV.wait_for(lock, _wait_for_time, finished);
}

bool Job::IsRunning() const noexcept
{
    return m_IsRunning;
}

void Job::SetFinishCallback(std::function<void()> _callback)
{
    const auto guard = std::lock_guard{m_CallbackLock};
    m_OnFinish = std::move(_callback);
}

bool Job::IsCompleted() const noexcept
{
    return m_IsCompleted;
}

bool Job::IsStopped() const noexcept
{
    return m_IsStopped;
}

bool Job::Stop()
{
    const auto transition_guard = std::lock_guard{m_TransitionMutex};
    bool was_paused = false;
    {
        const auto guard = std::lock_guard{m_StateMutex};
        if( m_IsStopped || m_IsCompleted || !OnStopRequested() )
            return false;

        m_IsStopped = true;
        was_paused = m_IsPaused.exchange(false);
    }

    if( was_paused ) {
        m_PauseCV.notify_all();
        NotifyResumed();
    }
    try {
        OnStopped();
    }
    catch( ... ) {
        std::cerr << "Error: operation " << typeid(*this).name() << " stop callback has thrown." << '\n';
    }
    return true;
}

bool Job::OnStopRequested() noexcept
{
    return true;
}

void Job::OnStopped()
{
}

void Job::SetCompleted()
{
    const auto transition_guard = std::lock_guard{m_TransitionMutex};
    bool was_paused = false;
    {
        const auto guard = std::lock_guard{m_StateMutex};
        if( m_IsCompleted || m_IsStopped )
            return;
        m_IsCompleted = true;
        was_paused = m_IsPaused.exchange(false);
    }

    if( was_paused ) {
        m_PauseCV.notify_all();
        NotifyResumed();
    }
}

class Statistics &Job::Statistics()
{
    return m_Stats;
}

const class Statistics &Job::Statistics() const
{
    return m_Stats;
}

std::optional<std::string> Job::CurrentItemPath() const
{
    const auto guard = std::lock_guard{m_CurrentItemPathMutex};
    return m_CurrentItemPath;
}

void Job::PublishCurrentItemPath(std::string _path)
{
    const auto guard = std::lock_guard{m_CurrentItemPathMutex};
    m_CurrentItemPath = std::move(_path);
}

void Job::Pause()
{
    const auto transition_guard = std::lock_guard{m_TransitionMutex};
    {
        const auto guard = std::lock_guard{m_StateMutex};
        if( m_IsPaused || m_IsCompleted || m_IsStopped )
            return;
        m_IsPaused = true;
    }

    std::function<void()> callback;
    try {
        const auto guard = std::lock_guard{m_CallbackLock};
        callback = m_OnPause;
    }
    catch( ... ) {
        std::cerr << "Error: operation " << typeid(*this).name() << " could not copy its pause callback." << '\n';
        return;
    }
    if( callback ) {
        try {
            callback();
        }
        catch( ... ) {
            std::cerr << "Error: operation " << typeid(*this).name() << " pause callback has thrown." << '\n';
        }
    }
}

void Job::Resume()
{
    const auto transition_guard = std::lock_guard{m_TransitionMutex};
    {
        const auto guard = std::lock_guard{m_StateMutex};
        if( !m_IsPaused )
            return;
        m_IsPaused = false;
    }
    m_PauseCV.notify_all();

    NotifyResumed();
}

void Job::NotifyResumed() noexcept
{
    std::function<void()> callback;
    try {
        const auto guard = std::lock_guard{m_CallbackLock};
        callback = m_OnResume;
    }
    catch( ... ) {
        std::cerr << "Error: operation " << typeid(*this).name() << " could not copy its resume callback." << '\n';
        return;
    }
    if( callback ) {
        try {
            callback();
        }
        catch( ... ) {
            std::cerr << "Error: operation " << typeid(*this).name() << " resume callback has thrown." << '\n';
        }
    }
}

bool Job::IsPaused() const noexcept
{
    return m_IsPaused;
}

void Job::BlockIfPaused()
{
    std::unique_lock lock{m_StateMutex};
    if( m_IsPaused && !m_IsStopped && !m_IsCompleted ) {
        const auto predicate = [this] { return !m_IsPaused || m_IsStopped || m_IsCompleted; };

        m_Stats.PauseTiming();
        m_PauseCV.wait(lock, predicate);
        m_Stats.ResumeTiming();
    }
}

void Job::SetPauseCallback(std::function<void()> _callback)
{
    const auto guard = std::lock_guard{m_CallbackLock};
    m_OnPause = std::move(_callback);
}

void Job::SetResumeCallback(std::function<void()> _callback)
{
    const auto guard = std::lock_guard{m_CallbackLock};
    m_OnResume = std::move(_callback);
}

void Job::SetItemStateReportCallback(ItemStateReportCallback _callback)
{
    if( m_IsRunning )
        throw std::logic_error("Job::SetItemStateReportCallback should be called only before job start");
    const auto guard = std::lock_guard{m_CallbackLock};
    m_OnItemStateReport = std::move(_callback);
}

void Job::TellItemReport(ItemStateReport _report)
{
    ItemStateReportCallback callback;
    {
        const auto guard = std::lock_guard{m_CallbackLock};
        callback = m_OnItemStateReport;
    }
    if( callback )
        callback(_report);
}

} // namespace nc::ops
