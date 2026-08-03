// Copyright (C) 2016-2020 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/VFS.h>

#include <Panel/PanelDataItemVolatileData.h>
#include <Panel/PanelDataStatistics.h>
#include "PanelViewFooterTheme.h"

namespace nc::core {
struct PaneSnapshot;
}

typedef NS_ENUM(NSInteger, NCPanelViewFooterLayoutKind) {
    NCPanelViewFooterLayoutKindNone,
    NCPanelViewFooterLayoutKindDetails,
    NCPanelViewFooterLayoutKindIcons,
    NCPanelViewFooterLayoutKindContent,
};

@interface NCPanelViewFooter : NSView

- (id)initWithFrame:(NSRect)frameRect NS_UNAVAILABLE;

- (id)initWithFrame:(NSRect)frameRect theme:(std::unique_ptr<nc::panel::FooterTheme>)_theme;

- (id)initWithFrame:(NSRect)frameRect
                theme:(std::unique_ptr<nc::panel::FooterTheme>)_theme
    explorerAppearance:(bool)_explorer_appearance;

- (void)updateFocusedItem:(const VFSListingItem &)_item VD:(nc::panel::data::ItemVolatileData)_vd; // may be empty
- (void)updateStatistics:(const nc::panel::data::Statistics &)_stats;
- (void)updateListing:(const VFSListingPtr &)_listing;
// Explorer status ownership belongs to the matching PaneStore snapshot.
- (void)applyExplorerPaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot;
- (void)updateExplorerLayoutKind:(NCPanelViewFooterLayoutKind)_layout_kind;

@property(nonatomic) bool active;
@property(nonatomic, readonly) bool explorerAppearance;
@property(nonatomic, readonly) CGFloat preferredHeight;

@end
