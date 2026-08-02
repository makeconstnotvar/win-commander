// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "VisualState.h"
#include <WinCommander/Core/Commands/Command.h>
#include <WinCommander/Core/Errors/FileManagerError.h>
#include <WinCommander/Core/Pane/PaneSnapshot.h>

namespace nc::core {

/** Pure projection from semantic engine snapshots to toolkit-independent presentation state. */
class VisualStateMapper final
{
public:
    /** Uses an explicit error when supplied, otherwise the request-scoped error in the snapshot. */
    [[nodiscard]] static PaneVisualState MapPane(const PaneSnapshot &_snapshot,
                                                 const FileManagerError *_current_navigation_error = nullptr);
    [[nodiscard]] static CommandVisualState MapCommand(const CommandState &_state);
};

} // namespace nc::core
