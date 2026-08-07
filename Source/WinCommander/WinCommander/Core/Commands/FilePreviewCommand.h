// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandRegistry.h"

#include <functional>

namespace nc::core {

/** Borrowed synchronous invocation intent delivered to the live pane preview port. */
struct FilePreviewIntent final {
    CommandInvocationSource source = CommandInvocationSource::Programmatic;
    const void *native_sender = nullptr;

    bool operator==(const FilePreviewIntent &) const noexcept = default;
};

/**
 * Synchronously asks a borrowed live pane target to toggle Quick Look.
 *
 * The target, sender and exact listing item are valid only for the call and must not be retained.
 * The item is a read-only presentation handle, not mutation authority. The handler revalidates
 * that exact item against the live pane before performing the preview action.
 */
using FilePreviewHandler =
    std::function<bool(void *native_target, const vfs::ListingItem &item, FilePreviewIntent intent)>;

/** Builds the pure read-only file.preview Registry definition. */
[[nodiscard]] CommandRegistry::Registration MakeFilePreviewCommand(FilePreviewHandler _handler);

} // namespace nc::core
