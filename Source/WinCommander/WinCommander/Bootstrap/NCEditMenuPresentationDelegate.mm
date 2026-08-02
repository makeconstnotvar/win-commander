// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCEditMenuPresentationDelegate.h"

#include <WinCommander/States/CommandPresentationAdapter.h>

@implementation NCEditMenuPresentationDelegate {
    NSMenuItem *m_CutMenuItem;
    NSMenuItem *m_CopyMenuItem;
    NSString *m_CutBaseTitle;
    NSString *m_CopyBaseTitle;
}

- (instancetype)initWithCutMenuItem:(NSMenuItem *)_cut_menu_item copyMenuItem:(NSMenuItem *)_copy_menu_item
{
    if( _cut_menu_item == nil || _copy_menu_item == nil )
        return nil;

    self = [super init];
    if( self ) {
        m_CutMenuItem = _cut_menu_item;
        m_CopyMenuItem = _copy_menu_item;
        m_CutBaseTitle = [_cut_menu_item.title copy];
        m_CopyBaseTitle = [_copy_menu_item.title copy];
    }
    return self;
}

- (void)menuDidClose:(NSMenu *) [[maybe_unused]] _menu
{
    nc::presentation::CommandPresentationAdapter::Clear(m_CutMenuItem);
    m_CutMenuItem.hidden = false;
    m_CutMenuItem.title = m_CutBaseTitle;

    nc::presentation::CommandPresentationAdapter::Clear(m_CopyMenuItem);
    m_CopyMenuItem.hidden = false;
    m_CopyMenuItem.title = m_CopyBaseTitle;
}

@end
