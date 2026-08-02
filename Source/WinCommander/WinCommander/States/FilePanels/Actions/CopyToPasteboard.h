// Copyright (C) 2017-2018 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/VFS.h>
#include "DefaultAction.h"
#include "../Helpers/Pasteboard.h"

namespace nc::panel::actions {

void UpdateCopyToPasteboardMenuItemTitle(PanelController *_target, NSMenuItem *_item, NSBundle *_bundle = nil);
void UpdateCopyToPasteboardMenuItemTitle(std::span<const VFSListingItem> _items,
                                         NSMenuItem *_item,
                                         NSBundle *_bundle = nil);
void UpdateCutToPasteboardMenuItemTitle(PanelController *_target, NSMenuItem *_item, NSBundle *_bundle = nil);
void UpdateCutToPasteboardMenuItemTitle(std::span<const VFSListingItem> _items,
                                        NSMenuItem *_item,
                                        NSBundle *_bundle = nil);

} // namespace nc::panel::actions
