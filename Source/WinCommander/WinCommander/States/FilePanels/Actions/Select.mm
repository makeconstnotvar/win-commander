// Copyright (C) 2017-2024 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Select.h"
#include <Utility/FileMask.h>
#include <Utility/StringExtras.h>
#include <Panel/PanelDataSelection.h>
#include <Panel/PanelData.h>
#include <Panel/FindFilesData.h>
#include <Panel/UI/SelectionWithMaskPopupViewController.h>
#include "../PanelView.h"
#include "../PanelController.h"
#include <VFS/VFS.h>
#include <WinCommander/Bootstrap/Config.h>
#include <Config/RapidJSON.h>
#include <ankerl/unordered_dense.h>
#include <mutex>
#include <bit>
#include <algorithm>

namespace nc::panel::actions {

class PerWindowMaskStorage
{
public:
    FindFilesMask InitialMask(NSWindow *_for_window)
    {
        const auto num = ToNumber(_for_window);
        const std::lock_guard lock{m_Mut};
        if( m_InitialMasks.contains(num) ) {
            return m_InitialMasks[num];
        }
        else {
            FindFilesMask mask;
            mask.string = "*.*";
            mask.type = FindFilesMask::Classic;
            return mask;
        }
    }

    void ReportRecent(const nc::panel::FindFilesMask &_mask, NSWindow *_for_window)
    {
        const std::lock_guard lock{m_Mut};
        m_InitialMasks[ToNumber(_for_window)] = _mask;
    }

private:
    using MapT = ankerl::unordered_dense::map<ptrdiff_t, nc::panel::FindFilesMask>;

    static ptrdiff_t ToNumber(NSWindow *_wnd) noexcept
    {
        // mb mix in the window number here as well? i.e. .windowNumber
        return std::bit_cast<ptrdiff_t>((__bridge void *)_wnd);
    }

