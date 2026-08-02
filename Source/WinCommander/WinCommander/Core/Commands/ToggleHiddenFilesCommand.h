// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandRegistry.h"
#include <functional>
#include <stdexcept>

namespace nc::core {

class ViewToggleHiddenFilesError final : public std::runtime_error
{
public:
    ViewToggleHiddenFilesError();
};

using HiddenFilesVisibilitySetter = std::function<bool(void *native_target, bool shows_hidden_files)>;

/**
 * Builds the pure view.toggleHiddenFiles command. The caller supplies the current pane visibility
 * snapshot in CommandContext and the application composition layer owns the synchronous update.
 */
[[nodiscard]] CommandRegistry::Registration
MakeViewToggleHiddenFilesCommand(HiddenFilesVisibilitySetter _setter);

} // namespace nc::core
