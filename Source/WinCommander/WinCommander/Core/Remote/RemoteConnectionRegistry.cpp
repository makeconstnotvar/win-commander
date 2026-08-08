// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "RemoteConnectionRegistry.h"

#include <algorithm>
#include <utility>

namespace nc::core {

RemoteConnectionRegistry::RemoteConnectionRegistry(RemoteRetryPolicy _policy) noexcept : m_Policy{_policy}
{
}

std::optional<RemoteConnectionState> RemoteConnectionRegistry::State(const std::string_view _key) const
{
    const auto lock = std::lock_guard{m_Lock};
    const auto found = m_Entries.find(_key);
    return found == m_Entries.end() ? std::nullopt : std::optional{found->second.state};
}

std::vector<std::pair<std::string, RemoteConnectionState>> RemoteConnectionRegistry::Snapshot() const
{
    const auto lock = std::lock_guard{m_Lock};
    std::vector<std::pair<std::string, RemoteConnectionState>> rows;
    rows.reserve(m_Entries.size());
    for( const auto &[key, entry] : m_Entries )
        rows.emplace_back(key, entry.state);
    return rows;
}

void RemoteConnectionRegistry::RecordConnected(const std::string_view _key,
                                               const int64_t _at,
                                               const std::optional<std::chrono::milliseconds> _latency,
                                               const bool _read_only)
{
    const auto lock = std::lock_guard{m_Lock};
    Entry &entry = m_Entries[std::string{_key}];
    entry.state = ApplyRemoteConnected(std::move(entry.state), _at, _latency, _read_only).state;
    // A connection that succeeded has nothing left to retry. Leaving the deadline armed would start
    // an attempt against a link that is already up.
    entry.retry_deadline.reset();
}

void RemoteConnectionRegistry::RecordFailure(const std::string_view _key,
                                             const RemoteConnectionFailure _failure,
                                             const int64_t _at,
                                             std::string _detail,
                                             const Instant _now)
{
    const auto lock = std::lock_guard{m_Lock};
    Entry &entry = m_Entries[std::string{_key}];
    RemoteConnectionTransition transition =
        ApplyRemoteFailure(std::move(entry.state), _failure, _at, std::move(_detail), m_Policy);
    entry.state = std::move(transition.state);
    entry.retry_deadline =
        transition.retry_after ? std::optional{_now + *transition.retry_after} : std::optional<Instant>{};
}

void RemoteConnectionRegistry::RecordDisconnected(const std::string_view _key)
{
    const auto lock = std::lock_guard{m_Lock};
    const auto found = m_Entries.find(_key);
    if( found == m_Entries.end() )
        return;
    found->second.state = ApplyRemoteDisconnected(std::move(found->second.state)).state;
    // The user asked for this one to stop. Reconnecting on a timer they never set would undo it.
    found->second.retry_deadline.reset();
}

void RemoteConnectionRegistry::Forget(const std::string_view _key)
{
    const auto lock = std::lock_guard{m_Lock};
    m_Entries.erase(std::string{_key});
}

std::optional<RemoteConnectionRegistry::Instant>
RemoteConnectionRegistry::RetryDeadline(const std::string_view _key) const
{
    const auto lock = std::lock_guard{m_Lock};
    const auto found = m_Entries.find(_key);
    return found == m_Entries.end() ? std::nullopt : found->second.retry_deadline;
}

std::optional<RemoteConnectionRegistry::Instant> RemoteConnectionRegistry::NextDeadline() const
{
    const auto lock = std::lock_guard{m_Lock};
    std::optional<Instant> earliest;
    for( const auto &[key, entry] : m_Entries ) {
        if( !entry.retry_deadline )
            continue;
        if( !earliest || *entry.retry_deadline < *earliest )
            earliest = entry.retry_deadline;
    }
    return earliest;
}

std::vector<std::string> RemoteConnectionRegistry::ClaimDueRetries(const Instant _now)
{
    const auto lock = std::lock_guard{m_Lock};
    std::vector<std::string> due;
    for( auto &[key, entry] : m_Entries ) {
        if( !entry.retry_deadline || *entry.retry_deadline > _now )
            continue;
        // Disarmed as it is handed out, so a second tick before the attempt reports back cannot
        // start a competing one.
        entry.retry_deadline.reset();
        due.push_back(key);
    }
    // Sorted so a caller starting attempts in order does the same thing every run - an unordered
    // container's iteration order is not something a caller should have to be robust against.
    std::ranges::sort(due);
    return due;
}

} // namespace nc::core
