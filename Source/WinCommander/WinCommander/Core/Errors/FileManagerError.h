// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Base/Error.h>
#include <WinCommander/Core/Commands/CommandId.h>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nc::core {

enum class FileManagerErrorCategory : uint8_t {
    PermissionError,
    PathNotFoundError,
    VolumeUnavailableError,
    NetworkError,
    ConflictError,
    ValidationError,
    ProviderUnsupportedError,
    InsufficientSpaceError,
    FileBusyError,
    ReadOnlyError,
    ChecksumMismatchError,
    OperationCancelledError,
    PartialFailureError,
    TimeoutError,
    AuthenticationError,
    RateLimitError,
    UnknownError
};

enum class FileManagerErrorSeverity : uint8_t {
    Info,
    Warning,
    RecoverableError,
    BlockingError,
    DestructiveRisk,
    FatalError
};

namespace file_manager_error_messages {

inline constexpr std::string_view UnknownErrorKey = "errors.unknown";
inline constexpr std::string_view UnknownErrorFallback = "The operation could not be completed.";

} // namespace file_manager_error_messages

struct FileManagerErrorCode {
    std::string domain;
    int64_t value = 0;

    friend bool operator==(const FileManagerErrorCode &, const FileManagerErrorCode &) = default;
};

/** Flow-specific presentation policy supplied by a caller that owns recovery commands. */
struct FileManagerErrorRecoveryDisposition {
    FileManagerErrorSeverity severity = FileManagerErrorSeverity::BlockingError;
    bool recoverable = false;
    bool retryable = false;
    bool requires_user_action = false;
    std::vector<CommandId> suggested_actions;
};

struct FileManagerErrorContext {
    std::vector<std::string> affected_items;
    std::optional<std::string> operation_id;
    std::optional<std::string> provider_id;
    std::optional<FileManagerErrorRecoveryDisposition> recovery_disposition;
};

/**
 * A product-facing, immutable-by-convention snapshot of one low-level failure.
 *
 * user_message_key is resolved by the app presenter. user_message is a captured provider message
 * or source-language fallback used when localization resolution is unavailable. technical_message
 * is diagnostic text. original_error keeps the structured low-level value for provider-specific
 * handling.
 */
struct FileManagerError {
    FileManagerErrorCode code;
    FileManagerErrorCategory category = FileManagerErrorCategory::UnknownError;
    FileManagerErrorSeverity severity = FileManagerErrorSeverity::BlockingError;
    std::string user_message_key;
    std::string user_message;
    std::string technical_message;
    std::vector<std::string> affected_items;
    std::optional<std::string> operation_id;
    std::optional<std::string> provider_id;
    bool recoverable = false;
    bool retryable = false;
    bool requires_user_action = false;
    std::vector<CommandId> suggested_actions;
    nc::Error original_error;
    std::chrono::system_clock::time_point timestamp;

    friend bool operator==(const FileManagerError &, const FileManagerError &) = default;
};

} // namespace nc::core
