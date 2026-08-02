// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Errors/FileManagerErrorAdapter.h>
#include <array>
#include <cerrno>
#include <stdexcept>

namespace {

using nc::core::CommandId;
using nc::core::FileManagerErrorAdapter;
using nc::core::FileManagerErrorCategory;
using nc::core::FileManagerErrorContext;
using nc::core::FileManagerErrorRecoveryDisposition;
using nc::core::FileManagerErrorSeverity;

struct ExpectedClassification {
    int code;
    FileManagerErrorCategory category;
    FileManagerErrorSeverity severity;
    std::string_view user_message_key;
};

constexpr auto g_POSIXCases = std::to_array<ExpectedClassification>({
    {EACCES, FileManagerErrorCategory::PermissionError, FileManagerErrorSeverity::BlockingError, "errors.permission"},
    {EPERM, FileManagerErrorCategory::PermissionError, FileManagerErrorSeverity::BlockingError, "errors.permission"},
    {ENOENT,
     FileManagerErrorCategory::PathNotFoundError,
     FileManagerErrorSeverity::BlockingError,
     "errors.pathNotFound"},
    {ENOTDIR,
     FileManagerErrorCategory::PathNotFoundError,
     FileManagerErrorSeverity::BlockingError,
     "errors.pathNotFound"},
    {EROFS, FileManagerErrorCategory::ReadOnlyError, FileManagerErrorSeverity::BlockingError, "errors.readOnly"},
    {EEXIST, FileManagerErrorCategory::ConflictError, FileManagerErrorSeverity::BlockingError, "errors.conflict"},
    {ENOTEMPTY, FileManagerErrorCategory::ConflictError, FileManagerErrorSeverity::BlockingError, "errors.conflict"},
    {ENOSPC,
     FileManagerErrorCategory::InsufficientSpaceError,
     FileManagerErrorSeverity::BlockingError,
     "errors.insufficientSpace"},
    {EDQUOT,
     FileManagerErrorCategory::InsufficientSpaceError,
     FileManagerErrorSeverity::BlockingError,
     "errors.insufficientSpace"},
    {EBUSY, FileManagerErrorCategory::FileBusyError, FileManagerErrorSeverity::BlockingError, "errors.fileBusy"},
#ifdef ETXTBSY
    {ETXTBSY, FileManagerErrorCategory::FileBusyError, FileManagerErrorSeverity::BlockingError, "errors.fileBusy"},
#endif
    {ETIMEDOUT, FileManagerErrorCategory::TimeoutError, FileManagerErrorSeverity::BlockingError, "errors.timeout"},
    {ENETDOWN, FileManagerErrorCategory::NetworkError, FileManagerErrorSeverity::BlockingError, "errors.network"},
    {ENETUNREACH, FileManagerErrorCategory::NetworkError, FileManagerErrorSeverity::BlockingError, "errors.network"},
    {ENETRESET, FileManagerErrorCategory::NetworkError, FileManagerErrorSeverity::BlockingError, "errors.network"},
    {ECONNABORTED, FileManagerErrorCategory::NetworkError, FileManagerErrorSeverity::BlockingError, "errors.network"},
    {ECONNRESET, FileManagerErrorCategory::NetworkError, FileManagerErrorSeverity::BlockingError, "errors.network"},
    {ENOTCONN, FileManagerErrorCategory::NetworkError, FileManagerErrorSeverity::BlockingError, "errors.network"},
    {ESHUTDOWN, FileManagerErrorCategory::NetworkError, FileManagerErrorSeverity::BlockingError, "errors.network"},
    {ECONNREFUSED, FileManagerErrorCategory::NetworkError, FileManagerErrorSeverity::BlockingError, "errors.network"},
    {EHOSTDOWN, FileManagerErrorCategory::NetworkError, FileManagerErrorSeverity::BlockingError, "errors.network"},
    {EHOSTUNREACH, FileManagerErrorCategory::NetworkError, FileManagerErrorSeverity::BlockingError, "errors.network"},
#ifdef EAUTH
    {EAUTH,
     FileManagerErrorCategory::AuthenticationError,
     FileManagerErrorSeverity::BlockingError,
     "errors.authentication"},
#endif
#ifdef ENEEDAUTH
    {ENEEDAUTH,
     FileManagerErrorCategory::AuthenticationError,
     FileManagerErrorSeverity::BlockingError,
     "errors.authentication"},
#endif
    {ENOTSUP,
     FileManagerErrorCategory::ProviderUnsupportedError,
     FileManagerErrorSeverity::BlockingError,
     "errors.providerUnsupported"},
    {ENOSYS,
     FileManagerErrorCategory::ProviderUnsupportedError,
     FileManagerErrorSeverity::BlockingError,
     "errors.providerUnsupported"},
    {ECANCELED,
     FileManagerErrorCategory::OperationCancelledError,
     FileManagerErrorSeverity::Info,
     "errors.operationCancelled"},
});

constexpr auto g_AmbiguousPOSIXCases = std::to_array({EINVAL, ENODEV, EIO, EPIPE, EADDRNOTAVAIL});

} // namespace

