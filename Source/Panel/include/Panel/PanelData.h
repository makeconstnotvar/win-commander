// Copyright (C) 2013-2020 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/VFSListing.h>
#include "PanelDataSortMode.h"
#include "PanelDataStatistics.h"
#include "PanelDataFilter.h"
#include "PanelDataItemVolatileData.h"

#include <vector>
#include <memory>
#include <functional>
#include <string_view>
#include <span>
#include <cstdint>

namespace nc::panel::data {

struct ItemVolatileData;
struct ExternalEntryKey;

/**
 * PanelData actually does the following things:
 * - sorting provided data
 * - handling reloading with preserving of custom entries data
 * - searching
 * - paths accessing
 * - custom information setting/getting
 * - statistics
 * These methods should be called by a controller, since some view's props have to be updated.
 * PanelData is solely sync class - it does not know about concurrency,
 * any parallelism should be done by callers (i.e. controller).
 */
class Model
{
public:
    enum class PanelType : int8_t {
        Directory = 0,
        Temporary = 1
    };

    /**
     * A small owning snapshot of the presentation options required to prepare a replacement
     * model without reading the live Model from a worker thread.
     */
    struct PreparationOptions {
        struct SortMode sort_mode;
        HardFilter hard_filter;
        TextualFilter soft_filter;
        uint64_t generation = 0;
        bool operator==(const PreparationOptions &) const = default;
    };

    /**
     * An owning main-thread snapshot used to prepare a replacement Model without reading the live
     * instance from a worker. Copying the two flat arrays is the only O(N) work on the main queue;
     * filtering, sorting, reload reconciliation and statistics are performed by the detached path.
     */
    struct PreparationSnapshot {
        VFSListingPtr listing;
        std::vector<ItemVolatileData> volatile_data;
        std::vector<unsigned> entries_by_raw_name;
        PanelType type = PanelType::Directory;
        PreparationOptions options;
        uint64_t selection_projection_generation = 0;
    };

    using PreparationCancelChecker = std::function<bool()>;

    // creates a Model with an empty listing
    Model();

    Model(const Model &);

    Model(Model &&) noexcept;

    ~Model();

    Model &operator=(const Model &);
    Model &operator=(Model &&) noexcept;

    // Initializes a new model with _listing, allocates fresh volatile data, builds search indices,
    // updates statistics.
    void Load(const VFSListingPtr &_listing, PanelType _type);

    /**
     * Captures the exact options needed by PrepareDetached(). The returned value owns its filter
     * strings and can safely outlive the live model.
     */
    [[nodiscard]] PreparationOptions CapturePreparationOptions() const;

    [[nodiscard]] PreparationSnapshot CapturePreparationSnapshot() const;

    /**
     * Builds a fresh, owning model on a non-main worker while keeping Load() main-thread-only.
     * Requested names receive the same exact visible-selection semantics as the synchronous path.
     */
    [[nodiscard]] static std::unique_ptr<Model>
    PrepareDetached(const VFSListingPtr &_listing,
                    PanelType _type,
                    PreparationOptions _options,
                    std::span<const std::string> _requested_selection = {},
                    PreparationCancelChecker _is_cancelled = {});

    /** Builds a replacement for the same listing while preserving exact volatile item data. */
    [[nodiscard]] static std::unique_ptr<Model>
    PrepareDetachedFromSnapshot(const PreparationSnapshot &_snapshot,
                                PreparationOptions _options,
                                PreparationCancelChecker _is_cancelled = {});

    /** Reconciles a refreshed listing with a captured source while preserving matching volatile data. */
    [[nodiscard]] static std::unique_ptr<Model>
    PrepareDetachedReload(const PreparationSnapshot &_snapshot,
                          const VFSListingPtr &_listing,
                          PreparationOptions _options,
                          PreparationCancelChecker _is_cancelled = {});

    /**
     * Validates that no newer sort/filter preference replaced a captured preparation snapshot.
     */
    [[nodiscard]] bool MatchesPreparationOptions(const PreparationOptions &_options) const noexcept;

    void ReLoad(const VFSListingPtr &_listing);

    /**
     * Tells whether Model was provided with a valid listing object.
     */
    [[nodiscard]] bool IsLoaded() const noexcept;

    /**
     * Returns a common VHS host referred by the stored listing.
     * Will throw logic_error if called on a listing with no common host.
     */
    [[nodiscard]] const std::shared_ptr<VFSHost> &Host() const;

    /**
     * Returns a stored VFS listing.
     */
    [[nodiscard]] const VFSListing &Listing() const noexcept;

    /**
     * Returns a shared pointer to a stored VFS listing.
     */
    [[nodiscard]] const VFSListingPtr &ListingPtr() const noexcept;

    /**
     * Returns a panel type provided upen loading.
     */
    [[nodiscard]] PanelType Type() const noexcept;

