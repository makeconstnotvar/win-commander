// Copyright (C) 2013-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "MainWindowStateProtocol.h"
#include <VFS/VFS.h>
#include <span>

@class OperationsController;
@class MainWindowFilePanelState;
@class NCTermShellState;
@class NCMainWindow;
@class NCExplorerState;
@class PanelController;

namespace nc::ops {
class Pool;
class Operation;
} // namespace nc::ops

@interface NCMainWindowController : NSWindowController <NSWindowDelegate, NSWindowRestoration, NSMenuItemValidation>

- (instancetype)initWithWindow:(NCMainWindow *)_window;

// Window state manipulations
- (void)ResignAsWindowState:(id)_state;

- (void)requestViewerFor:(std::string)_filepath at:(std::shared_ptr<VFSHost>)_host;

- (void)requestTerminal:(const std::string &)_cwd;
- (void)requestTerminalExecution:(const char *)_filename at:(const char *)_cwd;
- (void)requestTerminalExecution:(const char *)_filename at:(const char *)_cwd withParameters:(const char *)_params;
- (void)requestTerminalExecutionWithFullPath:(const std::filesystem::path &)_binary_path
                                andArguments:(std::span<const std::string>)_params;
- (void)RequestExternalEditorTerminalExecution:(const std::string &)_full_app_path
                                        params:(const std::string &)_params
                                     fileTitle:(const std::string &)_file_title;

- (bool)restoreDefaultWindowStateFromConfig;
- (void)restoreDefaultWindowStateFromLastOpenedWindow;
+ (bool)canRestoreDefaultWindowStateFromLastOpenedWindow;

// Installs the single-pane Explorer state over the permanent Commander base. The operation is
// idempotent and is accepted only while the base state is visible.
- (BOOL)ensureExplorerMode;

// Access to states
@property(nonatomic, readwrite) MainWindowFilePanelState *filePanelsState; // one and only one per window
@property(nonatomic, readonly) NCTermShellState *terminalState;            // zero or one per window
@property(nonatomic, readonly) id<NCMainWindowState> topmostState;
// The pane presented by the currently visible file-management state. Terminal/viewer states do
// not expose a hidden backing pane through this boundary.
@property(nonatomic, readonly) PanelController *visibleActivePanelController;
@property(nonatomic, readonly) nc::ops::Pool &operationsPool;

- (void)setOperationsPool:(nc::ops::Pool &)_pool;

// Toolbar support
- (void)OnShowToolbar:(id)sender;

// Toggles between the dual-pane Commander mode (the permanent base state) and the single-pane
// Explorer mode, pushing/popping NCExplorerState on top of the base state.
- (IBAction)toggleExplorerMode:(id)sender;

+ (NCMainWindowController *)lastFocused;

- (void)enqueueOperation:(const std::shared_ptr<nc::ops::Operation> &)_operation;
- (void)beginSheet:(NSWindow *)sheetWindow completionHandler:(void (^)(NSModalResponse rc))handler;

@end
