// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>
#include <WinCommander/Core/VisualState/VisualState.h>

/**
 * Presents blocking and content-empty Explorer pane states derived from the shared typed visual-state model.
 * The owner keeps the file view mounted and calls -updateWithVisualState: for each admitted active snapshot.
 */
@interface NCExplorerPaneStateView : NSView

- (instancetype)initWithFrame:(NSRect)_frame;

/**
 * Replaces the complete presentation from one admitted visual state. Error states that retain committed
 * content leave this overlay hidden so a failed refresh cannot cover the active listing.
 */
- (void)updateWithVisualState:(const nc::core::PaneVisualState &)_state;

@end
