// Copyright (C) 2021-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"
#include "../source/Pool.h"
#include "../source/Job.h"
#include "../source/Operation.h"
#include <Base/mach_time.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace PoolTests {

using namespace nc;
using namespace nc::ops;
using namespace std::chrono_literals;
using VecOp = std::vector<std::shared_ptr<Operation>>;

#define PREFIX "nc::ops::Pool "

static bool check_until_or_die(std::function<bool()> _predicate, std::chrono::nanoseconds _deadline)
{
    assert(_predicate);
    const auto _poll_period = std::chrono::microseconds(10);
    const auto deadline = nc::base::machtime() + _deadline;
    while( true ) {
        if( _predicate() )
            return true;
        if( nc::base::machtime() >= deadline )
            return false;
        std::this_thread::sleep_for(_poll_period);
    }
}

TEST_CASE(PREFIX "Is constructible and empty by default")
{
    auto pool = Pool::Make();
    CHECK(pool->Empty());
    CHECK(pool->OperationsCount() == 0);
    CHECK(pool->RunningOperationsCount() == 0);
    CHECK(pool->Operations().empty());
    CHECK(pool->RunningOperations().empty());
    CHECK(pool->IsInteractive() == false);
}

TEST_CASE(PREFIX "Enques and reports the operation back as running")
{
    struct MyJob : public Job {
        void Perform() override
        {
            while( !done )
                std::this_thread::sleep_for(std::chrono::microseconds{100});
            SetCompleted();
        }
        std::atomic_bool done{false};
    };
    struct MyOperation : public Operation {
        ~MyOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        MyJob job;
    };

    auto pool = Pool::Make();

    // add an operation and check it's running and reported
    auto op = std::make_shared<MyOperation>();
    CHECK(op->State() == nc::ops::OperationState::Cold);

    pool->Enqueue(op);
    CHECK(op->State() == nc::ops::OperationState::Running);
    CHECK(pool->Empty() == false);
    CHECK(pool->OperationsCount() == 1);
    CHECK(pool->RunningOperationsCount() == 1);
    CHECK(pool->Operations() == VecOp{op});
    CHECK(pool->RunningOperations() == VecOp{op});

    // now finish the operation and wait for the pool to drain
    op->job.done = true;
    CHECK(check_until_or_die([&] { return pool->Empty(); }, 1s));
    CHECK(op->State() == nc::ops::OperationState::Completed);
    CHECK(pool->Empty() == true);
    CHECK(pool->OperationsCount() == 0);
    CHECK(pool->RunningOperationsCount() == 0);
    CHECK(pool->Operations().empty());
    CHECK(pool->RunningOperations().empty());
}

TEST_CASE(PREFIX "Keeps an enqueued operation alive through worker teardown")
{
    struct MyJob : public Job {
        explicit MyJob(std::shared_ptr<std::atomic_bool> _done) : done{std::move(_done)} {}

        void Perform() override
        {
            while( !*done )
                std::this_thread::sleep_for(std::chrono::microseconds{100});
            SetCompleted();
        }

        std::shared_ptr<std::atomic_bool> done;
    };
    struct MyOperation : public Operation {
        explicit MyOperation(std::shared_ptr<std::atomic_bool> _done) : job{std::move(_done)} {}
        ~MyOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        MyJob job;
    };

    auto pool = Pool::Make();
    const auto done = std::make_shared<std::atomic_bool>(false);
    auto operation = std::make_shared<MyOperation>(done);
    const auto weak_operation = std::weak_ptr<Operation>{operation};

    pool->Enqueue(operation);
    REQUIRE(operation->State() == OperationState::Running);
    operation.reset();

    *done = true;
    CHECK(check_until_or_die([&] { return pool->Empty(); }, 1s));
    CHECK(check_until_or_die([&] { return weak_operation.expired(); }, 1s));
}

