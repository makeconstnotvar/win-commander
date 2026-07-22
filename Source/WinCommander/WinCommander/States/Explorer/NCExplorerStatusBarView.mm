// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerStatusBarView.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/PanelViewFooterVolumeInfoFetcher.h"
#include <Panel/PanelData.h>
#include <Panel/PanelDataStatistics.h>
#include <Utility/ByteCountFormatter.h>
#include <Utility/Layout.h>

using namespace nc::panel;

@implementation NCExplorerStatusBarView {
    PanelController *m_Panel;
    NSBox *m_Separator;
    NSTextField *m_ItemsLabel;
    NSTextField *m_SelectionLabel;
    NSTextField *m_VolumeLabel;
    FooterVolumeInfoFetcher m_VolumeInfoFetcher;
}

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        m_Panel = _panel;

        [self createControls];
        [self installConstraints];

        __weak NCExplorerStatusBarView *weak_self = self;
        m_VolumeInfoFetcher.SetCallback([=](const VFSStatFS &) {
            if( NCExplorerStatusBarView *const strong_self = weak_self )
                [strong_self updateVolumeInfo];
        });

        [self refresh];
    }
    return self;
}

- (NSTextField *)makeLabelWithAlignment:(NSTextAlignment)_alignment
{
    NSTextField *label = [[NSTextField alloc] initWithFrame:NSRect()];
    label.translatesAutoresizingMaskIntoConstraints = false;
    label.stringValue = @"";
    label.bordered = false;
    label.editable = false;
    label.selectable = false;
    label.drawsBackground = false;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.maximumNumberOfLines = 1;
    label.alignment = _alignment;
    label.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    label.textColor = NSColor.secondaryLabelColor;
    return label;
}

- (void)createControls
{
    m_Separator = [[NSBox alloc] initWithFrame:NSRect()];
    m_Separator.translatesAutoresizingMaskIntoConstraints = false;
    m_Separator.boxType = NSBoxSeparator;

    m_ItemsLabel = [self makeLabelWithAlignment:NSTextAlignmentLeft];
    m_SelectionLabel = [self makeLabelWithAlignment:NSTextAlignmentCenter];
    m_VolumeLabel = [self makeLabelWithAlignment:NSTextAlignmentRight];

    [self addSubview:m_Separator];
    [self addSubview:m_ItemsLabel];
    [self addSubview:m_SelectionLabel];
    [self addSubview:m_VolumeLabel];
}

- (void)installConstraints
{
    const auto views = NSDictionaryOfVariableBindings(m_Separator, m_ItemsLabel, m_SelectionLabel, m_VolumeLabel);
    const auto ac = [&](NSString *_vf) {
        [self addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:_vf
                                                                       options:0
                                                                       metrics:nil
                                                                         views:views]];
    };
    ac(@"V:|-(0)-[m_Separator(==1)]");
    ac(@"|-(0)-[m_Separator]-(0)-|");
    ac(@"|-(8)-[m_ItemsLabel(>=60)]-(>=8)-[m_SelectionLabel]-(>=8)-[m_VolumeLabel(>=100)]-(8)-|");

    [self addConstraint:LayoutConstraintForCenteringViewVertically(m_ItemsLabel, self)];
    [self addConstraint:LayoutConstraintForCenteringViewVertically(m_SelectionLabel, self)];
    [self addConstraint:LayoutConstraintForCenteringViewVertically(m_VolumeLabel, self)];
}

- (void)refresh
{
    if( !m_Panel )
        return;

    const data::Statistics &stats = m_Panel.data.Stats();
    auto &fmter = ByteCountFormatter::Instance();

    const auto items_fmt =
        NSLocalizedString(@"%d items", "Explorer status bar, total number of items in the current directory");
    m_ItemsLabel.stringValue = [NSString stringWithFormat:items_fmt, stats.total_entries_amount];

    if( stats.selected_entries_amount > 0 ) {
        const auto size_str = fmter.ToNSString(stats.bytes_in_selected_entries, ByteCountFormatter::Adaptive6);
        const auto sel_fmt = NSLocalizedString(
            @"%d selected (%@)", "Explorer status bar, number and total size of currently selected items");
        m_SelectionLabel.stringValue = [NSString stringWithFormat:sel_fmt, stats.selected_entries_amount, size_str];
    }
    else {
        m_SelectionLabel.stringValue = @"";
    }

    const VFSListingPtr &listing = m_Panel.data.ListingPtr();
    if( listing ) {
        m_VolumeInfoFetcher.SetTarget(listing);
        [self updateVolumeInfo];
    }
}

- (void)updateVolumeInfo
{
    auto &fmter = ByteCountFormatter::Instance();
    const auto avail = fmter.ToNSString(m_VolumeInfoFetcher.Current().avail_bytes, ByteCountFormatter::Adaptive6);
    const auto fmt =
        NSLocalizedString(@"%@ available", "Explorer status bar, free space available on the current volume");
    m_VolumeLabel.stringValue = [NSString stringWithFormat:fmt, avail];
}

- (void)viewDidMoveToWindow
{
    if( self.window )
        m_VolumeInfoFetcher.ResumeUpdates();
    else
        m_VolumeInfoFetcher.PauseUpdates();
}

@end
