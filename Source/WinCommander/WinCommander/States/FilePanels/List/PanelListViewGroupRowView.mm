// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PanelListViewGroupRowView.h"

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
        m_Title.textColor = NSColor.controlAccentColor;
        m_Title.lineBreakMode = NSLineBreakByTruncatingTail;
        [self addSubview:m_Title];

        [NSLayoutConstraint activateConstraints:@[
            [m_Title.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:10.0],
            [m_Title.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-10.0],
            [m_Title.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-5.0],
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
    [NSColor.controlBackgroundColor setFill];
    NSRectFill(_dirty_rect);

    [NSColor.separatorColor setFill];
    NSRectFill(NSMakeRect(10.0, self.bounds.size.height - 1.0, self.bounds.size.width - 20.0, 1.0));
}

@end
