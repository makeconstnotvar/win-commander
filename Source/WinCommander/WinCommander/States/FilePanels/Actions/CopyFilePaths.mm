// Copyright (C) 2016-2020 Michael Kazakov. Subject to GNU General Public License version 3.
#include <WinCommander/Bootstrap/Config.h>
#include "../PanelController.h"
#include "../PanelView.h"
#include "CopyFilePaths.h"
#include <VFS/VFS.h>
#include <Panel/PanelData.h>
#include <functional>
#include <Utility/StringExtras.h>
#include <algorithm>
#include <numeric>
#include <tuple>

namespace nc::panel::actions {

static const char *Separator()
{
    static const auto config_path = "filePanel.general.separatorForCopyingMultipleFilenames";
    [[clang::no_destroy]] static const auto s = GlobalConfig().GetString(config_path);
    return s.c_str();
}

static bool WriteSingleStringToClipboard(const std::string &_s)
{
    NSPasteboard *const pb = NSPasteboard.generalPasteboard;
    if( !pb )
        return false;
    [pb declareTypes:@[NSPasteboardTypeString] owner:nil];
    return [pb setString:[NSString stringWithUTF8StdString:_s] forType:NSPasteboardTypeString];
}

static std::string JoinItemStrings(const std::vector<VFSListingItem> &_entries,
                                   const std::function<std::string(const VFSListingItem &)> &_projection)
{
    return std::accumulate(std::begin(_entries), std::end(_entries), std::string{}, [&](const auto &a, const auto &b) {
        return a + (a.empty() ? "" : Separator()) + _projection(b);
    });
}

bool CopyFileName::Predicate(PanelController *_source) const
{
    return _source.view.item;
}

bool CopyFilePath::Predicate(PanelController *_source) const
{
    return _source.view.item;
}

bool CopyFileDirectory::Predicate(PanelController *_source) const
{
    return _source.view.item;
}

CopyPathSubmissionResult EvaluateCopyPathSubmission(const std::span<const VFSListingItem> _items,
                                                    PanelController *_source)
{
    if( !_source )
        return CopyPathSubmissionResult::PaneUnavailable;
    if( _source.isDoingBackgroundLoading )
        return CopyPathSubmissionResult::Loading;
    const VFSListingPtr listing = _source.data.ListingPtr();
    if( !listing || _items.empty() )
        return CopyPathSubmissionResult::SelectionUnavailable;
    if( std::ranges::any_of(_items, [](const VFSListingItem &_item) { return _item.IsDotDot(); }) )
        return CopyPathSubmissionResult::ParentEntryUnsupported;
    if( std::ranges::any_of(_items, [&](const VFSListingItem &_item) {
            return !_item.Host() || _item.Listing().get() != listing.get();
        }) ) {
        return CopyPathSubmissionResult::StaleContext;
    }
    return CopyPathSubmissionResult::Submitted;
}

CopyPathSubmissionResult SubmitCopyPaths(const std::span<const VFSListingItem> _items, PanelController *_source)
{
    const CopyPathSubmissionResult live = EvaluateCopyPathSubmission(_items, _source);
    if( live != CopyPathSubmissionResult::Submitted )
        return live;
    const std::vector<VFSListingItem> entries{_items.begin(), _items.end()};
    return WriteSingleStringToClipboard(JoinItemStrings(entries, [](const auto &item) { return item.Path(); }))
               ? CopyPathSubmissionResult::Submitted
               : CopyPathSubmissionResult::ClipboardUnavailable;
}

void CopyFileName::Perform(PanelController *_source, id /*_sender*/) const
{
    const auto entries = _source.selectedEntriesOrFocusedEntry;
    WriteSingleStringToClipboard(JoinItemStrings(entries, [](const auto &item) { return item.Filename(); }));
}

void CopyFilePath::Perform(PanelController *_source, id /*_sender*/) const
{
    const auto entries = _source.selectedEntriesOrFocusedEntry;
    std::ignore = SubmitCopyPaths(entries, _source);
}

void CopyFileDirectory::Perform(PanelController *_source, id /*_sender*/) const
{
    const auto entries = _source.selectedEntriesOrFocusedEntry;
    WriteSingleStringToClipboard(JoinItemStrings(entries, [](const auto &item) { return item.Directory(); }));
}

context::CopyPathname::CopyPathname(const std::vector<VFSListingItem> &_items) : m_Items(_items)
{
    if( _items.empty() )
        throw std::invalid_argument("CopyPathname was made with empty items set");
}

bool context::CopyPathname::ValidateMenuItem([[maybe_unused]] PanelController *_source, NSMenuItem *_item) const
{
    if( m_Items.size() > 1 ) {
        _item.title = [NSString stringWithFormat:NSLocalizedStringFromTable(@"Copy %lu Items as Pathnames",
                                                                            @"FilePanelsContextMenu",
                                                                            "Copy many items as plain-text pathnames"),
                                                 m_Items.size()];
    }
    else {
        _item.title = [NSString stringWithFormat:NSLocalizedStringFromTable(@"Copy “%@” as Pathname",
                                                                            @"FilePanelsContextMenu",
                                                                            "Copy one item as a plain-text pathname"),
                                                 m_Items.front().DisplayNameNS()];
    }
    return Predicate(_source);
}

void context::CopyPathname::Perform([[maybe_unused]] PanelController *_source, id /*_sender*/) const
{
    std::ignore = SubmitCopyPaths(m_Items, _source);
}

} // namespace nc::panel::actions
