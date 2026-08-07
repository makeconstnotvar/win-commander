// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

@class PanelController;

#ifdef __cplusplus
namespace nc::core {
struct PaneSnapshot;
}
#endif

NS_ASSUME_NONNULL_BEGIN

/** Explorer address bar: path segments, sibling menus, editable path and Find Files entry point. */
@interface NCExplorerBreadcrumbControl : NSView

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel;

/** Cancels pane-local work and binds every subsequent action to the supplied controller. */
- (void)rebindToPanelController:(PanelController *)_panel;

@property(nonatomic, readonly) PanelController *panelController;

#ifdef __cplusplus
/** Renders immutable pane state. Must be called on the main queue. */
- (void)applyPaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot;
#endif

/** Switches to editable path mode and focuses the path field. */
- (void)focusAddressField;

/** Loading feedback hosted by the visible Explorer chrome. */
@property(nonatomic, readonly) NSProgressIndicator *busyIndicator;

/** Persistent pane failure/notice surface. Hidden when the current snapshot has no visible error. */
@property(nonatomic, readonly) NSTextField *errorLabel;

/** Request-local address failure, cleared when a newer address navigation starts. */
@property(nonatomic, readonly, nullable) NSString *requestErrorMessage;

@end

NS_ASSUME_NONNULL_END
