// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

/** Restores shared responder-chain Cut, Copy, Paste and Select All items after panel presentation. */
@interface NCEditMenuPresentationDelegate : NSObject <NSMenuDelegate>
- (instancetype)initWithCutMenuItem:(NSMenuItem *)_cut_menu_item
                       copyMenuItem:(NSMenuItem *)_copy_menu_item
                      pasteMenuItem:(NSMenuItem *)_paste_menu_item
                  selectAllMenuItem:(NSMenuItem *)_select_all_menu_item NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
@end
