// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <WinCommander/Core/Pane/PaneNavigationAvailability.h>
#include <WinCommander/Core/Pane/PaneSnapshot.h>
#include <optional>

namespace nc::explorer {

/** Store-backed presentation input scoped to one Explorer pane identity. */
class PanePresentationModel
{
public:
    explicit constexpr PanePresentationModel(const core::PaneId _pane_id) noexcept : m_PaneId(_pane_id) {}

    bool Apply(const core::PaneSnapshot &_snapshot) noexcept
    {
        const auto navigation_availability =
            core::MapMatchingPaneNavigationAvailability(m_PaneId, _snapshot);
        if( !navigation_availability ) {
            m_HiddenFilesVisibility.reset();
            m_SortState.reset();
            m_GroupingState.reset();
            m_ViewState.reset();
            m_HistoryAvailability.reset();
            m_NavigationAvailability.reset();
            return false;
        }
        m_HiddenFilesVisibility = _snapshot.state.shows_hidden_files;
        m_SortState = _snapshot.state.sort_state;
        m_GroupingState = _snapshot.state.grouping_state;
        m_ViewState = _snapshot.state.view_state;
        m_HistoryAvailability = _snapshot.state.history_availability;
        m_NavigationAvailability = navigation_availability;
        return true;
    }

    [[nodiscard]] constexpr std::optional<bool> HiddenFilesVisibility() const noexcept
    {
        return m_HiddenFilesVisibility;
    }
    [[nodiscard]] constexpr std::optional<core::PaneSortState> SortState() const noexcept
    {
        return m_SortState;
    }
    [[nodiscard]] constexpr std::optional<core::PaneGroupingState> GroupingState() const noexcept
    {
        return m_GroupingState;
    }
    [[nodiscard]] constexpr std::optional<core::PaneViewState> ViewState() const noexcept
    {
        return m_ViewState;
    }
    [[nodiscard]] constexpr bool NoGroupingMarkerActive() const noexcept
    {
        return m_GroupingState && !m_GroupingState->enabled;
    }
    [[nodiscard]] constexpr bool
    GroupingMarkerActive(const core::PaneGroupingKey _key) const noexcept
    {
        return m_GroupingState && m_GroupingState->enabled && m_GroupingState->key == _key;
    }
    [[nodiscard]] constexpr bool LayoutMarkerActive(const int32_t _layout_index) const noexcept
    {
        return m_ViewState && m_ViewState->layout_index &&
               *m_ViewState->layout_index == _layout_index;
    }
    [[nodiscard]] constexpr bool CanGoBack() const noexcept
    {
        return m_HistoryAvailability && m_HistoryAvailability->can_go_back;
    }
    [[nodiscard]] constexpr bool CanGoForward() const noexcept
    {
        return m_HistoryAvailability && m_HistoryAvailability->can_go_forward;
    }
    [[nodiscard]] constexpr std::optional<core::PaneHistoryAvailability>
    HistoryAvailability() const noexcept
    {
        return m_HistoryAvailability;
    }
    [[nodiscard]] constexpr std::optional<core::PaneNavigationAvailability>
    NavigationAvailability() const noexcept
    {
        return m_NavigationAvailability;
    }
    [[nodiscard]] constexpr std::optional<core::PaneSortDirection>
    ActiveSortDirection(const core::PaneSortKey _key) const noexcept
    {
        if( !m_SortState || m_SortState->key != _key ||
            (m_SortState->direction != core::PaneSortDirection::Ascending &&
             m_SortState->direction != core::PaneSortDirection::Descending) )
            return std::nullopt;
        return m_SortState->direction;
    }

private:
    core::PaneId m_PaneId;
    std::optional<bool> m_HiddenFilesVisibility;
    std::optional<core::PaneSortState> m_SortState;
    std::optional<core::PaneGroupingState> m_GroupingState;
    std::optional<core::PaneViewState> m_ViewState;
    std::optional<core::PaneHistoryAvailability> m_HistoryAvailability;
    std::optional<core::PaneNavigationAvailability> m_NavigationAvailability;
};

} // namespace nc::explorer
