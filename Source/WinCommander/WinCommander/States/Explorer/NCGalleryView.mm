// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCGalleryView.h"

#include <Utility/StringExtras.h>
#include <Utility/ObjCpp.h>

#include <QuickLookThumbnailing/QuickLookThumbnailing.h>

#include <memory>
#include <utility>
#include <vector>

using nc::core::GalleryContents;
using nc::core::GalleryEligibility;
using nc::core::GalleryEmptiness;
using nc::core::GalleryRow;

/** One tile: an image and its filename. */
@interface NCGalleryItem : NSCollectionViewItem
- (void)applyRow:(const GalleryRow &)_row;
/** Replaces the symbol with the real picture, once there is one. */
- (void)applyThumbnail:(NSImage *)_image;
@end

@implementation NCGalleryItem {
    NSImageView *m_Image;
    NSTextField *m_Label;
}

- (void)loadView
{
    NSView *const container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 120, 140)];
    m_Image = [[NSImageView alloc] initWithFrame:NSZeroRect];
    m_Image.translatesAutoresizingMaskIntoConstraints = NO;
    m_Image.imageScaling = NSImageScaleProportionallyUpOrDown;
    m_Label = [NSTextField labelWithString:@""];
    m_Label.translatesAutoresizingMaskIntoConstraints = NO;
    m_Label.alignment = NSTextAlignmentCenter;
    m_Label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [container addSubview:m_Image];
    [container addSubview:m_Label];
    [NSLayoutConstraint activateConstraints:@[
        [m_Image.topAnchor constraintEqualToAnchor:container.topAnchor constant:4],
        [m_Image.leadingAnchor constraintEqualToAnchor:container.leadingAnchor constant:4],
        [m_Image.trailingAnchor constraintEqualToAnchor:container.trailingAnchor constant:-4],
        [m_Label.topAnchor constraintEqualToAnchor:m_Image.bottomAnchor constant:4],
        [m_Label.leadingAnchor constraintEqualToAnchor:container.leadingAnchor constant:4],
        [m_Label.trailingAnchor constraintEqualToAnchor:container.trailingAnchor constant:-4],
        [m_Label.bottomAnchor constraintEqualToAnchor:container.bottomAnchor constant:-4],
    ]];
    self.view = container;
}

- (void)applyRow:(const GalleryRow &)_row
{
    NSString *const filename = [NSString stringWithUTF8StdString:_row.filename];
    m_Label.stringValue = filename;

    // A placeholder is not a failed thumbnail: the bytes are simply elsewhere, and saying so is what
    // stops a user thinking the file is damaged.
    const bool placeholder = _row.eligibility == GalleryEligibility::PlaceholderOnly;
    const bool folder = _row.eligibility == GalleryEligibility::Folder;
    m_Image.image = [NSImage imageWithSystemSymbolName:folder      ? @"folder"
                                                       : placeholder ? @"icloud.and.arrow.down"
                                                                     : @"photo"
                              accessibilityDescription:nil];
    m_Image.alphaValue = placeholder ? 0.5 : 1.0;

    // Announced as one element with a name and a kind. Read as an image plus a separate label, a
    // screen reader would say the filename twice and the state not at all.
    self.view.accessibilityElement = YES;
    self.view.accessibilityLabel = filename;
    self.view.accessibilityRoleDescription =
        folder ? NSLocalizedString(@"folder", "Gallery item kind, spoken by VoiceOver")
               : placeholder
                     ? NSLocalizedString(@"photo not downloaded", "Gallery item kind, spoken by VoiceOver")
                     : NSLocalizedString(@"photo", "Gallery item kind, spoken by VoiceOver");
    m_Image.accessibilityElement = NO;
    m_Label.accessibilityElement = NO;
}

- (void)applyThumbnail:(NSImage *)_image
{
    if( _image == nil )
        return;
    m_Image.image = _image;
    m_Image.alphaValue = 1.0;
}

@end

@interface NCGalleryView () <NSCollectionViewDataSource>
@end

@implementation NCGalleryView {
    NSCollectionView *m_Collection;
    NSScrollView *m_Scroll;
    NSTextField *m_Empty;
    std::vector<GalleryRow> m_Rows;
    std::string m_Directory;
    std::unique_ptr<nc::core::GalleryThumbnailCache> m_Thumbnails;
    nc::core::GalleryThumbnailCache::Generator m_Generator;
    std::function<void(std::function<void()>)> m_Scheduler;
}

