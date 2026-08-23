// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

/**
 * Appearance-resolving colour tokens for the Explorer presentation.
 *
 * Each token is one process-wide NSColor built with +colorWithName:dynamicProvider:, so its value is
 * resolved by AppKit against the appearance current at draw time and the accessor allocates nothing
 * after the first call. NSApp.appearance is pinned to the selected theme's appearance
 * (Bootstrap/AppDelegate.mm:975), so these follow both an explicit Light/Dark theme choice and the
 * automatic system switch with no observation on the consumer side.
 *
 * Dark values are not chosen by eye. Each mirrors its light value's own delta from its reference
 * plane: light chrome #ECEEF1 sits 17/255 below the #FFFFFF content plane, so dark chrome sits
 * 17/255 above the #1E1E1E content plane. That preserves the elevation ranking identically in both
 * appearances while reversing its direction. Do not hand-tune one value without re-deriving it.
 *
 * Scope. Only surfaces the Explorer alone draws. Anything the Commander also draws keeps its
 * nc::Theme value. Anything the platform already answers honestly keeps its semantic NSColor and
 * deliberately has no token here: row and workspace grounds are controlBackgroundColor, row text is
 * labelColor/secondaryLabelColor, and row selection and the accent command derive from
 * controlAccentColor. The flat `.pen` sidebar deliberately shares ChromeFillColor and applies its
 * own rounded accent wash. Command bar chips and status bar view controls use stock AppKit bezels
 * and need no token.
 *
 * Consumption. Pass a token straight to -set / -setFill, NSBox.fillColor, NSTextField.textColor,
 * NSTableView.backgroundColor or NSImageView.contentTintColor. Do NOT cache -CGColor: a dynamic
 * colour resolves at the instant CGColor is read, so a CALayer.backgroundColor assigned once freezes
 * in whichever appearance was current then. If a layer must be painted, re-resolve inside
 * -updateLayer under [self.effectiveAppearance performAsCurrentDrawingAppearance:], the idiom
 * already in the tree at PanelListViewRowView.mm:209.
 */
namespace nc::explorer {

/** Chrome ground: the status bar, and any hand-drawn chrome beside the unified toolbar. */
NSColor *ChromeFillColor();

/** Structural hairline: command bar bottom edge, status bar top edge. */
NSColor *ChromeDividerColor();

/** Command bar ground. */
NSColor *CommandBarFillColor();

/** Ground behind the split views, seen once the layout grows a real workspace region. */
NSColor *WorkspaceFillColor();

/** Details table header ground. */
NSColor *TableHeaderFillColor();

/** Hairline under the details table header. */
NSColor *TableHeaderDividerColor();

/** Hairline under each details row. */
NSColor *RowDividerColor();

} // namespace nc::explorer
