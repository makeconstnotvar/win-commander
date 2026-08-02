// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

/** Restores the shared responder-chain Cut and Copy items after panel-specific menu presentation. */
@interface NCEditMenuPresentationDelegate : NSObject <NSMenuDelegate>
- (instancetype)initWithCutMenuItem:(NSMenuItem *)_cut_menu_item
                       copyMenuItem:(NSMenuItem *)_copy_menu_item NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
@end
