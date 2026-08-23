// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerToolbarDelegate.h"
#include "NCExplorerPanePresentationModel.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/PanelView.h"
#include "../FilePanels/PanelControllerActionsDispatcher.h"
#include "../CommandPresentationAdapter.h"
#include "../MainWindowController.h"
#include "../../Core/Pane/PaneSnapshot.h"
#include "NCExplorerBreadcrumbControl.h"
#include <optional>

static auto g_ToolbarIdentifier = @"ExplorerToolbar";

static NSString *const g_BackItem = @"explorer_back";
static NSString *const g_ForwardItem = @"explorer_forward";
static NSString *const g_UpItem = @"explorer_up";
static NSString *const g_RefreshItem = @"explorer_refresh";
static NSString *const g_BreadcrumbItem = @"explorer_breadcrumb";
static NSString *const g_CommanderModeItem = @"explorer_commander_mode";

static const CGFloat g_ToolbarButtonWidth = 32.0;
static const CGFloat g_ToolbarButtonHeight = 30.0;
static const CGFloat g_AddressBarHeight = 34.0;

namespace {

NSString *ToolbarButtonHelp(NSString *_identifier)
{
    if( [_identifier isEqualToString:@"wincommander.explorer.toolbar.back"] )
        return NSLocalizedString(@"Go to the previous folder", "Explorer toolbar accessibility help");
    if( [_identifier isEqualToString:@"wincommander.explorer.toolbar.forward"] )
        return NSLocalizedString(@"Go to the next folder", "Explorer toolbar accessibility help");
    if( [_identifier isEqualToString:@"wincommander.explorer.toolbar.up"] )
        return NSLocalizedString(@"Go to the enclosing folder", "Explorer toolbar accessibility help");
    if( [_identifier isEqualToString:@"wincommander.explorer.toolbar.refresh"] )
        return NSLocalizedString(@"Reload the current folder", "Explorer toolbar accessibility help");
    if( [_identifier isEqualToString:@"wincommander.explorer.toolbar.commanderMode"] )
        return NSLocalizedString(@"Switch to the dual-pane file manager", "Explorer toolbar accessibility help");
    return nil;
}

void ConfigureToolbarButton(NSButton *_button, NSString *_identifier, NSString *_label)
{
    _button.accessibilityIdentifier = _identifier;
    _button.accessibilityLabel = _label;
    _button.toolTip = ToolbarButtonHelp(_identifier);
    _button.accessibilityHelp = _button.toolTip;
}

void ApplyToolbarCommandState(const nc::core::CommandState &_state, NSButton *_button)
{
    nc::presentation::CommandPresentationAdapter::Apply(_state, _button);
    if( _button.accessibilityHelp.length == 0 ) {
        NSString *const help = ToolbarButtonHelp(_button.accessibilityIdentifier);
        _button.toolTip = help;
        _button.accessibilityHelp = help;
    }
}

} // namespace

@implementation NCExplorerToolbarDelegate {
    NSToolbar *m_Toolbar;
    NSButton *m_BackButton;
    NSButton *m_ForwardButton;
    NSButton *m_UpButton;
    NSButton *m_RefreshButton;
    NSButton *m_CommanderModeButton;
    NCExplorerBreadcrumbControl *m_Breadcrumb;
    __weak PanelController *m_Panel;
    __weak NCPanelControllerActionsDispatcher *m_ActionsDispatcher;
    std::optional<nc::explorer::PanePresentationModel> m_PanePresentation;
}

@synthesize toolbar = m_Toolbar;
@synthesize panelController = m_Panel;

- (NSProgressIndicator *)busyIndicator
{
    return m_Breadcrumb.busyIndicator;
}

- (instancetype)initWithPanelController:(PanelController *)_panel
{
    return [self initWithPanelController:_panel actionsDispatcher:_panel.view.actionsDispatcher];
}

- (instancetype)initWithPanelController:(PanelController *)_panel
                      actionsDispatcher:(NCPanelControllerActionsDispatcher *)_dispatcher
{
    self = [super init];
    if( self ) {
        m_Panel = _panel;
        m_ActionsDispatcher = _dispatcher;
        m_PanePresentation.emplace(_panel.paneId);
        [self buildControlsForPanel:_panel];
        [self buildToolbar];
    }
    return self;
}

