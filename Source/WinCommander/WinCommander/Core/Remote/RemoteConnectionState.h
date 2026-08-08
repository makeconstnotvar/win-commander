// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nc::core {

/** Where a remote connection currently stands. */
enum class RemoteConnectionStatus : uint8_t {
    /** Never connected, or disconnected on purpose. Not an error. */
    Disconnected,
    Connecting,
    Connected,
    /** Was connected, lost the link, and is retrying under the policy. */
    Reconnecting,
    /** Retries are exhausted or the host is unreachable; only the user restarts this. */
    Offline,
    /** Terminal until the stored credentials or the host trust decision change. */
    Blocked
};

/** Why an attempt failed. The distinction drives whether a retry is even allowed. */
enum class RemoteConnectionFailure : uint8_t {
    None,
    /** No route, refused, DNS failure - transient in principle. */
    Unreachable,
    TimedOut,
    /** The server rejected the credentials. */
    AuthenticationRejected,
    /** The host key or certificate did not match what was pinned. */
    HostVerificationFailed,
    /** Authenticated, but the account may not do this. */
    PermissionDenied,
    /** Malformed or unsupported response - retrying will not change it. */
    ProtocolError
};

/**
 * A failure that must never be retried without the user, and why.
 *
 * `AuthenticationRejected` is excluded because repeating a rejected credential is how an account
 * gets locked out, and the credential cannot fix itself between attempts.
 * `HostVerificationFailed` is excluded because a mismatched host key is exactly the signature of an
 * interception, and silently retrying until it succeeds would defeat the check entirely.
 * `PermissionDenied` and `ProtocolError` are excluded because nothing about them changes with time.
 */
[[nodiscard]] constexpr bool IsRetryableRemoteFailure(const RemoteConnectionFailure _failure) noexcept
{
    return _failure == RemoteConnectionFailure::Unreachable || _failure == RemoteConnectionFailure::TimedOut;
}

/** Exponential backoff with a ceiling. Values are whole milliseconds. */
struct RemoteRetryPolicy {
    /** Attempts *after* the first one. Zero disables automatic reconnection entirely. */
    unsigned maximum_attempts = 5;
    std::chrono::milliseconds initial_backoff{500};
    std::chrono::milliseconds maximum_backoff{30'000};
    /** Applied per attempt. Values below 1 are treated as 1 - backoff never shrinks. */
    double multiplier = 2.0;

    friend bool operator==(const RemoteRetryPolicy &, const RemoteRetryPolicy &) = default;
};

/**
 * How long to wait before retry number `_completed_attempts + 1`, or nothing when there must be no
 * automatic retry at all - either because the failure is not retryable or the budget is spent.
 *
 * Returning nothing for both cases is deliberate: a caller that cannot tell them apart still does
 * the safe thing, and one that wants to explain the difference asks `IsRetryableRemoteFailure`.
 */
[[nodiscard]] std::optional<std::chrono::milliseconds>
NextRemoteRetryDelay(const RemoteRetryPolicy &_policy, unsigned _completed_attempts, RemoteConnectionFailure _failure);

/** One recorded failure, for the connection's error history. */
struct RemoteConnectionEvent {
    RemoteConnectionFailure failure = RemoteConnectionFailure::None;
    /** Whole seconds since the epoch. */
    int64_t at = 0;
    /** Provider-supplied detail. Never contains credentials - see RemoteConnectionState. */
    std::string detail;

    friend bool operator==(const RemoteConnectionEvent &, const RemoteConnectionEvent &) = default;
};

/**
 * Value model of one remote connection's live state.
 *
 * It holds **no credential material of any kind** - not a password, not a token, not a key. The
 * spec requires credentials live in the Keychain and are never logged, and the cheapest way to keep
 * that true is for the type that gets copied into snapshots, error histories and UI to have nowhere
 * to put them.
 */
struct RemoteConnectionState {
    RemoteConnectionStatus status = RemoteConnectionStatus::Disconnected;
    RemoteConnectionFailure last_failure = RemoteConnectionFailure::None;
    /** Attempts completed since the last success. Reset by a successful connection. */
    unsigned completed_attempts = 0;
    /** Whole seconds since the epoch; absent until the connection has ever succeeded. */
    std::optional<int64_t> last_successful_connection;
    /** Round-trip sample from the last successful exchange. */
    std::optional<std::chrono::milliseconds> latency;
    /** The provider reported the mount as writable, or it did not. */
    bool read_only = false;
    /** Most recent first, bounded by MaximumHistory. */
    std::vector<RemoteConnectionEvent> history;

    static constexpr size_t MaximumHistory = 16;

    friend bool operator==(const RemoteConnectionState &, const RemoteConnectionState &) = default;
};

/** Result of folding one outcome into a connection's state. */
struct RemoteConnectionTransition {
    RemoteConnectionState state;
    /** Present when the caller should schedule an automatic retry after this delay. */
    std::optional<std::chrono::milliseconds> retry_after;

    friend bool operator==(const RemoteConnectionTransition &, const RemoteConnectionTransition &) = default;
};

/** Folds a successful connection: clears the failure budget and records the sample. */
[[nodiscard]] RemoteConnectionTransition ApplyRemoteConnected(RemoteConnectionState _state,
                                                              int64_t _at,
                                                              std::optional<std::chrono::milliseconds> _latency,
                                                              bool _read_only);

/**
 * Folds a failed attempt: records it in the bounded history, then either schedules a retry or
 * settles into a terminal status. A non-retryable failure goes to `Blocked` rather than `Offline`,
 * because those two need different things from the user - new credentials or a trust decision
 * versus simply trying again later.
 */
[[nodiscard]] RemoteConnectionTransition ApplyRemoteFailure(RemoteConnectionState _state,
                                                             RemoteConnectionFailure _failure,
                                                             int64_t _at,
                                                             std::string _detail,
                                                             const RemoteRetryPolicy &_policy);

/** Folds an explicit user disconnect: not a failure, and it clears the retry budget. */
[[nodiscard]] RemoteConnectionTransition ApplyRemoteDisconnected(RemoteConnectionState _state);

} // namespace nc::core
