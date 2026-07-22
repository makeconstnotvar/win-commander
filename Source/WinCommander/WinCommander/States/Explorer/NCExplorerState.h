// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>
#include "../MainWindowStateProtocol.h"
#include "NCPanelControllerHostingState.h"
#include <Panel/PanelViewKeystrokeSink.h>

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
@interface NCExplorerState : NSView <NCMainWindowState, NCPanelControllerHostingState, NCPanelViewKeystrokeSink>

- (instancetype)initWithFrame:(NSRect)frameRect operationsPool:(nc::ops::Pool &)_pool;

@property(nonatomic, readonly) PanelController *panelController;

@end
