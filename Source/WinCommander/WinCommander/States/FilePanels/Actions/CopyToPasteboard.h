// Copyright (C) 2017-2018 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/VFS.h>
#include "DefaultAction.h"
#include "../Helpers/Pasteboard.h"

namespace nc::panel::actions {

struct CopyToPasteboard : PanelAction {
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    [[nodiscard]] bool ValidateMenuItem(PanelController *_target, NSMenuItem *_item) const override;
    void Perform(PanelController *_target, id _sender) const override;

protected:
    static void PerformWithItems(const std::vector<VFSListingItem> &_items,
                                 PasteboardFileOperation _operation = PasteboardFileOperation::Copy);
};

struct CutToPasteboard final : CopyToPasteboard {
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    [[nodiscard]] bool ValidateMenuItem(PanelController *_target, NSMenuItem *_item) const override;
    void Perform(PanelController *_target, id _sender) const override;
};

namespace context {

struct CopyToPasteboard final : panel::actions::CopyToPasteboard {
    CopyToPasteboard(const std::vector<VFSListingItem> &_items);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    [[nodiscard]] bool ValidateMenuItem(PanelController *_target, NSMenuItem *_item) const override;
    void Perform(PanelController *_target, id _sender) const override;

private:
    const std::vector<VFSListingItem> &m_Items;
};

} // namespace context

} // namespace nc::panel::actions
