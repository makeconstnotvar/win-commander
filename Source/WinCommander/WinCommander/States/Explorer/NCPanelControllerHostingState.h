// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>
#include "../FilePanels/PanelPreview.h"
#include <string>

@class PanelController;
@class BriefSystemOverview;
@class FilePanelMainSplitView;

/**
 * The minimal surface that PanelController requires from whatever NCMainWindowState hosts it.
 * Extracted verbatim from MainWindowFilePanelState (every selector any PanelController/Actions
 * code sends to `.state`, confirmed by a repo-wide audit - see anyPanelCollapsed/splitView/
 * isRightController: which a first pass missed) so that a single-panel state (e.g.
 * NCExplorerState) can host a PanelController without depending on the dual-pane state's
 * concrete type.
 */
@protocol NCPanelControllerHostingState <NSObject>

@property(nonatomic, readonly) NSWindow *window;
@property(nonatomic, readonly) FilePanelMainSplitView *splitView;
@property(nonatomic, readonly) bool anyPanelCollapsed;
@property(nonatomic, readonly) bool bothPanelsAreVisible;
@property(nonatomic, readonly) PanelController *leftPanelController;
@property(nonatomic, readonly) PanelController *rightPanelController;

- (void)closeAttachedUI:(PanelController *)_panel;
- (void)PanelPathChanged:(PanelController *)_panel;
- (void)activePanelChangedTo:(PanelController *)controller;
- (void)ActivatePanelByController:(PanelController *)controller;
- (BriefSystemOverview *)briefSystemOverviewForPanel:(PanelController *)_panel make:(bool)_make_if_absent;
- (id<NCPanelPreview>)quickLookForPanel:(PanelController *)_panel make:(bool)_make_if_absent;
- (void)requestTerminalExecution:(const std::string &)_filename at:(const std::string &)_cwd;
- (bool)isLeftController:(PanelController *)_controller;
- (bool)isRightController:(PanelController *)_controller;

@end
