// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerState.h"
#include "NCExplorerToolbarDelegate.h"
#include "NCExplorerSidebarView.h"
#include "NCExplorerCommandBarView.h"
#include "NCExplorerStatusBarView.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/PanelView.h"
#include "../FilePanels/PanelControllerActionsDispatcher.h"
#include "../../Bootstrap/AppDelegate.h"
#include "../../Bootstrap/AppDelegate+MainWindowCreation.h"

static const CGFloat g_SidebarWidth = 220.0;
static const CGFloat g_CommandBarHeight = 36.0;
static const CGFloat g_StatusBarHeight = 24.0;

@implementation NCExplorerState {
    PanelController *m_Panel;
    NCExplorerToolbarDelegate *m_ToolbarDelegate;
    NCExplorerSidebarView *m_Sidebar;
    NCExplorerCommandBarView *m_CommandBar;
    NCExplorerStatusBarView *m_StatusBar;
}

@synthesize panelController = m_Panel;

- (instancetype)initWithFrame:(NSRect)frameRect
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        m_Panel = [NCAppDelegate.me allocateExplorerPanelController];
        m_Panel.state = self;
        [m_Panel.view addKeystrokeSink:self];

        [self buildLayout];

        m_ToolbarDelegate = [[NCExplorerToolbarDelegate alloc] initWithPanelController:m_Panel];

        NCPanelControllerActionsDispatcher *const dispatcher = m_Panel.view.actionsDispatcher;
        [dispatcher OnGoToHome:self];
    }
    return self;
}

- (void)buildLayout
{
    m_Sidebar = [[NCExplorerSidebarView alloc] initWithFrame:NSRect() panelController:m_Panel];
    m_CommandBar = [[NCExplorerCommandBarView alloc] initWithFrame:NSRect() panelController:m_Panel];
    m_StatusBar = [[NCExplorerStatusBarView alloc] initWithFrame:NSRect() panelController:m_Panel];

    m_Sidebar.translatesAutoresizingMaskIntoConstraints = false;
    m_CommandBar.translatesAutoresizingMaskIntoConstraints = false;
    m_StatusBar.translatesAutoresizingMaskIntoConstraints = false;
    m_Panel.view.translatesAutoresizingMaskIntoConstraints = false;

    [self addSubview:m_Sidebar];
    [self addSubview:m_CommandBar];
    [self addSubview:m_Panel.view];
    [self addSubview:m_StatusBar];

    [NSLayoutConstraint activateConstraints:@[
        [m_Sidebar.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [m_Sidebar.topAnchor constraintEqualToAnchor:self.topAnchor],
        [m_Sidebar.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        [m_Sidebar.widthAnchor constraintEqualToConstant:g_SidebarWidth],

        [m_CommandBar.leadingAnchor constraintEqualToAnchor:m_Sidebar.trailingAnchor],
        [m_CommandBar.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [m_CommandBar.topAnchor constraintEqualToAnchor:self.topAnchor],
        [m_CommandBar.heightAnchor constraintEqualToConstant:g_CommandBarHeight],

        [m_Panel.view.leadingAnchor constraintEqualToAnchor:m_Sidebar.trailingAnchor],
        [m_Panel.view.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [m_Panel.view.topAnchor constraintEqualToAnchor:m_CommandBar.bottomAnchor],
        [m_Panel.view.bottomAnchor constraintEqualToAnchor:m_StatusBar.topAnchor],

        [m_StatusBar.leadingAnchor constraintEqualToAnchor:m_Sidebar.trailingAnchor],
        [m_StatusBar.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [m_StatusBar.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        [m_StatusBar.heightAnchor constraintEqualToConstant:g_StatusBarHeight],
    ]];
}

#pragma mark - NCMainWindowState

- (NSView *)windowStateContentView
{
    return self;
}

- (NSToolbar *)windowStateToolbar
{
    return m_ToolbarDelegate.toolbar;
}

- (void)windowStateDidBecomeAssigned
{
    [self.window makeFirstResponder:m_Panel.view];
}

- (bool)windowStateNeedsTitle
{
    return true;
}

#pragma mark - NCPanelControllerHostingState

- (FilePanelMainSplitView *)splitView
{
    // Explorer mode has no split view - guarded by anyPanelCollapsed==false at every call site.
    return nil;
}

- (bool)anyPanelCollapsed
{
    return false;
}

- (bool)bothPanelsAreVisible
{
    return false;
}

- (PanelController *)leftPanelController
{
    return m_Panel;
}

- (PanelController *)rightPanelController
{
    return nil;
}

- (bool)isLeftController:(PanelController *) [[maybe_unused]] _controller
{
    return true;
}

- (bool)isRightController:(PanelController *) [[maybe_unused]] _controller
{
    return false;
}

- (void)closeAttachedUI:(PanelController *) [[maybe_unused]] _panel
{
    // No brief-system-overview/quick-look overlay exists in Explorer mode yet.
}

- (void)PanelPathChanged:(PanelController *) [[maybe_unused]] _panel
{
    [m_ToolbarDelegate panelPathChanged];
    [m_StatusBar refresh];
    // NOTE: selection-only changes (no directory change) don't refresh the status bar yet - there
    // is no existing PanelController -> hosting-state notification for selection changes to hook
    // (verified: MainWindowFilePanelState doesn't receive one either, PanelViewFooter is updated
    // from inside PanelView/PanelController directly). A follow-up pass should either extend
    // NCPanelControllerHostingState with a selection-changed callback or observe PanelView itself.
}

- (void)activePanelChangedTo:(PanelController *) [[maybe_unused]] controller
{
}

- (void)ActivatePanelByController:(PanelController *) [[maybe_unused]] controller
{
}

- (BriefSystemOverview *)briefSystemOverviewForPanel:(PanelController *) [[maybe_unused]] _panel
                                                 make:(bool) [[maybe_unused]] _make_if_absent
{
    return nil;
}

- (id<NCPanelPreview>)quickLookForPanel:(PanelController *) [[maybe_unused]] _panel
                                    make:(bool) [[maybe_unused]] _make_if_absent
{
    return nil;
}

- (void)requestTerminalExecution:(const std::string &) [[maybe_unused]] _filename
                               at:(const std::string &) [[maybe_unused]] _cwd
{
    NSBeep();
}

#pragma mark - NCPanelViewKeystrokeSink

- (int)bidForHandlingKeyDown:(NSEvent *) [[maybe_unused]] _event
                 forPanelView:(PanelView *) [[maybe_unused]] _panel_view
{
    return nc::panel::view::BiddingPriority::Skip;
}

- (void)handleKeyDown:(NSEvent *) [[maybe_unused]] _event forPanelView:(PanelView *) [[maybe_unused]] _panel_view
{
}

@end