TEST_CASE(PREFIX "Drains and starts pending work when lifecycle callbacks throw")
{
    struct ThrowingJob : public Job {
        explicit ThrowingJob(std::shared_ptr<std::atomic_bool> _done) : done{std::move(_done)} {}
        void Perform() override
        {
            while( !*done )
                std::this_thread::sleep_for(100us);
            SetCompleted();
        }
        std::shared_ptr<std::atomic_bool> done;
    };
    struct ThrowingOperation : public Operation {
        ThrowingOperation(std::shared_ptr<std::atomic_bool> _done, bool _throw_on_finish)
            : job{std::move(_done)}, throw_on_finish{_throw_on_finish}
        {
        }
        ~ThrowingOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        void OnJobFinished() override
        {
            if( throw_on_finish )
                throw std::runtime_error{"intentional finish failure"};
        }
        ThrowingJob job;
        bool throw_on_finish;
    };

    auto pool = Pool::Make();
    pool->SetConcurrency(1);
    pool->ObserveUnticketed(Pool::NotifyAboutRemoval, [] { throw std::runtime_error{"intentional observer failure"}; });

    const auto first_done = std::make_shared<std::atomic_bool>(false);
    const auto second_done = std::make_shared<std::atomic_bool>(false);
    auto first = std::make_shared<ThrowingOperation>(first_done, true);
    auto second = std::make_shared<ThrowingOperation>(second_done, false);
    first->ObserveUnticketed(Operation::NotifyAboutFinish,
                             [] { throw std::runtime_error{"intentional operation observer failure"}; });

    pool->Enqueue(first);
    pool->Enqueue(second);
    *first_done = true;
    REQUIRE(check_until_or_die([&] { return second->State() == OperationState::Running; }, 1s));
    *second_done = true;
    CHECK(check_until_or_die([&] { return pool->Empty(); }, 1s));
}

TEST_CASE(PREFIX "Obeys concurrency settings")
{
    auto pool = Pool::Make();

    struct MyJob : public Job {
        void Perform() override
        {
            while( !done )
                std::this_thread::sleep_for(std::chrono::microseconds{100});
            SetCompleted();
        }
        std::atomic_bool done{false};
    };
    struct MyOperation : public Operation {
        ~MyOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        MyJob job;
    };
    auto op1 = std::make_shared<MyOperation>();
    auto op2 = std::make_shared<MyOperation>();
    auto op3 = std::make_shared<MyOperation>();
    SECTION("Concurrency = 5")
    {
        pool->SetConcurrency(5);
        pool->Enqueue(op1);
        pool->Enqueue(op2);
        pool->Enqueue(op3);
        CHECK(op1->State() == nc::ops::OperationState::Running);
        CHECK(op2->State() == nc::ops::OperationState::Running);
        CHECK(op3->State() == nc::ops::OperationState::Running);
        CHECK(pool->Empty() == false);
        CHECK(pool->OperationsCount() == 3);
        CHECK(pool->RunningOperationsCount() == 3);
        CHECK(pool->Operations() == VecOp{op1, op2, op3});
        CHECK(pool->RunningOperations() == VecOp{op1, op2, op3});
    }
    SECTION("Concurrency = 2")
    {
        pool->SetConcurrency(2);
        pool->Enqueue(op1);
        pool->Enqueue(op2);
        pool->Enqueue(op3);
        CHECK(op1->State() == nc::ops::OperationState::Running);
        CHECK(op2->State() == nc::ops::OperationState::Running);
        CHECK(op3->State() == nc::ops::OperationState::Cold);
        CHECK(pool->Empty() == false);
        CHECK(pool->OperationsCount() == 3);
        CHECK(pool->RunningOperationsCount() == 2);
        CHECK(pool->Operations() == VecOp{op1, op2, op3});
        CHECK(pool->RunningOperations() == VecOp{op1, op2});
    }
    SECTION("Concurrency = 1")
    {
        pool->SetConcurrency(1);
        pool->Enqueue(op1);
        pool->Enqueue(op2);
        pool->Enqueue(op3);
        CHECK(op1->State() == nc::ops::OperationState::Running);
        CHECK(op2->State() == nc::ops::OperationState::Cold);
        CHECK(op3->State() == nc::ops::OperationState::Cold);
        CHECK(pool->Empty() == false);
        CHECK(pool->OperationsCount() == 3);
        CHECK(pool->RunningOperationsCount() == 1);
        CHECK(pool->Operations() == VecOp{op1, op2, op3});
        CHECK(pool->RunningOperations() == VecOp{op1});
    }
    op1->job.done = true;
    op2->job.done = true;
    op3->job.done = true;
}

