// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "GalleryEligibility.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nc::core {

/** One listing entry, reduced to what Gallery needs to lay it out. */
struct GalleryListingItem {
    std::string_view filename;
    GalleryItemFacts facts;
    /** True for the ".." entry, which Gallery treats specially. */
    bool is_dot_dot = false;

    friend bool operator==(const GalleryListingItem &, const GalleryListingItem &) = default;
};

/** One row Gallery draws. */
struct GalleryRow {
    std::string filename;
    GalleryEligibility eligibility = GalleryEligibility::IconOnly;
    /** Index into the listing this came from, so a selection maps straight back. */
    size_t listing_index = 0;

    friend bool operator==(const GalleryRow &, const GalleryRow &) = default;
};

/** Why a Gallery is showing nothing. The three cases need different words on screen. */
enum class GalleryEmptiness : uint8_t {
    /** It is showing something. */
    NotEmpty,
    /** The folder itself has nothing in it. */
    FolderEmpty,
    /**
     * The folder has contents, but none of them are media. Distinct from an empty folder, because
     * "nothing here" and "nothing to look at here" send the user to different places.
     */
    NothingToShow
};

struct GalleryContents {
    /** Folders first, then media, each in the listing's own order. */
    std::vector<GalleryRow> rows;
    GalleryEmptiness emptiness = GalleryEmptiness::NotEmpty;

    /** How many rows are placeholders whose bytes are not local. */
    [[nodiscard]] size_t PlaceholderCount() const noexcept;

    friend bool operator==(const GalleryContents &, const GalleryContents &) = default;
};

/**
 * Lays out what a Gallery view shows for one listing.
 *
 * Three decisions, and each is about what Gallery is *for*:
 *
 * - **Non-media files are left out entirely**, not shown as icons. Gallery is a way to look at
 *   pictures; a folder of source files rendered as a grid of identical document icons is a worse
 *   view of them than the list the user came from, and it hides the photographs among them.
 * - **Folders stay, and come first.** Dropping them would strand the user in a leaf directory with
 *   no way out but switching view modes back, and a Gallery that cannot be navigated is a dead end.
 * - **A cloud-only media file keeps its place as a placeholder.** Leaving it out would make the
 *   Gallery quietly disagree with the folder about what is in it; downloading it to find out is the
 *   very thing `ClassifyGalleryItem` refuses to do behind the user's back.
 *
 * The relative order within each group is the listing's own, so whatever sort the user chose still
 * decides what comes first.
 */
[[nodiscard]] GalleryContents BuildGalleryContents(std::span<const GalleryListingItem> _listing);

} // namespace nc::core
