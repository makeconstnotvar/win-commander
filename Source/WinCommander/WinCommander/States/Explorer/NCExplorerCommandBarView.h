// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

@class PanelController;

#ifdef __cplusplus
namespace nc::core {
struct PaneSnapshot;
}
#endif

/**
 * A horizontal row of file-operation buttons for placement below the Explorer toolbar:
 * New, Cut, Copy, Paste, Rename, Share, Delete, Sort, View, More.
 *
 * New/Cut/Copy/Paste/Rename/Delete are wired directly to the panel's own
 * NCPanelControllerActionsDispatcher, the same idiom NCExplorerToolbarDelegate uses for its
 * Back/Forward/Up/Refresh buttons. Share is self-contained, built on NSSharingServicePicker over
 * the panel's currently selected items. Sort/View/More each show a CUI NCCommandPopover: Sort
 * lists the existing sort-mode toggle actions (see ToggleSort.h), View includes the Store-backed
 * hidden-files command, and More remains a minimal overflow placeholder.
 */
@interface NCExplorerCommandBarView : NSView

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel;

#ifdef __cplusplus
/** Updates Store-backed command presentation state. Must be called on the main queue. */
- (void)applyPaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot;
#endif

@end
