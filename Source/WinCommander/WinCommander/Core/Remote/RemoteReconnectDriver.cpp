// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "RemoteReconnectDriver.h"

#include "RemoteFailureClassification.h"

#include <utility>

namespace nc::core {

RemoteReconnectDriver::RemoteReconnectDriver(RemoteConnectionRegistry &_registry,
                                             Connector _connector,
                                             WallClock _wall_clock)
    : m_Registry{&_registry}, m_Connector{std::move(_connector)}, m_WallClock{std::move(_wall_clock)}
{
}

void RemoteReconnectDriver::ReportConnected(const std::string_view _key,
                                            const std::optional<std::chrono::milliseconds> _latency,
                                            const bool _read_only)
{
    m_Registry->RecordConnected(_key, m_WallClock ? m_WallClock() : 0, _latency, _read_only);
}

void RemoteReconnectDriver::ReportFailure(const std::string_view _key,
                                          const Error &_error,
                                          const RemoteConnectionRegistry::Instant _now)
{
    // The detail is the provider's own description of the error. It is the one field on a connection
    // that a careless caller could use to leak a credential, so nothing is composed here beyond what
    // the error already says about itself.
    m_Registry->RecordFailure(_key,
                              ClassifyRemoteFailure(_error),
                              m_WallClock ? m_WallClock() : 0,
                              _error.LocalizedFailureReason(),
                              _now);
}

void RemoteReconnectDriver::Attempt(const std::string &_key,
                                    const RemoteConnectionRegistry::Instant _now,
                                    RemoteReconnectPassReport &_report)
{
    std::expected<void, Error> outcome =
        std::unexpected(Error{Error::POSIX, ENOSYS}); // classified as unretryable, which is right
                                                      // for a driver with no way to connect
    if( m_Connector ) {
        try {
            outcome = m_Connector(_key);
        } catch( ... ) {
            // A connector that threw told us nothing about the server. Recording it as a protocol
            // error keeps it out of the retryable set rather than spinning on a broken caller.
            outcome = std::unexpected(Error{Error::POSIX, EIO});
        }
    }

    if( outcome ) {
        ReportConnected(_key, std::nullopt, false);
        _report.reconnected.push_back(_key);
        return;
    }
    ReportFailure(_key, outcome.error(), _now);
    _report.failed.push_back(_key);
}

RemoteReconnectPassReport RemoteReconnectDriver::RunDueRetries(const RemoteConnectionRegistry::Instant _now)
{
    RemoteReconnectPassReport report;
    // Claimed up front, and the pass works only from this list. A failure whose fresh backoff lands
    // in the past would otherwise be claimed again inside the same pass, and the loop would spin
    // against a server that is plainly not answering.
    const std::vector<std::string> due = m_Registry->ClaimDueRetries(_now);
    report.reconnected.reserve(due.size());
    for( const std::string &key : due )
        Attempt(key, _now, report);

    report.next_deadline = m_Registry->NextDeadline();
    return report;
}

} // namespace nc::core
