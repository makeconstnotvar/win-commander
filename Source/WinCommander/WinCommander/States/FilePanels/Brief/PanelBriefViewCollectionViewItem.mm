// Copyright (C) 2016-2024 Michael Kazakov. Subject to GNU General Public License version 3.
#include <WinCommander/Core/Theming/Theme.h>
#include <Panel/UI/PanelViewPresentationItemsColoringFilter.h>
#include "../PanelView.h"
#include "PanelBriefView.h"
#include "PanelBriefViewItemCarrier.h"
#include "PanelBriefViewCollectionViewItem.h"
#include "../Helpers/Pasteboard.h"
#include "../PanelItemAccessibility.h"

using namespace nc::panel;

@interface PanelBriefViewItem ()
- (void)updateAccessibilityPresentation;
@end

@implementation PanelBriefViewItem {
    VFSListingItem m_Item;
    data::ItemVolatileData m_VD;
    bool m_PanelActive;
}

@synthesize panelActive = m_PanelActive;

- (void)prepareForReuse
{
    [super prepareForReuse];
    m_Item = VFSListingItem{};
    m_VD = data::ItemVolatileData{};
    m_PanelActive = false;
    [super setSelected:false];
    self.carrier.backgroundColor = nil;
    self.carrier.tagAccentColor = nil;
    self.carrier.qsHighlight = {};
    self.view.alphaValue = 1.0;
    [self updateAccessibilityPresentation];
}

- (instancetype)initWithNibName:(nullable NSString *) [[maybe_unused]] nibNameOrNil
                         bundle:(nullable NSBundle *) [[maybe_unused]] nibBundleOrNil
{
    self = [super initWithNibName:nil bundle:nil];
    if( self ) {
        m_PanelActive = false;
        const auto rc = NSMakeRect(0, 0, 10, 10);
        PanelBriefViewItemCarrier *v = [[PanelBriefViewItemCarrier alloc] initWithFrame:rc];
        v.controller = self;
        v.accessibilityElement = true;
        v.accessibilityRole = NSAccessibilityRowRole;
        v.accessibilityIdentifier = @"wincommander.explorer.brief.item";
        self.view = v;
        [self updateAccessibilityPresentation];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(pasteboardCutStateDidChange:)
                                                   name:NCPanelPasteboardCutStateDidChangeNotification
                                                 object:NSPasteboard.generalPasteboard];
    }
    return self;
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (PanelBriefViewItemCarrier *)carrier
{
    return static_cast<PanelBriefViewItemCarrier *>(self.view);
}

- (VFSListingItem)item
{
    return m_Item;
}

- (void)setItem:(VFSListingItem)_item
{
    m_Item = _item;
    self.carrier.filename = m_Item.DisplayNameNS();
    self.carrier.isSymlink = m_Item.IsSymlink();
    [self updateCutAppearance];
    [self updateItemLayout];
    [self updateAccessibilityPresentation];
}

- (void)pasteboardCutStateDidChange:(NSNotification *) [[maybe_unused]] _notification
{
    [self updateCutAppearance];
}

- (void)updateCutAppearance
{
    self.view.alphaValue =
        m_Item && PasteboardSupport::IsCutItem(NSPasteboard.generalPasteboard, m_Item.Path()) ? 0.55 : 1.0;
}

- (void)updateItemLayout
{
    if( auto *bv = self.briefView )
        self.carrier.layoutConstants = bv.layoutConstants;
}

- (void)setPanelActive:(bool)_active
{
    if( m_PanelActive == _active )
        return;
    m_PanelActive = _active;

    [self updateBackgroundColor];
    [self updateForegroundColor];
    [self updateAccentColor];
    [self updateAccessibilityPresentation];
}

- (void)setSelected:(BOOL)selected
{
    if( self.selected == selected )
        return;
    [super setSelected:selected];

    [self updateBackgroundColor];
    [self updateForegroundColor];
    [self updateAccentColor];
    [self updateAccessibilityPresentation];
}

