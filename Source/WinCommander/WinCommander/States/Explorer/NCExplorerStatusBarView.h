// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

@class PanelController;

/**
 * A thin horizontal status bar for the bottom of the Explorer window (single-pane mode).
 * Shows the total item count of the current directory, the selected item count together with
 * the selected items' total size, and the free space available on the current volume.
 *
 * This view does not observe the panel on its own - the caller is expected to invoke -refresh
 * explicitly whenever the panel's directory or selection changes.
 */
@interface NCExplorerStatusBarView : NSView

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel;

/**
 * Recomputes the item/selection counts and sizes from the panel's current data and re-triggers
 * a free-space lookup for the panel's current volume. Call this whenever the directory or
 * selection changes.
 */
- (void)refresh;

@end
