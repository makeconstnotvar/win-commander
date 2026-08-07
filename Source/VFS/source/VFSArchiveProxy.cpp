// Copyright (C) 2014-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#include "../include/VFS/VFSArchiveProxy.h"
#include "ArcLA/Host.h"
#include "ArcLARaw/Host.h"
#include <cerrno>
#include <utility>

namespace nc::vfs {

namespace {

ArchiveOpenFailureKind ClassifyFailure(const VFSHostPtr &_parent, const Error &_error) noexcept
{
    if( _parent ) {
        switch( _parent->ClassifyError(_error) ) {
            case HostErrorKind::Missing:
                return ArchiveOpenFailureKind::SourceMissing;
            case HostErrorKind::PermissionDenied:
                return ArchiveOpenFailureKind::SourcePermissionDenied;
            case HostErrorKind::Cancelled:
                return ArchiveOpenFailureKind::Cancelled;
            case HostErrorKind::TimedOut:
            case HostErrorKind::Unavailable:
                return ArchiveOpenFailureKind::SourceUnavailable;
            case HostErrorKind::Unsupported:
                return ArchiveOpenFailureKind::InvalidOrUnsupportedArchive;
            case HostErrorKind::Other:
                break;
        }
    }

    if( _error.Domain() == Error::POSIX ) {
        switch( _error.Code() ) {
            case EFTYPE:
            case EINVAL:
                return ArchiveOpenFailureKind::InvalidOrUnsupportedArchive;
            case ECANCELED:
                return ArchiveOpenFailureKind::Cancelled;
            default:
                break;
        }
    }
    return ArchiveOpenFailureKind::ReadFailed;
}

bool IsSourceOperationalFailure(const ArchiveOpenFailureKind _kind) noexcept
{
    switch( _kind ) {
        case ArchiveOpenFailureKind::SourceMissing:
        case ArchiveOpenFailureKind::SourcePermissionDenied:
        case ArchiveOpenFailureKind::SourceUnavailable:
        case ArchiveOpenFailureKind::Cancelled:
            return true;
        case ArchiveOpenFailureKind::PasswordRequired:
        case ArchiveOpenFailureKind::PasswordCancelled:
        case ArchiveOpenFailureKind::PasswordRejectedOrInvalidArchive:
        case ArchiveOpenFailureKind::InvalidOrUnsupportedArchive:
        case ArchiveOpenFailureKind::ReadFailed:
            return false;
    }
}

ArchiveOpenFailure Failure(ArchiveOpenFailureKind _kind,
                           const Error &_primary,
                           std::optional<Error> _fallback = std::nullopt)
{
    return ArchiveOpenFailure{
        .kind = _kind,
        .primary_error = _primary,
        .fallback_error = std::move(_fallback),
    };
}

} // namespace

ArchiveOpenResult VFSArchiveProxy::OpenFileAsArchiveResult(const std::string &_path,
                                                           const VFSHostPtr &_parent,
                                                           ArchivePasswordProvider _passwd,
                                                           VFSCancelChecker _cancel_checker)
{
    if( !_parent || _path.empty() ) {
        const Error error{Error::POSIX, EINVAL};
        return std::unexpected(Failure(ArchiveOpenFailureKind::SourceUnavailable, error));
    }

    Error archive_error{Error::POSIX, EFTYPE};
    try {
        return std::make_shared<ArchiveHost>(_path, _parent, std::nullopt, _cancel_checker);
    } catch( const ErrorException &e ) {
        archive_error = e.error();
    }

    if( archive_error == Error{Error::POSIX, ENEEDAUTH} ) {
        if( !_passwd )
            return std::unexpected(Failure(ArchiveOpenFailureKind::PasswordRequired, archive_error));

        std::optional<std::string> password = _passwd();
        if( !password )
            return std::unexpected(Failure(ArchiveOpenFailureKind::PasswordCancelled, archive_error));

        try {
            return std::make_shared<ArchiveHost>(_path, _parent, std::move(*password), _cancel_checker);
        } catch( const ErrorException &e ) {
            const ArchiveOpenFailureKind retry_kind = ClassifyFailure(_parent, e.error());
            if( IsSourceOperationalFailure(retry_kind) )
                return std::unexpected(Failure(retry_kind, e.error(), archive_error));
            return std::unexpected(
                Failure(ArchiveOpenFailureKind::PasswordRejectedOrInvalidArchive, e.error(), archive_error));
        }
    }

    if( ArchiveRawHost::HasSupportedExtension(_path) ) {
        try {
            return std::make_shared<ArchiveRawHost>(_path, _parent, _cancel_checker);
        } catch( const ErrorException &e ) {
            const ArchiveOpenFailureKind raw_kind = ClassifyFailure(_parent, e.error());
            if( IsSourceOperationalFailure(raw_kind) )
                return std::unexpected(Failure(raw_kind, e.error(), archive_error));

            const ArchiveOpenFailureKind archive_kind = ClassifyFailure(_parent, archive_error);
            if( IsSourceOperationalFailure(archive_kind) )
                return std::unexpected(Failure(archive_kind, archive_error, e.error()));

            const ArchiveOpenFailureKind kind =
                raw_kind == ArchiveOpenFailureKind::InvalidOrUnsupportedArchive &&
                        archive_kind == ArchiveOpenFailureKind::InvalidOrUnsupportedArchive
                    ? ArchiveOpenFailureKind::InvalidOrUnsupportedArchive
                    : ArchiveOpenFailureKind::ReadFailed;
            return std::unexpected(Failure(kind, archive_error, e.error()));
        }
    }

    return std::unexpected(Failure(ClassifyFailure(_parent, archive_error), archive_error));
}

VFSHostPtr VFSArchiveProxy::OpenFileAsArchive(const std::string &_path,
                                              const VFSHostPtr &_parent,
                                              std::function<std::string()> _passwd,
                                              VFSCancelChecker _cancel_checker)
{
    ArchivePasswordProvider password_provider;
    if( _passwd ) {
        password_provider = [legacy = std::move(_passwd)]() mutable -> std::optional<std::string> {
            std::string password = legacy();
            if( password.empty() )
                return std::nullopt;
            return password;
        };
    }

    ArchiveOpenResult result =
        OpenFileAsArchiveResult(_path, _parent, std::move(password_provider), std::move(_cancel_checker));
    return result ? std::move(*result) : nullptr;
}

} // namespace nc::vfs
