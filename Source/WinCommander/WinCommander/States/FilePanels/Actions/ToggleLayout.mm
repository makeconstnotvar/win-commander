// Copyright (C) 2017-2019 Michael Kazakov. Subject to GNU General Public License version 3.
#include "../PanelController.h"
#include "../PanelViewLayoutSupport.h"
#include "ToggleLayout.h"

namespace nc::panel::actions {

ToggleLayout::ToggleLayout(int _layout_index) : m_Index(_layout_index)
{
}

bool ToggleLayout::Predicate(PanelController *_target) const
{
    if( auto l = _target.layoutStorage.GetLayout(m_Index) )
        return !l->is_disabled();
    return false;
}

bool ToggleLayout::ValidateMenuItem(PanelController *_target, NSMenuItem *_item) const
{
    if( auto layout = _target.layoutStorage.GetLayout(m_Index) ) {
        _item.hidden = false;
        _item.title = layout->name.empty() ? [NSString stringWithFormat:@"Layout #%d", m_Index + 1]
                                          : [NSString stringWithUTF8String:layout->name.c_str()];
        _item.state = _target.layoutIndex == m_Index;
        return !layout->is_disabled();
    }
    _item.hidden = true;
    _item.state = NSControlStateValueOff;
    return false;
}

void ToggleLayout::Perform(PanelController *_target, [[maybe_unused]] id _sender) const
{
    [_target setLayoutIndex:m_Index];
}

} // namespace nc::panel::actions
