// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandRegistry.h"
#include <functional>
#include <stdexcept>

namespace nc::core {

class FileRenameInitiationError final : public std::runtime_error
{
public:
    FileRenameInitiationError();
};

using FileRenameInitiator =
    std::function<bool(void *native_target, const vfs::ListingItem &item)>;

/**
 * Builds the file.rename initiation command. Execution only opens the pane-owned inline editor;
 * the existing rename commit path remains responsible for scheduling the filesystem operation.
 */
[[nodiscard]] CommandRegistry::Registration MakeFileRenameCommand(FileRenameInitiator _initiator);

} // namespace nc::core