TEST_CASE(PREFIX "Drains pending queues as operation complete")
{
    auto pool = Pool::Make();
    pool->SetConcurrency(1);

    struct MyJob : public Job {
        void Perform() override
        {
            while( !done )
                std::this_thread::sleep_for(std::chrono::microseconds{100});
            SetCompleted();
        }
        std::atomic_bool done{false};
    };
    struct MyOperation : public Operation {
        ~MyOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        MyJob job;
    };

    auto op1 = std::make_shared<MyOperation>();
    auto op2 = std::make_shared<MyOperation>();
    auto op3 = std::make_shared<MyOperation>();
    pool->Enqueue(op1);
    pool->Enqueue(op2);
    pool->Enqueue(op3);

    CHECK(op1->State() == nc::ops::OperationState::Running);
    CHECK(op2->State() == nc::ops::OperationState::Cold);
    CHECK(op3->State() == nc::ops::OperationState::Cold);

    op1->job.done = true;
    CHECK(check_until_or_die([&] { return op2->State() == nc::ops::OperationState::Running; }, 1s));
    CHECK(op1->State() == nc::ops::OperationState::Completed);
    CHECK(op2->State() == nc::ops::OperationState::Running);
    CHECK(op3->State() == nc::ops::OperationState::Cold);

    op2->job.done = true;
    CHECK(check_until_or_die([&] { return op3->State() == nc::ops::OperationState::Running; }, 1s));
    CHECK(op1->State() == nc::ops::OperationState::Completed);
    CHECK(op2->State() == nc::ops::OperationState::Completed);
    CHECK(op3->State() == nc::ops::OperationState::Running);

    op3->job.done = true;
    CHECK(check_until_or_die([&] { return pool->Empty(); }, 1s));
    CHECK(op1->State() == nc::ops::OperationState::Completed);
    CHECK(op2->State() == nc::ops::OperationState::Completed);
    CHECK(op3->State() == nc::ops::OperationState::Completed);
}

TEST_CASE(PREFIX "Does enqueueing as the callback says")
{
    auto pool = Pool::Make();
    struct MyJob : public Job {
        void Perform() override
        {
            while( !done )
                std::this_thread::sleep_for(std::chrono::microseconds{100});
            SetCompleted();
        }
        std::atomic_bool done{false};
    };
    struct MyOperation : public Operation {
        ~MyOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        MyJob job;
    };

    auto op1 = std::make_shared<MyOperation>();
    auto op2 = std::make_shared<MyOperation>();
    bool enqueue_1st = true;
    bool enqueue_2nd = true;
    pool->SetEnqueuingCallback([&](const Operation &_operation) {
        if( &_operation == op1.get() )
            return enqueue_1st;
        if( &_operation == op2.get() )
            return enqueue_2nd;
        throw std::logic_error("");
    });
    SECTION("concurrency = 1")
    {
        pool->SetConcurrency(1);
        SECTION("true, true")
        {
            enqueue_1st = true;
            enqueue_2nd = true;
            pool->Enqueue(op1);
            pool->Enqueue(op2);
            CHECK(op1->State() == nc::ops::OperationState::Running);
            CHECK(op2->State() == nc::ops::OperationState::Cold);
            op1->job.done = true;
            CHECK(check_until_or_die([&] { return op2->State() == nc::ops::OperationState::Running; }, 1s));
            CHECK(op1->State() == nc::ops::OperationState::Completed);
            CHECK(op2->State() == nc::ops::OperationState::Running);
            op2->job.done = true;
            CHECK(check_until_or_die([&] { return pool->Empty(); }, 1s));
            CHECK(op1->State() == nc::ops::OperationState::Completed);
            CHECK(op2->State() == nc::ops::OperationState::Completed);
        }
        SECTION("true, false")
        {
            enqueue_1st = true;
            enqueue_2nd = false;
            pool->Enqueue(op1);
            pool->Enqueue(op2);
            CHECK(op1->State() == nc::ops::OperationState::Running);
            CHECK(op2->State() == nc::ops::OperationState::Running);
            op1->job.done = true;
            op2->job.done = true;
            CHECK(check_until_or_die([&] { return pool->Empty(); }, 1s));
            CHECK(op1->State() == nc::ops::OperationState::Completed);
            CHECK(op2->State() == nc::ops::OperationState::Completed);
        }
        SECTION("false, false")
        {
            enqueue_1st = false;
            enqueue_2nd = false;
            pool->Enqueue(op1);
            pool->Enqueue(op2);
            CHECK(op1->State() == nc::ops::OperationState::Running);
            CHECK(op2->State() == nc::ops::OperationState::Running);
            op1->job.done = true;
            op2->job.done = true;
            CHECK(check_until_or_die([&] { return pool->Empty(); }, 1s));
            CHECK(op1->State() == nc::ops::OperationState::Completed);
            CHECK(op2->State() == nc::ops::OperationState::Completed);
        }
        SECTION("false, true")
        {
            enqueue_1st = false;
            enqueue_2nd = true;
            pool->Enqueue(op1);
            pool->Enqueue(op2);
            CHECK(op1->State() == nc::ops::OperationState::Running);
            CHECK(op2->State() == nc::ops::OperationState::Cold);
            op1->job.done = true;
            CHECK(check_until_or_die([&] { return op2->State() == nc::ops::OperationState::Running; }, 1s));
            CHECK(op1->State() == nc::ops::OperationState::Completed);
            CHECK(op2->State() == nc::ops::OperationState::Running);
            op2->job.done = true;
            CHECK(check_until_or_die([&] { return pool->Empty(); }, 1s));
            CHECK(op1->State() == nc::ops::OperationState::Completed);
            CHECK(op2->State() == nc::ops::OperationState::Completed);
        }
    }
    SECTION("concurrency = 2")
    {
        pool->SetConcurrency(2);
        SECTION("false, false")
        {
            enqueue_1st = false;
            enqueue_2nd = false;
        }
        SECTION("false, true")
        {
            enqueue_1st = false;
            enqueue_2nd = true;
        }
        SECTION("true, false")
        {
            enqueue_1st = true;
            enqueue_2nd = false;
        }
        SECTION("true, true")
        {
            enqueue_1st = true;
            enqueue_2nd = true;
        }
        pool->Enqueue(op1);
        pool->Enqueue(op2);
        CHECK(op1->State() == nc::ops::OperationState::Running);
        CHECK(op2->State() == nc::ops::OperationState::Running);
        op1->job.done = true;
        op2->job.done = true;
        CHECK(check_until_or_die([&] { return pool->Empty(); }, 1s));
        CHECK(op1->State() == nc::ops::OperationState::Completed);
        CHECK(op2->State() == nc::ops::OperationState::Completed);
    }
}

