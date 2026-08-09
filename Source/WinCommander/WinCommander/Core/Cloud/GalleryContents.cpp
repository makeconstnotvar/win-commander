// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "GalleryContents.h"

#include <algorithm>

namespace nc::core {

size_t GalleryContents::PlaceholderCount() const noexcept
{
    return static_cast<size_t>(std::ranges::count_if(rows, [](const GalleryRow &_row) {
        return _row.eligibility == GalleryEligibility::PlaceholderOnly;
    }));
}

GalleryContents BuildGalleryContents(const std::span<const GalleryListingItem> _listing)
{
    GalleryContents contents;
    std::vector<GalleryRow> folders;
    std::vector<GalleryRow> media;

    for( size_t index = 0; index < _listing.size(); ++index ) {
        const GalleryListingItem &item = _listing[index];
        // ".." is navigation, not content. It belongs to whatever chrome the view has, and counting
        // it as a row would make an empty folder look like it holds something.
        if( item.is_dot_dot )
            continue;

        const GalleryEligibility eligibility = ClassifyGalleryItem(item.facts);
        GalleryRow row{
            .filename = std::string{item.filename}, .eligibility = eligibility, .listing_index = index};

        switch( eligibility ) {
            case GalleryEligibility::Folder:
                // Kept, and first. Dropping folders would strand the user in a leaf directory with
                // no way out but switching view modes back.
                folders.push_back(std::move(row));
                break;
            case GalleryEligibility::Thumbnail:
            case GalleryEligibility::PlaceholderOnly:
                // A cloud-only media file keeps its place: leaving it out would make the Gallery
                // quietly disagree with the folder about what is in it.
                media.push_back(std::move(row));
                break;
            case GalleryEligibility::IconOnly:
                // Left out entirely. A grid of identical document icons is a worse view of source
                // files than the list the user came from, and it hides the photographs among them.
                break;
        }
    }

    contents.rows = std::move(folders);
    contents.rows.insert(contents.rows.end(),
                         std::make_move_iterator(media.begin()),
                         std::make_move_iterator(media.end()));

    if( !contents.rows.empty() )
        contents.emptiness = GalleryEmptiness::NotEmpty;
    else {
        // "Nothing here" and "nothing to look at here" send the user to different places, so the
        // two are told apart rather than collapsed into one blank view.
        const bool has_contents = std::ranges::any_of(
            _listing, [](const GalleryListingItem &_item) { return !_item.is_dot_dot; });
        contents.emptiness = has_contents ? GalleryEmptiness::NothingToShow : GalleryEmptiness::FolderEmpty;
    }
    return contents;
}

} // namespace nc::core
