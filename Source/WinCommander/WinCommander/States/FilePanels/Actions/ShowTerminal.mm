// Copyright (C) 2017-2019 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ShowTerminal.h"
#include "../MainWindowFilePanelState.h"
#include "../PanelController.h"
#include "../../MainWindowController.h"
#include <Utility/ObjCpp.h>
#include <WinCommander/Core/Tools/LocalWorkingDirectory.h>

namespace nc::panel::actions {

static const auto g_ShowTitle = NSLocalizedString(@"Show Terminal", "Menu item title for showing terminal");

bool ShowTerminal::ValidateMenuItem(MainWindowFilePanelState *_target, NSMenuItem *_item) const
{
    _item.title = g_ShowTitle;
    return Predicate(_target);
}

void ShowTerminal::Perform(MainWindowFilePanelState *_target, [[maybe_unused]] id _sender) const
{
    std::string path;

    // The one rule, rather than a second copy of it here. Besides keeping the two from drifting, it
    // strips the trailing separator, which the shell echoes back doubled.
    if( auto pc = _target.activePanelController; pc != nil && pc.vfs != nil ) {
        const std::string current = pc.currentDirectoryPath;
        const nc::core::LocalWorkingDirectory resolved =
            nc::core::ResolveLocalWorkingDirectory({.is_native_filesystem = pc.vfs->IsNativeFS(),
                                                    .is_uniform = pc.isUniform != 0,
                                                    .path = current});
        // An unusable location leaves the path empty, which starts the shell at the user's home -
        // the same thing it did before, and better than a directory they were not looking at.
        if( resolved.Usable() )
            path = resolved.path;
    }

    if( const auto mwc = objc_cast<NCMainWindowController>(_target.window.delegate) )
        [mwc requestTerminal:path];
}

} // namespace nc::panel::actions