    /**
     * Returns the number of raw i.e. unfiltered entires in the listing.
     */
    [[nodiscard]] int RawEntriesCount() const noexcept;

    /**
     * Returns the number of sorted i.e. possibly filtered entires in the listing.
     */
    [[nodiscard]] int SortedEntriesCount() const noexcept;

    [[nodiscard]] const std::vector<unsigned> &SortedDirectoryEntries() const noexcept;

    /**
     * EntriesBySoftFiltering return a vector of filtered indeces of sorted entries (not raw ones)
     */
    [[nodiscard]] const std::vector<unsigned> &EntriesBySoftFiltering() const noexcept;

    // will return an "empty" item upon invalid index
    [[nodiscard]] VFSListingItem EntryAtRawPosition(int _pos) const noexcept;

    // will throw an exception upon invalid index
    ItemVolatileData &VolatileDataAtRawPosition(int _pos);

    // will throw an exception upon invalid index
    [[nodiscard]] const ItemVolatileData &VolatileDataAtRawPosition(int _pos) const;

    [[nodiscard]] bool IsValidSortPosition(int _pos) const noexcept;

    // will return an "empty" item upon invalid index
    [[nodiscard]] VFSListingItem EntryAtSortPosition(int _pos) const noexcept;

    // will throw an exception upon invalid index
    ItemVolatileData &VolatileDataAtSortPosition(int _pos);

    // will throw an exception upon invalid index
    [[nodiscard]] const ItemVolatileData &VolatileDataAtSortPosition(int _pos) const;

    // Syntax sugar around SortedIndexForRawIndex(_item.Index()), but also checks
    // if the item's listing is the same as the model's.
    // Returns "-1" if the item is not found in the sorted representation.
    // O(1) complexity.
    [[nodiscard]] int SortPositionOfEntry(const VFSListingItem &_item) const noexcept;

    [[nodiscard]] std::vector<std::string> SelectedEntriesFilenames() const;

    /**
     * Returns a list of selected VFS items, without a specific order,
     * according to the raw structure of a listing.
     * O(N) complexity.
     */
    [[nodiscard]] std::vector<VFSListingItem> SelectedEntriesUnsorted() const;

    /**
     * Returns a list of selected VFS items, ordered according to the selected sort mode.
     * O(N) complexity.
     */
    [[nodiscard]] std::vector<VFSListingItem> SelectedEntriesSorted() const;

    /**
     * Returns a monotonic token for the exact SelectedEntriesSorted() projection.
     * The token advances when selection membership, order, visibility, or current-listing item
     * identity can change. Pure reads and soft-filter-only changes preserve it.
     */
    [[nodiscard]] uint64_t SelectionProjectionGeneration() const noexcept;

    /**
     * Will throw an invalid_argument on invalid _pos.
     */
    [[nodiscard]] ExternalEntryKey EntrySortKeysAtSortPosition(int _pos) const;

    /**
     * will redirect ".." upwards
     */
    [[nodiscard]] std::string FullPathForEntry(int _raw_index) const;

    /**
     * Converts sorted index into raw index. Returns -1 on any errors.
     * O(1) complexity.
     */
    [[nodiscard]] int RawIndexForSortIndex(int _index) const noexcept;

    /**
     * Performs a binary case-sensivitive search.
     * Return -1 if didn't found.
     * Returning value is in raw land, that is DirectoryEntries[N], not sorted ones.
     * NB! it has issues with non-uniform listings - it can return only the first entry.
     * Complexity: O(logN ), N - total number of items in the listing.
     */
    [[nodiscard]] int RawIndexForName(std::string_view _filename) const noexcept;

    /**
     * Performs a binary case-sensivitive search.
     * Return a non-owning range of indices.
     * Returning value is in raw land, that is Listing()[N], not sorted ones.
     * Complexity: O(2 * logN ), N - total number of items in the listing.
     */
    [[nodiscard]] std::span<const unsigned> RawIndicesForName(std::string_view _filename) const noexcept;

    /**
     * Performs a search using current sort settings with prodived keys.
     * Return a lower bound entry - first entry with is not less than a key from _keys.
     * Returns -1 if such entry wasn't found.
     */
    [[nodiscard]] int SortLowerBoundForEntrySortKeys(const ExternalEntryKey &_key) const;

    /**
     * Returns a sorted index for a given filename.
     * Returns -1 if such entry wasn't found.
     * Returned value is in sorted indxs land.
     * O(logN) complexity, N - total number of items in the listing.
     * NB! for non-uniform listings this will return only the first item, while there can be more, as filename is not
     * unique there.
     */
    [[nodiscard]] int SortedIndexForName(std::string_view _filename) const noexcept;

    /**
     * Returns a sorted index for the raw index.
     * If the raw index is not present in the sorted indices - returns -1.
     * For OOB access returns -1 as well.
     * O(1) complexity.
     */
    [[nodiscard]] int SortedIndexForRawIndex(int _desired_raw_index) const noexcept;

