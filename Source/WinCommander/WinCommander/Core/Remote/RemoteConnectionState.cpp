// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "RemoteConnectionState.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace nc::core {

namespace {

/** Trims the history to its bound, dropping the oldest entries. */
void BoundHistory(std::vector<RemoteConnectionEvent> &_history)
{
    if( _history.size() > RemoteConnectionState::MaximumHistory )
        _history.resize(RemoteConnectionState::MaximumHistory);
}

} // namespace

std::optional<std::chrono::milliseconds> NextRemoteRetryDelay(const RemoteRetryPolicy &_policy,
                                                              const unsigned _completed_attempts,
                                                              const RemoteConnectionFailure _failure)
{
    if( !IsRetryableRemoteFailure(_failure) )
        return std::nullopt;
    if( _completed_attempts >= _policy.maximum_attempts )
        return std::nullopt;

    const double multiplier = _policy.multiplier < 1.0 ? 1.0 : _policy.multiplier;
    const double initial = static_cast<double>(std::max<int64_t>(0, _policy.initial_backoff.count()));
    const double ceiling = static_cast<double>(std::max<int64_t>(0, _policy.maximum_backoff.count()));

    // std::pow on a large attempt count overflows to infinity long before the ceiling matters, so
    // the growth is stepped and clamped as it goes rather than computed and clamped afterwards.
    double delay = initial;
    for( unsigned step = 0; step < _completed_attempts; ++step ) {
        delay *= multiplier;
        if( delay >= ceiling ) {
            delay = ceiling;
            break;
        }
    }
    if( !std::isfinite(delay) || delay > ceiling )
        delay = ceiling;
    return std::chrono::milliseconds{static_cast<int64_t>(delay)};
}

RemoteConnectionTransition ApplyRemoteConnected(RemoteConnectionState _state,
                                                const int64_t _at,
                                                const std::optional<std::chrono::milliseconds> _latency,
                                                const bool _read_only)
{
    RemoteConnectionState state = std::move(_state);
    state.status = RemoteConnectionStatus::Connected;
    state.last_failure = RemoteConnectionFailure::None;
    // Success is what resets the budget; without this a connection that flaps would exhaust its
    // retries over a long uptime and then refuse to come back.
    state.completed_attempts = 0;
    state.last_successful_connection = _at;
    state.latency = _latency;
    state.read_only = _read_only;
    return {.state = std::move(state), .retry_after = std::nullopt};
}

RemoteConnectionTransition ApplyRemoteFailure(RemoteConnectionState _state,
                                              const RemoteConnectionFailure _failure,
                                              const int64_t _at,
                                              std::string _detail,
                                              const RemoteRetryPolicy &_policy)
{
    RemoteConnectionState state = std::move(_state);
    if( _failure == RemoteConnectionFailure::None )
        return {.state = std::move(state), .retry_after = std::nullopt};

    state.last_failure = _failure;
    // The delay is chosen from the attempts completed *before* this one: the retry being scheduled
    // is the first one, so it must wait initial_backoff rather than one multiplier step past it.
    const unsigned attempts_before = state.completed_attempts;
    ++state.completed_attempts;
    // A failed attempt says nothing about latency, and keeping the old sample would show a stale
    // "fast" reading next to an offline connection.
    state.latency = std::nullopt;
    state.history.insert(state.history.begin(),
                         RemoteConnectionEvent{.failure = _failure, .at = _at, .detail = std::move(_detail)});
    BoundHistory(state.history);

    const auto retry_after = NextRemoteRetryDelay(_policy, attempts_before, _failure);
    if( retry_after ) {
        state.status = RemoteConnectionStatus::Reconnecting;
        return {.state = std::move(state), .retry_after = retry_after};
    }

    // Blocked and Offline need different things from the user - new credentials or a trust decision
    // versus simply trying again later - so they are not collapsed into one status.
    state.status =
        IsRetryableRemoteFailure(_failure) ? RemoteConnectionStatus::Offline : RemoteConnectionStatus::Blocked;
    return {.state = std::move(state), .retry_after = std::nullopt};
}

RemoteConnectionTransition ApplyRemoteDisconnected(RemoteConnectionState _state)
{
    RemoteConnectionState state = std::move(_state);
    state.status = RemoteConnectionStatus::Disconnected;
    state.last_failure = RemoteConnectionFailure::None;
    state.completed_attempts = 0;
    state.latency = std::nullopt;
    return {.state = std::move(state), .retry_after = std::nullopt};
}

} // namespace nc::core
