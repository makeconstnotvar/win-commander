// Copyright (C) 2018 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "AppDelegate.h"

#include <optional>

@class PanelController;

namespace nc::panel {
class DirectoryAccessProvider;
class FileOpener;
} // namespace nc::panel

namespace nc::bootstrap {

enum class MainWindowCreationKind : uint8_t {
    Default,
    ManualRestoration,
    SystemRestoration
};

struct DefaultExplorerStartupPlan {
    bool restore_stored_session = false;
    bool ensure_explorer_without_restore = false;
};

[[nodiscard]] constexpr DefaultExplorerStartupPlan
PlanDefaultExplorerStartup(const MainWindowCreationKind _kind,
                           const bool _can_copy_last_window,
                           const bool _restore_stored_session) noexcept
{
    if( _kind == MainWindowCreationKind::SystemRestoration )
        return {};
    if( _kind == MainWindowCreationKind::ManualRestoration && !_can_copy_last_window && _restore_stored_session )
        return {.restore_stored_session = true};
    return {.ensure_explorer_without_restore = true};
}

[[nodiscard]] constexpr bool
ShouldEnsureDefaultExplorer(const DefaultExplorerStartupPlan &_plan,
                            const std::optional<bool> _stored_session_restore_result = std::nullopt) noexcept
{
    if( _plan.restore_stored_session )
        return _stored_session_restore_result.has_value() && !*_stored_session_restore_result;
    return _plan.ensure_explorer_without_restore;
}

} // namespace nc::bootstrap

// this category is private to NCAppDelegate
@interface NCAppDelegate (MainWindowCreation)

// these methods don't call showWindow, it's client's responsibility.

- (NCMainWindowController *)allocateDefaultMainWindow;
- (NCMainWindowController *)allocateMainWindowRestoredManually;
- (NCMainWindowController *)allocateMainWindowRestoredBySystem;

// Builds a standalone PanelController (own PanelView, icon repository, actions dispatcher) with no
// dual-pane assumptions, for hosting inside NCExplorerState.
- (PanelController *)allocateExplorerPanelController;

/** Shared opener used by production file.open composition and Open With surfaces. */
- (nc::panel::FileOpener &)fileOpener;

/** Shared access gate used by window-scoped operation planning. */
- (nc::panel::DirectoryAccessProvider &)directoryAccessProvider;

@end
