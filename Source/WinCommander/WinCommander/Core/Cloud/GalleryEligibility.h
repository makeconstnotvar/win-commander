// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CloudSyncState.h"

#include <cstdint>
#include <string_view>

namespace nc::core {

/** What a Gallery row can do with an item. */
enum class GalleryEligibility : uint8_t {
    /** Renders a real thumbnail from the item's own content. */
    Thumbnail,
    /** A media file whose bytes are not here yet - a placeholder row, not a thumbnail. */
    PlaceholderOnly,
    /** Not media; Gallery shows it as a plain icon rather than pretending. */
    IconOnly,
    /** A folder: navigable, never a thumbnail of itself. */
    Folder
};

/** Whether the extension names a format Gallery can render a thumbnail from. */
[[nodiscard]] bool IsGalleryMediaExtension(std::string_view _extension) noexcept;

/** Facts a listing already knows, plus the cloud state from CL-1. */
struct GalleryItemFacts {
    /** Filename extension without the dot. Case is ignored. */
    std::string_view extension;
    bool is_directory = false;
    CloudSyncState cloud_state = CloudSyncState::NotCloud;

    friend bool operator==(const GalleryItemFacts &, const GalleryItemFacts &) = default;
};

/**
 * Decides what a Gallery row may show.
 *
 * The one judgement here: a cloud-only media file is **not** thumbnailable. Asking for a thumbnail
 * would make the file manager silently download it - potentially gigabytes, on a metered link,
 * merely because the user switched view mode. That is a data-transfer decision, and switching a
 * view is not consent to it. Such items get a placeholder row instead, and downloading stays an
 * explicit action.
 */
[[nodiscard]] GalleryEligibility ClassifyGalleryItem(const GalleryItemFacts &_facts) noexcept;

} // namespace nc::core
