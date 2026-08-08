// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>
#include "../MainWindowStateProtocol.h"
#include "NCExplorerInspectorPresenting.h"
#include "NCExplorerSearchPresenting.h"
#include "ExplorerSessionPersistency.h"
#include "NCPanelControllerHostingState.h"
#include <Panel/PanelViewKeystrokeSink.h>
#include <Panel/UI/PanelTabBarView.h>

namespace nc::ops {
class Pool;
}

@class PanelController;
@class PanelView;

/**
 * A single-pane, Windows-Explorer-like alternative to the dual-pane MainWindowFilePanelState.
 * It is pushed on top of the existing dual-pane state via NCMainWindowController and popped back
 * to it - it never replaces the dual-pane state, which remains the app's permanent base window
 * state (see NCMainWindowController.setFilePanelsState:).
 */
@interface NCExplorerState : NSView <NCMainWindowState,
                                     NCPanelControllerHostingState,
                                     NCPanelViewKeystrokeSink,
                                     NCExplorerInspectorPresenting,
                                     NCExplorerSearchPresenting,
                                     NCPanelTabBarViewDelegate,
                                     NSMenuItemValidation>

- (instancetype)initWithFrame:(NSRect)frameRect operationsPool:(nc::ops::Pool &)_pool;

@property(nonatomic, readonly) PanelController *panelController;

/**
 * Replaces the initial Home intent with a decoded Explorer pane layout: the left side's ordered
 * tabs and, when the session recorded dual-pane mode, the right side's own tabs plus the focused
 * side and divider ratio. Every restored tab receives a newly allocated process-local PaneId;
 * persisted locations never carry pane identity, history or view settings. May be accepted only
 * once, before runtime tab mutations begin. A right side that cannot be rebuilt leaves the already
 * restored left side as a single-pane window rather than failing the whole session.
 */
- (BOOL)restorePanesFromSession:(const nc::explorer::ExplorerPanesSession &)_session;

/**
 * Captures each live side's ordered exact tab locations and active index, plus the dual-pane
 * layout, focused side and divider ratio, for window-session persistence.
 */
- (nc::explorer::ExplorerPanesSession)capturePanesSession;

// Called by NCMainWindow before Commander-mode configurable menu shortcuts are resolved.
- (BOOL)handleModeSpecificKeyEquivalent:(NSEvent *)_event;

@end
