// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include <Operations/Job.h>
#include <Operations/Operation.h>
#include "../source/AsyncDialogResponse.h"

#include <Base/dispatch_cpp.h>
#include <catch2/catch_all.hpp>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

#define PREFIX "Job: "

namespace nc::ops {
namespace {

class ImmediatelyFinishingJob final : public Job
{
private:
    void Perform() override {}
};

class JobUTPauseBlockingJob final : public Job
{
public:
    bool WaitUntilEntered(std::chrono::milliseconds _timeout)
    {
        std::unique_lock lock{mutex};
        return entered_cv.wait_for(lock, _timeout, [this] { return entered; });
    }

    std::atomic_bool left_pause_point{false};

private:
    void Perform() override
    {
        {
            const auto guard = std::lock_guard{mutex};
            entered = true;
        }
        entered_cv.notify_one();
        BlockIfPaused();
        left_pause_point = true;
    }

    std::mutex mutex;
    std::condition_variable entered_cv;
    bool entered{false};
};

class JobUTManualTerminalJob final : public Job
{
public:
    void Complete() { SetCompleted(); }
};

class JobUTThrowingLauncherJob final : public Job
{
public:
    explicit JobUTThrowingLauncherJob(bool _throw_on_stopped) : throw_on_stopped{_throw_on_stopped} {}

    int launch_calls{0};
    int perform_calls{0};
    int stopped_calls{0};
    bool stopped_callback_saw_terminal{false};
    bool stopped_callback_self_wait_rejected{false};
    const bool throw_on_stopped;
    std::vector<std::string_view> callbacks;

private:
    void LaunchWorker(std::shared_ptr<void>) override
    {
        ++launch_calls;
        throw std::runtime_error{"deterministic worker launch failure"};
    }

    void Perform() override { ++perform_calls; }

    void OnStopped() override
    {
        ++stopped_calls;
        stopped_callback_saw_terminal = IsStopped();
        stopped_callback_self_wait_rejected = !Wait(1ms);
        callbacks.emplace_back("stopped");
        if( throw_on_stopped )
            throw 1;
    }
};

struct JobUTTransitionOrderState final {
    std::mutex mutex;
    std::condition_variable cv;
    bool block_next_copy{false};
    bool copy_entered{false};
    bool release_copy{false};
    std::vector<std::string_view> notifications;
};

class JobUTSlowPauseCallback final
{
public:
    explicit JobUTSlowPauseCallback(std::shared_ptr<JobUTTransitionOrderState> _state) : state{std::move(_state)} {}

    JobUTSlowPauseCallback(const JobUTSlowPauseCallback &_other) : state{_other.state}
    {
        std::unique_lock lock{state->mutex};
        if( !state->block_next_copy )
            return;
        state->block_next_copy = false;
        state->copy_entered = true;
        state->cv.notify_all();
        state->cv.wait(lock, [this] { return state->release_copy; });
    }

    JobUTSlowPauseCallback(JobUTSlowPauseCallback &&) noexcept = default;

    void operator()() const
    {
        std::this_thread::sleep_for(25ms);
        const auto guard = std::lock_guard{state->mutex};
        state->notifications.emplace_back("pause");
    }

private:
    std::shared_ptr<JobUTTransitionOrderState> state;
};

class JobUTDialogWaitingOperation final : public Operation
{
public:
    ~JobUTDialogWaitingOperation() override { Wait(); }

    void WaitForResponse(std::shared_ptr<AsyncDialogResponse> _response)
    {
        WaitForDialogResponse(std::move(_response));
    }

    void DispatchForTest(std::shared_ptr<AsyncDialogResponse> _response, std::function<void()> _callback)
    {
        DispatchDialog(std::move(_response), std::move(_callback));
    }

private:
    Job *GetJob() noexcept override { return &job; }

    JobUTManualTerminalJob job;
};

class JobUTObservableOperation final : public Operation
{
public:
    ~JobUTObservableOperation() override { Wait(); }

