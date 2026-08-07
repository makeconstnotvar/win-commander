// Copyright (C) 2014-2025 Michael Kazakov. Subject to GNU General Public License version 3.

#pragma once

#include "Host.h"
#include <optional>

namespace nc::vfs {

enum class ArchiveOpenFailureKind : uint8_t {
    PasswordRequired,
    PasswordCancelled,
    PasswordRejectedOrInvalidArchive,
    SourceMissing,
    SourcePermissionDenied,
    SourceUnavailable,
    Cancelled,
    InvalidOrUnsupportedArchive,
    ReadFailed
};

/**
 * Typed failure from archive-host acquisition.
 *
 * primary_error is the failure selected for presentation. fallback_error retains the other
 * format attempt when the proxy tried both ArchiveHost and ArchiveRawHost.
 */
struct ArchiveOpenFailure {
    ArchiveOpenFailureKind kind;
    Error primary_error;
    std::optional<Error> fallback_error;
};

using ArchiveOpenResult = std::expected<VFSHostPtr, ArchiveOpenFailure>;
using ArchivePasswordProvider = std::function<std::optional<std::string>()>;

class VFSArchiveProxy
{
public:
    /** Opens an archive while preserving acquisition failure and password-cancellation semantics. */
    static ArchiveOpenResult OpenFileAsArchiveResult(const std::string &_path,
                                                     const VFSHostPtr &_parent,
                                                     ArchivePasswordProvider _passwd = nullptr,
                                                     VFSCancelChecker _cancel_checker = nullptr);

    /** Compatibility wrapper for existing archive-browsing callers. */
    static VFSHostPtr OpenFileAsArchive(const std::string &_path,
                                        const VFSHostPtr &_parent,
                                        std::function<std::string()> _passwd = nullptr,
                                        VFSCancelChecker _cancel_checker = nullptr);
};

} // namespace nc::vfs
