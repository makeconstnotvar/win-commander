// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>
#include <VFS/VFS.h>

namespace nc::panel {

inline void UpdatePanelItemAccessibility(NSView *_element,
                                         const VFSListingItem &_item,
                                         const bool _selected,
                                         const bool _focused)
{
    const bool bound = static_cast<bool>(_item);
    _element.accessibilityElement = bound;
    _element.accessibilityLabel = bound ? _item.DisplayNameNS() : @"";
    _element.accessibilityHelp = !bound ? @""
                                 : _item.IsDir()
                                     ? NSLocalizedString(@"Folder", "File item accessibility description")
                                     : NSLocalizedString(@"File", "File item accessibility description");
    _element.accessibilitySelected = bound && _selected;

    if( !bound )
        _element.accessibilityValue = @"";
    else if( _selected && _focused )
        _element.accessibilityValue = NSLocalizedString(@"Selected, focused", "File item accessibility state");
    else if( _focused )
        _element.accessibilityValue = NSLocalizedString(@"Focused", "File item accessibility state");
    else if( _selected )
        _element.accessibilityValue = NSLocalizedString(@"Selected", "File item accessibility state");
    else
        _element.accessibilityValue = @"";
}

} // namespace nc::panel
