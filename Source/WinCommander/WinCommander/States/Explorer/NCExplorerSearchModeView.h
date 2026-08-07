// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

#ifdef __cplusplus
#include <WinCommander/Core/Search/SearchState.h>
#include <functional>
#include <optional>
#endif

/**
 * Bounded, value-only search-mode surface for one Explorer pane.
 * The view constructs requests from its controls and never owns backend, store or pane authority.
 */
@interface NCExplorerSearchModeView : NSView <NSSearchFieldDelegate>

- (instancetype)initWithFrame:(NSRect)_frame;

/** Moves keyboard focus to the query field after Search Mode is mounted and visible. */
- (BOOL)focusQueryField;

#ifdef __cplusplus
- (void)applySnapshot:(const std::optional<nc::core::SearchSnapshot> &)_snapshot
    resultSelectionEligible:(bool)_result_selection_eligible;
- (void)setStartHandler:(std::function<void(nc::core::SearchRequest)>)_handler;
- (void)setCancelHandler:(std::function<void()>)_handler;
- (void)setRevealOriginalHandler:(std::function<void()>)_handler;
- (void)setCloseHandler:(std::function<void()>)_handler;
#endif

@end
