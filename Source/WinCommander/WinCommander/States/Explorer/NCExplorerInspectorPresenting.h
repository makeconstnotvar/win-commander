// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>
#include <WinCommander/Core/Commands/FileGetInfoCommand.h>

@class PanelController;

/** Borrowed synchronous port implemented by states that own a Details / Preview inspector. */
@protocol NCExplorerInspectorPresenting <NSObject>

- (BOOL)presentFileGetInfo:(const nc::core::FileGetInfoPresentation &)_presentation
                  forPanel:(PanelController *)_panel;
- (BOOL)previewPaneVisibleForPanel:(PanelController *)_panel;
- (BOOL)setPreviewPaneVisible:(BOOL)_desired
                     expected:(BOOL)_expected
                     forPanel:(PanelController *)_panel;

@end
