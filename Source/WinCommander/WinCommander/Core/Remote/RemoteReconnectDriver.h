// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "RemoteConnectionRegistry.h"

#include <Base/Error.h>

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace nc::core {

/** What one pass of due retries did, so a caller can log it and arm the next timer. */
struct RemoteReconnectPassReport {
    std::vector<std::string> reconnected;
    std::vector<std::string> failed;
    /** When the next armed retry comes due, or nothing when none is armed. */
    std::optional<RemoteConnectionRegistry::Instant> next_deadline;

    friend bool operator==(const RemoteReconnectPassReport &, const RemoteReconnectPassReport &) = default;
};

/**
 * Runs the reconnect loop: claim what is due, attempt it, classify the outcome, fold it back.
 *
 * The three decisions were already made and had no caller - `IsRetryableRemoteFailure` says
 * *whether*, `ClassifyRemoteFailure` says *what a failure was*, and `RemoteConnectionRegistry` says
 * *when*. This is the piece that asks them in order.
 *
 * Deliberately owns no timer. Arming one is the caller's business, and it needs only
 * `RemoteReconnectPassReport::next_deadline` to do it - which keeps this type testable against a
 * supplied instant rather than against real elapsed time.
 */
class RemoteReconnectDriver final
{
public:
    /** Attempts one connection. The key is the caller's own connection identity. */
    using Connector = std::function<std::expected<void, Error>(std::string_view _key)>;
    /** Whole seconds since the epoch, for the connection's history. */
    using WallClock = std::function<int64_t()>;

    RemoteReconnectDriver(RemoteConnectionRegistry &_registry, Connector _connector, WallClock _wall_clock);

    /** Folds a connection that came up, from wherever it was established. */
    void ReportConnected(std::string_view _key, std::optional<std::chrono::milliseconds> _latency, bool _read_only);

    /**
     * Folds a failed attempt, classifying the error into the vocabulary the retry policy reads and
     * arming a deadline when - and only when - that policy allows one.
     */
    void ReportFailure(std::string_view _key, const Error &_error, RemoteConnectionRegistry::Instant _now);

    /**
     * Attempts every connection whose retry has come due.
     *
     * A connection claimed in this pass is never attempted twice within it, however its new deadline
     * lands. Without that rule a failure whose backoff is already in the past would be retried again
     * immediately, and the pass would spin against a server that is plainly not answering.
     */
    [[nodiscard]] RemoteReconnectPassReport RunDueRetries(RemoteConnectionRegistry::Instant _now);

private:
    void Attempt(const std::string &_key, RemoteConnectionRegistry::Instant _now, RemoteReconnectPassReport &_report);

    RemoteConnectionRegistry *m_Registry;
    Connector m_Connector;
    WallClock m_WallClock;
};

} // namespace nc::core
