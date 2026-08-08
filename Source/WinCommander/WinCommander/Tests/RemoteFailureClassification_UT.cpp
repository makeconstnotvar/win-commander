// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Remote/RemoteFailureClassification.h>

#include <VFS/NetSFTP.h>

#include <cerrno>

namespace {

using nc::Error;
using nc::core::ClassifyRemoteFailure;
using nc::core::IsRetryableRemoteFailure;
using nc::core::RemoteConnectionFailure;
namespace sftp = nc::vfs::sftp;

Error SFTPError(const long _code)
{
    return Error{sftp::ErrorDomain, _code};
}

} // namespace

#define PREFIX "nc::core::ClassifyRemoteFailure "

TEST_CASE(PREFIX "carries a refused host key all the way to the no-retry decision")
{
    // This is the whole point of the adapter. Host verification refuses at the handshake; unless
    // that arrives here as HostVerificationFailed, a reconnect timer would go on reaching a server
    // whose identity failed to check out - which is what verification exists to stop.
    for( const long code : {sftp::Errors::host_verification_failed, sftp::Errors::host_key_unavailable} ) {
        const RemoteConnectionFailure failure = ClassifyRemoteFailure(SFTPError(code));
        CHECK(failure == RemoteConnectionFailure::HostVerificationFailed);
        CHECK_FALSE(IsRetryableRemoteFailure(failure));
    }
}

TEST_CASE(PREFIX "never lets a rejected credential become a retry")
{
    // Repeating a rejected credential is how an account gets locked out, and nothing about the
    // credential changes between attempts.
    for( const long code : {sftp::Errors::authentication_failed,
                            sftp::Errors::publickey_unverified,
                            sftp::Errors::keyfile_auth_failed,
                            sftp::Errors::password_expired} ) {
        const RemoteConnectionFailure failure = ClassifyRemoteFailure(SFTPError(code));
        CHECK(failure == RemoteConnectionFailure::AuthenticationRejected);
        CHECK_FALSE(IsRetryableRemoteFailure(failure));
    }
    CHECK(ClassifyRemoteFailure(Error{Error::POSIX, EAUTH}) == RemoteConnectionFailure::AuthenticationRejected);
}

TEST_CASE(PREFIX "admits the two failures that a later attempt could actually resolve")
{
    CHECK(ClassifyRemoteFailure(SFTPError(sftp::Errors::timeout)) == RemoteConnectionFailure::TimedOut);
    CHECK(ClassifyRemoteFailure(SFTPError(sftp::Errors::socket_timeout)) == RemoteConnectionFailure::TimedOut);
    CHECK(ClassifyRemoteFailure(Error{Error::POSIX, ETIMEDOUT}) == RemoteConnectionFailure::TimedOut);

    for( const long code : {sftp::Errors::connect_failed,
                            sftp::Errors::couldnt_resolve,
                            sftp::Errors::socket_disconnect,
                            sftp::Errors::fx_connection_lost} ) {
        CHECK(ClassifyRemoteFailure(SFTPError(code)) == RemoteConnectionFailure::Unreachable);
    }
    for( const int code : {ECONNREFUSED, EHOSTUNREACH, ENETDOWN, ECONNRESET, EPIPE} ) {
        CHECK(ClassifyRemoteFailure(Error{Error::POSIX, code}) == RemoteConnectionFailure::Unreachable);
    }

    CHECK(IsRetryableRemoteFailure(RemoteConnectionFailure::TimedOut));
    CHECK(IsRetryableRemoteFailure(RemoteConnectionFailure::Unreachable));
}

TEST_CASE(PREFIX "keeps a permission refusal apart from a credential refusal")
{
    // Authenticated but not allowed is a different message to the user than "your password was
    // refused", and collapsing them would send someone to change a credential that is working.
    CHECK(ClassifyRemoteFailure(SFTPError(sftp::Errors::fx_permission_denied)) ==
          RemoteConnectionFailure::PermissionDenied);
    CHECK(ClassifyRemoteFailure(SFTPError(sftp::Errors::fx_write_protect)) ==
          RemoteConnectionFailure::PermissionDenied);
    CHECK(ClassifyRemoteFailure(Error{Error::POSIX, EACCES}) == RemoteConnectionFailure::PermissionDenied);
    CHECK(ClassifyRemoteFailure(Error{Error::POSIX, EPERM}) == RemoteConnectionFailure::PermissionDenied);
}

TEST_CASE(PREFIX "refuses to retry a failure it cannot name")
{
    // Retrying is the privilege of failures we can name. An unrecognised error put on a timer is
    // how a connection ends up hammering a server for reasons nobody understands.
    const RemoteConnectionFailure unknown_domain = ClassifyRemoteFailure(Error{"SomeOtherDomain", 42});
    CHECK(unknown_domain == RemoteConnectionFailure::ProtocolError);
    CHECK_FALSE(IsRetryableRemoteFailure(unknown_domain));

    const RemoteConnectionFailure unknown_sftp_code = ClassifyRemoteFailure(SFTPError(sftp::Errors::proto));
    CHECK(unknown_sftp_code == RemoteConnectionFailure::ProtocolError);
    CHECK_FALSE(IsRetryableRemoteFailure(unknown_sftp_code));

    CHECK(ClassifyRemoteFailure(Error{Error::POSIX, ENOSYS}) == RemoteConnectionFailure::ProtocolError);
}

TEST_CASE(PREFIX "does not report success as a failure")
{
    // A zero POSIX code is not "no error" arriving here - callers classify only what already failed.
    // It must still land somewhere that will not be retried rather than on None.
    CHECK(ClassifyRemoteFailure(Error{Error::POSIX, 0}) == RemoteConnectionFailure::ProtocolError);
    CHECK(ClassifyRemoteFailure(SFTPError(sftp::Errors::none)) == RemoteConnectionFailure::ProtocolError);
}

#undef PREFIX
