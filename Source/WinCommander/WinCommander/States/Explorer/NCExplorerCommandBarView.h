// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

@class PanelController;

#ifdef __cplusplus
#include <memory>
#include <vector>

#include <Operations/OperationCenterModel.h>

namespace nc::core {
class CommandRegistry;
struct PaneSnapshot;
} // namespace nc::core

namespace nc::ops {
class OperationCenterCoordinator;
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
 * hidden-files command, and More also exposes a compact value-only operation control list.
 */
@interface NCExplorerCommandBarView : NSView

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel;

@property(nonatomic, readonly) PanelController *panelController;

/** Retargets every command and context observer to the newly active Explorer pane. */
- (void)rebindToPanelController:(PanelController *)_panel;

#ifdef __cplusplus
/**
 * Injects the app-owned value-model coordinator and Registry for the compact Operations menu.
 * The coordinator is weak; each cancel invocation contains only an immutable ID/revision target.
 */
- (instancetype)initWithFrame:(NSRect)frameRect
               panelController:(PanelController *)_panel
    operationCenterCoordinator:(std::weak_ptr<nc::ops::OperationCenterCoordinator>)_operation_center
               commandRegistry:(nc::core::CommandRegistry *)_command_registry;

/**
 * Presents one static, value-only Operation Center snapshot. The caller transfers copies of
 * `OperationRecord`; this view retains no coordinator, Journal, Pool, executor, or observer.
 * Reopening the command supplies a fresh snapshot.
 */
- (BOOL)presentOperationCenterSnapshot:(std::vector<nc::ops::OperationRecord>)_snapshot;

/** Updates Store-backed command presentation state. Must be called on the main queue. */
- (void)applyPaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot;
#endif

@end