    MapT m_InitialMasks;
    std::mutex m_Mut;
};

static constexpr size_t g_MaximumSelectWithMaskHistoryElements = 16;
static const auto g_SelectWithMaskHistoryPath = "filePanel.selectWithMaskPopup.masks";
[[clang::no_destroy]] static PerWindowMaskStorage g_PerWindowMasks;

void SelectAll::Perform(PanelController *_target, id /*_sender*/) const
{
    if( ApplySelectAll(_target) != PaneSelectionActionResult::Available )
        NSBeep();
}

void DeselectAll::Perform(PanelController *_target, id /*_sender*/) const
{
    [_target setEntriesSelection:std::vector<bool>(_target.data.SortedEntriesCount(), false)];
}

void InvertSelection::Perform(PanelController *_target, id /*_sender*/) const
{
    if( ApplyInvertSelection(_target) != PaneSelectionActionResult::Available )
        NSBeep();
}

PaneSelectionActionResult EvaluatePaneSelectionAction(PanelController *_target)
{
    if( !_target )
        return PaneSelectionActionResult::PaneUnavailable;
    if( _target.isDoingBackgroundLoading )
        return PaneSelectionActionResult::Loading;
    if( !_target.data.ListingPtr() )
        return PaneSelectionActionResult::ListingUnavailable;
    const auto &visible = _target.data.EntriesBySoftFiltering();
    const bool has_selectable = std::ranges::any_of(visible, [_target](const unsigned _position) {
        const VFSListingItem item = _target.data.EntryAtSortPosition(static_cast<int>(_position));
        return item && !item.IsDotDot();
    });
    return has_selectable ? PaneSelectionActionResult::Available : PaneSelectionActionResult::Empty;
}

static std::vector<bool> VisibleSelection(PanelController *_target, const bool _invert)
{
    std::vector<bool> selection(static_cast<size_t>(_target.data.SortedEntriesCount()), false);
    for( const unsigned position : _target.data.EntriesBySoftFiltering() ) {
        const VFSListingItem item = _target.data.EntryAtSortPosition(static_cast<int>(position));
        if( item && !item.IsDotDot() ) {
            selection[position] =
                _invert ? !_target.data.VolatileDataAtSortPosition(static_cast<int>(position)).is_selected() : true;
        }
    }
    return selection;
}

PaneSelectionActionResult ApplySelectAll(PanelController *_target)
{
    const PaneSelectionActionResult availability = EvaluatePaneSelectionAction(_target);
    if( availability != PaneSelectionActionResult::Available )
        return availability;
    const VFSListingPtr listing = _target.data.ListingPtr();
    std::vector<bool> selection = VisibleSelection(_target, false);
    if( _target.isDoingBackgroundLoading || _target.data.ListingPtr() != listing )
        return PaneSelectionActionResult::Loading;
    [_target setEntriesSelection:selection];
    return PaneSelectionActionResult::Available;
}

PaneSelectionActionResult ApplyInvertSelection(PanelController *_target)
{
    const PaneSelectionActionResult availability = EvaluatePaneSelectionAction(_target);
    if( availability != PaneSelectionActionResult::Available )
        return availability;
    const VFSListingPtr listing = _target.data.ListingPtr();
    std::vector<bool> selection = VisibleSelection(_target, true);
    if( _target.isDoingBackgroundLoading || _target.data.ListingPtr() != listing )
        return PaneSelectionActionResult::Loading;
    [_target setEntriesSelection:selection];
    return PaneSelectionActionResult::Available;
}

SelectAllByExtension::SelectAllByExtension(bool _result_selection) : m_ResultSelection(_result_selection)
{
}

bool SelectAllByExtension::Predicate(PanelController *_target) const
{
    return _target.view.item;
}

void SelectAllByExtension::Perform(PanelController *_target, id /*_sender*/) const
{
    auto item = _target.view.item;
    if( !item )
        return;

    const std::string extension = item.HasExtension() ? item.Extension() : "";
    auto selector = data::SelectionBuilder(_target.data, _target.ignoreDirectoriesOnSelectionByMask);
    auto selection = selector.SelectionByExtension(extension, m_ResultSelection);
    [_target setEntriesSelection:selection];
}

SelectAllByMask::SelectAllByMask(bool _result_selection) : m_ResultSelection(_result_selection)
{
}

static void CommitRecentFindFilesMask(const nc::panel::FindFilesMask &_mask)
{
    auto history = LoadFindFilesMasks(StateConfig(), g_SelectWithMaskHistoryPath);
    std::erase(history, _mask);
    history.insert(history.begin(), _mask);
    if( history.size() > g_MaximumSelectWithMaskHistoryElements )
        history.resize(g_MaximumSelectWithMaskHistoryElements);
    StoreFindFilesMasks(StateConfig(), g_SelectWithMaskHistoryPath, history);
}

void SelectAllByMask::Perform(PanelController *_target, id /*_sender*/) const
{
    const auto history = LoadFindFilesMasks(StateConfig(), g_SelectWithMaskHistoryPath);
    const auto initial = g_PerWindowMasks.InitialMask(_target.window);
    const auto view = [[SelectionWithMaskPopupViewController alloc] initInitialQuery:initial
                                                                             history:history
                                                                          doesSelect:m_ResultSelection];
    __weak PanelController *wp = _target;
    view.onSelect = [wp, this](const nc::panel::FindFilesMask &_mask) {
        using utility::FileMask;
        CommitRecentFindFilesMask(_mask);
        if( PanelController *const panel = wp ) {
            g_PerWindowMasks.ReportRecent(_mask, panel.window);
            FileMask match_mask;
            if( _mask.type == FindFilesMask::Classic ) {
                if( FileMask::IsWildCard(_mask.string) ) {
                    match_mask = FileMask(_mask.string, FileMask::Type::Mask);
                }
                else {
                    match_mask = FileMask(FileMask::ToExtensionWildCard(_mask.string), FileMask::Type::Mask);
                }
            }
            else if( _mask.type == FindFilesMask::RegEx ) {
                match_mask = FileMask(_mask.string, FileMask::Type::RegEx);
            }

            auto selector = data::SelectionBuilder(panel.data, panel.ignoreDirectoriesOnSelectionByMask);
            auto selection = selector.SelectionByMask(match_mask, m_ResultSelection);
            [panel setEntriesSelection:selection];
        }
    };
    view.onClearHistory = [] {
        using namespace nc::config;
        const Value arr(rapidjson::kArrayType);
        StateConfig().Set(g_SelectWithMaskHistoryPath, arr);
    };

    [_target.view showPopoverUnderPathBarWithView:view andDelegate:view];
}

} // namespace nc::panel::actions
