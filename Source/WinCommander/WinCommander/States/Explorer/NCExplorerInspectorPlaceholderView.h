// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

/**
 * A trivial placeholder for the Explorer-mode inspector/metadata pane: a single centered label
 * telling the user to select an item. This is an intentional stub for Milestone 1 - it has no
 * PanelController dependency and does not bind to any live data. The real inspector pane is out
 * of scope for this pass.
 */
@interface NCExplorerInspectorPlaceholderView : NSView

- (instancetype)initWithFrame:(NSRect)frameRect;

@end
