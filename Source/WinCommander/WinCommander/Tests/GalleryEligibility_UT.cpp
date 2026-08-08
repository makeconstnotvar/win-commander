// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Cloud/GalleryEligibility.h>

namespace {

using nc::core::ClassifyGalleryItem;
using nc::core::CloudSyncState;
using nc::core::GalleryEligibility;
using nc::core::GalleryItemFacts;
using nc::core::IsGalleryMediaExtension;

} // namespace

#define PREFIX "nc::core::GalleryEligibility "

TEST_CASE(PREFIX "recognises media extensions regardless of case")
{
    for( const auto extension : {"jpg", "JPG", "Jpg", "heic", "MOV", "pdf", "dng"} )
        CHECK(IsGalleryMediaExtension(extension));
}

TEST_CASE(PREFIX "degrades an unknown extension to an icon rather than guessing")
{
    // A wrong guess produces an empty frame, which reads as a broken file; an icon just looks
    // correct. So the list is conservative on purpose.
    for( const auto extension : {"", "txt", "cpp", "zip", "docx", "jpgx", "verylongextension"} )
        CHECK_FALSE(IsGalleryMediaExtension(extension));
}

TEST_CASE(PREFIX "never thumbnails a folder, whatever it is named")
{
    // A directory called "holiday.jpg" is still a directory.
    const GalleryItemFacts facts{.extension = "jpg", .is_directory = true};
    CHECK(ClassifyGalleryItem(facts) == GalleryEligibility::Folder);
}

TEST_CASE(PREFIX "refuses to download a cloud-only file just because the view changed")
{
    // Asking for a thumbnail would make the file manager fetch the bytes - possibly gigabytes, on
    // a metered link - because the user switched view mode. Switching a view is not consent.
    const GalleryItemFacts placeholder{
        .extension = "mov", .is_directory = false, .cloud_state = CloudSyncState::CloudOnly};
    CHECK(ClassifyGalleryItem(placeholder) == GalleryEligibility::PlaceholderOnly);

    // Once the bytes are local - or on their way - a thumbnail costs nothing extra.
    for( const auto state : {CloudSyncState::NotCloud,
                             CloudSyncState::Synced,
                             CloudSyncState::Downloading,
                             CloudSyncState::Uploading,
                             CloudSyncState::Conflicted,
                             CloudSyncState::Excluded} ) {
        const GalleryItemFacts facts{.extension = "mov", .is_directory = false, .cloud_state = state};
        INFO("cloud state ordinal: " << static_cast<int>(state));
        CHECK(ClassifyGalleryItem(facts) == GalleryEligibility::Thumbnail);
    }
}

TEST_CASE(PREFIX "a cloud-only non-media file is still just an icon")
{
    // The placeholder distinction only matters for something Gallery would otherwise render.
    const GalleryItemFacts facts{
        .extension = "txt", .is_directory = false, .cloud_state = CloudSyncState::CloudOnly};
    CHECK(ClassifyGalleryItem(facts) == GalleryEligibility::IconOnly);
}

TEST_CASE(PREFIX "thumbnails an ordinary local media file")
{
    const GalleryItemFacts facts{.extension = "png", .is_directory = false};
    CHECK(ClassifyGalleryItem(facts) == GalleryEligibility::Thumbnail);
}
