// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandRegistry.h"
#include <functional>
#include <stdexcept>

namespace nc::core {

class FileOpenExecutionError final : public std::runtime_error
{
public:
    FileOpenExecutionError();
};

using FileOpenExecutor = std::function<bool(void *native_target, std::span<const vfs::ListingItem> items)>;

/**
 * Builds the explicit external-open command. Availability is derived synchronously from borrowed
 * item/provider facts; the injected executor owns the mandatory live submission check.
 */
[[nodiscard]] CommandRegistry::Registration MakeFileOpenCommand(FileOpenExecutor _executor);

} // namespace nc::core
