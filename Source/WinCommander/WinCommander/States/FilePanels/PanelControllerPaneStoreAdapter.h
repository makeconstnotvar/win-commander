// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Panel/PanelDataSortMode.h>
#include <WinCommander/Core/Pane/PaneStoreAdapter.h>
#include <memory>
#include <optional>

@class PanelController;

namespace nc::panel {

struct PanelViewLayout;

namespace data {
class Model;
}

/** Maps the legacy panel ordering bits into the toolkit-independent pane read model. */
[[nodiscard]] core::PaneSortState ProjectPaneSortState(data::SortMode _sort_mode) noexcept;

/** Strict inverse of ProjectPaneSortState; incomplete or contradictory semantic states fail closed. */
[[nodiscard]] std::optional<data::SortMode> RestorePanelSortMode(const core::PaneSortState &_sort_state) noexcept;

/** Derives the active grouping key from the legacy ordering only while grouping is enabled. */
[[nodiscard]] core::PaneGroupingState ProjectPaneGroupingState(data::SortMode _sort_mode,
                                                               bool _enabled) noexcept;

/** Maps an actual presentation layout and a validated slot into the pane read model. */
[[nodiscard]] core::PaneViewState ProjectPaneViewState(const PanelViewLayout &_layout,
                                                       std::optional<int32_t> _layout_index) noexcept;

/**
 * Projects the committed Panel model, exact selected items, and current view focus into the shared
 * pane read model.
 *
 * A focused item is published only while it is the exact item currently reachable through the
 * model's sorted representation. Selected items use that representation's deterministic display
 * order. This keeps stale engine identities out of immutable pane snapshots.
 */
[[nodiscard]] core::PaneState ProjectPaneState(const data::Model &_data,
                                               unsigned long _location_generation,
                                               VFSListingItem _focused_item = {},
                                               core::PaneGroupingState _grouping_state = {},
                                               core::PaneViewState _view_state = {},
                                               core::PaneHistoryAvailability _history_availability = {},
                                               std::optional<uint64_t> _current_history_entry_id = {});

/** Production cache entry point. The supplied payload must match Model's current selection token. */
[[nodiscard]] core::PaneState ProjectPaneState(const data::Model &_data,
                                               unsigned long _location_generation,
                                               VFSListingItem _focused_item,
                                               core::PaneSelectedItems _selected_items,
                                               core::PaneGroupingState _grouping_state = {},
                                               core::PaneViewState _view_state = {},
                                               core::PaneHistoryAvailability _history_availability = {},
                                               std::optional<uint64_t> _current_history_entry_id = {});

/**
 * Production PaneStoreAdapter bridge for the existing PanelController.
 *
 * Construction and destruction are main-queue operations. The bridge owns its source controller,
 * while context notifications are scoped to controller.view and schedule coalesced main-queue
 * reads from controller.data/controller.dataGeneration.
 */
class PanelControllerPaneStoreAdapter
{
public:
    /** Throws std::invalid_argument when the controller or its view is absent. */
    explicit PanelControllerPaneStoreAdapter(PanelController *_controller);
    PanelControllerPaneStoreAdapter(const PanelControllerPaneStoreAdapter &) = delete;
    PanelControllerPaneStoreAdapter(PanelControllerPaneStoreAdapter &&) = delete;
    ~PanelControllerPaneStoreAdapter();

    PanelControllerPaneStoreAdapter &operator=(const PanelControllerPaneStoreAdapter &) = delete;
    PanelControllerPaneStoreAdapter &operator=(PanelControllerPaneStoreAdapter &&) = delete;

    [[nodiscard]] core::PaneStoreAdapter &Store() noexcept;
    [[nodiscard]] const core::PaneStoreAdapter &Store() const noexcept;

private:
    class Impl;
    std::shared_ptr<Impl> m_Impl;
};

} // namespace nc::panel