- (NSColor *)selectedBackgroundColor
{
    if( self.briefView.explorerAppearance ) {
        if( m_PanelActive )
            return [NSColor.controlAccentColor colorWithAlphaComponent:0.20];
        return NSColor.unemphasizedSelectedContentBackgroundColor;
    }
    if( m_PanelActive )
        return nc::CurrentTheme().FilePanelsBriefFocusedActiveItemBackgroundColor();
    else
        return nc::CurrentTheme().FilePanelsBriefFocusedInactiveItemBackgroundColor();
}

- (NCPanelBriefView *)briefView
{
    return static_cast<NCPanelBriefView *>(self.collectionView.delegate);
}

- (int)itemIndex
{
    if( auto c = self.collectionView )
        if( auto p = [c indexPathForItem:self] )
            return static_cast<int>(p.item);
    return -1;
}

- (int)columnIndex
{
    const auto index = self.itemIndex;
    if( index < 0 )
        return -1;

    const auto items_per_column = self.briefView.itemsInColumn;
    if( items_per_column == 0 )
        return -1;

    return index / items_per_column;
}

- (void)updateForegroundColor
{
    if( !m_Item )
        return;

    if( self.briefView ) {
        // Explorer appearance paints filenames with semantic system colours only. The legacy
        // colouring rules end in a catch-all that matches every item, so consulting them here
        // overwrote the semantic colour on the very next line and painted Commander's marked-item
        // red into an Explorer window. The cursor row stays at full strength so the row the user is
        // on is always legible against its accent fill.
        if( self.briefView.explorerAppearance ) {
            self.carrier.filenameColor =
                (m_Item.IsHidden() && !self.selected) ? NSColor.secondaryLabelColor : NSColor.labelColor;
            return;
        }

        const auto &rules = nc::CurrentTheme().FilePanelsItemsColoringRules();
        const bool focus = self.selected && m_PanelActive;
        for( const auto &i : rules )
            if( i.filter.Filter(m_Item, m_VD) ) {
                self.carrier.filenameColor = focus ? i.focused : i.regular;
                break;
            }
    }
}

- (void)updateBackgroundColor
{
    if( self.selected ) {
        self.carrier.backgroundColor = self.selectedBackgroundColor;
    }
    else {
        if( m_VD.is_selected() ) {
            self.carrier.backgroundColor = self.briefView.explorerAppearance
                                               ? [NSColor.controlAccentColor colorWithAlphaComponent:0.12]
                                               : nc::CurrentTheme().FilePanelsBriefSelectedItemBackgroundColor();
        }
        else {
            self.carrier.backgroundColor = nil;
        }
    }
}

- (void)updateAccentColor
{
    if( !m_Item )
        return;

    if( m_PanelActive && m_Item.HasTags() && (m_VD.is_selected() || self.selected) ) {
        self.carrier.tagAccentColor =
            self.briefView.explorerAppearance ? nil : NSColor.whiteColor; // TODO: Pick from Themes
    }
    else {
        self.carrier.tagAccentColor = nil;
    }
}

- (void)setVD:(data::ItemVolatileData)_vd
{
    if( m_VD == _vd )
        return;
    m_VD = _vd;
    [self updateForegroundColor];
    [self updateBackgroundColor];
    [self updateAccentColor];
    self.carrier.qsHighlight = _vd.highlight;
    self.carrier.highlighted = _vd.is_highlighted();
    [self updateAccessibilityPresentation];
}

- (void)updateAccessibilityPresentation
{
    NSView *const element = self.carrier;
    const bool focused = self.selected && m_PanelActive;
    const bool selected = self.selected || m_VD.is_selected();
    UpdatePanelItemAccessibility(element, m_Item, selected, focused);
}

- (void)setIcon:(NSImage *)_icon
{
    self.carrier.icon = _icon;
}

- (void)setupFieldEditor:(NCPanelViewFieldEditor *)_editor
{
    [self.carrier setupFieldEditor:_editor];
}

@end
