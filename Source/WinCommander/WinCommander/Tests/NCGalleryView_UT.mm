// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/States/Explorer/NCGalleryView.h>

#include <memory>
#include <string>
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
                                 GalleryEmptiness::NotEmpty) inDirectory:"/folder"];
    CHECK(view.drawnItemCount == 3);
    CHECK(view.emptyMessage == nil);

    // Re-applying replaces rather than appends: a folder change must not leave the previous folder's
    // photographs on screen beside the new ones.
    [view applyContents:Contents({Row("only.jpg", GalleryEligibility::Thumbnail)}, GalleryEmptiness::NotEmpty) inDirectory:"/folder"];
    CHECK(view.drawnItemCount == 1);
}

TEST_CASE(PREFIX "says something different for an empty folder and one with nothing to look at")
{
    // The two send the user to different places, so a single blank view would be the wrong answer to
    // both.
    NCGalleryView *const view = [[NCGalleryView alloc] initWithFrame:NSMakeRect(0, 0, 400, 400)];

    [view applyContents:Contents({}, GalleryEmptiness::FolderEmpty) inDirectory:"/folder"];
    NSString *const empty_folder = view.emptyMessage;
    REQUIRE(empty_folder != nil);
    CHECK(empty_folder.length > 0);
    CHECK(view.drawnItemCount == 0);

    [view applyContents:Contents({}, GalleryEmptiness::NothingToShow) inDirectory:"/folder"];
    NSString *const no_media = view.emptyMessage;
    REQUIRE(no_media != nil);
    CHECK(no_media.length > 0);
    CHECK_FALSE([no_media isEqualToString:empty_folder]);

    // And it goes away again once there is something to show.
    [view applyContents:Contents({Row("a.jpg", GalleryEligibility::Thumbnail)}, GalleryEmptiness::NotEmpty) inDirectory:"/folder"];
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

TEST_CASE(PREFIX "generates a thumbnail once, and never for a photo that is not local")
{
    // Generating is what would fetch the bytes, and switching to Gallery is not consent to that -
    // so the placeholder row must not even be attempted.
    NCGalleryView *const view = [[NCGalleryView alloc] initWithFrame:NSMakeRect(0, 0, 400, 400)];
    auto asked = std::make_shared<std::vector<std::string>>();
    view.thumbnailGenerator = [asked](const std::string &_path) -> nc::core::GalleryThumbnailCache::Thumbnail {
        asked->push_back(_path);
        return std::make_shared<int>(1);
    };
    // Inline, so the result is there to assert on rather than raced against.
    view.thumbnailScheduler = [](std::function<void()> _work) { _work(); };

    [view applyContents:Contents({Row("a.jpg", GalleryEligibility::Thumbnail),
                                  Row("cloud.jpg", GalleryEligibility::PlaceholderOnly, 1),
                                  Row("pictures", GalleryEligibility::Folder, 2)},
                                 GalleryEmptiness::NotEmpty)
            inDirectory:"/folder"];

    REQUIRE(asked->size() == 1);
    CHECK(asked->front() == "/folder/a.jpg");
    CHECK(view.thumbnailCache.State("/folder/a.jpg") == nc::core::GalleryThumbnailState::Ready);
    CHECK(view.thumbnailCache.State("/folder/cloud.jpg") == nc::core::GalleryThumbnailState::Withheld);

    // Applying the same folder again costs nothing: a redraw must not re-run the whole folder.
    [view applyContents:Contents({Row("a.jpg", GalleryEligibility::Thumbnail)}, GalleryEmptiness::NotEmpty)
            inDirectory:"/folder"];
    CHECK(asked->size() == 1);
}

TEST_CASE(PREFIX "drops the previous folder's thumbnails when the folder changes")
{
    // They apply to nothing here, and keeping them would spend memory on a folder nobody is looking
    // at.
    NCGalleryView *const view = [[NCGalleryView alloc] initWithFrame:NSMakeRect(0, 0, 400, 400)];
    view.thumbnailGenerator = [](const std::string &) -> nc::core::GalleryThumbnailCache::Thumbnail {
        return std::make_shared<int>(1);
    };
    view.thumbnailScheduler = [](std::function<void()> _work) { _work(); };

    [view applyContents:Contents({Row("a.jpg", GalleryEligibility::Thumbnail)}, GalleryEmptiness::NotEmpty)
            inDirectory:"/first"];
    REQUIRE(view.thumbnailCache.State("/first/a.jpg") == nc::core::GalleryThumbnailState::Ready);

    [view applyContents:Contents({Row("b.jpg", GalleryEligibility::Thumbnail)}, GalleryEmptiness::NotEmpty)
            inDirectory:"/second"];
    CHECK(view.thumbnailCache.State("/first/a.jpg") == nc::core::GalleryThumbnailState::Unknown);
    CHECK(view.thumbnailCache.State("/second/b.jpg") == nc::core::GalleryThumbnailState::Ready);
}

#undef PREFIX
