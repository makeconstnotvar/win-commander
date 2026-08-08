// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>
#include <WinCommander/Core/Commands/CommandPaletteFilter.h>

#include <span>
#include <string>
#include <vector>

@class NCExplorerCommandPaletteView;

@protocol NCExplorerCommandPaletteDelegate <NSObject>
/** The user committed a row. The id is a registry command id; the palette has already closed. */
- (void)commandPalette:(NCExplorerCommandPaletteView *)_palette didChooseCommandId:(const std::string &)_command_id;
/** The user dismissed without choosing, or the palette closed itself. */
- (void)commandPaletteDidDismiss:(NCExplorerCommandPaletteView *)_palette;
@end

/**
 * Keyboard-first command palette overlay: a query field above a ranked, single-selection list.
 *
 * The view owns no command semantics. It renders the roster it is given, ranks it through
 * nc::core::FilterCommandPalette on every keystroke, and reports the chosen id back - so what the
 * palette can run is decided entirely by the roster its owner builds.
 */
@interface NCExplorerCommandPaletteView : NSVisualEffectView

@property(nonatomic, weak) id<NCExplorerCommandPaletteDelegate> paletteDelegate;

/** Intrinsic size of the overlay: query field plus the rows it shows before scrolling. */
+ (NSSize)preferredSize;

/** Replaces the roster and resets the query. Safe to call while presented. */
- (void)setRoster:(std::vector<nc::core::CommandPaletteEntry>)_roster;

/** Moves keyboard focus into the query field; call after the view is in a window. */
- (void)focusQueryField;

/** Ranked rows currently shown, for testing. */
- (std::vector<std::string>)visibleCommandIdsForTesting;
/** Row the user would commit with Return right now, or an empty string when none. */
- (std::string)selectedCommandIdForTesting;
- (void)setQueryForTesting:(NSString *)_query;
- (BOOL)moveSelectionByForTesting:(NSInteger)_delta;
- (BOOL)commitSelectionForTesting;

@end
