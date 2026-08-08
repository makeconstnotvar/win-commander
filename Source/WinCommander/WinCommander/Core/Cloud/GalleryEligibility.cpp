// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "GalleryEligibility.h"

#include <algorithm>
#include <array>
#include <string>

namespace nc::core {

namespace {

/**
 * Formats macOS can produce a thumbnail for without a third-party decoder. Deliberately a
 * conservative list: an unrecognized extension degrades to an icon, which is correct-looking, while
 * a wrong guess produces an empty frame that reads as a broken file.
 */
constexpr std::array g_MediaExtensions{
    // Images
    std::string_view{"jpg"}, std::string_view{"jpeg"}, std::string_view{"png"}, std::string_view{"gif"},
    std::string_view{"heic"}, std::string_view{"heif"}, std::string_view{"tiff"}, std::string_view{"tif"},
    std::string_view{"bmp"}, std::string_view{"webp"}, std::string_view{"avif"}, std::string_view{"psd"},
    std::string_view{"raw"}, std::string_view{"cr2"}, std::string_view{"nef"}, std::string_view{"arw"},
    std::string_view{"dng"}, std::string_view{"svg"}, std::string_view{"icns"},
    // Video
    std::string_view{"mov"}, std::string_view{"mp4"}, std::string_view{"m4v"}, std::string_view{"avi"},
    std::string_view{"mkv"}, std::string_view{"webm"},
    // Documents that render a first page
    std::string_view{"pdf"},
};

} // namespace

bool IsGalleryMediaExtension(const std::string_view _extension) noexcept
{
    if( _extension.empty() || _extension.size() > 8 )
        return false;
    // ASCII-lowered locally: filename extensions in this table are ASCII, and a locale-sensitive
    // fold could map an unrelated character onto one of them.
    std::array<char, 8> folded{};
    for( size_t index = 0; index < _extension.size(); ++index ) {
        const char character = _extension[index];
        folded[index] = character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    }
    const std::string_view candidate{folded.data(), _extension.size()};
    return std::ranges::find(g_MediaExtensions, candidate) != g_MediaExtensions.end();
}

GalleryEligibility ClassifyGalleryItem(const GalleryItemFacts &_facts) noexcept
{
    // A folder is navigable and never a thumbnail of itself, whatever it happens to be named.
    if( _facts.is_directory )
        return GalleryEligibility::Folder;

    if( !IsGalleryMediaExtension(_facts.extension) )
        return GalleryEligibility::IconOnly;

    // The judgement this type exists for: thumbnailing a cloud-only file would make the file
    // manager download it - possibly gigabytes, possibly metered - because the user switched view
    // mode. Switching a view is not consent to a transfer, so it stays a placeholder.
    if( _facts.cloud_state == CloudSyncState::CloudOnly )
        return GalleryEligibility::PlaceholderOnly;

    return GalleryEligibility::Thumbnail;
}

} // namespace nc::core
