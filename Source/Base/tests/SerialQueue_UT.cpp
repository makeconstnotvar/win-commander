// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "UnitTests_main.h"
#include <Base/SerialQueue.h>
#include <atomic>

#define PREFIX "SerialQueue "

namespace {

struct CallableConstructionFailure {
};

struct TaskExecutionFailure {
};

struct ObserverFailure {
};

struct ThrowingMoveCallable {
    ThrowingMoveCallable() = default;
    ThrowingMoveCallable(const ThrowingMoveCallable &) = default;
    ThrowingMoveCallable(ThrowingMoveCallable &&) { throw CallableConstructionFailure{}; }

    void operator()() const noexcept {}
};

} // namespace

TEST_CASE(PREFIX "does not count a callable that fails to enter the queue")
{
    nc::base::SerialQueue queue;
    const ThrowingMoveCallable callable;

    CHECK_THROWS_AS(queue.Run(callable), CallableConstructionFailure);
    CHECK(queue.Empty());
}

TEST_CASE(PREFIX "restores accounting after a task throws")
{
    nc::base::SerialQueue queue;

    queue.Run([] { throw TaskExecutionFailure{}; });
    queue.Wait();

    CHECK(queue.Empty());
}

TEST_CASE(PREFIX "continues through throwing observers")
{
    nc::base::SerialQueue queue;
    std::atomic_bool task_executed = false;

    queue.SetOnWet([] { throw ObserverFailure{}; });
    queue.SetOnDry([] { throw ObserverFailure{}; });
    queue.SetOnChange([] { throw ObserverFailure{}; });

    CHECK_NOTHROW(queue.Run([&task_executed] { task_executed = true; }));
    CHECK_NOTHROW(queue.Wait());

    CHECK(task_executed);
    CHECK(queue.Empty());
}

#undef PREFIX
