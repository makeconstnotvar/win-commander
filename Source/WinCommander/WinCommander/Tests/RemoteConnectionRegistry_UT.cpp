// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Remote/RemoteConnectionRegistry.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

using nc::core::RemoteConnectionFailure;
using nc::core::RemoteConnectionRegistry;
using nc::core::RemoteConnectionStatus;
using nc::core::RemoteRetryPolicy;
using namespace std::chrono_literals;

RemoteRetryPolicy Policy()
{
    return RemoteRetryPolicy{
        .maximum_attempts = 3, .initial_backoff = 500ms, .maximum_backoff = 30'000ms, .multiplier = 2.0};
}

} // namespace

#define PREFIX "nc::core::RemoteConnectionRegistry "

TEST_CASE(PREFIX "arms a deadline once instead of restarting the wait every time it is asked")
{
    RemoteConnectionRegistry registry{Policy()};

    registry.RecordFailure("a", RemoteConnectionFailure::TimedOut, 100, "", 1'000ms);
    REQUIRE(registry.RetryDeadline("a") == 1'500ms);

    // Asking repeatedly, and at later moments, must not move the deadline. A delay recomputed on
    // every tick would restart the wait each time anybody looked, and nothing would ever come due.
    CHECK(registry.RetryDeadline("a") == 1'500ms);
    CHECK(registry.ClaimDueRetries(1'200ms).empty());
    CHECK(registry.RetryDeadline("a") == 1'500ms);
    CHECK(registry.NextDeadline() == 1'500ms);
}

TEST_CASE(PREFIX "hands a due connection out exactly once")
{
    RemoteConnectionRegistry registry{Policy()};
    registry.RecordFailure("a", RemoteConnectionFailure::Unreachable, 100, "", 1'000ms);

    // Claiming rather than reporting: two ticks that both saw the same due connection would each
    // start an attempt, and the second would race the first.
    CHECK(registry.ClaimDueRetries(1'500ms) == std::vector<std::string>{"a"});
    CHECK(registry.ClaimDueRetries(1'500ms).empty());
    CHECK(registry.RetryDeadline("a") == std::nullopt);
    CHECK(registry.NextDeadline() == std::nullopt);
    // The connection itself is still known and still says why it is waiting.
    REQUIRE(registry.State("a"));
    CHECK(registry.State("a")->last_failure == RemoteConnectionFailure::Unreachable);
}

TEST_CASE(PREFIX "arms nothing for a failure that must not be retried")
{
    RemoteConnectionRegistry registry{Policy()};

    // A refused host key is the signature of an interception. A timer that kept reaching the server
    // anyway would defeat the check entirely.
    registry.RecordFailure("a", RemoteConnectionFailure::HostVerificationFailed, 100, "", 1'000ms);
    CHECK(registry.RetryDeadline("a") == std::nullopt);
    REQUIRE(registry.State("a"));
    CHECK(registry.State("a")->status == RemoteConnectionStatus::Blocked);

    registry.RecordFailure("b", RemoteConnectionFailure::AuthenticationRejected, 100, "", 1'000ms);
    CHECK(registry.RetryDeadline("b") == std::nullopt);
    CHECK(registry.ClaimDueRetries(1'000'000ms).empty());
}

TEST_CASE(PREFIX "stops retrying once the budget is spent")
{
    RemoteConnectionRegistry registry{Policy()};

    nc::core::RemoteConnectionRegistry::Instant now = 0ms;
    for( unsigned attempt = 0; attempt < Policy().maximum_attempts; ++attempt ) {
        registry.RecordFailure("a", RemoteConnectionFailure::TimedOut, 100, "", now);
        const auto deadline = registry.RetryDeadline("a");
        REQUIRE(deadline);
        now = *deadline;
        REQUIRE(registry.ClaimDueRetries(now) == std::vector<std::string>{"a"});
    }

    registry.RecordFailure("a", RemoteConnectionFailure::TimedOut, 100, "", now);
    CHECK(registry.RetryDeadline("a") == std::nullopt);
    REQUIRE(registry.State("a"));
    // Spent budget on a retryable failure is Offline - try again later - not Blocked, which is what
    // a user has to resolve themselves.
    CHECK(registry.State("a")->status == RemoteConnectionStatus::Offline);
}

TEST_CASE(PREFIX "disarms a pending retry when the connection comes up or the user stops it")
{
    RemoteConnectionRegistry registry{Policy()};

    registry.RecordFailure("a", RemoteConnectionFailure::TimedOut, 100, "", 1'000ms);
    REQUIRE(registry.RetryDeadline("a"));
    // Leaving it armed would start an attempt against a link that is already up.
    registry.RecordConnected("a", 200, 40ms, false);
    CHECK(registry.RetryDeadline("a") == std::nullopt);
    REQUIRE(registry.State("a"));
    CHECK(registry.State("a")->status == RemoteConnectionStatus::Connected);
    CHECK(registry.State("a")->completed_attempts == 0);

    registry.RecordFailure("b", RemoteConnectionFailure::Unreachable, 100, "", 1'000ms);
    REQUIRE(registry.RetryDeadline("b"));
    // The user asked this one to stop. Reconnecting on a timer they never set would undo it.
    registry.RecordDisconnected("b");
    CHECK(registry.RetryDeadline("b") == std::nullopt);
    CHECK(registry.ClaimDueRetries(1'000'000ms).empty());
}

TEST_CASE(PREFIX "reports the earliest deadline, since that is when a timer should fire")
{
    RemoteConnectionRegistry registry{Policy()};
    registry.RecordFailure("late", RemoteConnectionFailure::TimedOut, 100, "", 5'000ms);
    registry.RecordFailure("early", RemoteConnectionFailure::TimedOut, 100, "", 1'000ms);
    registry.RecordFailure("blocked", RemoteConnectionFailure::PermissionDenied, 100, "", 0ms);

    CHECK(registry.NextDeadline() == 1'500ms);

    // Coming due in order, and a claim only takes what is actually due.
    CHECK(registry.ClaimDueRetries(1'500ms) == std::vector<std::string>{"early"});
    CHECK(registry.NextDeadline() == 5'500ms);
    CHECK(registry.ClaimDueRetries(5'500ms) == std::vector<std::string>{"late"});
    CHECK(registry.NextDeadline() == std::nullopt);
}

TEST_CASE(PREFIX "keeps connections apart and forgets one completely")
{
    RemoteConnectionRegistry registry{Policy()};
    registry.RecordFailure("a", RemoteConnectionFailure::TimedOut, 100, "first", 1'000ms);
    registry.RecordFailure("b", RemoteConnectionFailure::Unreachable, 100, "second", 1'000ms);
    CHECK(registry.Snapshot().size() == 2);

    REQUIRE(registry.State("a"));
    REQUIRE(registry.State("b"));
    CHECK(registry.State("a")->last_failure == RemoteConnectionFailure::TimedOut);
    CHECK(registry.State("b")->last_failure == RemoteConnectionFailure::Unreachable);

    registry.Forget("a");
    CHECK(registry.State("a") == std::nullopt);
    CHECK(registry.RetryDeadline("a") == std::nullopt);
    CHECK(registry.Snapshot().size() == 1);
    // Forgetting one leaves the other's armed retry alone.
    CHECK(registry.NextDeadline() == 1'500ms);

    // An unknown key is answered, not created.
    registry.RecordDisconnected("never-seen");
    CHECK(registry.State("never-seen") == std::nullopt);
    CHECK(registry.Snapshot().size() == 1);
}

TEST_CASE(PREFIX "hands a due connection to exactly one of several racing claimants")
{
    // The claim is the whole reason this type is synchronized: attempts run on background queues,
    // and two of them arriving at once must not both be told to reconnect.
    RemoteConnectionRegistry registry{Policy()};
    for( int i = 0; i < 64; ++i )
        registry.RecordFailure("c" + std::to_string(i), RemoteConnectionFailure::TimedOut, 100, "", 0ms);

    std::atomic<int> total{0};
    std::vector<std::thread> threads;
    threads.reserve(8);
    for( int t = 0; t < 8; ++t )
        threads.emplace_back([&] { total += static_cast<int>(registry.ClaimDueRetries(10'000ms).size()); });
    for( std::thread &thread : threads )
        thread.join();

    CHECK(total.load() == 64);
    CHECK(registry.NextDeadline() == std::nullopt);
}

#undef PREFIX
