// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PanelListViewProjection.h"
#include <cassert>
#include <utility>

namespace nc::panel {

void PanelListViewProjection::RebuildIdentity(int _items_count)
{
    assert(_items_count >= 0);
    m_Identity = true;
    m_ItemsCount = _items_count;
    m_Rows.clear();
    m_SortedToRow.clear();
    m_Groups.clear();
}

void PanelListViewProjection::RebuildGrouped(std::span<const PanelListViewProjectionItem> _items)
{
    m_Identity = false;
    m_ItemsCount = static_cast<int>(_items.size());
    m_Rows.clear();
    m_SortedToRow.assign(_items.size(), -1);
    m_Groups.clear();
    m_Rows.reserve(_items.size() + 16);

    int current_group = -1;
    for( const auto &item : _items ) {
        assert(item.sorted_index >= 0 && item.sorted_index < m_ItemsCount);

        if( item.is_dotdot ) {
            current_group = -1;
        }
        else if( current_group < 0 || !(m_Groups[current_group].key == item.group) ) {
            current_group = static_cast<int>(m_Groups.size());
            PanelListViewProjectionGroup group;
            group.key = item.group;
            group.header_row = static_cast<int>(m_Rows.size());
            m_Groups.emplace_back(std::move(group));
            m_Rows.emplace_back(PanelListViewProjectionRow{
                .kind = PanelListViewProjectionRow::Kind::GroupHeader,
                .sorted_index = -1,
                .group_index = current_group,
            });
        }

        const auto row = static_cast<int>(m_Rows.size());
        m_SortedToRow[item.sorted_index] = row;
        m_Rows.emplace_back(PanelListViewProjectionRow{
            .kind = PanelListViewProjectionRow::Kind::Item,
            .sorted_index = item.sorted_index,
            .group_index = current_group,
        });
        if( current_group >= 0 )
            ++m_Groups[current_group].item_count;
    }
}

int PanelListViewProjection::RowsCount() const noexcept
{
    return m_Identity ? m_ItemsCount : static_cast<int>(m_Rows.size());
}

int PanelListViewProjection::SortedIndexForRow(int _row) const noexcept
{
    if( m_Identity )
        return _row >= 0 && _row < m_ItemsCount ? _row : -1;
    if( const auto row = RowAt(_row); row && row->kind == PanelListViewProjectionRow::Kind::Item )
        return row->sorted_index;
    return -1;
}

int PanelListViewProjection::RowForSortedIndex(int _sorted_index) const noexcept
{
    if( m_Identity )
        return _sorted_index >= 0 && _sorted_index < m_ItemsCount ? _sorted_index : -1;
    return _sorted_index >= 0 && _sorted_index < static_cast<int>(m_SortedToRow.size())
               ? m_SortedToRow[_sorted_index]
               : -1;
}

const PanelListViewProjectionRow *PanelListViewProjection::RowAt(int _row) const noexcept
{
    if( m_Identity || _row < 0 || _row >= static_cast<int>(m_Rows.size()) )
        return nullptr;
    return &m_Rows[_row];
}

const PanelListViewProjectionGroup *PanelListViewProjection::GroupAt(int _group_index) const noexcept
{
    if( _group_index < 0 || _group_index >= static_cast<int>(m_Groups.size()) )
        return nullptr;
    return &m_Groups[_group_index];
}

bool PanelListViewProjection::IsIdentity() const noexcept
{
    return m_Identity;
}

} // namespace nc::panel
