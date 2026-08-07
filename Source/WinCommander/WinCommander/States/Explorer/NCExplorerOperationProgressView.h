// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

#ifdef __cplusplus
#include <WinCommander/Core/VisualState/ExplorerOperationProgressModel.h>
#endif

/**
 * Compact, value-only presentation of the primary operation in one Explorer window.
 * The view never receives Pool or Operation authority.
 */
@interface NCExplorerOperationProgressView : NSView

- (instancetype)initWithFrame:(NSRect)_frame;

#ifdef __cplusplus
- (void)applySnapshot:(const std::optional<nc::core::ExplorerOperationProgressSnapshot> &)_snapshot;
#endif

@end
