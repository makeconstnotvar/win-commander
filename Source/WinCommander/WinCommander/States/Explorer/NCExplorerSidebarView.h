// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

@class PanelController;

/**
 * A persistent, Finder-like sidebar for NCExplorerState: static "Favorites" / "This Mac" /
 * "Network" sections built from the app-wide FavoriteLocationsStorage / NativeFSManager /
 * NetworkConnectionsManager (see NCAppDelegate.me). Clicking a row navigates the PanelController
 * supplied at construction time to that location via -GoToDirWithContext:, reusing the same
 * location-resolution logic as the dual-pane "Go to" popups (see Actions/ShowGoToPopup.mm).
 *
 * This view does not observe the underlying storages on its own - the owner may call -reloadData
 * to rebuild the sections (e.g. after the app-wide favorites/volumes/connections change).
 */
@interface NCExplorerSidebarView : NSView

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel;

@property(nonatomic, readonly) PanelController *panelController;

/**
 * Rebuilds the Favorites/This Mac/Network sections from the current app-wide storages.
 * Called automatically once at construction time.
 */
- (void)reloadData;

@end