- (void)rebindToPanelController:(PanelController *)_panel
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( !_panel || m_Panel == _panel )
        return;

    m_Panel = _panel;
    m_ActionsDispatcher = _panel.view.actionsDispatcher;
    m_PanePresentation.emplace(_panel.paneId);
    for( NSButton *button in @[m_BackButton, m_ForwardButton, m_UpButton, m_RefreshButton] )
        button.target = m_ActionsDispatcher;
    [m_Breadcrumb rebindToPanelController:_panel];

    const auto back_state =
        [m_ActionsDispatcher navigationBackCommandStateForAvailability:std::nullopt
                                                                source:nc::core::CommandInvocationSource::Toolbar];
    const auto forward_state =
        [m_ActionsDispatcher navigationForwardCommandStateForAvailability:std::nullopt
                                                                   source:nc::core::CommandInvocationSource::Toolbar];
    const auto up_state =
        [m_ActionsDispatcher navigationUpCommandStateForAvailability:std::nullopt
                                                              source:nc::core::CommandInvocationSource::Toolbar];
    const auto refresh_state =
        [m_ActionsDispatcher navigationRefreshCommandStateForAvailability:std::nullopt
                                                                   source:nc::core::CommandInvocationSource::Toolbar];
    ApplyToolbarCommandState(back_state, m_BackButton);
    ApplyToolbarCommandState(forward_state, m_ForwardButton);
    ApplyToolbarCommandState(up_state, m_UpButton);
    ApplyToolbarCommandState(refresh_state, m_RefreshButton);
}

- (NSButton *)makeButtonWithSymbol:(NSString *)_symbol_name target:(id)_target action:(SEL)_action
{
    NSButton *button =
        [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, g_ToolbarButtonWidth, g_ToolbarButtonHeight)];
    button.bezelStyle = NSBezelStyleToolbar;
    button.controlSize = NSControlSizeRegular;
    button.refusesFirstResponder = true;
    button.title = @"";
    NSImage *const image = [NSImage imageWithSystemSymbolName:_symbol_name accessibilityDescription:nil];
    button.image = [image imageWithSymbolConfiguration:
                              [NSImageSymbolConfiguration configurationWithPointSize:15.0
                                                                              weight:NSFontWeightRegular]];
    button.imageScaling = NSImageScaleProportionallyDown;
    button.target = _target;
    button.action = _action;
    return button;
}

