// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerState.h"
#include "NCExplorerToolbarDelegate.h"
#include "NCExplorerSidebarView.h"
#include "NCExplorerCommandBarView.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/PanelControllerPaneStoreAdapter.h"
#include "../FilePanels/PanelView.h"
#include "../FilePanels/PanelViewHeader.h"
#include "../FilePanels/PanelControllerActionsDispatcher.h"
#include "../FilePanels/Helpers/Pasteboard.h"
#include "../MainWindowController.h"
#include "../../Bootstrap/AppDelegate.h"
#include "../../Bootstrap/AppDelegate+MainWindowCreation.h"
#include <WinCommander/Core/Commands/CommandRegistry.h>

static const CGFloat g_SidebarWidth = 220.0;
static const CGFloat g_CommandBarHeight = 36.0;

static bool IsFocusAddressShortcut(NSEvent *_event)
{
    NSEventModifierFlags flags = _event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    flags &= ~(NSEventModifierFlagCapsLock | NSEventModifierFlagNumericPad | NSEventModifierFlagFunction);
    return flags == NSEventModifierFlagCommand &&
           (_event.keyCode == 37 || [_event.charactersIgnoringModifiers.lowercaseString isEqualToString:@"l"]);
}

@interface NCExplorerQuickSearchOverlayView : NSVisualEffectView <NCPanelQuickSearchPresentation, NSTextFieldDelegate>
@end

@implementation NCExplorerQuickSearchOverlayView {
    NSTextField *m_SearchField;
    NSTextField *m_MatchesLabel;
    NSButton *m_ClearButton;
    NSString *m_SearchPrompt;
    int m_SearchMatches;
    std::function<void(NSString *)> m_SearchRequestChangeCallback;
    __weak NSResponder *m_DefaultResponder;
}

