// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

@class PanelController;

/** Explorer address bar: path segments, sibling menus, editable path and Find Files entry point. */
@interface NCExplorerBreadcrumbControl : NSView

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel;

/**
 * Re-reads the panel controller's current directory/listing state and updates the displayed
 * breadcrumb (or the fallback label) accordingly. Must be called by the owner whenever the panel's
 * path changes.
 */
- (void)panelPathChanged;

/** Switches to editable path mode and focuses the path field. */
- (void)focusAddressField;

/** Loading feedback hosted by the visible Explorer chrome. */
@property(nonatomic, readonly) NSProgressIndicator *busyIndicator;

@end
