// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

@class PanelController;
@class NCPanelControllerActionsDispatcher;

#ifdef __cplusplus
namespace nc::core {
struct PaneSnapshot;
}
#endif

/**
 * Minimal toolbar for NCExplorerState: Back/Forward/Up/Refresh (wired directly to the panel's own
 * NCPanelControllerActionsDispatcher, same actions the dual-pane menu/shortcuts use) plus a button
 * to toggle back into the dual-pane Commander mode.
 */
@interface NCExplorerToolbarDelegate : NSObject <NSToolbarDelegate>

- (instancetype)initWithPanelController:(PanelController *)_panel;
- (instancetype)initWithPanelController:(PanelController *)_panel
                      actionsDispatcher:(NCPanelControllerActionsDispatcher *)_dispatcher;

@property(nonatomic, readonly) NSToolbar *toolbar;
@property(nonatomic, readonly) NSProgressIndicator *busyIndicator;
@property(nonatomic, readonly) PanelController *panelController;

/** Atomically retargets navigation controls and the breadcrumb to a different Explorer pane. */
- (void)rebindToPanelController:(PanelController *)_panel;

#ifdef __cplusplus
/** Forwards immutable pane state to the breadcrumb renderer. Must be called on the main queue. */
- (void)applyPaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot;
#endif

- (void)focusAddressField;

@end
