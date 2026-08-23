// Copyright (C) 2021-2024 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PanelListViewExtensionView.h"
#include "PanelListView.h"
#include "PanelListViewGeometry.h"
#include "PanelListViewGrouping.h"
#include "PanelListViewRowView.h"
#include "PanelListViewTableView.h"
#include <Utility/ObjCpp.h>
#include <Base/CFPtr.h>
#include <cassert>

using namespace nc;

static NSParagraphStyle *const g_Style = [] {
    NSMutableParagraphStyle *const style = [NSMutableParagraphStyle new];
    style.alignment = NSTextAlignmentLeft;
    style.lineBreakMode = NSLineBreakByTruncatingTail;
    style.allowsDefaultTighteningForTruncation = false;
    return style;
}();

@implementation NCPanelListViewExtensionView {
    NSString *m_Extension;
    base::CFPtr<CTLineRef> m_Line;
    __weak PanelListViewRowView *m_RowView;
}

- (id)initWithFrame:(NSRect) [[maybe_unused]] _frameRect
{
    self = [super initWithFrame:NSRect()];
    if( self ) {
    }
    return self;
}

- (BOOL)isOpaque
{
    return true;
}

- (BOOL)wantsDefaultClipping
{
    return false;
}

- (void)prepareForReuse
{
    [super prepareForReuse];
    m_Extension = nil;
    m_Line.reset();
    m_RowView = nil;
}

- (void)viewDidMoveToSuperview
{
    [super viewDidMoveToSuperview];
    if( auto rv = nc::objc_cast<PanelListViewRowView>(self.superview) )
        m_RowView = rv;
}

- (void)setExtension:(NSString *)_extension
{
    m_Extension = _extension;
    [self buildPresentation];
}

- (void)buildPresentation
{
    PanelListViewRowView *row_view = nc::objc_cast<PanelListViewRowView>(self.superview);
    if( !row_view )
        return;

    // A recycled row is handed an empty item before it is refilled or discarded, and an empty item
    // carries no listing to answer questions about itself. The Explorer type column asks the item
    // what it is, so it must not be asked until the row actually holds one.
    const bool explorer = row_view.listView.presentationStyle == NCPanelListViewPresentationStyleExplorer;
    const bool explorer_type = explorer && static_cast<bool>(row_view.item);
    if( m_Extension != nil || explorer_type ) {
        NSString *const display_string =
            explorer_type ? nc::panel::ExplorerFileTypeDescription(row_view.item) : m_Extension;
        NSDictionary *attrs = @{
            NSFontAttributeName: row_view.listView.font,
            NSForegroundColorAttributeName: row_view.rowSecondaryTextColor,
            NSParagraphStyleAttributeName: g_Style
        };
        auto attr_str = [[NSMutableAttributedString alloc] initWithString:display_string attributes:attrs];
        m_Line = base::CFPtr<CTLineRef>::adopt(
            CTLineCreateWithAttributedString(static_cast<CFAttributedStringRef>(attr_str)));
    }
    else {
        m_Line.reset();
    }

    [self setNeedsDisplay:true];
}

- (void)drawRect:(NSRect) [[maybe_unused]] _rect
{
    if( auto rv = m_RowView ) {
        if( auto lv = rv.listView ) {
            [rv.rowBackgroundColor set];
            NSRectFill(self.bounds);
            [PanelListViewTableView drawVerticalSeparatorForView:self];

            if( m_Line ) {
                const auto geometry = lv.geometry;
                const auto context = NSGraphicsContext.currentContext.CGContext;
                CGContextSetFillColorWithColor(context, rv.rowSecondaryTextColor.CGColor);
                CGContextSetTextPosition(
                    context, geometry.LeftInset(), geometry.TextBaseLine());
                CGContextSetTextDrawingMode(context, kCGTextFill);
                CTLineDraw(m_Line.get(), context);
            }
        }
    }
}

@end