TEST_CASE(PREFIX "Does not partially promote pending operations when enqueueing callback throws")
{
    struct ControlledJob : public Job {
        void Perform() override
        {
            while( !done && !IsStopped() )
                std::this_thread::yield();
            if( !IsStopped() )
                SetCompleted();
        }
        std::atomic_bool done{false};
    };
    struct ControlledOperation : public Operation {
        ~ControlledOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        ControlledJob job;
    };

    auto pool = Pool::Make();
    pool->SetConcurrency(1);
    auto running = std::make_shared<ControlledOperation>();
    auto first_pending = std::make_shared<ControlledOperation>();
    auto second_pending = std::make_shared<ControlledOperation>();
    bool inject_failure = false;
    int decisions_during_failure = 0;
    pool->SetEnqueuingCallback([&](const Operation &_operation) {
        if( !inject_failure )
            return true;
        ++decisions_during_failure;
        if( &_operation == first_pending.get() )
            return false;
        if( &_operation == second_pending.get() )
            throw std::runtime_error{"intentional enqueueing decision failure"};
        return true;
    });

    pool->Enqueue(running);
    pool->Enqueue(first_pending);
    REQUIRE(running->State() == OperationState::Running);
    REQUIRE(first_pending->State() == OperationState::Cold);

    inject_failure = true;
    CHECK(pool->TryEnqueue(second_pending) == PoolEnqueueResult::Accepted);

    CHECK(decisions_during_failure == 2);
    CHECK(running->State() == OperationState::Running);
    CHECK(first_pending->State() == OperationState::Cold);
    CHECK(second_pending->State() == OperationState::Cold);
    CHECK(pool->OperationsCount() == 3);
    CHECK(pool->RunningOperationsCount() == 1);
    CHECK(pool->Operations() == VecOp{running, first_pending, second_pending});
    CHECK(pool->RunningOperations() == VecOp{running});

    inject_failure = false;
    running->job.done = true;
    first_pending->job.done = true;
    second_pending->job.done = true;
    CHECK(check_until_or_die([&] { return pool->Empty(); }, 1s));
}