#define PREFIX "nc::core::FileManagerErrorAdapter "

TEST_CASE(PREFIX "maps only the conservative POSIX baseline")
{
    const auto timestamp = std::chrono::system_clock::time_point{std::chrono::seconds{123}};

    for( const auto &expected : g_POSIXCases ) {
        CAPTURE(expected.code);
        const nc::Error source{nc::Error::POSIX, expected.code};
        const auto result = FileManagerErrorAdapter::FromError(source, {}, timestamp);

        CHECK(result.code.domain == nc::Error::POSIX);
        CHECK(result.code.value == expected.code);
        CHECK(result.category == expected.category);
        CHECK(result.severity == expected.severity);
        CHECK(result.user_message == source.LocalizedFailureReason());
        CHECK(result.technical_message == source.Description());
        CHECK(result.user_message_key == expected.user_message_key);
        CHECK_FALSE(result.recoverable);
        CHECK_FALSE(result.retryable);
        CHECK_FALSE(result.requires_user_action);
        CHECK(result.original_error == source);
        CHECK(result.timestamp == timestamp);
        CHECK(result.affected_items.empty());
        CHECK_FALSE(result.operation_id);
        CHECK_FALSE(result.provider_id);
        CHECK(result.suggested_actions.empty());
    }
}

TEST_CASE(PREFIX "snapshots messages and supplied context")
{
    nc::Error source{nc::Error::POSIX, ETIMEDOUT};
    source.LocalizedFailureReason("Injected timeout failure");
    const std::string technical_message = source.Description();

    FileManagerErrorContext context;
    context.affected_items = {"/private/example", "/private/second"};
    context.operation_id = "operation-42";
    context.provider_id = "native";
    context.recovery_disposition = FileManagerErrorRecoveryDisposition{
        .severity = FileManagerErrorSeverity::RecoverableError,
        .recoverable = true,
        .retryable = true,
        .requires_user_action = true,
        .suggested_actions = {CommandId{nc::core::command_ids::FileOpen}},
    };
    const auto timestamp = std::chrono::system_clock::time_point{std::chrono::milliseconds{456}};

    const auto action_validator = [](const CommandId &_id) {
        return _id == CommandId{nc::core::command_ids::FileOpen};
    };
    const auto result = FileManagerErrorAdapter::FromError(source, context, timestamp, action_validator);
    source.LocalizedFailureReason("Changed after adaptation");

    CHECK(result.category == FileManagerErrorCategory::TimeoutError);
    CHECK(result.user_message_key == "errors.timeout");
    CHECK(result.user_message == "Injected timeout failure");
    CHECK(result.technical_message == technical_message);
    CHECK(result.original_error == source);
    CHECK(result.original_error.LocalizedFailureReason() == "Injected timeout failure");
    CHECK(result.affected_items == context.affected_items);
    CHECK(result.operation_id == context.operation_id);
    CHECK(result.provider_id == context.provider_id);
    CHECK(result.severity == FileManagerErrorSeverity::RecoverableError);
    CHECK(result.recoverable);
    CHECK(result.retryable);
    CHECK(result.requires_user_action);
    CHECK(result.suggested_actions == context.recovery_disposition->suggested_actions);
    CHECK(result.timestamp == timestamp);

    for( const auto &item : context.affected_items ) {
        CHECK(result.user_message.find(item) == std::string::npos);
        CHECK(result.technical_message.find(item) == std::string::npos);
    }
}

TEST_CASE(PREFIX "leaves ambiguous POSIX values unknown without flow context")
{
    for( const int code : g_AmbiguousPOSIXCases ) {
        CAPTURE(code);
        const nc::Error source{nc::Error::POSIX, code};
        const auto result = FileManagerErrorAdapter::FromError(source);

        CHECK(result.code.domain == nc::Error::POSIX);
        CHECK(result.code.value == code);
        CHECK(result.category == FileManagerErrorCategory::UnknownError);
        CHECK(result.severity == FileManagerErrorSeverity::BlockingError);
        CHECK(result.user_message_key == nc::core::file_manager_error_messages::UnknownErrorKey);
        CHECK_FALSE(result.recoverable);
        CHECK_FALSE(result.retryable);
        CHECK_FALSE(result.requires_user_action);
        CHECK(result.original_error == source);
    }
}