    bool WaitUntilEntered(std::chrono::milliseconds _timeout)
    {
        std::unique_lock lock{mutex};
        return entered_cv.wait_for(lock, _timeout, [this] { return entered; });
    }

private:
    class ObservableJob final : public Job
    {
    public:
        explicit ObservableJob(JobUTObservableOperation &_owner) : owner{_owner} {}

    private:
        void Perform() override
        {
            {
                const auto guard = std::lock_guard{owner.mutex};
                owner.entered = true;
            }
            owner.entered_cv.notify_one();
            while( !IsStopped() ) {
                BlockIfPaused();
                std::this_thread::yield();
            }
        }

        JobUTObservableOperation &owner;
    };

    Job *GetJob() noexcept override { return &job; }

    std::mutex mutex;
    std::condition_variable entered_cv;
    bool entered{false};
    ObservableJob job{*this};
};

} // namespace

TEST_CASE(PREFIX "worker launch failure publishes stopped callbacks before finish and rethrows",
          "[job][job-launch]")
{
    for( const bool throw_on_stopped : {false, true} ) {
        DYNAMIC_SECTION("OnStopped throws=" << throw_on_stopped)
        {
            JobUTThrowingLauncherJob job{throw_on_stopped};
            int finish_calls = 0;
            bool finish_saw_stopped = false;
            bool finish_self_wait_rejected = false;
            job.SetFinishCallback([&] {
                ++finish_calls;
                finish_saw_stopped = job.IsStopped();
                finish_self_wait_rejected = !job.Wait(1ms);
                job.callbacks.emplace_back("finish");
            });

            REQUIRE_THROWS_AS(job.Run(), std::runtime_error);

            CHECK(job.launch_calls == 1);
            CHECK(job.perform_calls == 0);
            CHECK(job.stopped_calls == 1);
            CHECK(job.stopped_callback_saw_terminal);
            CHECK(job.stopped_callback_self_wait_rejected);
            CHECK(finish_calls == 1);
            CHECK(finish_saw_stopped);
            CHECK(finish_self_wait_rejected);
            CHECK(job.callbacks == std::vector<std::string_view>{"stopped", "finish"});
            CHECK(job.IsStopped());
            CHECK_FALSE(job.IsRunning());
            CHECK_FALSE(job.IsCompleted());
            CHECK(job.Wait(1ms));
        }
    }
}

TEST_CASE(PREFIX "Wait includes the finish callback lifetime", "[job]")
{
    ImmediatelyFinishingJob job;
    std::mutex mutex;
    std::condition_variable callback_entered_cv;
    std::condition_variable release_callback_cv;
    bool callback_entered = false;
    bool release_callback = false;
    std::atomic_bool self_wait_rejected = false;

    job.SetFinishCallback([&] {
        self_wait_rejected = !job.Wait(1ms);
        std::unique_lock lock{mutex};
        callback_entered = true;
        callback_entered_cv.notify_one();
        release_callback_cv.wait(lock, [&] { return release_callback; });
    });

    CHECK(job.Wait(1ms)); // A cold Job has no worker to join.
    job.Run();

    {
        std::unique_lock lock{mutex};
        REQUIRE(callback_entered_cv.wait_for(lock, 5s, [&] { return callback_entered; }));
    }

    CHECK_FALSE(job.Wait(1ms));

    {
        const auto guard = std::lock_guard{mutex};
        release_callback = true;
    }
    release_callback_cv.notify_one();

    CHECK(job.Wait(5s));
    CHECK(job.IsCompleted());
    CHECK(self_wait_rejected);
}

TEST_CASE(PREFIX "pause and resume use one synchronized predicate", "[job]")
{
    JobUTPauseBlockingJob job;
    std::atomic_int pauses{0};
    std::atomic_int resumes{0};
    job.SetPauseCallback([&] { ++pauses; });
    job.SetResumeCallback([&] { ++resumes; });

    job.Pause();
    REQUIRE(job.IsPaused());
    CHECK(pauses == 1);
    job.Run();
    REQUIRE(job.WaitUntilEntered(5s));
    CHECK_FALSE(job.Wait(1ms));

    job.Resume();
    REQUIRE(job.Wait(5s));
    CHECK(job.left_pause_point);
    CHECK(job.IsCompleted());
    CHECK_FALSE(job.IsPaused());
    CHECK(resumes == 1);
}

TEST_CASE(PREFIX "stop atomically clears pause and wins the terminal transition", "[job]")
{
    JobUTPauseBlockingJob job;
    std::atomic_int resumes{0};
    job.SetResumeCallback([&] { ++resumes; });

    job.Pause();
    job.Run();
    REQUIRE(job.WaitUntilEntered(5s));
    REQUIRE(job.Stop());
    CHECK_FALSE(job.Stop());
    REQUIRE(job.Wait(5s));

    CHECK(job.left_pause_point);
    CHECK(job.IsStopped());
    CHECK_FALSE(job.IsCompleted());
    CHECK_FALSE(job.IsPaused());
    CHECK(resumes == 1);

    job.Pause();
    CHECK_FALSE(job.IsPaused());
}

TEST_CASE(PREFIX "completion atomically clears pause and rejects later pauses", "[job]")
{
    JobUTManualTerminalJob job;
    std::atomic_int resumes{0};
    job.SetResumeCallback([&] { ++resumes; });

    job.Pause();
    REQUIRE(job.IsPaused());
    job.Complete();

    CHECK(job.IsCompleted());
    CHECK_FALSE(job.IsStopped());
    CHECK_FALSE(job.IsPaused());
    CHECK(resumes == 1);
    CHECK_FALSE(job.Stop());

    job.Pause();
    CHECK_FALSE(job.IsPaused());
}

TEST_CASE(PREFIX "pause cannot survive a concurrent terminal transition", "[job]")
{
    SECTION("stop")
    {
        for( int iteration = 0; iteration != 64; ++iteration ) {
            JobUTManualTerminalJob job;
            std::atomic_int pauses{0};
            std::atomic_int resumes{0};
            job.SetPauseCallback([&] { ++pauses; });
            job.SetResumeCallback([&] { ++resumes; });

            std::barrier start{3};
            std::thread pauser{[&] {
                start.arrive_and_wait();
                job.Pause();
            }};
            std::thread stopper{[&] {
                start.arrive_and_wait();
                (void)job.Stop();
            }};
            start.arrive_and_wait();
            pauser.join();
            stopper.join();

            CHECK(job.IsStopped());
            CHECK_FALSE(job.IsCompleted());
            CHECK_FALSE(job.IsPaused());
            CHECK(pauses == resumes);
        }
    }

    SECTION("completion")
    {
        for( int iteration = 0; iteration != 64; ++iteration ) {
            JobUTManualTerminalJob job;
            std::atomic_int pauses{0};
            std::atomic_int resumes{0};
            job.SetPauseCallback([&] { ++pauses; });
            job.SetResumeCallback([&] { ++resumes; });

            std::barrier start{3};
            std::thread pauser{[&] {
                start.arrive_and_wait();
                job.Pause();
            }};
            std::thread completer{[&] {
                start.arrive_and_wait();
                job.Complete();
            }};
            start.arrive_and_wait();
            pauser.join();
            completer.join();

            CHECK(job.IsCompleted());
            CHECK_FALSE(job.IsStopped());
            CHECK_FALSE(job.IsPaused());
            CHECK(pauses == resumes);
        }
    }
}

TEST_CASE(PREFIX "terminal resume notification cannot overtake pause notification", "[job]")
{
    for( const bool stop : {false, true} ) {
        CAPTURE(stop);
        JobUTManualTerminalJob job;
        auto state = std::make_shared<JobUTTransitionOrderState>();
        std::function<void()> pause_callback{JobUTSlowPauseCallback{state}};
        job.SetPauseCallback(std::move(pause_callback));
        job.SetResumeCallback([state] {
            const auto guard = std::lock_guard{state->mutex};
            state->notifications.emplace_back("resume");
        });
        {
            const auto guard = std::lock_guard{state->mutex};
            state->block_next_copy = true;
        }

        std::thread pauser{[&] { job.Pause(); }};
        bool copy_entered = false;
        {
            std::unique_lock lock{state->mutex};
            copy_entered = state->cv.wait_for(lock, 5s, [&] { return state->copy_entered; });
            if( !copy_entered )
                state->release_copy = true;
        }
        if( !copy_entered ) {
            state->cv.notify_all();
            pauser.join();
        }
        REQUIRE(copy_entered);

        std::atomic_bool terminal_started{false};
        std::thread terminator{[&] {
            terminal_started = true;
            if( stop )
                (void)job.Stop();
            else
                job.Complete();
        }};
        while( !terminal_started )
            std::this_thread::yield();
        std::this_thread::sleep_for(25ms);

        {
            const auto guard = std::lock_guard{state->mutex};
            state->release_copy = true;
        }
        state->cv.notify_all();
        pauser.join();
        terminator.join();

        REQUIRE(state->notifications.size() == 2);
        CHECK(state->notifications[0] == "pause");
        CHECK(state->notifications[1] == "resume");
        CHECK(job.IsStopped() == stop);
        CHECK(job.IsCompleted() != stop);
        CHECK_FALSE(job.IsPaused());
    }
}

TEST_CASE(PREFIX "Operation emits distinct pause and resume notifications", "[job]")
{
    auto operation = std::make_shared<JobUTObservableOperation>();
    std::atomic_int pauses{0};
    std::atomic_int resumes{0};
    operation->ObserveUnticketed(Operation::NotifyAboutPause, [&] { ++pauses; });
    operation->ObserveUnticketed(Operation::NotifyAboutResume, [&] { ++resumes; });

    operation->Start();
    REQUIRE(operation->WaitUntilEntered(5s));
    operation->Pause();
    CHECK(operation->State() == OperationState::Paused);
    CHECK(pauses == 1);
    CHECK(resumes == 0);

    operation->Resume();
    CHECK(operation->State() == OperationState::Running);
    CHECK(pauses == 1);
    CHECK(resumes == 1);

    operation->Stop();
    REQUIRE(operation->Wait(5s));
    CHECK(operation->State() == OperationState::Stopped);
}

TEST_CASE(PREFIX "Operation stop closes both sides of the dialog registration race", "[job]")
{
    const auto wait_until = [](const auto &_predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while( !_predicate() && std::chrono::steady_clock::now() < deadline )
            std::this_thread::yield();
        return _predicate();
    };

    auto operation = std::make_shared<JobUTDialogWaitingOperation>();
    auto response = std::make_shared<AsyncDialogResponse>();
    std::atomic_bool waiter_finished{false};

    SECTION("response registers before stop")
    {
        std::thread waiter{[&] {
            operation->WaitForResponse(response);
            waiter_finished = true;
        }};

        const bool registered = wait_until([&] { return operation->IsWaitingForUIResponse(); });
        operation->Stop();
        const bool finished_after_stop = wait_until([&] { return waiter_finished.load(); });
        if( !finished_after_stop )
            response->Abort();
        waiter.join();

        REQUIRE(registered);
        CHECK(finished_after_stop);
    }

    SECTION("stop wins before response registration")
    {
        operation->Stop();
        std::thread waiter{[&] {
            operation->WaitForResponse(response);
            waiter_finished = true;
        }};

        const bool finished_after_registration = wait_until([&] { return waiter_finished.load(); });
        if( !finished_after_registration )
            response->Abort();
        waiter.join();

        CHECK(finished_after_registration);
    }

    REQUIRE(response->response);
    CHECK(response->response == NSModalResponseAbort);
    CHECK(operation->State() == OperationState::Stopped);
}

TEST_CASE(PREFIX "resolved dialog responses are terminal and suppress late presentation", "[job]")
{
    REQUIRE(dispatch_is_main_queue());
    auto operation = std::make_shared<JobUTDialogWaitingOperation>();
    auto response = std::make_shared<AsyncDialogResponse>();
    response->Abort();
    response->Commit(NSModalResponseRetry);

    bool presented = false;
    operation->DispatchForTest(response, [&] { presented = true; });

    CHECK(response->IsResolved());
    CHECK(response->response == NSModalResponseAbort);
    CHECK_FALSE(presented);
}

} // namespace nc::ops

#undef PREFIX
