// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Remote/RemoteConnectionState.h>

#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using nc::core::ApplyRemoteConnected;
using nc::core::ApplyRemoteDisconnected;
using nc::core::ApplyRemoteFailure;
using nc::core::IsRetryableRemoteFailure;
using nc::core::NextRemoteRetryDelay;
using nc::core::RemoteConnectionFailure;
using nc::core::RemoteConnectionState;
using nc::core::RemoteConnectionStatus;
using nc::core::RemoteConnectionTransition;
using nc::core::RemoteRetryPolicy;

constexpr RemoteRetryPolicy TestPolicy()
{
    return {.maximum_attempts = 4, .initial_backoff = 100ms, .maximum_backoff = 1000ms, .multiplier = 2.0};
}

} // namespace

#define PREFIX "nc::core::RemoteConnection "

TEST_CASE(PREFIX "never auto-retries a failure the user has to resolve")
{
    // The two exclusions that matter most: repeating a rejected credential is how an account gets
    // locked out, and retrying a mismatched host key until it succeeds defeats the check entirely.
    CHECK_FALSE(IsRetryableRemoteFailure(RemoteConnectionFailure::AuthenticationRejected));
    CHECK_FALSE(IsRetryableRemoteFailure(RemoteConnectionFailure::HostVerificationFailed));
    // Nothing about these changes with time either.
    CHECK_FALSE(IsRetryableRemoteFailure(RemoteConnectionFailure::PermissionDenied));
    CHECK_FALSE(IsRetryableRemoteFailure(RemoteConnectionFailure::ProtocolError));
    CHECK_FALSE(IsRetryableRemoteFailure(RemoteConnectionFailure::None));

    CHECK(IsRetryableRemoteFailure(RemoteConnectionFailure::Unreachable));
    CHECK(IsRetryableRemoteFailure(RemoteConnectionFailure::TimedOut));

    for( const auto failure : {RemoteConnectionFailure::AuthenticationRejected,
                               RemoteConnectionFailure::HostVerificationFailed,
                               RemoteConnectionFailure::PermissionDenied,
                               RemoteConnectionFailure::ProtocolError} ) {
        // Not even on the very first attempt, with the whole budget unspent.
        CHECK_FALSE(NextRemoteRetryDelay(TestPolicy(), 0, failure));
    }
}

TEST_CASE(PREFIX "backs off exponentially and stops at the ceiling and the budget")
{
    const auto policy = TestPolicy();
    CHECK(NextRemoteRetryDelay(policy, 0, RemoteConnectionFailure::TimedOut) == 100ms);
    CHECK(NextRemoteRetryDelay(policy, 1, RemoteConnectionFailure::TimedOut) == 200ms);
    CHECK(NextRemoteRetryDelay(policy, 2, RemoteConnectionFailure::TimedOut) == 400ms);
    CHECK(NextRemoteRetryDelay(policy, 3, RemoteConnectionFailure::TimedOut) == 800ms);
    // Budget spent: four attempts were allowed and four are done.
    CHECK_FALSE(NextRemoteRetryDelay(policy, 4, RemoteConnectionFailure::TimedOut));

    SECTION("the ceiling clamps growth rather than being overshot")
    {
        const RemoteRetryPolicy capped{
            .maximum_attempts = 20, .initial_backoff = 100ms, .maximum_backoff = 500ms, .multiplier = 10.0};
        CHECK(NextRemoteRetryDelay(capped, 1, RemoteConnectionFailure::TimedOut) == 500ms);
        // A large attempt count must clamp, not overflow into a nonsense delay.
        CHECK(NextRemoteRetryDelay(capped, 19, RemoteConnectionFailure::TimedOut) == 500ms);
    }
    SECTION("a multiplier below one never shrinks the backoff")
    {
        const RemoteRetryPolicy shrinking{
            .maximum_attempts = 5, .initial_backoff = 100ms, .maximum_backoff = 1000ms, .multiplier = 0.1};
        CHECK(NextRemoteRetryDelay(shrinking, 3, RemoteConnectionFailure::TimedOut) == 100ms);
    }
    SECTION("zero attempts disables automatic reconnection outright")
    {
        const RemoteRetryPolicy manual{
            .maximum_attempts = 0, .initial_backoff = 100ms, .maximum_backoff = 1000ms, .multiplier = 2.0};
        CHECK_FALSE(NextRemoteRetryDelay(manual, 0, RemoteConnectionFailure::TimedOut));
    }
}

