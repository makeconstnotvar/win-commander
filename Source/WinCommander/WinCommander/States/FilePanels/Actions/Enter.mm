// Copyright (C) 2017-2024 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Enter.h"
#include "GoToFolder.h"
#include "ExecuteInTerminal.h"
#include "OpenFile.h"
#include "../PanelController.h"
#include "../PanelView.h"
#include <VFS/VFS.h>

namespace nc::panel::actions {

Enter::Enter(FileOpener &_file_opener) : m_OpenFilesAction(_file_opener), m_GoIntoFolder(false)
{
}

Enter::Route Enter::ResolveRoute(PanelController *_target)
{
    if( GoIntoFolder{false}.Predicate(_target) )
        return Route::EnterFolder;
    if( ExecuteInTerminal{}.Predicate(_target) )
        return Route::ExecuteInTerminal;
    return Route::OpenWithDefaultHandler;
}

bool Enter::UsesDefaultFileOpen(PanelController *_target)
{
    return ResolveRoute(_target) == Route::OpenWithDefaultHandler;
}

bool Enter::Predicate(PanelController *_target) const
{
    return _target.view.item;
}

bool Enter::ValidateMenuItem(PanelController *_target, NSMenuItem *_item) const
{
    switch( ResolveRoute(_target) ) {
        case Route::EnterFolder:
            return m_GoIntoFolder.ValidateMenuItem(_target, _item);
        case Route::ExecuteInTerminal:
            return m_ExecuteInTerminal.ValidateMenuItem(_target, _item);
        case Route::OpenWithDefaultHandler:
            return m_OpenFilesAction.ValidateMenuItem(_target, _item);
    }
    return false;
}

void Enter::Perform(PanelController *_target, id _sender) const
{
    switch( ResolveRoute(_target) ) {
        case Route::EnterFolder:
            m_GoIntoFolder.Perform(_target, _sender);
            return;
        case Route::ExecuteInTerminal:
            m_ExecuteInTerminal.Perform(_target, _sender);
            return;
        case Route::OpenWithDefaultHandler:
            m_OpenFilesAction.Perform(_target, _sender);
            return;
    }
}

} // namespace nc::panel::actions
