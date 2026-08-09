// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <WinCommander/Core/Cloud/GalleryContents.h>

#include <Cocoa/Cocoa.h>

/**
 * The Gallery surface: a grid of the media in one folder, plus the folders to walk into.
 *
 * Built in code rather than in a nib, and deliberately: it has no static layout to design - every
 * subview it owns exists because `GalleryContents` said so - and a view assembled here can be
 * constructed and asked questions in a test, which a nib-loaded one in this project cannot without
 * a window.
 */
@interface NCGalleryView : NSView

/** Replaces what is shown. Safe to call repeatedly; the empty state follows the contents. */
- (void)applyContents:(const nc::core::GalleryContents &)_contents;

/** How many rows are currently drawn. */
@property(readonly, nonatomic) NSInteger drawnItemCount;

/** The message shown when there is nothing to draw, or nil while there is. */
@property(readonly, nonatomic, nullable) NSString *emptyMessage;

@end
