// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ExplorerTabsModel.h"

#include <algorithm>
#include <iterator>

namespace nc::core {

namespace {

[[nodiscard]] bool IsZero(const PaneId _pane) noexcept
{
    return _pane.value == 0;
}

} // namespace

ExplorerTabObservationGate::BindResult ExplorerTabObservationGate::Bind(const PaneId _pane_id) noexcept
{
    if( IsZero(_pane_id) )
        return std::unexpected(ExplorerTabsFailure::ZeroPaneId);
    ++m_Current.generation;
    m_Current.pane_id = _pane_id;
    return m_Current;
}

void ExplorerTabObservationGate::Invalidate() noexcept
{
    ++m_Current.generation;
    m_Current.pane_id = {};
}

bool ExplorerTabObservationGate::Accepts(const ExplorerTabObservationToken _token,
                                         const PaneId _model_active,
                                         const PaneId _snapshot_pane) const noexcept
{
    return _token == m_Current && !IsZero(_token.pane_id) && _token.pane_id == _model_active &&
           _token.pane_id == _snapshot_pane;
}

ExplorerTabsModel::ExplorerTabsModel(const PaneId _initial_pane) : m_Panes{_initial_pane}
{
}

ExplorerTabsModel::CreateResult ExplorerTabsModel::Create(const PaneId _initial_pane)
{
    if( IsZero(_initial_pane) )
        return std::unexpected(ExplorerTabsFailure::ZeroPaneId);
    return ExplorerTabsModel{_initial_pane};
}

ExplorerTabsMutationResult ExplorerTabsModel::Activate(const PaneId _pane)
{
    if( IsZero(_pane) )
        return std::unexpected(ExplorerTabsFailure::ZeroPaneId);

    const auto iterator = std::ranges::find(m_Panes, _pane);
    if( iterator == m_Panes.end() )
        return std::unexpected(ExplorerTabsFailure::PaneNotFound);

    m_ActiveIndex = static_cast<size_t>(std::distance(m_Panes.begin(), iterator));
    return {};
}

ExplorerTabsMutationResult ExplorerTabsModel::Append(const PaneId _pane)
{
    return Insert(m_Panes.size(), _pane);
}

ExplorerTabsMutationResult ExplorerTabsModel::Insert(const size_t _index, const PaneId _pane)
{
    if( IsZero(_pane) )
        return std::unexpected(ExplorerTabsFailure::ZeroPaneId);
    if( std::ranges::find(m_Panes, _pane) != m_Panes.end() )
        return std::unexpected(ExplorerTabsFailure::DuplicatePaneId);
    if( _index > m_Panes.size() )
        return std::unexpected(ExplorerTabsFailure::IndexOutOfRange);

    m_Panes.insert(m_Panes.begin() + static_cast<std::ptrdiff_t>(_index), _pane);
    m_ActiveIndex = _index;
    return {};
}

ExplorerTabsMutationResult ExplorerTabsModel::Close(const PaneId _pane)
{
    if( IsZero(_pane) )
        return std::unexpected(ExplorerTabsFailure::ZeroPaneId);

    const auto iterator = std::ranges::find(m_Panes, _pane);
    if( iterator == m_Panes.end() )
        return std::unexpected(ExplorerTabsFailure::PaneNotFound);
    if( m_Panes.size() == 1 )
        return std::unexpected(ExplorerTabsFailure::LastTab);

    const size_t closing_index = static_cast<size_t>(std::distance(m_Panes.begin(), iterator));
    if( closing_index == m_ActiveIndex ) {
        m_Panes.erase(iterator);
        m_ActiveIndex = std::min(closing_index, m_Panes.size() - 1);
    }
    else {
        m_Panes.erase(iterator);
        if( closing_index < m_ActiveIndex )
            --m_ActiveIndex;
    }
    return {};
}

ExplorerTabsMutationResult ExplorerTabsModel::Reorder(const PaneId _pane, const size_t _final_index)
{
    if( IsZero(_pane) )
        return std::unexpected(ExplorerTabsFailure::ZeroPaneId);

    const auto iterator = std::ranges::find(m_Panes, _pane);
    if( iterator == m_Panes.end() )
        return std::unexpected(ExplorerTabsFailure::PaneNotFound);
    if( _final_index >= m_Panes.size() )
        return std::unexpected(ExplorerTabsFailure::IndexOutOfRange);

    const size_t current_index = static_cast<size_t>(std::distance(m_Panes.begin(), iterator));
    if( current_index == _final_index )
        return {};

    const PaneId active = Active();
    const PaneId moving = *iterator;
    m_Panes.erase(iterator);
    m_Panes.insert(m_Panes.begin() + static_cast<std::ptrdiff_t>(_final_index), moving);
    m_ActiveIndex = static_cast<size_t>(std::distance(m_Panes.begin(), std::ranges::find(m_Panes, active)));
    return {};
}

} // namespace nc::core
