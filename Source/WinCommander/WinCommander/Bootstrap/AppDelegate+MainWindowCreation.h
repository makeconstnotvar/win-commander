// Copyright (C) 2018 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "AppDelegate.h"

@class PanelController;

namespace nc::panel {
class DirectoryAccessProvider;
class FileOpener;
} // namespace nc::panel

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
