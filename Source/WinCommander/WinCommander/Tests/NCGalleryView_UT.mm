// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/States/Explorer/NCGalleryView.h>

#include <vector>

using nc::core::GalleryContents;
using nc::core::GalleryEligibility;
using nc::core::GalleryEmptiness;
using nc::core::GalleryRow;

namespace {

GalleryContents Contents(std::vector<GalleryRow> _rows, const GalleryEmptiness _emptiness)
{
    GalleryContents contents;
    contents.rows = std::move(_rows);
    contents.emptiness = _emptiness;
    return contents;
}

GalleryRow Row(const std::string &_name, const GalleryEligibility _eligibility, const size_t _index = 0)
{
    return GalleryRow{.filename = _name, .eligibility = _eligibility, .listing_index = _index};
}

} // namespace

#define PREFIX "NCGalleryView "

TEST_CASE(PREFIX "draws one tile per row and says so")
{
    NCGalleryView *const view = [[NCGalleryView alloc] initWithFrame:NSMakeRect(0, 0, 400, 400)];
    REQUIRE(view != nil);

    [view applyContents:Contents({Row("pictures", GalleryEligibility::Folder),
                                  Row("a.jpg", GalleryEligibility::Thumbnail, 1),
                                  Row("b.jpg", GalleryEligibility::PlaceholderOnly, 2)},
                                 GalleryEmptiness::NotEmpty)];
    CHECK(view.drawnItemCount == 3);
    CHECK(view.emptyMessage == nil);

    // Re-applying replaces rather than appends: a folder change must not leave the previous folder's
    // photographs on screen beside the new ones.
    [view applyContents:Contents({Row("only.jpg", GalleryEligibility::Thumbnail)}, GalleryEmptiness::NotEmpty)];
    CHECK(view.drawnItemCount == 1);
}

TEST_CASE(PREFIX "says something different for an empty folder and one with nothing to look at")
{
    // The two send the user to different places, so a single blank view would be the wrong answer to
    // both.
    NCGalleryView *const view = [[NCGalleryView alloc] initWithFrame:NSMakeRect(0, 0, 400, 400)];

    [view applyContents:Contents({}, GalleryEmptiness::FolderEmpty)];
    NSString *const empty_folder = view.emptyMessage;
    REQUIRE(empty_folder != nil);
    CHECK(empty_folder.length > 0);
    CHECK(view.drawnItemCount == 0);

    [view applyContents:Contents({}, GalleryEmptiness::NothingToShow)];
    NSString *const no_media = view.emptyMessage;
    REQUIRE(no_media != nil);
    CHECK(no_media.length > 0);
    CHECK_FALSE([no_media isEqualToString:empty_folder]);

    // And it goes away again once there is something to show.
    [view applyContents:Contents({Row("a.jpg", GalleryEligibility::Thumbnail)}, GalleryEmptiness::NotEmpty)];
    CHECK(view.emptyMessage == nil);
}

TEST_CASE(PREFIX "is announced to a screen reader as itself")
{
    // An unlabelled view is announced by its class name, which tells a VoiceOver user nothing about
    // what they have landed in.
    NCGalleryView *const view = [[NCGalleryView alloc] initWithFrame:NSMakeRect(0, 0, 400, 400)];
    CHECK([view.accessibilityIdentifier isEqualToString:@"wincommander.explorer.gallery"]);
    REQUIRE(view.accessibilityLabel != nil);
    CHECK(view.accessibilityLabel.length > 0);
}

#undef PREFIX
