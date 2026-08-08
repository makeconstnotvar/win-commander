// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "RemoteFailureClassification.h"

#include <VFS/NetSFTP.h>

#include <cerrno>

namespace nc::core {

namespace {

RemoteConnectionFailure ClassifyPOSIX(const int64_t _code) noexcept
{
    switch( _code ) {
        case ETIMEDOUT:
            return RemoteConnectionFailure::TimedOut;
        case ECONNREFUSED:
        case ECONNRESET:
        case ECONNABORTED:
        case EHOSTDOWN:
        case EHOSTUNREACH:
        case ENETDOWN:
        case ENETUNREACH:
        case ENETRESET:
        case ENOTCONN:
        case EPIPE:
            return RemoteConnectionFailure::Unreachable;
        case EACCES:
        case EPERM:
            return RemoteConnectionFailure::PermissionDenied;
        case EAUTH:
            return RemoteConnectionFailure::AuthenticationRejected;
        default:
            return RemoteConnectionFailure::ProtocolError;
    }
}

RemoteConnectionFailure ClassifySFTP(const int64_t _code) noexcept
{
    using nc::vfs::sftp::Errors;
    switch( _code ) {
        // A refused host key must never be retried, and this is what carries that all the way from
        // the handshake to the reconnect policy. Getting it wrong here would let a timer keep
        // reconnecting to a server whose identity failed to check out.
        case Errors::host_verification_failed:
        case Errors::host_key_unavailable:
            return RemoteConnectionFailure::HostVerificationFailed;

        case Errors::authentication_failed:
        case Errors::publickey_unverified:
        case Errors::keyfile_auth_failed:
        case Errors::password_expired:
            return RemoteConnectionFailure::AuthenticationRejected;

        case Errors::timeout:
        case Errors::socket_timeout:
            return RemoteConnectionFailure::TimedOut;

        case Errors::connect_failed:
        case Errors::couldnt_resolve:
        case Errors::socket_none:
        case Errors::socket_send:
        case Errors::socket_recv:
        case Errors::socket_disconnect:
        case Errors::bad_socket:
        case Errors::banner_recv:
        case Errors::banner_send:
        case Errors::fx_no_connection:
        case Errors::fx_connection_lost:
            return RemoteConnectionFailure::Unreachable;

        case Errors::fx_permission_denied:
        case Errors::fx_write_protect:
            return RemoteConnectionFailure::PermissionDenied;

        default:
            return RemoteConnectionFailure::ProtocolError;
    }
}

} // namespace

RemoteConnectionFailure ClassifyRemoteFailure(const Error &_error) noexcept
{
    const std::string domain = _error.Domain();
    if( domain == nc::vfs::sftp::ErrorDomain )
        return ClassifySFTP(_error.Code());
    if( domain == Error::POSIX )
        return ClassifyPOSIX(_error.Code());
    // An unrecognised domain is not assumed to be transient. Retrying a failure nobody can name is
    // how a connection ends up hammering a server for reasons no one understands.
    return RemoteConnectionFailure::ProtocolError;
}

} // namespace nc::core
