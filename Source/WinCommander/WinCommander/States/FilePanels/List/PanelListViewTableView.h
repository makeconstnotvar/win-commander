// Copyright (C) 2016-2021 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

@interface PanelListViewTableView : NSTableView

@property(nonatomic) bool explorerAppearance;

+ (void)drawVerticalSeparatorForView:(NSView *)_view;

@end