- (instancetype)initWithFrame:(NSRect)_frame
{
    self = [super initWithFrame:_frame];
    if( self ) {
        self.material = NSVisualEffectMaterialPopover;
        self.blendingMode = NSVisualEffectBlendingModeWithinWindow;
        self.state = NSVisualEffectStateActive;
        self.wantsLayer = true;
        self.layer.cornerRadius = 8.0;
        self.layer.masksToBounds = true;
        self.hidden = true;
        self.accessibilityRole = NSAccessibilityGroupRole;
        self.accessibilityLabel = NSLocalizedString(@"Quick Search", "Explorer accessibility label");

        NSImageView *const icon = [NSImageView
            imageViewWithImage:[NSImage imageWithSystemSymbolName:@"magnifyingglass" accessibilityDescription:nil]];
        icon.contentTintColor = NSColor.secondaryLabelColor;
        m_SearchField = [[NSTextField alloc] initWithFrame:NSZeroRect];
        m_SearchField.bezeled = false;
        m_SearchField.drawsBackground = false;
        m_SearchField.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
        m_SearchField.placeholderString = NSLocalizedString(@"Quick Search", "Explorer quick search");
        m_SearchField.delegate = self;
        m_SearchField.target = self;
        m_SearchField.action = @selector(onSearchAction:);
        m_SearchField.accessibilityLabel = NSLocalizedString(@"Quick Search", "Explorer accessibility label");

        m_MatchesLabel = [NSTextField labelWithString:@""];
        m_MatchesLabel.textColor = NSColor.secondaryLabelColor;
        m_MatchesLabel.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize
                                                               weight:NSFontWeightRegular];
        m_MatchesLabel.alignment = NSTextAlignmentRight;

        m_ClearButton = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"xmark.circle.fill"
                                                                        accessibilityDescription:nil]
                                            target:self
                                            action:@selector(onClear:)];
        m_ClearButton.bordered = false;
        m_ClearButton.bezelStyle = NSBezelStyleInline;
        m_ClearButton.contentTintColor = NSColor.secondaryLabelColor;
        m_ClearButton.accessibilityLabel = NSLocalizedString(@"Clear Quick Search", "Explorer accessibility label");

        for( NSView *view in @[icon, m_SearchField, m_MatchesLabel, m_ClearButton] ) {
            view.translatesAutoresizingMaskIntoConstraints = false;
            [self addSubview:view];
        }
        [m_MatchesLabel setContentHuggingPriority:NSLayoutPriorityRequired
                                   forOrientation:NSLayoutConstraintOrientationHorizontal];
        [NSLayoutConstraint activateConstraints:@[
            [icon.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:9.0],
            [icon.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [icon.widthAnchor constraintEqualToConstant:14.0],
            [icon.heightAnchor constraintEqualToConstant:14.0],
            [m_SearchField.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:6.0],
            [m_SearchField.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [m_SearchField.trailingAnchor constraintEqualToAnchor:m_MatchesLabel.leadingAnchor constant:-6.0],
            [m_MatchesLabel.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [m_MatchesLabel.trailingAnchor constraintEqualToAnchor:m_ClearButton.leadingAnchor constant:-5.0],
            [m_ClearButton.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [m_ClearButton.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-7.0],
            [m_ClearButton.widthAnchor constraintEqualToConstant:16.0],
        ]];
    }
    return self;
}

- (NSString *)searchPrompt
{
    return m_SearchPrompt;
}

- (void)setSearchPrompt:(NSString *)_prompt
{
    m_SearchPrompt = _prompt.length ? [_prompt copy] : nil;
    m_SearchField.stringValue = m_SearchPrompt ? m_SearchPrompt : @"";
    self.hidden = m_SearchPrompt == nil;
}

- (int)searchMatches
{
    return m_SearchMatches;
}

- (void)setSearchMatches:(int)_matches
{
    m_SearchMatches = _matches;
    m_MatchesLabel.stringValue = [NSString stringWithFormat:NSLocalizedString(@"%d matches", "Explorer quick search"),
                                                            _matches];
}

- (std::function<void(NSString *)>)searchRequestChangeCallback
{
    return m_SearchRequestChangeCallback;
}

- (void)setSearchRequestChangeCallback:(std::function<void(NSString *)>)_callback
{
    m_SearchRequestChangeCallback = std::move(_callback);
}

- (NSResponder *)defaultResponder
{
    return m_DefaultResponder;
}

- (void)setDefaultResponder:(NSResponder *)_responder
{
    m_DefaultResponder = _responder;
}

- (void)controlTextDidChange:(NSNotification *)_notification
{
    if( _notification.object != m_SearchField )
        return;
    NSString *const query = m_SearchField.stringValue;
    if( query.length ) {
        if( m_SearchRequestChangeCallback )
            m_SearchRequestChangeCallback(query);
    }
    else {
        [self onClear:m_SearchField];
    }
}

- (void)onSearchAction:(id) [[maybe_unused]] _sender
{
    [self.window makeFirstResponder:m_DefaultResponder];
}

- (void)onClear:(id) [[maybe_unused]] _sender
{
    self.searchPrompt = nil;
    if( m_SearchRequestChangeCallback )
        m_SearchRequestChangeCallback(nil);
    [self.window makeFirstResponder:m_DefaultResponder];
}

- (void)cancelOperation:(id)_sender
{
    if( m_SearchPrompt )
        [self onClear:_sender];
    else
        [super cancelOperation:_sender];
}

@end

@implementation NCExplorerState {
    PanelController *m_Panel;
    NCExplorerToolbarDelegate *m_ToolbarDelegate;
    NCExplorerSidebarView *m_Sidebar;
    NCExplorerCommandBarView *m_CommandBar;
    NCExplorerQuickSearchOverlayView *m_QuickSearchOverlay;
    std::unique_ptr<nc::panel::PanelControllerPaneStoreAdapter> m_PaneStoreBridge;
    nc::core::PaneStoreAdapter::ObservationTicket m_PaneStoreObservation;
}

@synthesize panelController = m_Panel;

- (instancetype)initWithFrame:(NSRect)frameRect operationsPool:(nc::ops::Pool &) [[maybe_unused]] _pool
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        m_Panel = [NCAppDelegate.me allocateExplorerPanelController];
        m_Panel.state = self;
        [m_Panel.view addKeystrokeSink:self];

        m_ToolbarDelegate = [[NCExplorerToolbarDelegate alloc] initWithPanelController:m_Panel];
        [self buildLayout];

        m_PaneStoreBridge = std::make_unique<nc::panel::PanelControllerPaneStoreAdapter>(m_Panel);
        __weak NCExplorerState *weak_self = self;
        m_PaneStoreObservation =
            m_PaneStoreBridge->Store().Observe([weak_self](const nc::core::PaneSnapshot &_snapshot) {
                NCExplorerState *const strong_self = weak_self;
                if( strong_self ) {
                    [strong_self->m_ToolbarDelegate applyPaneSnapshot:_snapshot];
                    [strong_self->m_CommandBar applyPaneSnapshot:_snapshot];
                    [strong_self->m_Panel.view applyExplorerPaneSnapshot:_snapshot];
                }
            });
        const nc::core::PaneSnapshot snapshot = m_PaneStoreBridge->Store().Snapshot();
        [m_ToolbarDelegate applyPaneSnapshot:snapshot];
        [m_CommandBar applyPaneSnapshot:snapshot];
        [m_Panel.view applyExplorerPaneSnapshot:snapshot];

        m_Panel.view.headerBarVisible = false;
        m_Panel.view.busyIndicatorOverride = m_ToolbarDelegate.busyIndicator;
        m_Panel.quickSearchPresentation = m_QuickSearchOverlay;

        NCPanelControllerActionsDispatcher *const dispatcher = m_Panel.view.actionsDispatcher;
        [dispatcher OnGoToHome:self];
    }
    return self;
}

