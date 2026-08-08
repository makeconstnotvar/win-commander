// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "RemoteConnectionPresentation.h"

namespace nc::core {

RemoteLinkQuality ClassifyRemoteLinkQuality(const std::optional<std::chrono::milliseconds> &_latency,
                                            const RemoteLinkQualityThresholds &_thresholds)
{
    if( !_latency )
        return RemoteLinkQuality::Unknown;
    // A negative sample is not a fast link; it is a broken measurement, and reporting Good for it
    // would be the most misleading answer available.
    if( _latency->count() < 0 )
        return RemoteLinkQuality::Unknown;
    if( *_latency < _thresholds.good_below )
        return RemoteLinkQuality::Good;
    if( *_latency < _thresholds.fair_below )
        return RemoteLinkQuality::Fair;
    return RemoteLinkQuality::Poor;
}

RemoteConnectionPresentation PresentRemoteConnection(const RemoteConnectionState &_state,
                                                     const bool _has_stored_credential,
                                                     const RemoteHostTrustVerdict _trust,
                                                     const RemoteLinkQualityThresholds &_thresholds)
{
    RemoteConnectionPresentation presentation;
    presentation.status = _state.status;
    presentation.quality = ClassifyRemoteLinkQuality(_state.latency, _thresholds);
    presentation.trust = _trust;
    presentation.last_successful_connection = _state.last_successful_connection;
    presentation.read_only = _state.read_only;
    presentation.recorded_failures = _state.history.size();

    // A stored credential the server rejected is worse than none: it will keep failing until it is
    // replaced, and showing it as merely "stored" would leave the user with nothing to act on.
    if( _state.last_failure == RemoteConnectionFailure::AuthenticationRejected )
        presentation.credentials = RemoteCredentialsState::Rejected;
    else
        presentation.credentials = _has_stored_credential ? RemoteCredentialsState::Stored
                                                          : RemoteCredentialsState::Missing;

    // Not simply "status == Blocked": a mismatched pin needs the user even while the connection
    // sits idle and has never failed. Folding trust in here means no surface has to remember it.
    const bool trust_needs_user =
        _trust == RemoteHostTrustVerdict::Mismatch || _trust == RemoteHostTrustVerdict::Unusable;
    presentation.needs_attention = _state.status == RemoteConnectionStatus::Blocked ||
                                   presentation.credentials == RemoteCredentialsState::Rejected ||
                                   trust_needs_user;
    return presentation;
}

} // namespace nc::core