TEST_CASE(PREFIX "preserves an unknown domain without inventing recovery semantics")
{
    const nc::Error source{"TestErr", 9876};
    const auto timestamp = std::chrono::system_clock::time_point{std::chrono::seconds{789}};
    const auto result = FileManagerErrorAdapter::FromError(source, {}, timestamp);

    CHECK(result.code.domain == "TestErr");
    CHECK(result.code.value == 9876);
    CHECK(result.category == FileManagerErrorCategory::UnknownError);
    CHECK(result.severity == FileManagerErrorSeverity::BlockingError);
    CHECK(result.user_message_key == nc::core::file_manager_error_messages::UnknownErrorKey);
    CHECK(result.user_message == nc::core::file_manager_error_messages::UnknownErrorFallback);
    CHECK_FALSE(result.recoverable);
    CHECK_FALSE(result.retryable);
    CHECK_FALSE(result.requires_user_action);
    CHECK(result.technical_message == source.Description());
    CHECK(result.original_error == source);
    CHECK(result.timestamp == timestamp);
}

TEST_CASE(PREFIX "preserves a custom user message for an unknown domain")
{
    nc::Error source{"TestLocalized", 42};
    source.LocalizedFailureReason("A provider-specific explanation");

    const auto result = FileManagerErrorAdapter::FromError(source);

    CHECK(result.category == FileManagerErrorCategory::UnknownError);
    CHECK(result.user_message_key == nc::core::file_manager_error_messages::UnknownErrorKey);
    CHECK(result.user_message == "A provider-specific explanation");
    CHECK(result.technical_message == source.Description());
    CHECK(result.original_error.LocalizedFailureReason() == "A provider-specific explanation");
}

TEST_CASE(PREFIX "rejects inconsistent recovery dispositions")
{
    const nc::Error source{nc::Error::POSIX, EACCES};

    SECTION("actionable disposition without an action")
    {
        FileManagerErrorContext context;
        context.recovery_disposition = FileManagerErrorRecoveryDisposition{
            .severity = FileManagerErrorSeverity::RecoverableError,
            .recoverable = true,
        };
        CHECK_THROWS_AS(FileManagerErrorAdapter::FromError(source, context), std::invalid_argument);
    }

    SECTION("retryable disposition without an action")
    {
        FileManagerErrorContext context;
        context.recovery_disposition = FileManagerErrorRecoveryDisposition{
            .severity = FileManagerErrorSeverity::RecoverableError,
            .retryable = true,
        };
        CHECK_THROWS_AS(FileManagerErrorAdapter::FromError(source, context), std::invalid_argument);
    }

    SECTION("user-action disposition without an action")
    {
        FileManagerErrorContext context;
        context.recovery_disposition = FileManagerErrorRecoveryDisposition{
            .requires_user_action = true,
        };
        CHECK_THROWS_AS(FileManagerErrorAdapter::FromError(source, context), std::invalid_argument);
    }

    SECTION("invalid command id")
    {
        FileManagerErrorContext context;
        context.recovery_disposition = FileManagerErrorRecoveryDisposition{
            .suggested_actions = {CommandId{"invalid-action"}},
        };
        CHECK_THROWS_AS(FileManagerErrorAdapter::FromError(
                            source, context, FileManagerErrorAdapter::Timestamp{}, [](const CommandId &) { return true; }),
                        std::invalid_argument);
    }

    SECTION("suggested action without a validator")
    {
        FileManagerErrorContext context;
        context.recovery_disposition = FileManagerErrorRecoveryDisposition{
            .suggested_actions = {CommandId{nc::core::command_ids::FileOpen}},
        };
        CHECK_THROWS_AS(FileManagerErrorAdapter::FromError(source, context), std::invalid_argument);
    }

    SECTION("unregistered suggested action")
    {
        FileManagerErrorContext context;
        context.recovery_disposition = FileManagerErrorRecoveryDisposition{
            .suggested_actions = {CommandId{nc::core::command_ids::FileOpen}},
        };
        CHECK_THROWS_AS(
            FileManagerErrorAdapter::FromError(
                source, context, FileManagerErrorAdapter::Timestamp{}, [](const CommandId &) { return false; }),
            std::invalid_argument);
    }
}

#undef PREFIX
