// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <span>
#include <string>
#include <vector>

namespace nc::panel {

enum class PanelListViewGroupKind : unsigned char {
    Folders,
    NameInitial,
    Type,
    Empty,
    Tiny,
    Small,
    Medium,
    Large,
    Huge,
    Today,
    Yesterday,
    EarlierThisWeek,
    EarlierThisMonth,
    EarlierThisYear,
    LongAgo,
    Unknown,
};

struct PanelListViewGroupKey {
    PanelListViewGroupKind kind = PanelListViewGroupKind::Unknown;
    std::string value;

    bool operator==(const PanelListViewGroupKey &) const noexcept = default;
};

struct PanelListViewProjectionItem {
    int sorted_index = -1;
    PanelListViewGroupKey group;
    bool is_dotdot = false;
};

struct PanelListViewProjectionRow {
    enum class Kind : unsigned char {
        Item,
        GroupHeader,
    };

    Kind kind = Kind::Item;
    int sorted_index = -1;
    int group_index = -1;
};

struct PanelListViewProjectionGroup {
    PanelListViewGroupKey key;
    int header_row = -1;
    int item_count = 0;
};

/**
 * Maps presentation rows to PanelData sorted positions. Grouping only inserts
 * non-collapsible headers and deliberately preserves the original item order.
 */
class PanelListViewProjection
{
public:
    void RebuildIdentity(int _items_count);
    void RebuildGrouped(std::span<const PanelListViewProjectionItem> _items);

    [[nodiscard]] int RowsCount() const noexcept;
    [[nodiscard]] int SortedIndexForRow(int _row) const noexcept;
    [[nodiscard]] int RowForSortedIndex(int _sorted_index) const noexcept;
    [[nodiscard]] const PanelListViewProjectionRow *RowAt(int _row) const noexcept;
    [[nodiscard]] const PanelListViewProjectionGroup *GroupAt(int _group_index) const noexcept;
    [[nodiscard]] bool IsIdentity() const noexcept;

private:
    bool m_Identity = true;
    int m_ItemsCount = 0;
    std::vector<PanelListViewProjectionRow> m_Rows;
    std::vector<int> m_SortedToRow;
    std::vector<PanelListViewProjectionGroup> m_Groups;
};

} // namespace nc::panel