TEST_CASE(PREFIX "Reports accepted admission despite observer and scheduling callback failures")
{
    struct ControlledJob : public Job {
        void Perform() override
        {
            ++perform_count;
            while( !done && !IsStopped() )
                std::this_thread::yield();
            if( !IsStopped() )
                SetCompleted();
        }
        std::atomic_int perform_count{0};
        std::atomic_bool done{false};
    };
    struct ControlledOperation : public Operation {
        ~ControlledOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        ControlledJob job;
    };

    auto pool = Pool::Make();
    pool->ObserveUnticketed(Pool::NotifyAboutAddition,
                            [] { throw std::runtime_error{"intentional addition observer failure"}; });
    pool->SetEnqueuingCallback(
        [](const Operation &) -> bool { throw std::runtime_error{"intentional enqueue policy failure"}; });
    auto operation = std::make_shared<ControlledOperation>();

    CHECK(pool->TryEnqueue(operation) == PoolEnqueueResult::Accepted);
    CHECK(pool->Operations() == VecOp{operation});
    CHECK(pool->RunningOperations() == VecOp{operation});
    CHECK(operation->State() == OperationState::Running);
    CHECK(pool->TryEnqueue(operation) == PoolEnqueueResult::Duplicate);
    CHECK(pool->OperationsCount() == 1);

    pool->StopAndWaitForShutdown();
    CHECK(pool->Empty());
}

TEST_CASE(PREFIX "Reports typed pre-admission rejection reasons")
{
    struct ImmediateJob : public Job {
        void Perform() override { SetCompleted(); }
    };
    struct ImmediateOperation : public Operation {
        ~ImmediateOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        ImmediateJob job;
    };

    auto pool = Pool::Make();
    CHECK(pool->TryEnqueue(std::shared_ptr<Operation>{}) == PoolEnqueueResult::NotCold);

    auto completed = std::make_shared<ImmediateOperation>();
    completed->Start();
    REQUIRE(completed->Wait(1s));
    REQUIRE(completed->State() == OperationState::Completed);
    CHECK(pool->TryEnqueue(completed) == PoolEnqueueResult::NotCold);

    pool->StopAndWaitForShutdown();
    auto late = std::make_shared<ImmediateOperation>();
    CHECK(pool->TryEnqueue(late) == PoolEnqueueResult::ShuttingDown);
    CHECK(late->State() == OperationState::Cold);
}

TEST_CASE(PREFIX "Finalizes terminal operations before removal")
{
    struct ControlledJob : public Job {
        void Perform() override
        {
            while( !done )
                std::this_thread::yield();
            SetCompleted();
        }
        std::atomic_bool done{false};
    };
    struct ControlledOperation : public Operation {
        ~ControlledOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        ControlledJob job;
    };
    struct Evidence {
        std::mutex lock;
        std::vector<std::string> events;
        int finalizing_count = -1;
        int running_count = -1;
        int operations_count = -1;
        bool finalizing_operation_visible = false;
    };

    auto pool = Pool::Make();
    auto operation = std::make_shared<ControlledOperation>();
    auto evidence = std::make_shared<Evidence>();
    const auto weak_pool = std::weak_ptr<Pool>{pool};
    pool->ObserveUnticketed(Pool::NotifyAboutRemoval, [evidence] {
        const auto guard = std::lock_guard{evidence->lock};
        evidence->events.emplace_back("removal");
    });

    REQUIRE(pool->TryEnqueue(operation, [weak_pool, evidence](const std::shared_ptr<Operation> &_operation) {
        const auto pool = weak_pool.lock();
        if( !pool )
            return PoolTerminalFinalizationDecision::Retain;
        const auto finalizing_operations = pool->FinalizingOperations();
        const auto guard = std::lock_guard{evidence->lock};
        evidence->events.emplace_back("finalizer");
        evidence->finalizing_count = pool->FinalizingOperationsCount();
        evidence->running_count = pool->RunningOperationsCount();
        evidence->operations_count = pool->OperationsCount();
        evidence->finalizing_operation_visible = finalizing_operations == VecOp{_operation};
        return PoolTerminalFinalizationDecision::Release;
    }) == PoolEnqueueResult::Accepted);
    operation->job.done = true;

    REQUIRE(check_until_or_die(
        [&] {
            const auto guard = std::lock_guard{evidence->lock};
            return evidence->events.size() == 2;
        },
        1s));
    {
        const auto guard = std::lock_guard{evidence->lock};
        CHECK(evidence->events == std::vector<std::string>{"finalizer", "removal"});
        CHECK(evidence->finalizing_count == 1);
        CHECK(evidence->running_count == 0);
        CHECK(evidence->operations_count == 1);
        CHECK(evidence->finalizing_operation_visible);
    }
    CHECK(pool->Empty());
}