TEST_CASE(PREFIX "separates a connection to retry later from one the user must unblock")
{
    const auto policy = TestPolicy();

    SECTION("a retryable failure exhausts its budget and lands Offline")
    {
        RemoteConnectionState state;
        for( unsigned attempt = 0; attempt < policy.maximum_attempts; ++attempt ) {
            const auto transition =
                ApplyRemoteFailure(state, RemoteConnectionFailure::Unreachable, 100 + attempt, "no route", policy);
            CHECK(transition.state.status == RemoteConnectionStatus::Reconnecting);
            REQUIRE(transition.retry_after);
            state = transition.state;
        }
        const auto exhausted =
            ApplyRemoteFailure(state, RemoteConnectionFailure::Unreachable, 200, "no route", policy);
        CHECK(exhausted.state.status == RemoteConnectionStatus::Offline);
        CHECK_FALSE(exhausted.retry_after);
    }
    SECTION("a non-retryable failure lands Blocked immediately, on the very first attempt")
    {
        const auto blocked = ApplyRemoteFailure(
            {}, RemoteConnectionFailure::HostVerificationFailed, 100, "host key changed", policy);
        CHECK(blocked.state.status == RemoteConnectionStatus::Blocked);
        CHECK_FALSE(blocked.retry_after);
        CHECK(blocked.state.completed_attempts == 1);
    }
}

TEST_CASE(PREFIX "records a bounded, newest-first error history")
{
    const auto policy = TestPolicy();
    RemoteConnectionState state;
    for( int index = 0; index < 20; ++index ) {
        state = ApplyRemoteFailure(std::move(state),
                                   RemoteConnectionFailure::TimedOut,
                                   index,
                                   "attempt " + std::to_string(index),
                                   policy)
                    .state;
    }
    REQUIRE(state.history.size() == RemoteConnectionState::MaximumHistory);
    CHECK(state.history.front().detail == "attempt 19");
    CHECK(state.history.front().at == 19);
    // The oldest entries were dropped rather than the newest being refused.
    CHECK(state.history.back().detail == "attempt 4");
}

TEST_CASE(PREFIX "a success resets the retry budget so a flapping link can always come back")
{
    const auto policy = TestPolicy();
    RemoteConnectionState state;
    state = ApplyRemoteFailure(std::move(state), RemoteConnectionFailure::TimedOut, 1, "slow", policy).state;
    state = ApplyRemoteFailure(std::move(state), RemoteConnectionFailure::TimedOut, 2, "slow", policy).state;
    REQUIRE(state.completed_attempts == 2);

    const auto connected = ApplyRemoteConnected(std::move(state), 10, 42ms, false);
    CHECK(connected.state.status == RemoteConnectionStatus::Connected);
    CHECK(connected.state.completed_attempts == 0);
    CHECK(connected.state.last_failure == RemoteConnectionFailure::None);
    CHECK(connected.state.last_successful_connection == 10);
    CHECK(connected.state.latency == 42ms);
    CHECK_FALSE(connected.state.read_only);
    // The history is kept: it is the connection's record, not its current state.
    CHECK(connected.state.history.size() == 2);

    // With the budget reset, the next failure gets the full backoff sequence again.
    const auto again = ApplyRemoteFailure(connected.state, RemoteConnectionFailure::TimedOut, 11, "slow", policy);
    CHECK(again.retry_after == 100ms);
}

TEST_CASE(PREFIX "reports a read-only mount and drops a stale latency sample on failure")
{
    const auto policy = TestPolicy();
    const auto connected = ApplyRemoteConnected({}, 10, 42ms, true);
    CHECK(connected.state.read_only);
    CHECK(connected.state.latency == 42ms);

    // A failed attempt says nothing about latency; keeping the old sample would show a stale "fast"
    // reading beside an offline connection.
    const auto failed =
        ApplyRemoteFailure(connected.state, RemoteConnectionFailure::Unreachable, 11, "dropped", policy);
    CHECK_FALSE(failed.state.latency);
    CHECK(failed.state.last_successful_connection == 10);
}

TEST_CASE(PREFIX "an explicit disconnect is not a failure and clears the budget")
{
    const auto policy = TestPolicy();
    RemoteConnectionState state;
    state = ApplyRemoteFailure(std::move(state), RemoteConnectionFailure::TimedOut, 1, "slow", policy).state;
    state = ApplyRemoteConnected(std::move(state), 5, 10ms, false).state;

    const auto disconnected = ApplyRemoteDisconnected(state);
    CHECK(disconnected.state.status == RemoteConnectionStatus::Disconnected);
    CHECK(disconnected.state.last_failure == RemoteConnectionFailure::None);
    CHECK(disconnected.state.completed_attempts == 0);
    CHECK_FALSE(disconnected.state.latency);
    CHECK_FALSE(disconnected.retry_after);
    // The last success and the history survive: they are what the manager lists.
    CHECK(disconnected.state.last_successful_connection == 5);
    CHECK(disconnected.state.history.size() == 1);
}

TEST_CASE(PREFIX "a no-op outcome changes nothing")
{
    const auto policy = TestPolicy();
    const auto connected = ApplyRemoteConnected({}, 10, 42ms, false);
    const auto unchanged = ApplyRemoteFailure(connected.state, RemoteConnectionFailure::None, 11, "ignored", policy);
    CHECK(unchanged.state == connected.state);
    CHECK_FALSE(unchanged.retry_after);
}
