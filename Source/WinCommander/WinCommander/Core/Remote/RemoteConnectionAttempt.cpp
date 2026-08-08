// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "RemoteConnectionAttempt.h"

#include "RemoteFailureClassification.h"

#include <chrono>

namespace nc::core {

namespace {

int64_t WallClockSeconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

VFSHostPtr RecordConnectionAttempt(RemoteConnectionRegistry &_registry,
                                   const std::string_view _key,
                                   const RemoteConnectionRegistry::Instant _now,
                                   const std::function<VFSHostPtr()> &_spawn)
{
    if( !_spawn )
        return nullptr;

    VFSHostPtr host;
    try {
        host = _spawn();
    } catch( const ErrorException &exception ) {
        _registry.RecordFailure(_key,
                                ClassifyRemoteFailure(exception.error()),
                                WallClockSeconds(),
                                exception.error().LocalizedFailureReason(),
                                _now);
        throw;
    } catch( ... ) {
        // Not an Error, so nothing can be named about it. Recorded as a protocol failure, which is
        // never retried: something we cannot describe is not evidence that trying again will help.
        _registry.RecordFailure(_key, RemoteConnectionFailure::ProtocolError, WallClockSeconds(), {}, _now);
        throw;
    }

    // A null host is what a cancelled password prompt returns; nothing was ever asked of the server.
    // Recording it as a failure would spend a retry budget, and eventually block a connection,
    // because somebody pressed Cancel.
    if( host )
        _registry.RecordConnected(_key, WallClockSeconds(), std::nullopt, !host->IsWritable());

    return host;
}

} // namespace nc::core
