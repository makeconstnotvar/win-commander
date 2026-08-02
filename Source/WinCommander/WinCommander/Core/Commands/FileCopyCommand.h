// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandRegistry.h"
#include <functional>

namespace nc::core {

using FileCopyWriter = std::function<void(std::span<const vfs::ListingItem>)>;

/**
 * Builds the single file.copy definition used by menu, shortcut, command-bar and context-menu
 * adapters. The writer is an application-composition dependency and must consume the borrowed
 * items synchronously.
 */
[[nodiscard]] CommandRegistry::Registration MakeFileCopyCommand(FileCopyWriter _writer);

} // namespace nc::core
