// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

@class PanelController;

/**
 * A breadcrumb / address bar for NCExplorerState.
 * Shows the panel's current directory as clickable path segments ("Macintosh HD > Users > alex >
 * Documents") backed by a native NSPathControl when the panel's listing is uniform and resides on
 * a native filesystem. For anything else (a non-native VFS location, or a non-uniform listing such
 * as search results) it falls back to a plain, non-interactive text label describing the location.
 *
 * This control does not observe the panel controller on its own - the owner must call
 * -panelPathChanged explicitly whenever the panel's directory/listing changes (e.g. from
 * NCPanelControllerHostingState.PanelPathChanged:).
 */
@interface NCExplorerBreadcrumbControl : NSView

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel;

/**
 * Re-reads the panel controller's current directory/listing state and updates the displayed
 * breadcrumb (or the fallback label) accordingly. Must be called by the owner whenever the panel's
 * path changes.
 */
- (void)panelPathChanged;

@end