TEST_CASE(PREFIX "Release without completion removes terminal work and starts the next operation")
{
    struct ControlledJob : public Job {
        void Perform() override
        {
            ++perform_count;
            while( !done )
                std::this_thread::yield();
            SetCompleted();
        }
        std::atomic_int perform_count{0};
        std::atomic_bool done{false};
    };
    struct ControlledOperation : public Operation {
        ~ControlledOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        ControlledJob job;
    };

    auto pool = Pool::Make();
    pool->SetConcurrency(1);
    auto first = std::make_shared<ControlledOperation>();
    auto next = std::make_shared<ControlledOperation>();
    std::atomic_int removals{0};
    std::atomic_int next_starts{0};
    std::atomic_int completion_callbacks{0};
    pool->ObserveUnticketed(Pool::NotifyAboutRemoval, [&] { ++removals; });
    next->ObserveUnticketed(Operation::NotifyAboutStart, [&] { ++next_starts; });
    pool->SetOperationCompletionCallback([&](const std::shared_ptr<Operation> &) { ++completion_callbacks; });

    REQUIRE(pool->TryEnqueue(first, [](const std::shared_ptr<Operation> &) {
        return PoolTerminalFinalizationDecision::ReleaseWithoutCompletion;
    }) == PoolEnqueueResult::Accepted);
    REQUIRE(pool->TryEnqueue(next, [](const std::shared_ptr<Operation> &) {
        return PoolTerminalFinalizationDecision::Release;
    }) == PoolEnqueueResult::Accepted);
    REQUIRE(first->State() == OperationState::Running);
    REQUIRE(next->State() == OperationState::Cold);

    first->job.done = true;
    REQUIRE(first->Wait(1s));

    CHECK(first->State() == OperationState::Completed);
    CHECK(pool->Operations() == VecOp{next});
    CHECK(pool->RunningOperations() == VecOp{next});
    CHECK(next->State() == OperationState::Running);
    CHECK(check_until_or_die([&] { return next->job.perform_count == 1; }, 1s));
    CHECK(next_starts == 1);
    CHECK(removals == 1);
    CHECK(completion_callbacks == 0);

    next->job.done = true;
    REQUIRE(next->Wait(1s));

    CHECK(next->State() == OperationState::Completed);
    CHECK(pool->Empty());
    CHECK(removals == 2);
    CHECK(completion_callbacks == 1);
}

TEST_CASE(PREFIX "Retains terminal work when its finalizer returns an invalid decision")
{
    struct ControlledJob : public Job {
        void Perform() override
        {
            while( !done )
                std::this_thread::yield();
            SetCompleted();
        }
        std::atomic_bool done{false};
    };
    struct ControlledOperation : public Operation {
        ~ControlledOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        ControlledJob job;
    };

    auto pool = Pool::Make();
    auto operation = std::make_shared<ControlledOperation>();
    std::atomic_int removals{0};
    std::atomic_int completion_callbacks{0};
    pool->ObserveUnticketed(Pool::NotifyAboutRemoval, [&] { ++removals; });
    pool->SetOperationCompletionCallback([&](const std::shared_ptr<Operation> &) { ++completion_callbacks; });

    REQUIRE(pool->TryEnqueue(operation, [](const std::shared_ptr<Operation> &) {
        return static_cast<PoolTerminalFinalizationDecision>(0xff);
    }) == PoolEnqueueResult::Accepted);
    operation->job.done = true;
    REQUIRE(operation->Wait(1s));

    CHECK(operation->State() == OperationState::Completed);
    CHECK(pool->FinalizingOperations() == VecOp{operation});
    CHECK(pool->RunningOperations().empty());
    CHECK(pool->Operations() == VecOp{operation});
    CHECK(pool->FinalizingOperationsCount() == 1);
    CHECK(pool->OperationsCount() == 1);
    CHECK_FALSE(pool->Empty());
    CHECK(removals == 0);
    CHECK(completion_callbacks == 0);
}

