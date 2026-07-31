// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Cocoa/Cocoa.h>

@interface PanelListViewGroupRowView : NSTableRowView

- (void)setTitle:(NSString *)_title itemCount:(int)_item_count;

@end
