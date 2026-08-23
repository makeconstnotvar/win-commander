// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PanelListViewGroupRowView.h"
#include <WinCommander/Core/Theming/ExplorerPalette.h>

@implementation PanelListViewGroupRowView {
    NSTextField *m_Title;
}

- (instancetype)initWithFrame:(NSRect)_frame
{
    self = [super initWithFrame:_frame];
    if( self ) {
        self.selectionHighlightStyle = NSTableViewSelectionHighlightStyleNone;
        self.accessibilityRole = NSAccessibilityGroupRole;

        m_Title = [NSTextField labelWithString:@""];
        m_Title.translatesAutoresizingMaskIntoConstraints = false;
        m_Title.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightSemibold];
        m_Title.textColor = NSColor.secondaryLabelColor;
        m_Title.lineBreakMode = NSLineBreakByTruncatingTail;
        [self addSubview:m_Title];

        [NSLayoutConstraint activateConstraints:@[
            [m_Title.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:10.0],
            [m_Title.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-10.0],
            [m_Title.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        ]];
    }
    return self;
}

- (void)setTitle:(NSString *)_title itemCount:(int)_item_count
{
    NSString *const count = [NSString localizedStringWithFormat:NSLocalizedString(@"%d items", "Explorer group count"),
                                                              _item_count];
    m_Title.stringValue = [NSString stringWithFormat:@"%@  %@", _title, count];
    self.accessibilityLabel = _title;
    self.accessibilityValue = count;
}

- (BOOL)isOpaque
{
    return true;
}

- (void)drawRect:(NSRect)_dirty_rect
{
    // The band sits on the same plane as the table header and is closed by the same hairline the
    // rows carry, so the grouping reads as structure rather than as another row.
    [nc::explorer::TableHeaderFillColor() setFill];
    NSRectFill(_dirty_rect);

    [nc::explorer::RowDividerColor() setFill];
    NSRectFill(NSMakeRect(0., 0., self.bounds.size.width, 1.));
    NSRectFill(NSMakeRect(0., self.bounds.size.height - 1., self.bounds.size.width, 1.));
}

@end
