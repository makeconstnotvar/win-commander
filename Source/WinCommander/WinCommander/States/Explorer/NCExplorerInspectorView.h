// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>
#include <WinCommander/States/Explorer/NCExplorerInspectorModel.h>

namespace nc::utility {
class UTIDB;
}

namespace nc::panel {
class QuickLookVFSBridge;
}

/** Read-only Details / Preview surface driven exclusively by matching PaneSnapshot values. */
@interface NCExplorerInspectorView : NSView

- (instancetype)init NS_UNAVAILABLE;
- (instancetype)initWithFrame:(NSRect)_frame NS_UNAVAILABLE;
- (instancetype)initWithFrame:(NSRect)_frame
                       paneID:(nc::core::PaneId)_pane_id
                        UTIDB:(const nc::utility::UTIDB &)_UTIDB
    QLHazardousExtensionsList:(const std::string &)_ql_hazard_list
                  QLVFSBridge:(nc::panel::QuickLookVFSBridge &)_ql_vfs_bridge;

/** Returns NO for a foreign or stale snapshot and preserves the last accepted presentation. */
- (BOOL)applyPaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot;

/** Clears pane-local presentation state and accepts only snapshots for the new identity. */
- (BOOL)rebindToPaneID:(nc::core::PaneId)_pane_id;

/** Cancels and clears the embedded preview while retaining the last accepted metadata model. */
- (void)clearPreview;

@end