- (void)buildControlsForPanel:(PanelController *)_panel
{
    const id dispatcher = m_ActionsDispatcher;

    m_BackButton = [self makeButtonWithSymbol:@"chevron.left" target:dispatcher action:@selector(OnGoBack:)];
    m_ForwardButton = [self makeButtonWithSymbol:@"chevron.right" target:dispatcher action:@selector(OnGoForward:)];
    m_UpButton = [self makeButtonWithSymbol:@"chevron.up" target:dispatcher action:@selector(OnGoToUpperDirectory:)];
    m_RefreshButton = [self makeButtonWithSymbol:@"arrow.clockwise"
                                          target:dispatcher
                                          action:@selector(OnRefreshPanel:)];
    ConfigureToolbarButton(m_BackButton,
                           @"wincommander.explorer.toolbar.back",
                           NSLocalizedString(@"Back", "Explorer toolbar accessibility label"));
    ConfigureToolbarButton(m_ForwardButton,
                           @"wincommander.explorer.toolbar.forward",
                           NSLocalizedString(@"Forward", "Explorer toolbar accessibility label"));
    ConfigureToolbarButton(m_UpButton,
                           @"wincommander.explorer.toolbar.up",
                           NSLocalizedString(@"Up", "Explorer toolbar accessibility label"));
    ConfigureToolbarButton(m_RefreshButton,
                           @"wincommander.explorer.toolbar.refresh",
                           NSLocalizedString(@"Refresh", "Explorer toolbar accessibility label"));
    m_BackButton.enabled = false;
    m_ForwardButton.enabled = false;
    m_UpButton.enabled = false;
    m_RefreshButton.enabled = false;
    if( m_ActionsDispatcher ) {
        const auto back_state =
            [m_ActionsDispatcher navigationBackCommandStateForAvailability:std::nullopt
                                                                    source:nc::core::CommandInvocationSource::Toolbar];
        const auto forward_state = [m_ActionsDispatcher
            navigationForwardCommandStateForAvailability:std::nullopt
                                                  source:nc::core::CommandInvocationSource::Toolbar];
        const auto up_state =
            [m_ActionsDispatcher navigationUpCommandStateForAvailability:std::nullopt
                                                                  source:nc::core::CommandInvocationSource::Toolbar];
        const auto refresh_state = [m_ActionsDispatcher
            navigationRefreshCommandStateForAvailability:std::nullopt
                                                  source:nc::core::CommandInvocationSource::Toolbar];
        ApplyToolbarCommandState(back_state, m_BackButton);
        ApplyToolbarCommandState(forward_state, m_ForwardButton);
        ApplyToolbarCommandState(up_state, m_UpButton);
        ApplyToolbarCommandState(refresh_state, m_RefreshButton);
    }

    // target is nil - this is dispatched up the responder chain to NCMainWindowController.
    m_CommanderModeButton = [self makeButtonWithSymbol:@"rectangle.split.2x1"
                                                target:nil
                                                action:@selector(toggleExplorerMode:)];
    ConfigureToolbarButton(m_CommanderModeButton,
                           @"wincommander.explorer.toolbar.commanderMode",
                           NSLocalizedString(@"Commander Mode", "Explorer toolbar accessibility label"));

    m_Breadcrumb = [[NCExplorerBreadcrumbControl alloc]
        initWithFrame:NSMakeRect(0, 0, 600, g_AddressBarHeight)
        panelController:_panel];
    m_Breadcrumb.accessibilityIdentifier = @"wincommander.explorer.toolbar.path";
    m_Breadcrumb.accessibilityHelp =
        NSLocalizedString(@"Shows the current folder and lets you enter a path", "Explorer toolbar accessibility help");
}

- (void)applyPaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot
{
    dispatch_assert_queue(dispatch_get_main_queue());
    const bool matches = m_PanePresentation->Apply(_snapshot);
    const auto history_availability = m_PanePresentation->HistoryAvailability();
    const auto navigation_availability = m_PanePresentation->NavigationAvailability();
    if( m_ActionsDispatcher ) {
        const auto back_state =
            [m_ActionsDispatcher navigationBackCommandStateForAvailability:history_availability
                                                                    source:nc::core::CommandInvocationSource::Toolbar];
        const auto forward_state = [m_ActionsDispatcher
            navigationForwardCommandStateForAvailability:history_availability
                                                  source:nc::core::CommandInvocationSource::Toolbar];
        const auto up_state = [m_ActionsDispatcher
            navigationUpCommandStateForAvailability:navigation_availability ? std::optional{navigation_availability->up}
                                                                            : std::nullopt
                                             source:nc::core::CommandInvocationSource::Toolbar];
        const auto refresh_state = [m_ActionsDispatcher
            navigationRefreshCommandStateForAvailability:navigation_availability
                                                             ? std::optional{navigation_availability->refresh}
                                                             : std::nullopt
                                                  source:nc::core::CommandInvocationSource::Toolbar];
        ApplyToolbarCommandState(back_state, m_BackButton);
        ApplyToolbarCommandState(forward_state, m_ForwardButton);
        ApplyToolbarCommandState(up_state, m_UpButton);
        ApplyToolbarCommandState(refresh_state, m_RefreshButton);
    }
    else {
        m_BackButton.enabled = false;
        m_ForwardButton.enabled = false;
        m_UpButton.enabled = false;
        m_RefreshButton.enabled = false;
    }
    if( !matches )
        return;
    [m_Breadcrumb applyPaneSnapshot:_snapshot];
}

- (void)focusAddressField
{
    [m_Breadcrumb focusAddressField];
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
        item.minSize = NSMakeSize(420, g_AddressBarHeight);
        item.maxSize = NSMakeSize(1100, g_AddressBarHeight);
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