- (void)buildLayout
{
    m_Sidebar = [[NCExplorerSidebarView alloc] initWithFrame:NSRect() panelController:m_Panel];
    NCAppDelegate *const app = NCAppDelegate.me;
    nc::core::CommandRegistry &command_registry = app.commandRegistry;
    m_CommandBar = [[NCExplorerCommandBarView alloc] initWithFrame:NSRect()
                                                    panelController:m_Panel
                                          operationCenterCoordinator:app.operationCenterCoordinator
                                                     commandRegistry:&command_registry];
    m_QuickSearchOverlay = [[NCExplorerQuickSearchOverlayView alloc] initWithFrame:NSZeroRect];

    m_Sidebar.translatesAutoresizingMaskIntoConstraints = false;
    m_CommandBar.translatesAutoresizingMaskIntoConstraints = false;
    m_QuickSearchOverlay.translatesAutoresizingMaskIntoConstraints = false;
    m_Panel.view.translatesAutoresizingMaskIntoConstraints = false;

    [self addSubview:m_Sidebar];
    [self addSubview:m_CommandBar];
    [self addSubview:m_Panel.view];
    [self addSubview:m_QuickSearchOverlay];

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
        [m_Panel.view.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],

        [m_QuickSearchOverlay.topAnchor constraintEqualToAnchor:m_Panel.view.topAnchor constant:9.0],
        [m_QuickSearchOverlay.trailingAnchor constraintEqualToAnchor:m_Panel.view.trailingAnchor constant:-12.0],
        [m_QuickSearchOverlay.widthAnchor constraintEqualToConstant:280.0],
        [m_QuickSearchOverlay.heightAnchor constraintEqualToConstant:32.0],
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

- (BOOL)handleModeSpecificKeyEquivalent:(NSEvent *)_event
{
    if( IsFocusAddressShortcut(_event) ) {
        [self focusAddressFieldShowingToolbarIfNeeded];
        return true;
    }
    return false;
}

- (void)focusAddressFieldShowingToolbarIfNeeded
{
    if( m_ToolbarDelegate.toolbar.visible ) {
        [m_ToolbarDelegate focusAddressField];
        return;
    }

    NCMainWindowController *const controller = static_cast<NCMainWindowController *>(self.window.windowController);
    if( !controller )
        return;

    // OnShowToolbar: is the controller-owned path: it updates both the persisted setting and
    // controller state. Focus on the next run-loop turn, after the toolbar has been reattached.
    [controller OnShowToolbar:self];
    __weak NCExplorerState *weak_self = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      NCExplorerState *const strong_self = weak_self;
      if( strong_self && strong_self->m_ToolbarDelegate.toolbar.visible )
          [strong_self->m_ToolbarDelegate focusAddressField];
    });
}

- (BOOL)performKeyEquivalent:(NSEvent *)_event
{
    if( [self handleModeSpecificKeyEquivalent:_event] )
        return true;
    return [super performKeyEquivalent:_event];
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
    [m_Sidebar panelPathChanged];
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
    if( IsFocusAddressShortcut(_event) )
        return nc::panel::view::BiddingPriority::Max;
    if( _event.keyCode == 53 ) {
        NSPasteboard *const pasteboard = NSPasteboard.generalPasteboard;
        if( nc::panel::PasteboardSupport::CurrentCutToken(pasteboard) &&
            !nc::panel::PasteboardSupport::IsCutInFlight(pasteboard) )
            return nc::panel::view::BiddingPriority::High;
    }
    return nc::panel::view::BiddingPriority::Skip;
}

- (void)handleKeyDown:(NSEvent *)_event forPanelView:(PanelView *) [[maybe_unused]] _panel_view
{
    if( _event.keyCode == 53 )
        nc::panel::PasteboardSupport::CancelCut(NSPasteboard.generalPasteboard);
    else
        [self focusAddressFieldShowingToolbarIfNeeded];
}

@end
