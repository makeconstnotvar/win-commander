// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandRegistry.h"
#include <functional>
#include <stdexcept>

namespace nc::core {

enum class FileCutIntent {
    Move,
};

class FileCutWriteError final : public std::runtime_error
{
public:
    FileCutWriteError();
};

using FileCutWriter = std::function<bool(std::span<const vfs::ListingItem>, FileCutIntent)>;

/**
 * Builds the pure file.cut definition. The caller supplies a borrowed, whole item context and the
 * application composition layer owns the synchronous move-intent pasteboard writer.
 */
[[nodiscard]] CommandRegistry::Registration MakeFileCutCommand(FileCutWriter _writer);

} // namespace nc::core