TEST_CASE(PREFIX "Retains failed finalization for exactly one successful retry")
{
    struct ControlledJob : public Job {
        void Perform() override
        {
            while( !done )
                std::this_thread::yield();
            SetCompleted();
        }
        std::atomic_bool done{false};
    };
    struct ControlledOperation : public Operation {
        ~ControlledOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        ControlledJob job;
    };
    struct FinalizerState {
        std::mutex lock;
        std::condition_variable cv;
        int attempts = 0;
        bool first_entered = false;
        bool release_first_attempt = false;
    };

    auto pool = Pool::Make();
    auto operation = std::make_shared<ControlledOperation>();
    auto finalizer_state = std::make_shared<FinalizerState>();
    std::atomic_int removals{0};
    pool->ObserveUnticketed(Pool::NotifyAboutRemoval, [&] { ++removals; });

    REQUIRE(pool->TryEnqueue(operation, [finalizer_state](const std::shared_ptr<Operation> &) {
        auto guard = std::unique_lock{finalizer_state->lock};
        ++finalizer_state->attempts;
        if( finalizer_state->attempts == 1 ) {
            finalizer_state->first_entered = true;
            finalizer_state->cv.notify_all();
            finalizer_state->cv.wait(guard, [&] { return finalizer_state->release_first_attempt; });
            throw std::runtime_error{"intentional durable finalizer failure"};
        }
        return PoolTerminalFinalizationDecision::Release;
    }) == PoolEnqueueResult::Accepted);
    operation->job.done = true;

    {
        auto guard = std::unique_lock{finalizer_state->lock};
        REQUIRE(finalizer_state->cv.wait_for(guard, 1s, [&] { return finalizer_state->first_entered; }));
    }
    CHECK(pool->FinalizingOperations() == VecOp{operation});
    CHECK(pool->RunningOperations().empty());
    CHECK(pool->Operations() == VecOp{operation});
    CHECK(removals == 0);
    CHECK(pool->RetryFinalization(operation) == PoolRetryFinalizationResult::InProgress);

    {
        const auto guard = std::lock_guard{finalizer_state->lock};
        finalizer_state->release_first_attempt = true;
    }
    finalizer_state->cv.notify_all();

    REQUIRE(operation->Wait(1s));
    CHECK(pool->FinalizingOperations() == VecOp{operation});
    CHECK(removals == 0);
    CHECK(pool->RetryFinalization(operation) == PoolRetryFinalizationResult::Released);
    CHECK(removals == 1);
    CHECK(pool->Empty());
    CHECK(pool->RetryFinalization(operation) == PoolRetryFinalizationResult::NotFinalizing);
    CHECK(pool->RetryFinalization(operation) == PoolRetryFinalizationResult::NotFinalizing);
    CHECK(removals == 1);
    {
        const auto guard = std::lock_guard{finalizer_state->lock};
        CHECK(finalizer_state->attempts == 2);
    }
}

TEST_CASE(PREFIX "Shutdown runs finalizers and preserves retained terminal residency")
{
    struct StoppableJob : public Job {
        void Perform() override
        {
            ++perform_count;
            while( !IsStopped() )
                std::this_thread::yield();
        }
        std::atomic_int perform_count{0};
    };
    struct StoppableOperation : public Operation {
        ~StoppableOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        StoppableJob job;
    };

    auto pool = Pool::Make();
    pool->SetConcurrency(1);
    auto running = std::make_shared<StoppableOperation>();
    auto pending = std::make_shared<StoppableOperation>();
    const auto running_ptr = running.get();
    const auto pending_ptr = pending.get();
    std::atomic_int running_finalizations{0};
    std::atomic_int pending_finalizations{0};
    std::atomic_int removals{0};
    pool->ObserveUnticketed(Pool::NotifyAboutRemoval, [&] { ++removals; });
    const Pool::TerminalFinalizer finalizer = [&](const std::shared_ptr<Operation> &_operation) {
        if( _operation.get() == running_ptr ) {
            ++running_finalizations;
            return PoolTerminalFinalizationDecision::Release;
        }
        if( _operation.get() != pending_ptr )
            return PoolTerminalFinalizationDecision::Retain;
        return ++pending_finalizations == 1 ? PoolTerminalFinalizationDecision::Retain
                                            : PoolTerminalFinalizationDecision::Release;
    };

    REQUIRE(pool->TryEnqueue(running, finalizer) == PoolEnqueueResult::Accepted);
    REQUIRE(pool->TryEnqueue(pending, finalizer) == PoolEnqueueResult::Accepted);
    REQUIRE(running->State() == OperationState::Running);
    REQUIRE(pending->State() == OperationState::Cold);

    pool->StopAndWaitForShutdown();

    CHECK(running_finalizations == 1);
    CHECK(pending_finalizations == 1);
    CHECK(removals == 1);
    CHECK(pending->job.perform_count == 0);
    CHECK(pool->FinalizingOperations() == VecOp{pending});
    CHECK(pool->RunningOperations().empty());
    CHECK(pool->Operations() == VecOp{pending});
    CHECK(pool->OperationsCount() == 1);
    CHECK_FALSE(pool->Empty());

    CHECK(pool->RetryFinalization(pending) == PoolRetryFinalizationResult::Released);
    CHECK(pending_finalizations == 2);
    CHECK(removals == 2);
    CHECK(pool->Empty());
}

