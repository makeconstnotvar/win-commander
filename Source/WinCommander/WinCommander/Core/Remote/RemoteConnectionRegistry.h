// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "RemoteConnectionState.h"

#include <Base/UnorderedUtil.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nc::core {

/**
 * Live state for every remote connection the application knows about, and when each is next due for
 * an automatic retry.
 *
 * `RemoteConnectionState` folds one outcome into one connection; this owns the collection and the
 * clock-facing half - turning the `retry_after` duration a transition produces into a deadline that
 * survives until it comes due.
 *
 * Connections are keyed by whatever identity the caller already uses. Inventing a second identity
 * scheme here would be a way for one connection to be filed under two names and retried twice.
 *
 * Internally synchronized. Connection attempts run on background queues while the manager reads the
 * same rows to draw them, and `ClaimDueRetries` is only meaningful if the claim is atomic.
 */
class RemoteConnectionRegistry final
{
public:
    /** Whole milliseconds since an arbitrary but monotonic origin the caller keeps consistent. */
    using Instant = std::chrono::milliseconds;

    explicit RemoteConnectionRegistry(RemoteRetryPolicy _policy = {}) noexcept;

    /** The state of a connection, or nothing when it has never been recorded. */
    [[nodiscard]] std::optional<RemoteConnectionState> State(std::string_view _key) const;

    /** Every known connection, in no particular order. */
    [[nodiscard]] std::vector<std::pair<std::string, RemoteConnectionState>> Snapshot() const;

    /** Folds a successful connection and clears any pending retry. */
    void RecordConnected(std::string_view _key,
                         int64_t _at,
                         std::optional<std::chrono::milliseconds> _latency,
                         bool _read_only);

    /**
     * Folds a failed attempt and, when the transition asks for a retry, arms a deadline at
     * `_now + retry_after`.
     *
     * The deadline is absolute and computed once. Recomputing a delay on every tick would restart
     * the wait each time anybody asked, and a connection would never actually come due.
     */
    void RecordFailure(std::string_view _key,
                       RemoteConnectionFailure _failure,
                       int64_t _at,
                       std::string _detail,
                       Instant _now);

    /** Folds an explicit user disconnect. Not a failure, and it disarms any pending retry. */
    void RecordDisconnected(std::string_view _key);

    /** Forgets a connection entirely, including any armed retry. */
    void Forget(std::string_view _key);

    /** When this connection is next due, or nothing when no retry is armed. */
    [[nodiscard]] std::optional<Instant> RetryDeadline(std::string_view _key) const;

    /** The earliest armed deadline, which is when a timer should next fire. */
    [[nodiscard]] std::optional<Instant> NextDeadline() const;

    /**
     * Hands out every connection whose deadline has come, disarming each as it goes.
     *
     * Claiming rather than merely reporting is the point: two ticks that both observed the same due
     * connection would each start an attempt, and the second would race the first.
     */
    [[nodiscard]] std::vector<std::string> ClaimDueRetries(Instant _now);

private:
    struct Entry {
        RemoteConnectionState state;
        std::optional<Instant> retry_deadline;
    };

    mutable std::mutex m_Lock;
    RemoteRetryPolicy m_Policy;
    // Transparent hashing so a lookup by `string_view` does not allocate a key to ask a question.
    std::unordered_map<std::string, Entry, UnorderedStringHashEqual, UnorderedStringHashEqual> m_Entries;
};

} // namespace nc::core
