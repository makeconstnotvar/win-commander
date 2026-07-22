// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerToolbarDelegate.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/PanelView.h"
#include "../FilePanels/PanelControllerActionsDispatcher.h"
#include "../MainWindowController.h"
#include "NCExplorerBreadcrumbControl.h"

static auto g_ToolbarIdentifier = @"ExplorerToolbar";

static NSString *const g_BackItem = @"explorer_back";
static NSString *const g_ForwardItem = @"explorer_forward";
static NSString *const g_UpItem = @"explorer_up";
static NSString *const g_RefreshItem = @"explorer_refresh";
static NSString *const g_BreadcrumbItem = @"explorer_breadcrumb";
static NSString *const g_CommanderModeItem = @"explorer_commander_mode";

@implementation NCExplorerToolbarDelegate {
    NSToolbar *m_Toolbar;
    NSButton *m_BackButton;
    NSButton *m_ForwardButton;
    NSButton *m_UpButton;
    NSButton *m_RefreshButton;
    NSButton *m_CommanderModeButton;
    NCExplorerBreadcrumbControl *m_Breadcrumb;
}

@synthesize toolbar = m_Toolbar;

- (instancetype)initWithPanelController:(PanelController *)_panel
{
    self = [super init];
    if( self ) {
        [self buildControlsForPanel:_panel];
        [self buildToolbar];
    }
    return self;
}

- (NSButton *)makeButtonWithSymbol:(NSString *)_symbol_name target:(id)_target action:(SEL)_action
{
    NSButton *button = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 32, 27)];
    button.bezelStyle = NSBezelStyleTexturedRounded;
    button.refusesFirstResponder = true;
    button.title = @"";
    button.image = [NSImage imageWithSystemSymbolName:_symbol_name accessibilityDescription:nil];
    button.target = _target;
    button.action = _action;
    return button;
}

- (void)buildControlsForPanel:(PanelController *)_panel
{
    const id dispatcher = _panel.view.actionsDispatcher;

    m_BackButton = [self makeButtonWithSymbol:@"chevron.left" target:dispatcher action:@selector(OnGoBack:)];
    m_ForwardButton = [self makeButtonWithSymbol:@"chevron.right" target:dispatcher action:@selector(OnGoForward:)];
    m_UpButton = [self makeButtonWithSymbol:@"chevron.up" target:dispatcher action:@selector(OnGoToUpperDirectory:)];
    m_RefreshButton = [self makeButtonWithSymbol:@"arrow.clockwise" target:dispatcher action:@selector(OnRefreshPanel:)];

    // target is nil - this is dispatched up the responder chain to NCMainWindowController.
    m_CommanderModeButton = [self makeButtonWithSymbol:@"rectangle.split.2x1"
                                                 target:nil
                                                 action:@selector(toggleExplorerMode:)];

    m_Breadcrumb = [[NCExplorerBreadcrumbControl alloc] initWithFrame:NSMakeRect(0, 0, 360, 27)
                                                        panelController:_panel];
}

- (void)panelPathChanged
{
    [m_Breadcrumb panelPathChanged];
}

- (void)buildToolbar
{
    m_Toolbar = [[NSToolbar alloc] initWithIdentifier:g_ToolbarIdentifier];
    m_Toolbar.delegate = self;
    m_Toolbar.displayMode = NSToolbarDisplayModeIconOnly;
    m_Toolbar.showsBaselineSeparator = false;
}

- (NSToolbarItem *)toolbar:(NSToolbar *) [[maybe_unused]] _toolbar
        itemForItemIdentifier:(NSString *)itemIdentifier
    willBeInsertedIntoToolbar:(BOOL) [[maybe_unused]] _flag
{
    NSButton *view = nil;
    NSString *label = nil;
    if( [itemIdentifier isEqualToString:g_BackItem] ) {
        view = m_BackButton;
        label = NSLocalizedString(@"Back", "Toolbar palette");
    }
    else if( [itemIdentifier isEqualToString:g_ForwardItem] ) {
        view = m_ForwardButton;
        label = NSLocalizedString(@"Forward", "Toolbar palette");
    }
    else if( [itemIdentifier isEqualToString:g_UpItem] ) {
        view = m_UpButton;
        label = NSLocalizedString(@"Up", "Toolbar palette");
    }
    else if( [itemIdentifier isEqualToString:g_RefreshItem] ) {
        view = m_RefreshButton;
        label = NSLocalizedString(@"Refresh", "Toolbar palette");
    }
    else if( [itemIdentifier isEqualToString:g_CommanderModeItem] ) {
        view = m_CommanderModeButton;
        label = NSLocalizedString(@"Commander Mode", "Toolbar palette");
    }
    else if( [itemIdentifier isEqualToString:g_BreadcrumbItem] ) {
        NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:itemIdentifier];
        item.view = m_Breadcrumb;
        item.paletteLabel = item.label = NSLocalizedString(@"Path", "Toolbar palette");
        item.minSize = NSMakeSize(240, 27);
        item.maxSize = NSMakeSize(900, 27);
        return item;
    }

    if( !view )
        return nil;

    NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:itemIdentifier];
    item.view = view;
    item.paletteLabel = item.label = label;
    return item;
}

- (NSArray<NSString *> *)toolbarDefaultItemIdentifiers:(NSToolbar *) [[maybe_unused]] _toolbar
{
    return @[
        g_BackItem,
        g_ForwardItem,
        g_UpItem,
        g_RefreshItem,
        g_BreadcrumbItem,
        NSToolbarFlexibleSpaceItemIdentifier,
        g_CommanderModeItem
    ];
}

- (NSArray<NSString *> *)toolbarAllowedItemIdentifiers:(NSToolbar *) [[maybe_unused]] _toolbar
{
    return @[
        g_BackItem,
        g_ForwardItem,
        g_UpItem,
        g_RefreshItem,
        g_BreadcrumbItem,
        g_CommanderModeItem,
        NSToolbarFlexibleSpaceItemIdentifier
    ];
}

@end
