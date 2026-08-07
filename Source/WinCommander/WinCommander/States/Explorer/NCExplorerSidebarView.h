// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

@class PanelController;

/**
 * A persistent source-list sidebar for NCExplorerState. Its collapsible sections are populated
 * from FavoriteLocationsStorage, NativeFSManager, NetworkConnectionsManager and TagsStorage.
 * Row activation shares the location-resolution path used by the keyboard Go-To popovers.
 */
@interface NCExplorerSidebarView : NSView

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel;

@property(nonatomic, readonly) PanelController *panelController;

/** Rebuilds pane-bound actions and selection tests for a newly active Explorer tab. */
- (void)rebindToPanelController:(PanelController *)_panel;

/**
 * Rebuilds all sections from the current app-wide storages.
 * Called automatically once at construction time.
 */
- (void)reloadData;

/** Synchronizes the selected source-list item with the panel's current location. */
- (void)panelPathChanged;

@end