@synthesize thumbnailGenerator = m_Generator;
@synthesize thumbnailScheduler = m_Scheduler;

- (instancetype)initWithFrame:(NSRect)_frame
{
    self = [super initWithFrame:_frame];
    if( self ) {
        NSCollectionViewFlowLayout *const layout = [[NSCollectionViewFlowLayout alloc] init];
        layout.itemSize = NSMakeSize(120, 140);
        layout.sectionInset = NSEdgeInsetsMake(8, 8, 8, 8);

        m_Collection = [[NSCollectionView alloc] initWithFrame:NSZeroRect];
        m_Collection.collectionViewLayout = layout;
        m_Collection.dataSource = self;
        m_Collection.selectable = YES;
        m_Collection.allowsMultipleSelection = YES;
        [m_Collection registerClass:NCGalleryItem.class forItemWithIdentifier:@"item"];

        m_Scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        m_Scroll.translatesAutoresizingMaskIntoConstraints = NO;
        m_Scroll.hasVerticalScroller = YES;
        m_Scroll.documentView = m_Collection;

        m_Empty = [NSTextField labelWithString:@""];
        m_Empty.translatesAutoresizingMaskIntoConstraints = NO;
        m_Empty.alignment = NSTextAlignmentCenter;
        m_Empty.textColor = NSColor.secondaryLabelColor;
        m_Empty.hidden = YES;

        [self addSubview:m_Scroll];
        [self addSubview:m_Empty];
        [NSLayoutConstraint activateConstraints:@[
            [m_Scroll.topAnchor constraintEqualToAnchor:self.topAnchor],
            [m_Scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [m_Scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
            [m_Scroll.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [m_Empty.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
            [m_Empty.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        ]];

        m_Generator = [](const std::string &_path) -> nc::core::GalleryThumbnailCache::Thumbnail {
            NSURL *const url = [NSURL fileURLWithPath:[NSString stringWithUTF8StdString:_path]];
            QLThumbnailGenerationRequest *const request =
                [[QLThumbnailGenerationRequest alloc] initWithFileAtURL:url
                                                                   size:NSMakeSize(256, 256)
                                                                  scale:2.0
                                                    representationTypes:QLThumbnailGenerationRequestRepresentationTypeThumbnail];
            // Asked for synchronously because this already runs off the drawing thread, on a queue
            // whose whole purpose is to wait for this. The bounded wait is what stops one unreadable
            // file from holding that queue indefinitely.
            __block NSImage *produced = nil;
            dispatch_semaphore_t finished = dispatch_semaphore_create(0);
            [QLThumbnailGenerator.sharedGenerator
                generateBestRepresentationForRequest:request
                                   completionHandler:^(QLThumbnailRepresentation *_representation, NSError *) {
                                     produced = _representation.NSImage;
                                     dispatch_semaphore_signal(finished);
                                   }];
            if( dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0 )
                return {};
            if( produced == nil )
                return {};
            // The image is kept alive by the shared_ptr the cache holds; the deleter is what hands it
            // back to ARC when the cache evicts it.
            auto *const retained = (__bridge_retained void *)produced;
            return nc::core::GalleryThumbnailCache::Thumbnail{retained, [](void *_image) {
                                                                  if( _image != nullptr )
                                                                      CFRelease(_image);
                                                              }};
        };

        m_Scheduler = [](std::function<void()> _work) {
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), [_work = std::move(_work)] { _work(); });
        };
        m_Thumbnails = std::make_unique<nc::core::GalleryThumbnailCache>(
            [self](const std::string &_path) -> nc::core::GalleryThumbnailCache::Thumbnail {
                return m_Generator ? m_Generator(_path) : nc::core::GalleryThumbnailCache::Thumbnail{};
            });

        self.accessibilityIdentifier = @"wincommander.explorer.gallery";
        self.accessibilityLabel = NSLocalizedString(@"Gallery", "Accessibility label for the Gallery view");
    }
    return self;
}

- (const nc::core::GalleryThumbnailCache &)thumbnailCache
{
    return *m_Thumbnails;
}

- (void)applyContents:(const GalleryContents &)_contents inDirectory:(const std::string &)_directory
{
    // A different folder's thumbnails apply to nothing here, and keeping them would spend memory on
    // a folder nobody is looking at.
    if( _directory != m_Directory ) {
        m_Directory = _directory;
        m_Thumbnails->Clear();
    }
    m_Rows = _contents.rows;
    [m_Collection reloadData];

    // The two empty states get different words because they send the user to different places: one
    // says the folder is empty, the other that there is nothing here worth looking at.
    switch( _contents.emptiness ) {
        case GalleryEmptiness::NotEmpty:
            m_Empty.stringValue = @"";
            break;
        case GalleryEmptiness::FolderEmpty:
            m_Empty.stringValue = NSLocalizedString(@"This folder is empty.", "Gallery empty-folder message");
            break;
        case GalleryEmptiness::NothingToShow:
            m_Empty.stringValue =
                NSLocalizedString(@"Nothing here can be shown in Gallery.", "Gallery no-media message");
            break;
    }
    const bool empty = _contents.emptiness != GalleryEmptiness::NotEmpty;
    m_Empty.hidden = !empty;
    m_Scroll.hidden = empty;
    // Announced rather than merely drawn: an empty view that says nothing to a screen reader is
    // indistinguishable from one that failed to load.
    m_Empty.accessibilityElement = empty;

    [self generatePendingThumbnails];
}

- (void)generatePendingThumbnails
{
    for( const GalleryRow &row : m_Rows ) {
        const std::string path = m_Directory.empty() ? row.filename : m_Directory + "/" + row.filename;
        // Recorded here and not scheduled: a cloud-only row costs nothing to answer, and the answer
        // is the point - `Withheld` is what lets a tile say why it has no picture, instead of
        // looking like a thumbnail that has not arrived yet.
        if( row.eligibility == GalleryEligibility::PlaceholderOnly ) {
            m_Thumbnails->Generate(row, path);
            continue;
        }
        // Asked before scheduling: a row already answered is not asked again, so a redraw costs
        // nothing rather than re-running the whole folder.
        if( !m_Thumbnails->NeedsGeneration(row, path) )
            continue;
        __weak NCGalleryView *weak_self = self;
        m_Scheduler([weak_self, row, path] {
            NCGalleryView *const strong_self = weak_self;
            if( strong_self == nil )
                return;
            [strong_self generateThumbnailForRow:row atPath:path];
        });
    }
}

- (void)generateThumbnailForRow:(const GalleryRow &)_row atPath:(const std::string &)_path
{
    m_Thumbnails->Generate(_row, _path);
    // Redrawn on the main thread once the answer exists. Generation ran wherever the scheduler put
    // it, and touching a view from there is not something to leave to chance.
    __weak NCGalleryView *weak_self = self;
    const std::string path = _path;
    dispatch_async(dispatch_get_main_queue(), [weak_self, path] {
        NCGalleryView *const strong_self = weak_self;
        if( strong_self != nil && strong_self->m_Thumbnails->Known(path) != nullptr )
            [strong_self->m_Collection reloadData];
    });
}

- (NSInteger)drawnItemCount
{
    return static_cast<NSInteger>(m_Rows.size());
}

- (NSString *)emptyMessage
{
    return m_Empty.hidden ? nil : m_Empty.stringValue;
}

- (NSInteger)collectionView:(NSCollectionView *) [[maybe_unused]] _view
     numberOfItemsInSection:(NSInteger) [[maybe_unused]] _section
{
    return static_cast<NSInteger>(m_Rows.size());
}

- (NSCollectionViewItem *)collectionView:(NSCollectionView *)_view
     itemForRepresentedObjectAtIndexPath:(NSIndexPath *)_path
{
    NSCollectionViewItem *const item = [_view makeItemWithIdentifier:@"item" forIndexPath:_path];
    const auto index = static_cast<size_t>(_path.item);
    if( auto *const gallery_item = nc::objc_cast<NCGalleryItem>(item); gallery_item && index < m_Rows.size() ) {
        const GalleryRow &row = m_Rows[index];
        [gallery_item applyRow:row];
        const std::string path = m_Directory.empty() ? row.filename : m_Directory + "/" + row.filename;
        if( const auto thumbnail = m_Thumbnails->Known(path) )
            [gallery_item applyThumbnail:(__bridge NSImage *)thumbnail.get()];
    }
    return item;
}

@end
