// Copyright (C) 2017-2020 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CopyToPasteboard.h"
#include "../PanelController.h"
#include "../Helpers/Pasteboard.h"
#include <Panel/PanelData.h>
#include "../PanelView.h"
#include <VFS/VFS.h>
#include <algorithm>

// TODO: move localizable string to a new file. FilePanelsContextMenu.string was a bad idea!

namespace nc::panel::actions {

namespace {

NSString *const g_FileCopyTitleKey = @"commands.file.copy.title";
NSString *const g_FileCopyTitleFallback = @"Copy";
NSString *const g_FileCutTitleKey = @"commands.file.cut.title";
NSString *const g_FileCutTitleFallback = @"Cut";

NSString *LocalizedFileCopyTitle(NSBundle *_bundle)
{
    NSBundle *const bundle = _bundle != nil ? _bundle : NSBundle.mainBundle;
    NSString *const title = [bundle localizedStringForKey:g_FileCopyTitleKey value:g_FileCopyTitleFallback table:nil];
    if( title.length == 0 || [title isEqualToString:g_FileCopyTitleKey] )
        return g_FileCopyTitleFallback;
    return title;
}

NSString *LocalizedFileCutTitle(NSBundle *_bundle)
{
    NSBundle *const bundle = _bundle != nil ? _bundle : NSBundle.mainBundle;
    NSString *const title = [bundle localizedStringForKey:g_FileCutTitleKey value:g_FileCutTitleFallback table:nil];
    if( title.length == 0 || [title isEqualToString:g_FileCutTitleKey] )
        return g_FileCutTitleFallback;
    return title;
}

} // namespace

void UpdateCopyToPasteboardMenuItemTitle(PanelController *_target, NSMenuItem *_item, NSBundle *_bundle)
{
    const std::vector<VFSListingItem> items = _target.selectedEntriesOrFocusedEntryWithDotDot;
    UpdateCopyToPasteboardMenuItemTitle(items, _item, _bundle);
}

void UpdateCutToPasteboardMenuItemTitle(PanelController *_target, NSMenuItem *_item, NSBundle *_bundle)
{
    const std::vector<VFSListingItem> items = _target.selectedEntriesOrFocusedEntry;
    UpdateCutToPasteboardMenuItemTitle(items, _item, _bundle);
}

void UpdateCopyToPasteboardMenuItemTitle(const std::span<const VFSListingItem> _items,
                                         NSMenuItem *_item,
                                         NSBundle *_bundle)
{
    if( _items.size() > 1 )
        _item.title = [NSString
            stringWithFormat:NSLocalizedStringFromTable(@"Copy %lu Items", @"FilePanelsContextMenu", "Copy many items"),
                             _items.size()];
    else if( _items.size() == 1 )
        _item.title = [NSString stringWithFormat:NSLocalizedStringFromTable(
                                                     @"Copy \u201c%@\u201d", @"FilePanelsContextMenu", "Copy one item"),
                                                 _items.front().DisplayNameNS()];
    else
        _item.title = LocalizedFileCopyTitle(_bundle);
}

void UpdateCutToPasteboardMenuItemTitle(const std::span<const VFSListingItem> _items,
                                        NSMenuItem *_item,
                                        NSBundle *_bundle)
{
    if( _items.size() > 1 )
        _item.title = [NSString
            stringWithFormat:NSLocalizedStringFromTable(@"Cut %lu Items", @"FilePanelsContextMenu", "Cut many items"),
                             _items.size()];
    else if( _items.size() == 1 )
        _item.title = [NSString stringWithFormat:NSLocalizedStringFromTable(
                                                     @"Cut \u201c%@\u201d", @"FilePanelsContextMenu", "Cut one item"),
                                                 _items.front().DisplayNameNS()];
    else
        _item.title = LocalizedFileCutTitle(_bundle);
}

} // namespace nc::panel::actions
