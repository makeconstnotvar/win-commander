// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <WinCommander/Core/Cloud/GalleryContents.h>
#include <WinCommander/Core/Cloud/GalleryThumbnails.h>

#include <functional>
#include <string>

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

/**
 * Replaces what is shown. Safe to call repeatedly; the empty state follows the contents.
 *
 * The directory is needed because a row carries only a filename, and a thumbnail is generated from
 * a path. Applying a new folder drops the previous folder's thumbnails: none of them apply, and
 * holding them would spend memory on something nobody is looking at.
 */
- (void)applyContents:(const nc::core::GalleryContents &)_contents inDirectory:(const std::string &)_directory;

/**
 * Produces a thumbnail for a path. Replacing it is how a test drives the view without QuickLook and
 * without touching a disk.
 */
@property(nonatomic) nc::core::GalleryThumbnailCache::Generator thumbnailGenerator;

/**
 * Runs the generation work. Defaults to a background queue; a test replaces it with one that runs
 * inline, so the result is there to assert on rather than raced against.
 */
@property(nonatomic) std::function<void(std::function<void()>)> thumbnailScheduler;

/** What the view has generated so far. */
@property(readonly, nonatomic) const nc::core::GalleryThumbnailCache &thumbnailCache;

/** How many rows are currently drawn. */
@property(readonly, nonatomic) NSInteger drawnItemCount;

/** The message shown when there is nothing to draw, or nil while there is. */
@property(readonly, nonatomic, nullable) NSString *emptyMessage;

@end
