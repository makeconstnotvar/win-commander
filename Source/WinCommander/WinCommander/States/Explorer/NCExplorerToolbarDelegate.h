// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

@class PanelController;

/**
 * Minimal toolbar for NCExplorerState: Back/Forward/Up/Refresh (wired directly to the panel's own
 * NCPanelControllerActionsDispatcher, same actions the dual-pane menu/shortcuts use) plus a button
 * to toggle back into the dual-pane Commander mode.
 */
@interface NCExplorerToolbarDelegate : NSObject <NSToolbarDelegate>

- (instancetype)initWithPanelController:(PanelController *)_panel;

@property(nonatomic, readonly) NSToolbar *toolbar;

/**
 * Forwarded to the embedded breadcrumb control - call whenever the panel's directory changes.
 */
- (void)panelPathChanged;

@end
