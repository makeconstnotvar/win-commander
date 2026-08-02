// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "FileManagerErrorAdapter.h"
#include <cerrno>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace nc::core {

namespace {

struct Classification {
    FileManagerErrorCategory category = FileManagerErrorCategory::UnknownError;
    FileManagerErrorSeverity severity = FileManagerErrorSeverity::BlockingError;
    std::string_view user_message_key = file_manager_error_messages::UnknownErrorKey;
};

[[nodiscard]] constexpr Classification ClassifyPOSIX(const int64_t _code) noexcept
{
    if( _code == EACCES || _code == EPERM ) {
        return {
            .category = FileManagerErrorCategory::PermissionError,
            .severity = FileManagerErrorSeverity::BlockingError,
            .user_message_key = "errors.permission",
        };
    }

    if( _code == ENOENT || _code == ENOTDIR ) {
        return {
            .category = FileManagerErrorCategory::PathNotFoundError,
            .severity = FileManagerErrorSeverity::BlockingError,
            .user_message_key = "errors.pathNotFound",
        };
    }

    if( _code == EROFS ) {
        return {
            .category = FileManagerErrorCategory::ReadOnlyError,
            .severity = FileManagerErrorSeverity::BlockingError,
            .user_message_key = "errors.readOnly",
        };
    }

    if( _code == EEXIST || _code == ENOTEMPTY ) {
        return {
            .category = FileManagerErrorCategory::ConflictError,
            .severity = FileManagerErrorSeverity::BlockingError,
            .user_message_key = "errors.conflict",
        };
    }

    if( _code == ENOSPC || _code == EDQUOT ) {
        return {
            .category = FileManagerErrorCategory::InsufficientSpaceError,
            .severity = FileManagerErrorSeverity::BlockingError,
            .user_message_key = "errors.insufficientSpace",
        };
    }

    if( _code == EBUSY
#ifdef ETXTBSY
        || _code == ETXTBSY
#endif
    ) {
        return {
            .category = FileManagerErrorCategory::FileBusyError,
            .severity = FileManagerErrorSeverity::BlockingError,
            .user_message_key = "errors.fileBusy",
        };
    }

    if( _code == ETIMEDOUT ) {
        return {
            .category = FileManagerErrorCategory::TimeoutError,
            .severity = FileManagerErrorSeverity::BlockingError,
            .user_message_key = "errors.timeout",
        };
    }

    if( _code == ENETDOWN || _code == ENETUNREACH || _code == ENETRESET || _code == ECONNABORTED ||
        _code == ECONNRESET || _code == ENOTCONN || _code == ESHUTDOWN || _code == ECONNREFUSED ||
        _code == EHOSTDOWN || _code == EHOSTUNREACH ) {
        return {
            .category = FileManagerErrorCategory::NetworkError,
            .severity = FileManagerErrorSeverity::BlockingError,
            .user_message_key = "errors.network",
        };
    }

#ifdef EAUTH
    if( _code == EAUTH ) {
        return {
            .category = FileManagerErrorCategory::AuthenticationError,
            .severity = FileManagerErrorSeverity::BlockingError,
            .user_message_key = "errors.authentication",
        };
    }
#endif

#ifdef ENEEDAUTH
    if( _code == ENEEDAUTH ) {
        return {
            .category = FileManagerErrorCategory::AuthenticationError,
            .severity = FileManagerErrorSeverity::BlockingError,
            .user_message_key = "errors.authentication",
        };
    }
#endif

    if( _code == ENOTSUP || _code == ENOSYS
#ifdef EOPNOTSUPP
        || _code == EOPNOTSUPP
#endif
    ) {
        return {
            .category = FileManagerErrorCategory::ProviderUnsupportedError,
            .severity = FileManagerErrorSeverity::BlockingError,
            .user_message_key = "errors.providerUnsupported",
        };
    }

    if( _code == ECANCELED ) {
        return {
            .category = FileManagerErrorCategory::OperationCancelledError,
            .severity = FileManagerErrorSeverity::Info,
            .user_message_key = "errors.operationCancelled",
        };
    }

    return {};
}

void Validate(const FileManagerErrorRecoveryDisposition &_disposition,
              const FileManagerErrorAdapter::ActionValidator &_action_validator)
{
    const bool needs_action = _disposition.recoverable || _disposition.retryable || _disposition.requires_user_action;
    if( needs_action && _disposition.suggested_actions.empty() ) {
        throw std::invalid_argument{"FileManagerError recovery disposition requires a suggested action"};
    }

    if( !_disposition.suggested_actions.empty() && !_action_validator )
        throw std::invalid_argument{"FileManagerError recovery disposition requires an action validator"};

    for( const CommandId &action : _disposition.suggested_actions ) {
        if( !action.IsValid() )
            throw std::invalid_argument{"FileManagerError recovery disposition contains an invalid action"};
        if( !_action_validator(action) )
            throw std::invalid_argument{"FileManagerError recovery disposition contains an unregistered action"};
    }
}

[[nodiscard]] Classification Classify(const std::string_view _domain, const int64_t _code) noexcept
{
    if( _domain == nc::Error::POSIX )
        return ClassifyPOSIX(_code);
    return {};
}

} // namespace

FileManagerError
FileManagerErrorAdapter::FromError(nc::Error _error,
                                   FileManagerErrorContext _context,
                                   const Timestamp _timestamp,
                                   ActionValidator _action_validator)
{
    std::string domain = _error.Domain();
    const int64_t code = _error.Code();
    std::string user_message = _error.LocalizedFailureReason();
    std::string technical_message = _error.Description();
    const Classification classification = Classify(domain, code);
    if( classification.category == FileManagerErrorCategory::UnknownError && user_message == technical_message )
        user_message = file_manager_error_messages::UnknownErrorFallback;

    FileManagerErrorSeverity severity = classification.severity;
    bool recoverable = false;
    bool retryable = false;
    bool requires_user_action = false;
    std::vector<CommandId> suggested_actions;
    if( _context.recovery_disposition ) {
        Validate(*_context.recovery_disposition, _action_validator);
        severity = _context.recovery_disposition->severity;
        recoverable = _context.recovery_disposition->recoverable;
        retryable = _context.recovery_disposition->retryable;
        requires_user_action = _context.recovery_disposition->requires_user_action;
        suggested_actions = std::move(_context.recovery_disposition->suggested_actions);
    }

    return FileManagerError{
        .code = FileManagerErrorCode{.domain = std::move(domain), .value = code},
        .category = classification.category,
        .severity = severity,
        .user_message_key = std::string{classification.user_message_key},
        .user_message = std::move(user_message),
        .technical_message = std::move(technical_message),
        .affected_items = std::move(_context.affected_items),
        .operation_id = std::move(_context.operation_id),
        .provider_id = std::move(_context.provider_id),
        .recoverable = recoverable,
        .retryable = retryable,
        .requires_user_action = requires_user_action,
        .suggested_actions = std::move(suggested_actions),
        .original_error = std::move(_error),
        .timestamp = _timestamp,
    };
}

} // namespace nc::core
