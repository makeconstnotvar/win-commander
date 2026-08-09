// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Cloud/GalleryContents.h>

#include <vector>

namespace {

using nc::core::BuildGalleryContents;
using nc::core::CloudSyncState;
using nc::core::GalleryEligibility;
using nc::core::GalleryEmptiness;
using nc::core::GalleryItemFacts;
using nc::core::GalleryListingItem;

GalleryListingItem Media(const std::string_view _name,
                         const std::string_view _extension,
                         const CloudSyncState _cloud = CloudSyncState::NotCloud)
{
    return GalleryListingItem{
        .filename = _name,
        .facts = GalleryItemFacts{.extension = _extension, .is_directory = false, .cloud_state = _cloud},
        .is_dot_dot = false};
}

GalleryListingItem Other(const std::string_view _name, const std::string_view _extension)
{
    return Media(_name, _extension);
}

GalleryListingItem Folder(const std::string_view _name)
{
    return GalleryListingItem{
        .filename = _name,
        .facts = GalleryItemFacts{.extension = "", .is_directory = true, .cloud_state = CloudSyncState::NotCloud},
        .is_dot_dot = false};
}

GalleryListingItem DotDot()
{
    GalleryListingItem item = Folder("..");
    item.is_dot_dot = true;
    return item;
}

} // namespace

#define PREFIX "nc::core::BuildGalleryContents "

TEST_CASE(PREFIX "shows folders first, then media, each in the listing's own order")
{
    // Whatever sort the user chose still decides what comes first within each group.
    const std::vector<GalleryListingItem> listing{
        Media("b.jpg", "jpg"), Folder("zzz"), Media("a.png", "png"), Folder("aaa")};

    const auto contents = BuildGalleryContents(listing);
    REQUIRE(contents.rows.size() == 4);
    CHECK(contents.rows[0].filename == "zzz");
    CHECK(contents.rows[1].filename == "aaa");
    CHECK(contents.rows[2].filename == "b.jpg");
    CHECK(contents.rows[3].filename == "a.png");
    CHECK(contents.emptiness == GalleryEmptiness::NotEmpty);

    // The index maps straight back to the listing, so a selection survives the regrouping.
    CHECK(contents.rows[0].listing_index == 1);
    CHECK(contents.rows[2].listing_index == 0);
}

TEST_CASE(PREFIX "leaves non-media out entirely rather than showing it as an icon")
{
    // Gallery is a way to look at pictures. A grid of identical document icons is a worse view of
    // source files than the list the user came from, and it hides the photographs among them.
    const std::vector<GalleryListingItem> listing{
        Other("main.cpp", "cpp"), Media("photo.jpg", "jpg"), Other("notes.txt", "txt")};

    const auto contents = BuildGalleryContents(listing);
    REQUIRE(contents.rows.size() == 1);
    CHECK(contents.rows[0].filename == "photo.jpg");
    CHECK(contents.rows[0].eligibility == GalleryEligibility::Thumbnail);
}

TEST_CASE(PREFIX "keeps folders so the view is not a dead end")
{
    // Dropping them would strand the user in a leaf directory with no way out but switching view
    // modes back.
    const std::vector<GalleryListingItem> listing{Other("main.cpp", "cpp"), Folder("pictures")};

    const auto contents = BuildGalleryContents(listing);
    REQUIRE(contents.rows.size() == 1);
    CHECK(contents.rows[0].filename == "pictures");
    CHECK(contents.rows[0].eligibility == GalleryEligibility::Folder);
}

TEST_CASE(PREFIX "keeps a cloud-only photo in its place, as a placeholder")
{
    // Leaving it out would make the Gallery quietly disagree with the folder about what is in it -
    // and downloading it to find out is exactly what must not happen because a view mode changed.
    const std::vector<GalleryListingItem> listing{
        Media("local.jpg", "jpg"), Media("remote.jpg", "jpg", CloudSyncState::CloudOnly)};

    const auto contents = BuildGalleryContents(listing);
    REQUIRE(contents.rows.size() == 2);
    CHECK(contents.rows[0].eligibility == GalleryEligibility::Thumbnail);
    CHECK(contents.rows[1].filename == "remote.jpg");
    CHECK(contents.rows[1].eligibility == GalleryEligibility::PlaceholderOnly);
    CHECK(contents.PlaceholderCount() == 1);
}

TEST_CASE(PREFIX "tells an empty folder apart from one with nothing to look at")
{
    // "Nothing here" and "nothing to look at here" send the user to different places, so the two
    // are not collapsed into one blank view.
    const auto empty = BuildGalleryContents(std::vector<GalleryListingItem>{});
    CHECK(empty.rows.empty());
    CHECK(empty.emptiness == GalleryEmptiness::FolderEmpty);

    const auto only_dot_dot = BuildGalleryContents(std::vector{DotDot()});
    CHECK(only_dot_dot.emptiness == GalleryEmptiness::FolderEmpty);

    const auto no_media = BuildGalleryContents(std::vector{DotDot(), Other("a.cpp", "cpp"), Other("b.txt", "txt")});
    CHECK(no_media.rows.empty());
    CHECK(no_media.emptiness == GalleryEmptiness::NothingToShow);
}

TEST_CASE(PREFIX "does not count the parent entry as content")
{
    // It is navigation, not content, and counting it would make an empty folder look like it holds
    // something.
    const std::vector<GalleryListingItem> listing{DotDot(), Media("a.jpg", "jpg")};
    const auto contents = BuildGalleryContents(listing);
    REQUIRE(contents.rows.size() == 1);
    CHECK(contents.rows[0].filename == "a.jpg");
    CHECK(contents.rows[0].listing_index == 1);
}
#undef PREFIX