    /**
     * return current directory in long variant starting from /
     */
    [[nodiscard]] std::string DirectoryPathWithoutTrailingSlash() const;

    /**
     * same as DirectoryPathWithoutTrailingSlash() but path will ends with slash
     */
    [[nodiscard]] std::string DirectoryPathWithTrailingSlash() const;

    /**
     * return name of a current directory in a parent directory.
     * returns a zero string for a root dir.
     */
    [[nodiscard]] std::string DirectoryPathShort() const;

    [[nodiscard]] std::string VerboseDirectoryFullPath() const;

    // sorting
    void SetSortMode(SortMode _mode);
    [[nodiscard]] SortMode SortMode() const;

    // hard filtering filtering
    void SetHardFiltering(const HardFilter &_filter);
    [[nodiscard]] HardFilter HardFiltering() const;

    void SetSoftFiltering(const TextualFilter &_filter);
    [[nodiscard]] TextualFilter SoftFiltering() const;

    /**
     * ClearTextFiltering() efficiently sets SoftFiltering.text = nil and HardFiltering.text.text =
     * nil. It's better than consequent calls of SetHardFiltering()+SetSoftFiltering() - less
     * indeces rebuilding. Return true if calling of this method changed anything, and false if
     * indeces was unchanged
     */
    bool ClearTextFiltering();

    [[nodiscard]] const Statistics &Stats() const noexcept;

    // manupulation with user flags for directory entries

    // TODO: bool results?????

    void CustomFlagsSelectSorted(int _at_sorted_pos, bool _is_selected);
    bool CustomFlagsSelectSorted(const std::vector<bool> &_is_selected);

    void CustomIconClearAll();
    void CustomFlagsClearHighlights();

    /**
     * Searches for a directory named '_filename' in '_directory' using binary search with case-sensitive comparison and
     * sets its size. Return true if the entry was found and the size was set, false otherwise. _size should be less
     * than uint64_t(-1). Automatically rebuilds search/sort indices and statistics.
     */
    bool SetCalculatedSizeForDirectory(std::string_view _filename, std::string_view _directory, uint64_t _size);

    /**
     * A batch version of SetCalculatedSizeForDirectory.
     * Returns a number of entries found and set.
     */
    size_t SetCalculatedSizesForDirectories(std::span<const std::string_view> _filenames,
                                            std::span<const std::string_view> _directories,
                                            std::span<const uint64_t> _sizes);

    /**
     * A batch version of SetCalculatedSizeForDirectory that accepts raw item indices.
     * Returns a number of entries found and set.
     */
    size_t SetCalculatedSizesForDirectories(std::span<const unsigned> _raw_items_indices,
                                            std::span<const uint64_t> _sizes);

    /**
     * Call it in emergency case.
     */
    void __InvariantCheck() const;

private:
    void AdvanceSelectionProjectionGeneration() noexcept;
    void AdvancePreparationOptionsGeneration() noexcept;
    void LoadImpl(const VFSListingPtr &_listing,
                  PanelType _type,
                  const PreparationCancelChecker *_is_cancelled = nullptr);
    void ReLoadImpl(const VFSListingPtr &_listing, const PreparationCancelChecker *_is_cancelled);
    void DoSortWithHardFiltering(const PreparationCancelChecker *_is_cancelled = nullptr);
    void CustomFlagsSelectRaw(int _at_raw_pos, bool _is_selected);
    void ClearSelectedFlagsFromHiddenElements();
    void UpdateStatictics(const PreparationCancelChecker *_is_cancelled = nullptr);
    void BuildSoftFilteringIndeces(const PreparationCancelChecker *_is_cancelled = nullptr);
    void FinalizeSettingCalculatedSizes();

    // m_Listing container will change every time directory change/reloads,
    // while the following sort-indeces(except for m_EntriesByRawName) will be permanent with it's
    // content changing
    VFSListingPtr m_Listing;
    std::vector<ItemVolatileData> m_VolatileData;

    // sorted with raw strcmp comparison
    std::vector<unsigned> m_EntriesByRawName;

    // sorted with customly defined sort
    std::vector<unsigned> m_EntriesByCustomSort;

    // Reversed index: maps from the raw indices to the sorted indices. Can be
    // std::numeric_limits<unsigned>::max() if the entry is not present in the custom sort.
    std::vector<unsigned> m_ReverseToCustomSort;

    // sorted and filtered, points at m_EntriesByCustomSort indices, not the raw ones
    std::vector<unsigned> m_EntriesBySoftFiltering;
    struct SortMode m_CustomSortMode;
    HardFilter m_HardFiltering;
    TextualFilter m_SoftFiltering;
    Statistics m_Stats;
    PanelType m_Type;
    uint64_t m_SelectionProjectionGeneration = 0;
    uint64_t m_PreparationOptionsGeneration = 0;
};

} // namespace nc::panel::data