TEST_CASE(PREFIX "Does not start pending work during shutdown")
{
    struct FinishState {
        std::mutex lock;
        std::condition_variable cv;
        bool finish_observed = false;
    };
    struct FirstJob : public Job {
        explicit FirstJob(FinishState &_state) : state{_state} {}
        void Perform() override
        {
            while( !IsStopped() )
                std::this_thread::yield();
        }
        void OnStopped() override
        {
            auto guard = std::unique_lock{state.lock};
            state.cv.wait_for(guard, 1s, [this] { return state.finish_observed; });
        }
        FinishState &state;
    };
    struct PendingJob : public Job {
        void Perform() override
        {
            ++perform_count;
            while( !IsStopped() )
                std::this_thread::yield();
        }
        std::atomic_int perform_count{0};
    };
    struct FirstOperation : public Operation {
        explicit FirstOperation(FinishState &_state) : job{_state} {}
        ~FirstOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        FirstJob job;
    };
    struct PendingOperation : public Operation {
        ~PendingOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        PendingJob job;
    };

    auto pool = Pool::Make();
    pool->SetConcurrency(1);
    FinishState finish_state;
    auto first = std::make_shared<FirstOperation>(finish_state);
    auto pending = std::make_shared<PendingOperation>();
    std::atomic_int start_count{0};
    pending->ObserveUnticketed(Operation::NotifyAboutStart, [&] { ++start_count; });

    pool->Enqueue(first);
    first->ObserveUnticketed(Operation::NotifyAboutFinish, [&] {
        {
            const auto guard = std::lock_guard{finish_state.lock};
            finish_state.finish_observed = true;
        }
        finish_state.cv.notify_all();
    });
    pool->Enqueue(pending);
    REQUIRE(first->State() == OperationState::Running);
    REQUIRE(pending->State() == OperationState::Cold);

    pool->StopAndWaitForShutdown();

    CHECK(finish_state.finish_observed);
    CHECK(start_count == 0);
    CHECK(pending->job.perform_count == 0);
    CHECK(pending->State() == OperationState::Stopped);
    CHECK(pool->Empty());
}

TEST_CASE(PREFIX "Rejects enqueue after shutdown admission closes")
{
    struct StopState {
        std::mutex lock;
        std::condition_variable cv;
        bool stop_entered = false;
        bool release_stop = false;
    };
    struct BlockingStopJob : public Job {
        explicit BlockingStopJob(StopState &_state) : state{_state} {}
        void Perform() override
        {
            while( !IsStopped() )
                std::this_thread::yield();
        }
        void OnStopped() override
        {
            auto guard = std::unique_lock{state.lock};
            state.stop_entered = true;
            state.cv.notify_all();
            state.cv.wait(guard, [this] { return state.release_stop; });
        }
        StopState &state;
    };
    struct ImmediateJob : public Job {
        void Perform() override
        {
            ++perform_count;
            SetCompleted();
        }
        std::atomic_int perform_count{0};
    };
    struct BlockingStopOperation : public Operation {
        explicit BlockingStopOperation(StopState &_state) : job{_state} {}
        ~BlockingStopOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        BlockingStopJob job;
    };
    struct ImmediateOperation : public Operation {
        ~ImmediateOperation() override { Wait(); }
        Job *GetJob() noexcept override { return &job; }
        ImmediateJob job;
    };

    auto pool = Pool::Make();
    StopState stop_state;
    auto running = std::make_shared<BlockingStopOperation>(stop_state);
    auto late = std::make_shared<ImmediateOperation>();
    std::atomic_int start_count{0};
    late->ObserveUnticketed(Operation::NotifyAboutStart, [&] { ++start_count; });
    pool->Enqueue(running);
    REQUIRE(running->State() == OperationState::Running);

    auto shutdown = std::thread{[&] { pool->StopAndWaitForShutdown(); }};
    bool stop_entered = false;
    {
        auto guard = std::unique_lock{stop_state.lock};
        stop_entered = stop_state.cv.wait_for(guard, 1s, [&] { return stop_state.stop_entered; });
    }
    if( stop_entered )
        pool->Enqueue(late);
    {
        const auto guard = std::lock_guard{stop_state.lock};
        stop_state.release_stop = true;
    }
    stop_state.cv.notify_all();
    shutdown.join();

    REQUIRE(stop_entered);
    pool->Enqueue(late);
    CHECK(start_count == 0);
    CHECK(late->job.perform_count == 0);
    CHECK(late->State() == OperationState::Cold);
    CHECK(pool->Empty());
}

} // namespace PoolTests

#undef PREFIX
