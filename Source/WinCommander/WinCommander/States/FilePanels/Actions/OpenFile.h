// Copyright (C) 2017-2020 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/VFS.h>
#include "DefaultAction.h"
#include <span>

@class NCPanelOpenWithMenuDelegate;

namespace nc::panel {
class FileOpener;
}

namespace nc::panel::actions {

/** Hands an already validated item snapshot to FileOpener. The snapshot is consumed synchronously. */
[[nodiscard]] bool SubmitOpenItemsWithDefaultHandler(std::span<const VFSListingItem> _items,
                                                     PanelController *_target,
                                                     FileOpener &_file_opener);
void UpdateOpenWithDefaultHandlerMenuItemTitle(PanelController *_target, NSMenuItem *_item);

struct OpenFileWithSubmenu final : PanelAction {
    OpenFileWithSubmenu(NCPanelOpenWithMenuDelegate *_menu_delegate);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    [[nodiscard]] bool ValidateMenuItem(PanelController *_target, NSMenuItem *_item) const override;

private:
    NCPanelOpenWithMenuDelegate *m_MenuDelegate;
};

struct AlwaysOpenFileWithSubmenu final : PanelAction {
    AlwaysOpenFileWithSubmenu(NCPanelOpenWithMenuDelegate *_menu_delegate);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    [[nodiscard]] bool ValidateMenuItem(PanelController *_target, NSMenuItem *_item) const override;

private:
    NCPanelOpenWithMenuDelegate *m_MenuDelegate;
};

struct OpenFilesWithDefaultHandler final : PanelAction {
    OpenFilesWithDefaultHandler(FileOpener &_file_opener);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    [[nodiscard]] bool ValidateMenuItem(PanelController *_target, NSMenuItem *_item) const override;
    void Perform(PanelController *_target, id _sender) const override;

private:
    FileOpener &m_FileOpener;
};

} // namespace nc::panel::actions
