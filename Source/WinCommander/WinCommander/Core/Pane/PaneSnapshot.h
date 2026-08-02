// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/VFSListing.h>
#include <WinCommander/Core/Errors/FileManagerError.h>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nc::core {

struct PaneId {
    uint64_t value = 0;

    constexpr bool operator==(const PaneId &) const noexcept = default;
};

enum class PaneLoadPhase : uint8_t {
    Empty,
    Loading,
    Loaded,
    Refreshing,
    Failed
};

enum class PaneSortKey : uint8_t {
    Unknown,
    Unsorted,
    RawName,
    Name,
    Extension,
    Size,
    ModifiedTime,
    CreatedTime,
    AddedTime,
    AccessedTime
};

enum class PaneSortDirection : uint8_t {
    None,
    Ascending,
    Descending
};

enum class PaneTextCollation : uint8_t {
    Unknown,
    Natural,
    CaseInsensitive,
    CaseSensitive
};

/** Semantic, toolkit-independent projection of the engine's current display ordering. */
struct PaneSortState {
    PaneSortKey key = PaneSortKey::Unknown;
    PaneSortDirection direction = PaneSortDirection::None;
    PaneTextCollation collation = PaneTextCollation::Unknown;
    bool separates_directories = false;
    bool extensionless_directories = false;

    constexpr bool operator==(const PaneSortState &) const noexcept = default;
};

enum class PaneGroupingKey : uint8_t {
    Unknown,
    Name,
    Extension,
    Size,
    ModifiedTime,
    CreatedTime,
    AddedTime,
    AccessedTime
};

/** Semantic grouping preference derived from the current display ordering. */
struct PaneGroupingState {
    bool enabled = false;
    PaneGroupingKey key = PaneGroupingKey::Unknown;

    constexpr bool operator==(const PaneGroupingState &) const noexcept = default;
};

enum class PaneViewMode : uint8_t {
    Unknown,
    Icons,
    Details,
    Gallery
};

/** Active presentation kind plus its optional valid configured-layout identity. */
struct PaneViewState {
    PaneViewMode mode = PaneViewMode::Unknown;
    std::optional<int32_t> layout_index;

    constexpr bool operator==(const PaneViewState &) const noexcept = default;
};

struct PaneHistoryAvailability {
    bool can_go_back = false;
    bool can_go_forward = false;

    constexpr bool operator==(const PaneHistoryAvailability &) const noexcept = default;
};

/**
 * Immutable, shareable exact-selection payload.
 *
 * PaneState copies retain one shared vector instead of copying every VFSListingItem. A new payload
 * is minted only when the projected membership or display order changes. Empty payloads use a
 * null storage pointer and therefore require no allocation.
 */
class PaneSelectedItems final
{
public:
    using Storage = std::vector<VFSListingItem>;
    using const_iterator = Storage::const_iterator;

    PaneSelectedItems() = default;
    explicit PaneSelectedItems(Storage _items) :
        m_Items(_items.empty() ? nullptr : std::make_shared<const Storage>(std::move(_items)))
    {
    }
    PaneSelectedItems(std::initializer_list<VFSListingItem> _items) :
        PaneSelectedItems(Storage{_items})
    {
    }

    [[nodiscard]] bool empty() const noexcept { return m_Items == nullptr; }
    [[nodiscard]] size_t size() const noexcept { return m_Items ? m_Items->size() : 0; }
    [[nodiscard]] const VFSListingItem &operator[](const size_t _index) const noexcept
    {
        return (*m_Items)[_index];
    }
    [[nodiscard]] const_iterator begin() const noexcept { return Values().begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return Values().end(); }
    [[nodiscard]] const Storage &Values() const noexcept
    {
        static const Storage empty;
        return m_Items ? *m_Items : empty;
    }
    /** Stable identity used to prove O(1) payload reuse across pane snapshots. */
    [[nodiscard]] const Storage *StorageIdentity() const noexcept { return m_Items.get(); }

    bool operator==(const PaneSelectedItems &_rhs) const noexcept
    {
        return m_Items == _rhs.m_Items || Values() == _rhs.Values();
    }

private:
    std::shared_ptr<const Storage> m_Items;
};

/**
 * A semantic pane state read from the existing Panel/VFS engine.
 *
 * Host and listing are shared references to the engine-owned objects. This state deliberately does
 * not copy or independently model listing entries.
 */
struct PaneState {
    /** Monotonic controller generation used to reject delayed location results. */
    unsigned long location_generation = 0;
    PaneLoadPhase load_phase = PaneLoadPhase::Empty;
    bool is_uniform = false;
    std::string path;
    std::string display_title;
    VFSHostPtr host;
    VFSListingPtr listing;
    /** Focused entry from listing; an empty item means that the pane has no focused entry. */
    VFSListingItem focused_item;
    /**
     * Exact selected entries from listing in the model's current display/sort order.
     * Production Loaded projections keep this payload's size equal to selected_count.
     */
    PaneSelectedItems selected_items;
    /** Current semantic display ordering, available before a listing is loaded. */
    PaneSortState sort_state;
    /** Current grouping preference, available before a listing is loaded. */
    PaneGroupingState grouping_state;
    /** Current pane presentation, available before a listing is loaded. */
    PaneViewState view_state;
    /** Navigation availability owned by the panel history, independent of listing load state. */
    PaneHistoryAvailability history_availability;
    /** Stable identity of the history entry selected in recording or playback state. */
    std::optional<uint64_t> current_history_entry_id;
    /** Current hard-filter visibility setting for hidden entries. */
    bool shows_hidden_files = false;
    int32_t item_count = 0;
    int32_t selected_count = 0;
    int64_t selected_bytes = 0;
    /** Request-scoped typed failure retained until a newer accepted intent or committed location. */
    std::optional<FileManagerError> visible_error;

    bool operator==(const PaneState &) const noexcept = default;
};

/**
 * An immutable value published by PaneStoreAdapter.
 *
 * revision changes for every semantic state published after a scheduled rebuild.
 * listing_generation changes only when the referenced VFS listing identity changes. Both counters
 * start at zero for the initial state.
 */
struct PaneSnapshot {
    PaneId pane_id;
    uint64_t revision = 0;
    uint64_t listing_generation = 0;
    PaneState state;

    bool operator==(const PaneSnapshot &) const noexcept = default;
};

} // namespace nc::core
