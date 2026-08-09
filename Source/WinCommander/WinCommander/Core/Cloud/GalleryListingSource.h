// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "GalleryContents.h"
#include "NativeCloudItemFacts.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace nc::core {

/** One entry as a listing reports it, before anything is decided about it. */
struct NativeListingEntry {
    std::string filename;
    bool is_directory = false;

    friend bool operator==(const NativeListingEntry &, const NativeListingEntry &) = default;
};

/**
 * Gallery items, and the strings they point into.
 *
 * The items hold views, so the strings have to outlive them and cannot be duplicated: copying this
 * would leave every view pointing at the original's storage. Moving is fine - a moved vector keeps
 * its buffer, so the views stay valid - which is why the copy is deleted and the move is not.
 */
class GalleryListingSource final
{
public:
    GalleryListingSource() = default;
    GalleryListingSource(const GalleryListingSource &) = delete;
    GalleryListingSource &operator=(const GalleryListingSource &) = delete;
    GalleryListingSource(GalleryListingSource &&) noexcept = default;
    GalleryListingSource &operator=(GalleryListingSource &&) noexcept = default;

    [[nodiscard]] std::span<const GalleryListingItem> Items() const noexcept { return m_Items; }
    /** The user-visible name of an item, which is not always the name on disk. */
    [[nodiscard]] const std::string &DisplayName(size_t _index) const { return m_Names.at(_index); }
    [[nodiscard]] size_t Size() const noexcept { return m_Items.size(); }

private:
    friend GalleryListingSource BuildGalleryListing(const std::string &,
                                                    std::span<const NativeListingEntry>,
                                                    const std::function<NativeCloudProbe(const std::string &)> &);

    std::vector<std::string> m_Names;
    std::vector<std::string> m_Extensions;
    std::vector<GalleryListingItem> m_Items;
};

/**
 * Turns a native listing into what Gallery reasons about.
 *
 * Two decisions carry the weight:
 *
 * - **A placeholder's name is unmasked before anything reads its extension.** A not-yet-downloaded
 *   photograph is on disk as `.holiday.jpg.icloud`; taken at face value, its extension is `icloud`
 *   and Gallery would decide it is not media at all - so the one row the user most wants to see
 *   would silently vanish from the view.
 * - **Directories are not probed.** A folder is a folder to Gallery whatever its sync state, so
 *   asking would spend a filesystem call per row to learn something nothing reads.
 *
 * The prober is injected so this is testable without a cloud container, which no unit test has.
 */
[[nodiscard]] GalleryListingSource
BuildGalleryListing(const std::string &_directory,
                    std::span<const NativeListingEntry> _entries,
                    const std::function<NativeCloudProbe(const std::string &)> &_prober = {});

} // namespace nc::core
