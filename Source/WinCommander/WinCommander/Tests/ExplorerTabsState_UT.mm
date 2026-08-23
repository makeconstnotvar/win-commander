// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/States/Explorer/NCExplorerCommandBarView.h>
#include <WinCommander/States/Explorer/NCExplorerInspectorView.h>
#include <WinCommander/States/Explorer/NCExplorerSearchModeView.h>
#include <WinCommander/States/Explorer/ExplorerSearchController.h>
#include <WinCommander/States/Explorer/NCExplorerSidebarView.h>
#include <WinCommander/States/Explorer/NCExplorerState.h>
#include <WinCommander/States/Explorer/NCExplorerToolbarDelegate.h>
#include <WinCommander/States/FilePanels/PanelController.h>
#include <WinCommander/States/FilePanels/PanelControllerActionsDispatcher.h>
#include <WinCommander/States/FilePanels/PanelView.h>
#include <WinCommander/States/FilePanels/MainWindowFilePanelState.h>
#include <WinCommander/States/MainWindowController.h>
#include <WinCommander/States/MainWindow.h>
#include <WinCommander/States/Terminal/ShellState.h>
#include <WinCommander/Bootstrap/AppDelegate+MainWindowCreation.h>
#include <WinCommander/Core/ServicesHandler.h>

#include <algorithm>

@class NCPanelQLPanelAdaptor;

@interface NCExplorerState (ExplorerTabsStateTesting)
- (instancetype)initForTestingWithFrame:(NSRect)_frame
                        panelController:(PanelController *)_panel
                              panelView:(NSView *)_panel_view
                          inspectorView:(NCExplorerInspectorView *)_inspector
                         QLPanelAdaptor:(NCPanelQLPanelAdaptor *)_ql_panel_adaptor;
- (void)configureTabBindingForTestingWithToolbarDelegate:(NCExplorerToolbarDelegate *)_toolbar
                                                 sidebar:(NCExplorerSidebarView *)_sidebar
                                              commandBar:(NCExplorerCommandBarView *)_command_bar;
- (BOOL)addInactivePanelForTesting:(PanelController *)_panel;
- (void)setFocusedPaneSnapshotForTesting:(const nc::core::PaneSnapshot &)_snapshot;
- (BOOL)attachExplorerTabPanel:(PanelController *)_panel createPaneStore:(BOOL)_create_pane_store;
- (std::vector<nc::core::PaneId>)tabPaneIDsForTesting;
- (BOOL)setSearchControllerForTesting:(ExplorerSearchController *)_controller forPanel:(PanelController *)_panel;
- (void)setSearchModeViewForTesting:(NCExplorerSearchModeView *)_view;
- (IBAction)performClose:(id)_sender;
- (IBAction)onSwitchDualSinglePaneMode:(id)_sender;
- (void)changeFocusedSide;
- (IBAction)ToggleViewHiddenFiles:(id)_sender;
- (BOOL)validateMenuItem:(NSMenuItem *)_item;
- (IBAction)OnSwapPanels:(id)_sender;
- (IBAction)OnFileCopyCommand:(id)_sender;
- (IBAction)OnFileRenameMoveCommand:(id)_sender;
- (PanelController *)dualPaneOppositePanelControllerFor:(PanelController *)_panel;
- (IBAction)OnCompareDirectories:(id)_sender;
- (BOOL)canCompareDualPaneDirectories;
- (IBAction)OnSynchronizeDirectories:(id)_sender;
- (BOOL)canSynchronizeDualPaneDirectories;
@property(nonatomic, readonly) BOOL dualPaneEnabledForTesting;
@property(nonatomic, readonly) PanelController *rightPanelControllerForTesting;
@property(nonatomic, readonly) NSView *rightPanelContainerForTesting;
@property(nonatomic, readonly) double paneDividerRatioForTesting;
@end

@interface NCExplorerSearchModeView (ExplorerTabsStateTesting)
- (NSSearchField *)queryForTesting;
@end

@interface ExplorerTabsTestSearchController : ExplorerSearchController
@property(nonatomic, readonly) NSUInteger synchronizeCount;
@property(nonatomic, readonly) NSUInteger closeCount;
- (void)setTestSnapshot:(std::optional<nc::core::SearchSnapshot>)_snapshot;
@end

@implementation ExplorerTabsTestSearchController {
    std::optional<nc::core::SearchSnapshot> m_TestSnapshot;
    NSUInteger _synchronizeCount;
    NSUInteger _closeCount;
}
@synthesize synchronizeCount = _synchronizeCount;
@synthesize closeCount = _closeCount;
- (void)setTestSnapshot:(std::optional<nc::core::SearchSnapshot>)_snapshot
{
    m_TestSnapshot = std::move(_snapshot);
}
- (std::optional<nc::core::SearchSnapshot>)snapshot
{
    return m_TestSnapshot;
}
- (BOOL)isPresented
{
    return m_TestSnapshot.has_value();
}
- (BOOL)canRevealFocusedResult
{
    return NO;
}
- (void)synchronizeExternalContentChange
{
    ++_synchronizeCount;
}
- (void)close
{
    ++_closeCount;
    m_TestSnapshot.reset();
}
@end

@interface ExplorerTabsTestHiddenFilesDispatcher : NCPanelControllerActionsDispatcher
@property(nonatomic) BOOL validationResult;
@property(nonatomic, readonly) NSUInteger validationCount;
@property(nonatomic, readonly) NSUInteger executionCount;
@property(nonatomic, readonly, weak) NSMenuItem *lastValidatedItem;
@property(nonatomic, readonly, weak) id lastExecutionSender;
@end

@implementation ExplorerTabsTestHiddenFilesDispatcher {
    BOOL _validationResult;
    NSUInteger _validationCount;
    NSUInteger _executionCount;
    __weak NSMenuItem *_lastValidatedItem;
    __weak id _lastExecutionSender;
}
@synthesize validationResult = _validationResult;
- (NSUInteger)validationCount
{
    return _validationCount;
}
- (NSUInteger)executionCount
{
    return _executionCount;
}
- (NSMenuItem *)lastValidatedItem
{
    return _lastValidatedItem;
}
- (id)lastExecutionSender
{
    return _lastExecutionSender;
}
- (BOOL)validateMenuItem:(NSMenuItem *)_item
{
    ++_validationCount;
    _lastValidatedItem = _item;
    return self.validationResult;
}
- (IBAction)ToggleViewHiddenFiles:(id)_sender
{
    ++_executionCount;
    _lastExecutionSender = _sender;
}
@end

@interface ExplorerTabsTestPanelView : NSView
@property(nonatomic, weak) NSProgressIndicator *busyIndicatorOverride;
@property(nonatomic, weak) id<NCPanelViewKeystrokeSink> keystrokeSink;
@property(nonatomic) BOOL headerBarVisible;
@property(nonatomic, strong) NCPanelControllerActionsDispatcher *actionsDispatcher;
@end

@implementation ExplorerTabsTestPanelView {
    __weak NSProgressIndicator *_busyIndicatorOverride;
    __weak id<NCPanelViewKeystrokeSink> _keystrokeSink;
    BOOL _headerBarVisible;
    NCPanelControllerActionsDispatcher *_actionsDispatcher;
}
@synthesize busyIndicatorOverride = _busyIndicatorOverride;
@synthesize keystrokeSink = _keystrokeSink;
@synthesize headerBarVisible = _headerBarVisible;
@synthesize actionsDispatcher = _actionsDispatcher;
- (BOOL)acceptsFirstResponder
{
    return true;
}
- (void)addKeystrokeSink:(id<NCPanelViewKeystrokeSink>)_sink
{
    self.keystrokeSink = _sink;
}
- (void)removeKeystrokeSink:(id<NCPanelViewKeystrokeSink>)_sink
{
    if( self.keystrokeSink == _sink )
        self.keystrokeSink = nil;
}
@end

@interface ExplorerTabsTestOutsideResponder : NSView
@end

@implementation ExplorerTabsTestOutsideResponder
- (BOOL)acceptsFirstResponder
{
    return true;
}
@end

@interface ExplorerTabsTestPanelController : PanelController
@property(nonatomic) BOOL progressiveNavigationPreviewPresented;
@property(nonatomic, readonly) NSUInteger cancelBackgroundOperationsCount;
- (instancetype)initWithPaneID:(nc::core::PaneId)_pane_id view:(ExplorerTabsTestPanelView *)_view;
@end

@implementation ExplorerTabsTestPanelController {
    nc::core::PaneId m_TestPaneID;
    ExplorerTabsTestPanelView *m_TestView;
    BOOL _progressiveNavigationPreviewPresented;
    NSUInteger _cancelBackgroundOperationsCount;
}
@synthesize progressiveNavigationPreviewPresented = _progressiveNavigationPreviewPresented;
- (instancetype)initWithPaneID:(nc::core::PaneId)_pane_id view:(ExplorerTabsTestPanelView *)_view
{
    self = [super init];
    if( self ) {
        m_TestPaneID = _pane_id;
        m_TestView = _view;
    }
    return self;
}
- (nc::core::PaneId)paneId
{
    return m_TestPaneID;
}
- (PanelView *)view
{
    return reinterpret_cast<PanelView *>(m_TestView);
}
- (bool)isPresentingProgressiveNavigationPreview
{
    return self.progressiveNavigationPreviewPresented;
}
- (NSUInteger)cancelBackgroundOperationsCount
{
    return _cancelBackgroundOperationsCount;
}
- (void)CancelBackgroundOperations
{
    ++_cancelBackgroundOperationsCount;
}
@end

@interface ExplorerTabsTestToolbar : NSObject
@property(nonatomic, readonly) NSProgressIndicator *busyIndicator;
@property(nonatomic, readonly) PanelController *panelController;
@property(nonatomic) NSUInteger rebindCount;
// -focusAddressFieldShowingToolbarIfNeeded reads .toolbar.visible before doing anything else; a
// nil toolbar (the default here) makes that a safe no-op and routes it into the harmless
// NCMainWindowController-cast branch instead of a real address-field focus.
@property(nonatomic, readonly) NSToolbar *toolbar;
@end

@implementation ExplorerTabsTestToolbar {
    NSProgressIndicator *m_BusyIndicator;
    __weak PanelController *m_PanelController;
    NSUInteger _rebindCount;
}
@synthesize rebindCount = _rebindCount;
- (instancetype)init
{
    self = [super init];
    if( self )
        m_BusyIndicator = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    return self;
}
- (NSProgressIndicator *)busyIndicator
{
    return m_BusyIndicator;
}
- (NSToolbar *)toolbar
{
    return nil;
}
- (PanelController *)panelController
{
    return m_PanelController;
}
- (void)rebindToPanelController:(PanelController *)_panel
{
    m_PanelController = _panel;
    ++self.rebindCount;
}
@end

@interface ExplorerTabsTestSidebar : NSView
@property(nonatomic, readonly) PanelController *panelController;
@end

@implementation ExplorerTabsTestSidebar {
    __weak PanelController *m_PanelController;
}
- (PanelController *)panelController
{
    return m_PanelController;
}
- (void)rebindToPanelController:(PanelController *)_panel
{
    m_PanelController = _panel;
}
- (void)panelPathChanged
{
}
@end

@interface ExplorerTabsTestCommandBar : NSView
@property(nonatomic, readonly) PanelController *panelController;
@end

@implementation ExplorerTabsTestCommandBar {
    __weak PanelController *m_PanelController;
}
- (PanelController *)panelController
{
    return m_PanelController;
}
- (void)rebindToPanelController:(PanelController *)_panel
{
    m_PanelController = _panel;
}
@end

@interface ExplorerTabsTestInspector : NSView
@property(nonatomic, readonly) nc::core::PaneId paneID;
@end

@implementation ExplorerTabsTestInspector {
    nc::core::PaneId m_PaneID;
}
- (nc::core::PaneId)paneID
{
    return m_PaneID;
}
- (BOOL)rebindToPaneID:(nc::core::PaneId)_pane_id
{
    if( _pane_id.value == 0 )
        return false;
    m_PaneID = _pane_id;
    return true;
}
- (void)clearPreview
{
}
@end

@interface ExplorerTabsTestState : NCExplorerState
@property(nonatomic) NSUInteger closeAttachedUICount;
@property(nonatomic, weak) PanelController *sessionAttachmentFailurePanel;
@property(nonatomic, weak) PanelController *dualPanePanelForTesting;
- (void)setSessionRestorePanelsForTesting:(NSArray<PanelController *> *)_panels;
- (NSArray<PanelController *> *)restoredSessionPanelsForTesting;
- (NSArray<PanelController *> *)loadedHomePanelsForTesting;
@end

@implementation ExplorerTabsTestState {
    NSUInteger _closeAttachedUICount;
    __weak PanelController *_sessionAttachmentFailurePanel;
    __weak PanelController *_dualPanePanelForTesting;
    NSMutableArray<PanelController *> *m_SessionRestorePanelQueue;
    NSMutableArray<PanelController *> *m_RestoredSessionPanels;
    NSMutableArray<PanelController *> *m_LoadedHomePanels;
}
@synthesize closeAttachedUICount = _closeAttachedUICount;
@synthesize sessionAttachmentFailurePanel = _sessionAttachmentFailurePanel;
@synthesize dualPanePanelForTesting = _dualPanePanelForTesting;
- (instancetype)initForTestingWithFrame:(NSRect)_frame
                        panelController:(PanelController *)_panel
                              panelView:(NSView *)_panel_view
                          inspectorView:(NCExplorerInspectorView *)_inspector
                         QLPanelAdaptor:(NCPanelQLPanelAdaptor *)_ql_panel_adaptor
{
    self = [super initForTestingWithFrame:_frame
                          panelController:_panel
                                panelView:_panel_view
                            inspectorView:_inspector
                           QLPanelAdaptor:_ql_panel_adaptor];
    if( self )
        m_LoadedHomePanels = [NSMutableArray new];
    return self;
}
- (BOOL)attachExplorerTabPanel:(PanelController *)_panel createPaneStore:(BOOL)_create_pane_store
{
    if( _panel == self.sessionAttachmentFailurePanel )
        return false;
    return [super attachExplorerTabPanel:_panel createPaneStore:_create_pane_store];
}
- (void)setSessionRestorePanelsForTesting:(NSArray<PanelController *> *)_panels
{
    m_SessionRestorePanelQueue = [_panels mutableCopy];
    m_RestoredSessionPanels = [NSMutableArray new];
}
- (NSArray<PanelController *> *)restoredSessionPanelsForTesting
{
    return m_RestoredSessionPanels;
}
- (NSArray<PanelController *> *)loadedHomePanelsForTesting
{
    return m_LoadedHomePanels;
}
- (PanelController *)allocateExplorerPanelForSessionRestore
{
    if( m_SessionRestorePanelQueue.count == 0 )
        return nil;
    PanelController *const panel = m_SessionRestorePanelQueue.firstObject;
    [m_SessionRestorePanelQueue removeObjectAtIndex:0];
    return panel;
}
- (PanelController *)allocateExplorerPanelForDualPane
{
    return self.dualPanePanelForTesting;
}
- (BOOL)dualPaneCreatesPaneStore
{
    // The mock PanelController/PanelView pair does not support PanelControllerPaneStoreAdapter's
    // real lifecycle/context observation (e.g. PanelView.item, PanelController.data), so exercising
    // it here would crash rather than exercise this slice's actual logic.
    return false;
}
- (NSView *)dualPaneRightSideTestingContentViewForPanel:(PanelController *)_panel
{
    // A real FilePanelsTabbedHolder needs an app-wide bootstrapped ThemesManager (asserts
    // g_CurrentTheme != nullptr) that this test binary does not set up; route through the same
    // bare-panel-view container the Left side already uses via -initForTestingWithFrame:....
    return _panel.view;
}
- (void)restoreSessionLocation:(const std::optional<nc::panel::PersistentLocation> &) [[maybe_unused]] _location
                       forPanel:(PanelController *)_panel
{
    [m_RestoredSessionPanels addObject:_panel];
}
- (void)loadNativeHomeForSessionPanel:(PanelController *)_panel
{
    // The base implementation submits a real native directory load, which this fixture's mock
    // PanelController does not override - recording the call instead keeps dual-pane tests
    // deterministic and independent of the real filesystem.
    if( _panel )
        [m_LoadedHomePanels addObject:_panel];
}
- (void)closeAttachedUI:(PanelController *) [[maybe_unused]] _panel
{
    ++self.closeAttachedUICount;
}
@end

@interface ExplorerTabsTestWindow : NSWindow
@property(nonatomic) NSUInteger performCloseCount;
@end

@implementation ExplorerTabsTestWindow {
    NSUInteger _performCloseCount;
}
@synthesize performCloseCount = _performCloseCount;
- (void)performClose:(id) [[maybe_unused]] _sender
{
    ++self.performCloseCount;
}
@end

namespace {

struct Fixture {
    Fixture()
    {
        first_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSMakeRect(0, 0, 640, 480)];
        second_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSMakeRect(0, 0, 640, 480)];
        first = [[ExplorerTabsTestPanelController alloc] initWithPaneID:nc::core::PaneId{501} view:first_view];
        second = [[ExplorerTabsTestPanelController alloc] initWithPaneID:nc::core::PaneId{502} view:second_view];
        inspector = [[ExplorerTabsTestInspector alloc] initWithFrame:NSMakeRect(0, 0, 320, 480)];
        state = [[ExplorerTabsTestState alloc]
            initForTestingWithFrame:NSMakeRect(0, 0, 960, 480)
                    panelController:first
                          panelView:first_view
                      inspectorView:reinterpret_cast<NCExplorerInspectorView *>(inspector)
                     QLPanelAdaptor:nil];
        toolbar = [ExplorerTabsTestToolbar new];
        sidebar = [ExplorerTabsTestSidebar new];
        command_bar = [ExplorerTabsTestCommandBar new];
        [state
            configureTabBindingForTestingWithToolbarDelegate:reinterpret_cast<NCExplorerToolbarDelegate *>(toolbar)
                                                     sidebar:reinterpret_cast<NCExplorerSidebarView *>(sidebar)
                                                  commandBar:reinterpret_cast<NCExplorerCommandBarView *>(command_bar)];
    }

    __strong ExplorerTabsTestPanelView *first_view;
    __strong ExplorerTabsTestPanelView *second_view;
    __strong ExplorerTabsTestPanelController *first;
    __strong ExplorerTabsTestPanelController *second;
    __strong ExplorerTabsTestInspector *inspector;
    __strong ExplorerTabsTestState *state;
    __strong ExplorerTabsTestToolbar *toolbar;
    __strong ExplorerTabsTestSidebar *sidebar;
    __strong ExplorerTabsTestCommandBar *command_bar;
};

} // namespace

#define PREFIX "NCExplorerState tabs "

TEST_CASE(PREFIX "single-tab Cmd-W action uses the ordinary window close path")
{
    Fixture fixture;
    ExplorerTabsTestWindow *const window =
        [[ExplorerTabsTestWindow alloc] initWithContentRect:NSMakeRect(0, 0, 960, 480)
                                                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                    backing:NSBackingStoreBuffered
                                                      defer:false];
    window.contentView = fixture.state;

    [fixture.state performClose:nil];

    CHECK(window.performCloseCount == 1);
    CHECK(fixture.state.panelController == fixture.first);
}

TEST_CASE(PREFIX "Escape explicitly cancels progressive navigation preview")
{
    Fixture fixture;
    fixture.first.progressiveNavigationPreviewPresented = YES;
    NSEvent *const escape = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                             location:NSZeroPoint
                                        modifierFlags:0
                                            timestamp:0
                                         windowNumber:0
                                              context:nil
                                           characters:@"\x1b"
                          charactersIgnoringModifiers:@"\x1b"
                                            isARepeat:NO
                                              keyCode:53];

    CHECK([fixture.state bidForHandlingKeyDown:escape forPanelView:fixture.first.view] ==
          nc::panel::view::BiddingPriority::High);
    [fixture.state handleKeyDown:escape forPanelView:fixture.first.view];

    CHECK(fixture.first.cancelBackgroundOperationsCount == 1);
    CHECK(fixture.second.cancelBackgroundOperationsCount == 0);
}

TEST_CASE(PREFIX "routes hidden-files menu validation and action outside PanelView focus")
{
    Fixture fixture;
    ExplorerTabsTestHiddenFilesDispatcher *const dispatcher = [ExplorerTabsTestHiddenFilesDispatcher new];
    dispatcher.validationResult = YES;
    fixture.first_view.actionsDispatcher = dispatcher;

    ExplorerTabsTestWindow *const window =
        [[ExplorerTabsTestWindow alloc] initWithContentRect:NSMakeRect(0, 0, 960, 480)
                                                  styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered
                                                      defer:false];
    window.contentView = fixture.state;
    ExplorerTabsTestOutsideResponder *const outside =
        [[ExplorerTabsTestOutsideResponder alloc] initWithFrame:NSMakeRect(0, 0, 20, 20)];
    [fixture.state addSubview:outside];
    REQUIRE([window makeFirstResponder:outside]);
    REQUIRE(window.firstResponder == outside);
    REQUIRE(window.firstResponder != fixture.first_view);

    NSMenuItem *const item = [[NSMenuItem alloc] initWithTitle:@"Show Hidden Files"
                                                       action:@selector(ToggleViewHiddenFiles:)
                                                keyEquivalent:@""];
    CHECK([fixture.state validateMenuItem:item]);
    CHECK(dispatcher.validationCount == 1);
    CHECK(dispatcher.lastValidatedItem == item);
    CHECK(dispatcher.executionCount == 0);

    NSObject *const sender = [NSObject new];
    CHECK([window.firstResponder tryToPerform:item.action with:sender]);
    CHECK(dispatcher.executionCount == 1);
    CHECK(dispatcher.lastExecutionSender == sender);
    CHECK(dispatcher.validationCount == 1);
}

TEST_CASE(PREFIX "switch atomically rebinds active chrome and retires pane-local UI")
{
    Fixture fixture;
    ExplorerTabsTestWindow *const window =
        [[ExplorerTabsTestWindow alloc] initWithContentRect:NSMakeRect(0, 0, 960, 480)
                                                  styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered
                                                      defer:false];
    window.contentView = fixture.state;

    REQUIRE([fixture.state addInactivePanelForTesting:fixture.second]);
    REQUIRE(fixture.first_view.busyIndicatorOverride == fixture.toolbar.busyIndicator);
    REQUIRE(fixture.first.quickSearchPresentation != nil);
    const NSUInteger initial_rebind_count = fixture.toolbar.rebindCount;

    [fixture.state ActivatePanelByController:fixture.second];

    CHECK(fixture.state.panelController == fixture.second);
    CHECK(fixture.first_view.busyIndicatorOverride == nil);
    CHECK(fixture.second_view.busyIndicatorOverride == fixture.toolbar.busyIndicator);
    CHECK(fixture.first.quickSearchPresentation == nil);
    CHECK(fixture.second.quickSearchPresentation != nil);
    CHECK(fixture.toolbar.panelController == fixture.second);
    CHECK(fixture.sidebar.panelController == fixture.second);
    CHECK(fixture.command_bar.panelController == fixture.second);
    CHECK(fixture.inspector.paneID == fixture.second.paneId);
    CHECK(fixture.state.closeAttachedUICount == 1);
    CHECK(window.firstResponder == fixture.second_view);
    CHECK(fixture.first_view.hidden);
    CHECK_FALSE(fixture.second_view.hidden);

    [fixture.state ActivatePanelByController:fixture.second];
    CHECK(fixture.toolbar.rebindCount == initial_rebind_count + 1);
    CHECK(fixture.state.closeAttachedUICount == 1);
}

TEST_CASE(PREFIX "rejects a foreign controller even when it reuses an owned PaneId")
{
    Fixture fixture;
    ExplorerTabsTestPanelView *const foreign_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTabsTestPanelController *const foreign =
        [[ExplorerTabsTestPanelController alloc] initWithPaneID:fixture.first.paneId view:foreign_view];
    const NSUInteger rebind_count = fixture.toolbar.rebindCount;

    CHECK_FALSE([fixture.state isLeftController:foreign]);
    [fixture.state ActivatePanelByController:foreign];
    CHECK(fixture.state.panelController == fixture.first);
    CHECK(fixture.toolbar.rebindCount == rebind_count);
}

TEST_CASE(PREFIX "restores ordered fresh tab identities and the requested active index")
{
    Fixture fixture;
    ExplorerTabsTestPanelView *const third_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTabsTestPanelController *const third =
        [[ExplorerTabsTestPanelController alloc] initWithPaneID:nc::core::PaneId{503} view:third_view];
    [fixture.state setSessionRestorePanelsForTesting:@[ fixture.second, third ]];

    nc::explorer::ExplorerPanesSession session;
    session.left.tabs.resize(3);
    session.left.active_index = 1;
    REQUIRE([fixture.state restorePanesFromSession:session]);

    CHECK(fixture.state.panelController == fixture.second);
    CHECK([[fixture.state restoredSessionPanelsForTesting]
        isEqualToArray:@[ fixture.first, fixture.second, third ]]);
    CHECK([fixture.state tabPaneIDsForTesting] ==
          std::vector<nc::core::PaneId>{{501}, {502}, {503}});
    const nc::explorer::ExplorerPanesSession captured = [fixture.state capturePanesSession];
    CHECK(captured.left.tabs.size() == 3);
    CHECK(captured.left.active_index == 1);
    CHECK(std::ranges::none_of(captured.left.tabs, [](const auto &_tab) { return _tab.location.has_value(); }));
    CHECK_FALSE(captured.right);
    CHECK(fixture.first.state == fixture.state);
    CHECK(fixture.second.state == fixture.state);
    CHECK(third.state == fixture.state);
    CHECK(fixture.first_view.keystrokeSink == fixture.state);
    CHECK(fixture.second_view.keystrokeSink == fixture.state);
    CHECK(third_view.keystrokeSink == fixture.state);
    CHECK(fixture.first_view.hidden);
    CHECK_FALSE(fixture.second_view.hidden);
    CHECK(third_view.hidden);
    CHECK_FALSE([fixture.state restorePanesFromSession:session]);
}

TEST_CASE(PREFIX "rejects duplicate restored pane identity before changing tab topology")
{
    Fixture fixture;
    ExplorerTabsTestPanelView *const duplicate_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTabsTestPanelController *const duplicate =
        [[ExplorerTabsTestPanelController alloc] initWithPaneID:fixture.first.paneId view:duplicate_view];
    [fixture.state setSessionRestorePanelsForTesting:@[ duplicate ]];

    nc::explorer::ExplorerPanesSession session;
    session.left.tabs.resize(2);
    session.left.active_index = 1;
    CHECK_FALSE([fixture.state restorePanesFromSession:session]);
    CHECK(fixture.state.panelController == fixture.first);
    CHECK([fixture.state tabPaneIDsForTesting] == std::vector<nc::core::PaneId>{{501}});
    CHECK([fixture.state restoredSessionPanelsForTesting].count == 0);
}

TEST_CASE(PREFIX "rolls back a partially attached restored topology")
{
    Fixture fixture;
    ExplorerTabsTestPanelView *const third_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTabsTestPanelController *const third =
        [[ExplorerTabsTestPanelController alloc] initWithPaneID:nc::core::PaneId{503} view:third_view];
    fixture.state.sessionAttachmentFailurePanel = third;
    [fixture.state setSessionRestorePanelsForTesting:@[ fixture.second, third ]];

    nc::explorer::ExplorerPanesSession session;
    session.left.tabs.resize(3);
    session.left.active_index = 2;
    CHECK_FALSE([fixture.state restorePanesFromSession:session]);

    CHECK(fixture.state.panelController == fixture.first);
    CHECK([fixture.state tabPaneIDsForTesting] == std::vector<nc::core::PaneId>{{501}});
    CHECK(fixture.second.state == nil);
    CHECK(fixture.second_view.keystrokeSink == nil);
    CHECK(fixture.second_view.superview == nil);
    CHECK(third.state == nil);
    CHECK(third_view.keystrokeSink == nil);
    CHECK([fixture.state restoredSessionPanelsForTesting].count == 0);
    const nc::explorer::ExplorerPanesSession captured_after_failure = [fixture.state capturePanesSession];
    CHECK(captured_after_failure.left.tabs.size() == 1);
    CHECK(captured_after_failure.left.active_index == 0);

    ExplorerTabsTestPanelView *const replacement_second_view =
        [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTabsTestPanelView *const replacement_third_view =
        [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTabsTestPanelController *const replacement_second = [[ExplorerTabsTestPanelController alloc]
        initWithPaneID:nc::core::PaneId{504}
                  view:replacement_second_view];
    ExplorerTabsTestPanelController *const replacement_third = [[ExplorerTabsTestPanelController alloc]
        initWithPaneID:nc::core::PaneId{505}
                  view:replacement_third_view];
    fixture.state.sessionAttachmentFailurePanel = nil;
    [fixture.state setSessionRestorePanelsForTesting:@[ replacement_second, replacement_third ]];

    REQUIRE([fixture.state restorePanesFromSession:session]);
    CHECK([fixture.state tabPaneIDsForTesting] ==
          std::vector<nc::core::PaneId>{{501}, {504}, {505}});
    CHECK(fixture.state.panelController == replacement_third);
}

TEST_CASE(PREFIX "caps runtime attachment at the persistable session limit")
{
    Fixture fixture;
    REQUIRE([fixture.state addInactivePanelForTesting:fixture.second]);
    for( uint64_t value = 503; value <= 564; ++value ) {
        ExplorerTabsTestPanelView *const view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
        ExplorerTabsTestPanelController *const panel =
            [[ExplorerTabsTestPanelController alloc] initWithPaneID:nc::core::PaneId{value} view:view];
        REQUIRE([fixture.state addInactivePanelForTesting:panel]);
    }
    REQUIRE([fixture.state tabPaneIDsForTesting].size() ==
            nc::explorer::ExplorerSessionPersistency::MaximumTabs);

    ExplorerTabsTestPanelView *const overflow_view =
        [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTabsTestPanelController *const overflow =
        [[ExplorerTabsTestPanelController alloc] initWithPaneID:nc::core::PaneId{565} view:overflow_view];
    CHECK_FALSE([fixture.state addInactivePanelForTesting:overflow]);
    CHECK([fixture.state tabPaneIDsForTesting].size() ==
          nc::explorer::ExplorerSessionPersistency::MaximumTabs);
    CHECK(overflow.state == nil);
    CHECK(overflow_view.keystrokeSink == nil);
}

TEST_CASE(PREFIX "Search Mode snapshot follows its exact active tab")
{
    Fixture fixture;
    NCExplorerSearchModeView *const search_view = [[NCExplorerSearchModeView alloc] initWithFrame:NSZeroRect];
    [fixture.state setSearchModeViewForTesting:search_view];

    ExplorerTabsTestSearchController *const first_search = [ExplorerTabsTestSearchController new];
    nc::core::SearchSnapshot first_snapshot;
    first_snapshot.pane_id = fixture.first.paneId;
    first_snapshot.request = nc::core::SearchRequest{.query = "first"};
    [first_search setTestSnapshot:first_snapshot];
    REQUIRE([fixture.state setSearchControllerForTesting:first_search forPanel:fixture.first]);
    CHECK([search_view queryForTesting].stringValue.UTF8String == std::string_view{"first"});
    CHECK(fixture.first.quickSearchPresentation == nil);

    REQUIRE([fixture.state addInactivePanelForTesting:fixture.second]);
    ExplorerTabsTestSearchController *const second_search = [ExplorerTabsTestSearchController new];
    nc::core::SearchSnapshot second_snapshot;
    second_snapshot.pane_id = fixture.second.paneId;
    second_snapshot.request = nc::core::SearchRequest{.query = "second"};
    [second_search setTestSnapshot:second_snapshot];
    REQUIRE([fixture.state setSearchControllerForTesting:second_search forPanel:fixture.second]);

    [fixture.state ActivatePanelByController:fixture.second];
    CHECK([search_view queryForTesting].stringValue.UTF8String == std::string_view{"second"});
    CHECK(fixture.second.quickSearchPresentation == nil);

    [fixture.state ActivatePanelByController:fixture.first];
    CHECK([search_view queryForTesting].stringValue.UTF8String == std::string_view{"first"});
    CHECK(first_search.synchronizeCount == 0);
    CHECK(second_search.synchronizeCount == 0);
}

TEST_CASE(PREFIX "reuses presented Search Mode after results replace the uniform origin")
{
    Fixture fixture;
    NCExplorerSearchModeView *const search_view = [[NCExplorerSearchModeView alloc] initWithFrame:NSZeroRect];
    [fixture.state setSearchModeViewForTesting:search_view];
    ExplorerTabsTestSearchController *const search = [ExplorerTabsTestSearchController new];
    nc::core::SearchSnapshot snapshot;
    snapshot.pane_id = fixture.first.paneId;
    snapshot.phase = nc::core::SearchPhase::Completed;
    snapshot.request = nc::core::SearchRequest{.query = "retained"};
    [search setTestSnapshot:snapshot];
    REQUIRE([fixture.state setSearchControllerForTesting:search forPanel:fixture.first]);

    CHECK([fixture.state canPresentSearchForPanel:fixture.first]);
    CHECK([fixture.state presentSearchForPanel:fixture.first
                                  initialQuery:@"replacement"
                                preferredScope:NCExplorerSearchPreferredScopeCurrentFolder]);
    CHECK([search_view queryForTesting].stringValue.UTF8String == std::string_view{"retained"});
    CHECK(fixture.first.quickSearchPresentation == nil);

    [fixture.state PanelPathChanged:fixture.first];
    CHECK(search.synchronizeCount == 1);
    CHECK_FALSE([fixture.state canPresentSearchForPanel:fixture.second]);
}

TEST_CASE(PREFIX "dual pane toggle creates an independent right side and tears it down cleanly")
{
    Fixture fixture;
    fixture.state.dualPanePanelForTesting = fixture.second;
    CHECK_FALSE(fixture.state.dualPaneEnabledForTesting);
    CHECK(fixture.state.rightPanelControllerForTesting == nil);

    [fixture.state onSwitchDualSinglePaneMode:nil];

    REQUIRE(fixture.state.dualPaneEnabledForTesting);
    REQUIRE(fixture.state.rightPanelControllerForTesting == fixture.second);
    CHECK(fixture.state.rightPanelControllerForTesting.paneId != fixture.first.paneId);
    REQUIRE(fixture.state.rightPanelContainerForTesting != nil);
    CHECK(fixture.state.rightPanelContainerForTesting.superview != nil);
    CHECK([[fixture.state loadedHomePanelsForTesting] containsObject:fixture.second]);
    // Turning dual pane on does not steal focus/chrome from the already-focused Left side.
    CHECK(fixture.state.panelController == fixture.first);
    CHECK([fixture.state isLeftController:fixture.first]);
    CHECK([fixture.state isRightController:fixture.second]);
    CHECK_FALSE([fixture.state isRightController:fixture.first]);
    CHECK_FALSE([fixture.state isLeftController:fixture.second]);
    CHECK(fixture.state.bothPanelsAreVisible);

    [fixture.state onSwitchDualSinglePaneMode:nil];

    CHECK_FALSE(fixture.state.dualPaneEnabledForTesting);
    CHECK(fixture.state.rightPanelControllerForTesting == nil);
    CHECK(fixture.second.state == nil);
    CHECK(fixture.state.panelController == fixture.first);
    CHECK_FALSE(fixture.state.bothPanelsAreVisible);
}

TEST_CASE(PREFIX "restores a dual-pane session into two independent sides with its focus and divider")
{
    Fixture fixture;
    ExplorerTabsTestPanelView *const left_extra_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTabsTestPanelView *const right_extra_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTabsTestPanelController *const left_extra =
        [[ExplorerTabsTestPanelController alloc] initWithPaneID:nc::core::PaneId{503} view:left_extra_view];
    ExplorerTabsTestPanelController *const right_extra =
        [[ExplorerTabsTestPanelController alloc] initWithPaneID:nc::core::PaneId{504} view:right_extra_view];
    // The left side's extra tab is allocated first, then the right side's initial panel comes from
    // the dual-pane seam, then the right side's own extra tab.
    fixture.state.dualPanePanelForTesting = fixture.second;
    [fixture.state setSessionRestorePanelsForTesting:@[ left_extra, right_extra ]];

    nc::explorer::ExplorerPanesSession session;
    session.left.tabs.resize(2);
    session.left.active_index = 1;
    session.right = nc::explorer::ExplorerTabsSession{};
    session.right->tabs.resize(2);
    session.right->active_index = 0;
    session.right_focused = true;
    session.divider_ratio = 0.35;

    REQUIRE([fixture.state restorePanesFromSession:session]);

    REQUIRE(fixture.state.dualPaneEnabledForTesting);
    CHECK([fixture.state tabPaneIDsForTesting] == std::vector<nc::core::PaneId>{{502}, {504}});
    CHECK(fixture.state.rightPanelControllerForTesting == fixture.second);
    CHECK(fixture.state.panelController == fixture.second);
    CHECK([fixture.state isLeftController:left_extra]);
    CHECK([fixture.state isRightController:fixture.second]);
    CHECK(fixture.state.paneDividerRatioForTesting == 0.35);
    CHECK([[fixture.state restoredSessionPanelsForTesting]
        isEqualToArray:@[ fixture.first, left_extra, fixture.second, right_extra ]]);

    const nc::explorer::ExplorerPanesSession captured = [fixture.state capturePanesSession];
    CHECK(captured.left.tabs.size() == 2);
    CHECK(captured.left.active_index == 1);
    REQUIRE(captured.right);
    CHECK(captured.right->tabs.size() == 2);
    CHECK(captured.right->active_index == 0);
    CHECK(captured.right_focused);
    REQUIRE(captured.divider_ratio);
    CHECK(*captured.divider_ratio == 0.35);

    // The one-shot guard covers the whole layout, not just the side it started with.
    CHECK_FALSE([fixture.state restorePanesFromSession:session]);
}

TEST_CASE(PREFIX "a single-pane session leaves dual pane off and captures no right side")
{
    Fixture fixture;
    fixture.state.dualPanePanelForTesting = fixture.second;

    nc::explorer::ExplorerPanesSession session;
    session.left.tabs.resize(1);
    // Dual-pane hints without a right side must not turn the layout on by themselves.
    session.right_focused = true;
    session.divider_ratio = 0.2;

    REQUIRE([fixture.state restorePanesFromSession:session]);

    CHECK_FALSE(fixture.state.dualPaneEnabledForTesting);
    CHECK(fixture.state.rightPanelControllerForTesting == nil);
    CHECK(fixture.state.panelController == fixture.first);
    CHECK(fixture.state.paneDividerRatioForTesting == 0.5);

    const nc::explorer::ExplorerPanesSession captured = [fixture.state capturePanesSession];
    CHECK(captured.left.tabs.size() == 1);
    CHECK_FALSE(captured.right);
    CHECK_FALSE(captured.right_focused);
    CHECK_FALSE(captured.divider_ratio);
}

TEST_CASE(PREFIX "a right side that cannot be rebuilt degrades the restored window to single pane")
{
    Fixture fixture;

    SECTION("no panel available for the right side")
    {
        fixture.state.dualPanePanelForTesting = nil;
        nc::explorer::ExplorerPanesSession session;
        session.left.tabs.resize(1);
        session.right = nc::explorer::ExplorerTabsSession{};
        session.right->tabs.resize(1);

        REQUIRE([fixture.state restorePanesFromSession:session]);

        CHECK_FALSE(fixture.state.dualPaneEnabledForTesting);
        CHECK(fixture.state.panelController == fixture.first);
        CHECK([fixture.state tabPaneIDsForTesting] == std::vector<nc::core::PaneId>{{501}});
    }
    SECTION("the right side's own extra tab fails to attach")
    {
        ExplorerTabsTestPanelView *const right_extra_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
        ExplorerTabsTestPanelController *const right_extra =
            [[ExplorerTabsTestPanelController alloc] initWithPaneID:nc::core::PaneId{504} view:right_extra_view];
        fixture.state.dualPanePanelForTesting = fixture.second;
        // The right side names its content explicitly rather than going through the focused-side
        // entry point, so a duplicate identity is what fails its attach here.
        [fixture.state setSessionRestorePanelsForTesting:@[ fixture.second ]];

        nc::explorer::ExplorerPanesSession session;
        session.left.tabs.resize(1);
        session.right = nc::explorer::ExplorerTabsSession{};
        session.right->tabs.resize(2);

        REQUIRE([fixture.state restorePanesFromSession:session]);

        CHECK_FALSE(fixture.state.dualPaneEnabledForTesting);
        CHECK(fixture.state.rightPanelControllerForTesting == nil);
        CHECK(fixture.state.panelController == fixture.first);
        CHECK([fixture.state tabPaneIDsForTesting] == std::vector<nc::core::PaneId>{{501}});
        CHECK(fixture.second.state == nil);
        CHECK(right_extra.state == nil);

        const nc::explorer::ExplorerPanesSession captured = [fixture.state capturePanesSession];
        CHECK(captured.left.tabs.size() == 1);
        CHECK_FALSE(captured.right);
    }
}

TEST_CASE(PREFIX "swapping sides moves each side's tabs into the other side's captured session")
{
    Fixture fixture;
    fixture.state.dualPanePanelForTesting = fixture.second;
    [fixture.state onSwitchDualSinglePaneMode:nil];
    REQUIRE(fixture.state.dualPaneEnabledForTesting);
    const nc::explorer::ExplorerPanesSession before = [fixture.state capturePanesSession];
    REQUIRE(before.right);
    CHECK_FALSE(before.right_focused);

    [fixture.state OnSwapPanels:nil];

    CHECK([fixture.state isLeftController:fixture.second]);
    CHECK([fixture.state isRightController:fixture.first]);
    const nc::explorer::ExplorerPanesSession after = [fixture.state capturePanesSession];
    REQUIRE(after.right);
    // Swap exchanges the sides' contents, so the focused pane travels with its panel to the left.
    CHECK(fixture.state.panelController == fixture.second);
    CHECK_FALSE(after.right_focused);
}

TEST_CASE(PREFIX "Compare Directories is gated on dual pane with two uniform sides")
{
    Fixture fixture;
    NSMenuItem *const compare_item = [NSMenuItem new];
    compare_item.action = @selector(OnCompareDirectories:);

    // Single pane: nothing to compare against.
    CHECK_FALSE([fixture.state canCompareDualPaneDirectories]);
    CHECK_FALSE([fixture.state validateMenuItem:compare_item]);
    // Invoking it anyway must be inert rather than reach the comparison, exactly like DP-2's
    // cross-pane copy/move - a directly invoked action can never outrun its own menu gate.
    [fixture.state OnCompareDirectories:nil];
    CHECK(fixture.state.panelController == fixture.first);

    fixture.state.dualPanePanelForTesting = fixture.second;
    [fixture.state onSwitchDualSinglePaneMode:nil];
    REQUIRE(fixture.state.dualPaneEnabledForTesting);

    // Dual pane is on, but this fixture's mock panels report no uniform listing, so the compare
    // still declines instead of reading a listing that is not there.
    CHECK_FALSE(fixture.first.isUniform);
    CHECK_FALSE([fixture.state canCompareDualPaneDirectories]);
    CHECK_FALSE([fixture.state validateMenuItem:compare_item]);
    [fixture.state OnCompareDirectories:nil];
    CHECK(fixture.state.dualPaneEnabledForTesting);
    CHECK(fixture.state.panelController == fixture.first);
}

TEST_CASE(PREFIX "Synchronize Directories is gated at least as tightly as Compare")
{
    Fixture fixture;
    NSMenuItem *const sync_item = [NSMenuItem new];
    sync_item.action = @selector(OnSynchronizeDirectories:);

    CHECK_FALSE([fixture.state canSynchronizeDualPaneDirectories]);
    CHECK_FALSE([fixture.state validateMenuItem:sync_item]);
    // A directly invoked destructive action must not outrun its own gate, and must enqueue nothing.
    [fixture.state OnSynchronizeDirectories:nil];
    CHECK(fixture.state.panelController == fixture.first);

    fixture.state.dualPanePanelForTesting = fixture.second;
    [fixture.state onSwitchDualSinglePaneMode:nil];
    REQUIRE(fixture.state.dualPaneEnabledForTesting);

    // Sync additionally needs a writable destination, so it can never be available where Compare is
    // not - this fixture's mock panels satisfy neither.
    CHECK_FALSE([fixture.state canCompareDualPaneDirectories]);
    CHECK_FALSE([fixture.state canSynchronizeDualPaneDirectories]);
    CHECK_FALSE([fixture.state validateMenuItem:sync_item]);
    [fixture.state OnSynchronizeDirectories:nil];
    CHECK(fixture.state.dualPaneEnabledForTesting);
    CHECK(fixture.state.panelController == fixture.first);
}

TEST_CASE(PREFIX "closing the last tab of a side while dual pane is active is a disabled no-op")
{
    Fixture fixture;
    fixture.state.dualPanePanelForTesting = fixture.second;
    [fixture.state onSwitchDualSinglePaneMode:nil];
    REQUIRE(fixture.state.dualPaneEnabledForTesting);

    NSMenuItem *const close_tab_item = [NSMenuItem new];
    close_tab_item.action = @selector(performClose:);
    CHECK_FALSE([fixture.state validateMenuItem:close_tab_item]);

    ExplorerTabsTestWindow *const window =
        [[ExplorerTabsTestWindow alloc] initWithContentRect:NSMakeRect(0, 0, 960, 480)
                                                  styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered
                                                      defer:false];
    window.contentView = fixture.state;

    [fixture.state performClose:nil];

    CHECK(window.performCloseCount == 0);
    CHECK(fixture.state.dualPaneEnabledForTesting);
    CHECK(fixture.state.rightPanelControllerForTesting == fixture.second);
    CHECK(fixture.state.panelController == fixture.first);
}

TEST_CASE(PREFIX "toggling dual pane off and back on leaves the Left side's tabs untouched")
{
    Fixture fixture;
    REQUIRE([fixture.state addInactivePanelForTesting:fixture.second]);
    const std::vector<nc::core::PaneId> left_tabs_before = [fixture.state tabPaneIDsForTesting];
    REQUIRE(left_tabs_before.size() == 2);

    ExplorerTabsTestPanelView *const third_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTabsTestPanelController *const third =
        [[ExplorerTabsTestPanelController alloc] initWithPaneID:nc::core::PaneId{503} view:third_view];
    fixture.state.dualPanePanelForTesting = third;

    [fixture.state onSwitchDualSinglePaneMode:nil];
    REQUIRE(fixture.state.dualPaneEnabledForTesting);
    CHECK([fixture.state tabPaneIDsForTesting] == left_tabs_before);

    [fixture.state onSwitchDualSinglePaneMode:nil];
    CHECK_FALSE(fixture.state.dualPaneEnabledForTesting);
    CHECK([fixture.state tabPaneIDsForTesting] == left_tabs_before);
    CHECK(fixture.first.state == fixture.state);
    CHECK(fixture.second.state == fixture.state);
}

TEST_CASE(PREFIX "Tab key switches focus between sides only while dual pane is active")
{
    Fixture fixture;
    NSEvent *const tab_key = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                               location:NSZeroPoint
                                          modifierFlags:0
                                              timestamp:0
                                           windowNumber:0
                                                context:nil
                                             characters:@"\t"
                            charactersIgnoringModifiers:@"\t"
                                              isARepeat:NO
                                                keyCode:48];

    // Single pane: Tab is not claimed, and does not change focus.
    CHECK([fixture.state bidForHandlingKeyDown:tab_key forPanelView:fixture.first.view] ==
          nc::panel::view::BiddingPriority::Skip);
    [fixture.state handleKeyDown:tab_key forPanelView:fixture.first.view];
    CHECK(fixture.state.panelController == fixture.first);

    fixture.state.dualPanePanelForTesting = fixture.second;
    [fixture.state onSwitchDualSinglePaneMode:nil];
    REQUIRE(fixture.state.dualPaneEnabledForTesting);
    REQUIRE(fixture.state.panelController == fixture.first);
    const NSUInteger rebind_count_before = fixture.toolbar.rebindCount;

    CHECK([fixture.state bidForHandlingKeyDown:tab_key forPanelView:fixture.first.view] ==
          nc::panel::view::BiddingPriority::Max);
    [fixture.state handleKeyDown:tab_key forPanelView:fixture.first.view];

    CHECK(fixture.state.panelController == fixture.second);
    CHECK(fixture.toolbar.panelController == fixture.second);
    CHECK(fixture.toolbar.rebindCount > rebind_count_before);

    [fixture.state handleKeyDown:tab_key forPanelView:fixture.second.view];
    CHECK(fixture.state.panelController == fixture.first);
}

TEST_CASE(PREFIX "fallback window title follows focus while the Explorer toolbar suppresses its duplicate")
{
    Fixture fixture;
    CHECK_FALSE(fixture.state.windowStateNeedsTitle);
    ExplorerTabsTestWindow *const window =
        [[ExplorerTabsTestWindow alloc] initWithContentRect:NSMakeRect(0, 0, 960, 480)
                                                  styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered
                                                      defer:false];
    window.contentView = fixture.state;
    window.title = @"stale Commander path";

    fixture.state.dualPanePanelForTesting = fixture.second;
    [fixture.state onSwitchDualSinglePaneMode:nil];
    REQUIRE(fixture.state.dualPaneEnabledForTesting);
    REQUIRE(fixture.state.panelController == fixture.first);

    const auto titled = [](const nc::core::PaneId _pane_id, const char *_title) {
        nc::core::PaneSnapshot snapshot;
        snapshot.pane_id = _pane_id;
        snapshot.revision = 1;
        // Empty phase keeps the visual-state mapper off a listing this fixture has no reason to
        // build: the title is derived from display_title alone, independently of load phase.
        snapshot.state.load_phase = nc::core::PaneLoadPhase::Empty;
        snapshot.state.display_title = _title;
        return snapshot;
    };

    // The left side is focused, so this snapshot lands on it.
    [fixture.state setFocusedPaneSnapshotForTesting:titled(fixture.first.paneId, "left-folder")];

    NSEvent *const tab_key = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                               location:NSZeroPoint
                                          modifierFlags:0
                                              timestamp:0
                                           windowNumber:0
                                                context:nil
                                             characters:@"\t"
                            charactersIgnoringModifiers:@"\t"
                                              isARepeat:NO
                                                keyCode:48];

    // Focus moves to a side that has published nothing. The title must drop rather than keep
    // advertising the other pane's folder - the pane is idle and will publish no correcting
    // snapshot of its own.
    [fixture.state handleKeyDown:tab_key forPanelView:fixture.first.view];
    REQUIRE(fixture.state.panelController == fixture.second);
    CHECK([window.title isEqualToString:@""]);

    [fixture.state setFocusedPaneSnapshotForTesting:titled(fixture.second.paneId, "right-folder")];

    // Tab alone - with no new snapshot from either pane - must still re-derive the title from
    // whichever side now holds focus.
    [fixture.state handleKeyDown:tab_key forPanelView:fixture.second.view];
    REQUIRE(fixture.state.panelController == fixture.first);
    CHECK([window.title isEqualToString:@"left-folder"]);

    [fixture.state handleKeyDown:tab_key forPanelView:fixture.first.view];
    REQUIRE(fixture.state.panelController == fixture.second);
    CHECK([window.title isEqualToString:@"right-folder"]);

    // A panel view that belongs to neither side never claims or acts on the key.
    ExplorerTabsTestPanelView *const foreign_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    CHECK([fixture.state bidForHandlingKeyDown:tab_key forPanelView:reinterpret_cast<PanelView *>(foreign_view)] ==
          nc::panel::view::BiddingPriority::Skip);
}

TEST_CASE(PREFIX "Swap exchanges which panel occupies each side and rebinds focused chrome")
{
    Fixture fixture;
    fixture.state.dualPanePanelForTesting = fixture.second;
    [fixture.state onSwitchDualSinglePaneMode:nil];
    REQUIRE(fixture.state.dualPaneEnabledForTesting);
    REQUIRE(fixture.state.panelController == fixture.first);
    REQUIRE(fixture.state.rightPanelControllerForTesting == fixture.second);
    const NSUInteger rebind_count_before = fixture.toolbar.rebindCount;

    [fixture.state OnSwapPanels:nil];

    CHECK(fixture.state.panelController == fixture.second);
    CHECK(fixture.state.rightPanelControllerForTesting == fixture.first);
    CHECK(fixture.toolbar.panelController == fixture.second);
    CHECK(fixture.toolbar.rebindCount > rebind_count_before);
    CHECK([fixture.state isLeftController:fixture.second]);
    CHECK([fixture.state isRightController:fixture.first]);
    CHECK_FALSE([fixture.state isLeftController:fixture.first]);
    CHECK_FALSE([fixture.state isRightController:fixture.second]);

    // Swapping back restores the original arrangement, and both PaneIds survived the round trip.
    [fixture.state OnSwapPanels:nil];
    CHECK(fixture.state.panelController == fixture.first);
    CHECK(fixture.state.rightPanelControllerForTesting == fixture.second);
    CHECK(fixture.first.paneId.value == 501);
    CHECK(fixture.second.paneId.value == 502);
}

TEST_CASE(PREFIX "resolves the opposite panel only while dual pane is active and owns the queried panel")
{
    Fixture fixture;
    CHECK([fixture.state dualPaneOppositePanelControllerFor:fixture.first] == nil);

    fixture.state.dualPanePanelForTesting = fixture.second;
    [fixture.state onSwitchDualSinglePaneMode:nil];
    REQUIRE(fixture.state.dualPaneEnabledForTesting);

    CHECK([fixture.state dualPaneOppositePanelControllerFor:fixture.first] == fixture.second);
    CHECK([fixture.state dualPaneOppositePanelControllerFor:fixture.second] == fixture.first);

    ExplorerTabsTestPanelView *const foreign_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTabsTestPanelController *const foreign =
        [[ExplorerTabsTestPanelController alloc] initWithPaneID:nc::core::PaneId{599} view:foreign_view];
    CHECK([fixture.state dualPaneOppositePanelControllerFor:foreign] == nil);
}

TEST_CASE(PREFIX "cross-pane copy and move are disabled no-ops outside dual pane, swap requires both sides")
{
    Fixture fixture;
    NSMenuItem *const swap_item = [NSMenuItem new];
    swap_item.action = @selector(OnSwapPanels:);
    NSMenuItem *const copy_item = [NSMenuItem new];
    copy_item.action = @selector(OnFileCopyCommand:);
    NSMenuItem *const move_item = [NSMenuItem new];
    move_item.action = @selector(OnFileRenameMoveCommand:);

    CHECK_FALSE([fixture.state validateMenuItem:swap_item]);
    CHECK_FALSE([fixture.state validateMenuItem:copy_item]);
    CHECK_FALSE([fixture.state validateMenuItem:move_item]);

    // Safe no-ops while single-pane: nothing crashes, nothing about the Left side changes.
    [fixture.state OnSwapPanels:nil];
    [fixture.state OnFileCopyCommand:nil];
    [fixture.state OnFileRenameMoveCommand:nil];
    CHECK(fixture.state.panelController == fixture.first);
    CHECK_FALSE(fixture.state.dualPaneEnabledForTesting);

    fixture.state.dualPanePanelForTesting = fixture.second;
    [fixture.state onSwitchDualSinglePaneMode:nil];
    REQUIRE(fixture.state.dualPaneEnabledForTesting);

    CHECK([fixture.state validateMenuItem:swap_item]);
    // The mock PanelController's default (unnavigated) listing is never uniform/writable, so copy
    // and move stay disabled even with a real opposite side - fail-closed, not a crash.
    CHECK_FALSE([fixture.state validateMenuItem:copy_item]);
    CHECK_FALSE([fixture.state validateMenuItem:move_item]);
    [fixture.state OnFileCopyCommand:nil];
    [fixture.state OnFileRenameMoveCommand:nil];
    CHECK(fixture.state.panelController == fixture.first);
}

#undef PREFIX

@interface NCMainWindowController (ExplorerDefaultModeTesting)
- (instancetype)initForTestingWithWindow:(NCMainWindow *)_window;
- (NCExplorerState *)makeExplorerState;
- (NCTermShellState *)makeTerminalState;
- (void)windowWillClose:(NSNotification *)_notification;
@end

@interface ExplorerDefaultModeFileState : MainWindowFilePanelState
@property(nonatomic, weak) PanelController *testActivePanel;
@end

@implementation ExplorerDefaultModeFileState {
    __weak PanelController *_testActivePanel;
}
@synthesize testActivePanel = _testActivePanel;
- (NSToolbar *)windowStateToolbar
{
    return nil;
}
- (NSView *)windowStateContentView
{
    return self;
}
- (PanelController *)activePanelController
{
    return self.testActivePanel;
}
- (void)windowStateDidBecomeAssigned
{
}
- (void)windowStateDidResign
{
}
- (void)windowStateWillClose
{
}
@end

@interface ExplorerDefaultModeState : NCExplorerState
@property(nonatomic, weak) PanelController *testPanel;
@end

@implementation ExplorerDefaultModeState {
    __weak PanelController *_testPanel;
}
@synthesize testPanel = _testPanel;
- (NSToolbar *)windowStateToolbar
{
    return nil;
}
- (NSView *)windowStateContentView
{
    return self;
}
- (PanelController *)panelController
{
    return self.testPanel;
}
- (void)windowStateDidBecomeAssigned
{
}
- (void)windowStateDidResign
{
}
- (void)windowStateWillClose
{
}
@end

@interface ExplorerDefaultModeWindowController : NCMainWindowController
@property(nonatomic, strong) NCExplorerState *testExplorer;
@property(nonatomic, strong) NCTermShellState *testTerminal;
@property(nonatomic) NSUInteger explorerFactoryCalls;
@end

@implementation ExplorerDefaultModeWindowController {
    NCExplorerState *_testExplorer;
    NCTermShellState *_testTerminal;
    NSUInteger _explorerFactoryCalls;
}
@synthesize testExplorer = _testExplorer;
@synthesize testTerminal = _testTerminal;
@synthesize explorerFactoryCalls = _explorerFactoryCalls;
- (NCExplorerState *)makeExplorerState
{
    ++self.explorerFactoryCalls;
    return self.testExplorer;
}
- (NCTermShellState *)makeTerminalState
{
    return self.testTerminal;
}
@end

@interface ExplorerDefaultModeTerminal : NCTermShellState
- (const std::string &)recordedInitialWD;
- (const std::vector<std::string> &)recordedChangedDirectories;
- (NSUInteger)recordedExecutionCount;
@end

@implementation ExplorerDefaultModeTerminal {
    std::string m_RecordedInitialWD;
    std::vector<std::string> m_RecordedChangedDirectories;
    NSUInteger m_RecordedExecutionCount;
}
- (void)setInitialWD:(const std::string &)_wd
{
    m_RecordedInitialWD = _wd;
}
- (const std::string &)recordedInitialWD
{
    return m_RecordedInitialWD;
}
- (void)chDir:(const std::string &)_new_dir
{
    m_RecordedChangedDirectories.emplace_back(_new_dir);
}
- (const std::vector<std::string> &)recordedChangedDirectories
{
    return m_RecordedChangedDirectories;
}
- (void)executeWithFullPath:(const std::filesystem::path &) [[maybe_unused]] _binary_path
               andArguments:(std::span<const std::string>) [[maybe_unused]] _params
{
    ++m_RecordedExecutionCount;
}
- (NSUInteger)recordedExecutionCount
{
    return m_RecordedExecutionCount;
}
- (NSToolbar *)windowStateToolbar
{
    return nil;
}
- (NSView *)windowStateContentView
{
    return self;
}
- (void)windowStateDidBecomeAssigned
{
}
- (void)windowStateDidResign
{
}
@end

@interface ExplorerExternalNavigationPanel : PanelController
- (std::shared_ptr<nc::panel::DirectoryChangeRequest>)lastRequest;
@end

@implementation ExplorerExternalNavigationPanel {
    std::shared_ptr<nc::panel::DirectoryChangeRequest> m_LastRequest;
}
- (std::expected<void, nc::Error>)GoToDirWithContext:
    (std::shared_ptr<nc::panel::DirectoryChangeRequest>)_context
{
    m_LastRequest = std::move(_context);
    return {};
}
- (std::shared_ptr<nc::panel::DirectoryChangeRequest>)lastRequest
{
    return m_LastRequest;
}
@end

@interface ExplorerExternalNavigationWindowController : NCMainWindowController
@property(nonatomic, weak) PanelController *testVisiblePanel;
@end

@implementation ExplorerExternalNavigationWindowController {
    __weak PanelController *_testVisiblePanel;
}
@synthesize testVisiblePanel = _testVisiblePanel;
- (PanelController *)visibleActivePanelController
{
    return self.testVisiblePanel;
}
@end

class ExplorerExternalNavigationNativeHost final : public nc::vfs::Host
{
public:
    ExplorerExternalNavigationNativeHost() : Host("/", nullptr, "explorer-external-navigation-native") {}
    bool IsNativeFS() const noexcept override { return true; }
    bool IsDirectory(std::string_view _path,
                     unsigned long,
                     const VFSCancelChecker &) override
    {
        return _path == "/private/tmp";
    }
};

@interface ExplorerTerminalPanel : ExplorerTabsTestPanelController
- (instancetype)initWithPaneID:(nc::core::PaneId)_pane_id
                          view:(ExplorerTabsTestPanelView *)_view
                          host:(std::shared_ptr<VFSHost>)_host
                          path:(std::string)_path;
- (void)setTestPath:(std::string)_path;
@end

@implementation ExplorerTerminalPanel {
    std::shared_ptr<VFSHost> m_TestHost;
    std::string m_TestPath;
}
- (instancetype)initWithPaneID:(nc::core::PaneId)_pane_id
                          view:(ExplorerTabsTestPanelView *)_view
                          host:(std::shared_ptr<VFSHost>)_host
                          path:(std::string)_path
{
    self = [super initWithPaneID:_pane_id view:_view];
    if( self ) {
        m_TestHost = std::move(_host);
        m_TestPath = std::move(_path);
    }
    return self;
}
- (BOOL)isUniform
{
    return YES;
}
- (const std::shared_ptr<VFSHost> &)vfs
{
    return m_TestHost;
}
- (std::string)currentDirectoryPath
{
    return m_TestPath;
}
- (void)setTestPath:(std::string)_path
{
    m_TestPath = std::move(_path);
}
@end

static NCMainWindow *ExplorerDefaultModeTestWindow()
{
    NSWindow *const window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 600)
                                                         styleMask:NSWindowStyleMaskTitled
                                                           backing:NSBackingStoreBuffered
                                                             defer:YES];
    return reinterpret_cast<NCMainWindow *>(window);
}

#define PREFIX "NCMainWindowController default Explorer "

TEST_CASE(PREFIX "policy defaults fresh and failed-restoration windows to Explorer")
{
    using nc::bootstrap::MainWindowCreationKind;
    using nc::bootstrap::PlanDefaultExplorerStartup;
    using nc::bootstrap::ShouldEnsureDefaultExplorer;

    const auto fresh = PlanDefaultExplorerStartup(MainWindowCreationKind::Default, false, true);
    CHECK_FALSE(fresh.restore_stored_session);
    CHECK(ShouldEnsureDefaultExplorer(fresh));

    const auto restoring = PlanDefaultExplorerStartup(MainWindowCreationKind::ManualRestoration, false, true);
    REQUIRE(restoring.restore_stored_session);
    CHECK(ShouldEnsureDefaultExplorer(restoring, false));
    CHECK_FALSE(ShouldEnsureDefaultExplorer(restoring, true));

    const auto system = PlanDefaultExplorerStartup(MainWindowCreationKind::SystemRestoration, false, true);
    CHECK_FALSE(system.restore_stored_session);
    CHECK_FALSE(ShouldEnsureDefaultExplorer(system));
}

TEST_CASE(PREFIX "enters Explorer idempotently and publishes only the visible pane")
{
    NCMainWindow *const window = ExplorerDefaultModeTestWindow();
    ExplorerDefaultModeWindowController *const controller =
        [[ExplorerDefaultModeWindowController alloc] initForTestingWithWindow:window];

    ExplorerTabsTestPanelView *const commander_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTabsTestPanelController *const commander_panel =
        [[ExplorerTabsTestPanelController alloc] initWithPaneID:nc::core::PaneId{701} view:commander_view];
    ExplorerDefaultModeFileState *const commander = [[ExplorerDefaultModeFileState alloc] initWithFrame:NSZeroRect];
    commander.testActivePanel = commander_panel;
    controller.filePanelsState = commander;

    ExplorerTabsTestPanelView *const explorer_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTabsTestPanelController *const explorer_panel =
        [[ExplorerTabsTestPanelController alloc] initWithPaneID:nc::core::PaneId{702} view:explorer_view];
    ExplorerDefaultModeState *const explorer = [[ExplorerDefaultModeState alloc] initWithFrame:NSZeroRect];
    explorer.testPanel = explorer_panel;
    controller.testExplorer = explorer;

    CHECK(controller.visibleActivePanelController == commander_panel);
    REQUIRE([controller ensureExplorerMode]);
    CHECK(controller.topmostState == explorer);
    CHECK(controller.visibleActivePanelController == explorer_panel);
    CHECK(controller.explorerFactoryCalls == 1);

    CHECK([controller ensureExplorerMode]);
    CHECK(controller.topmostState == explorer);
    CHECK(controller.explorerFactoryCalls == 1);

    [controller toggleExplorerMode:nil];
    CHECK(controller.topmostState == commander);
    CHECK(controller.visibleActivePanelController == commander_panel);

    [controller windowWillClose:nil];
}

TEST_CASE(PREFIX "routes external open and reveal through the visible pane boundary")
{
    ExplorerExternalNavigationPanel *const panel = [ExplorerExternalNavigationPanel new];
    ExplorerExternalNavigationWindowController *const window = [[ExplorerExternalNavigationWindowController alloc]
        initForTestingWithWindow:ExplorerDefaultModeTestWindow()];
    REQUIRE(panel != nil);
    REQUIRE(window != nil);
    window.testVisiblePanel = panel;
    REQUIRE(window.visibleActivePanelController == panel);
    const auto native_host = std::make_shared<ExplorerExternalNavigationNativeHost>();
    nc::core::ServicesHandler handler{[window] { return window; }, native_host};

    handler.OpenFiles(@[ @"/private/tmp" ]);
    auto request = [panel lastRequest];
    REQUIRE(request);
    CHECK(request->RequestedDirectory == "/private/tmp");
    CHECK(request->RequestFocusedEntry.empty());
    CHECK(request->InitiatedByUser);

    handler.OpenFiles(@[ @"/private/tmp/q1-10-first", @"/private/tmp/q1-10-second" ]);
    request = [panel lastRequest];
    REQUIRE(request);
    CHECK(request->RequestedDirectory == "/private/tmp");
    CHECK(request->RequestFocusedEntry == "q1-10-first");
    CHECK(request->RequestSelectedEntries == std::vector<std::string>{"q1-10-first", "q1-10-second"});
    CHECK(request->InitiatedByUser);
}

TEST_CASE(PREFIX "uses the visible Explorer cwd for first and reused terminal executions")
{
    NCMainWindow *const window = ExplorerDefaultModeTestWindow();
    ExplorerDefaultModeWindowController *const controller =
        [[ExplorerDefaultModeWindowController alloc] initForTestingWithWindow:window];

    ExplorerDefaultModeFileState *const commander = [[ExplorerDefaultModeFileState alloc] initWithFrame:NSZeroRect];
    controller.filePanelsState = commander;

    const auto native_host = std::make_shared<ExplorerExternalNavigationNativeHost>();
    ExplorerTabsTestPanelView *const explorer_view = [[ExplorerTabsTestPanelView alloc] initWithFrame:NSZeroRect];
    ExplorerTerminalPanel *const explorer_panel = [[ExplorerTerminalPanel alloc]
        initWithPaneID:nc::core::PaneId{703}
                  view:explorer_view
                  host:native_host
                  path:"/private/tmp/q1-10-first/"];
    ExplorerDefaultModeState *const explorer = [[ExplorerDefaultModeState alloc] initWithFrame:NSZeroRect];
    explorer.testPanel = explorer_panel;
    controller.testExplorer = explorer;

    ExplorerDefaultModeTerminal *const terminal = [ExplorerDefaultModeTerminal new];
    controller.testTerminal = terminal;

    REQUIRE([controller ensureExplorerMode]);
    [controller requestTerminalExecutionWithFullPath:"/usr/bin/true" andArguments:{}];
    CHECK(terminal.recordedInitialWD == "/private/tmp/q1-10-first/");
    CHECK(terminal.recordedChangedDirectories.empty());
    CHECK(terminal.recordedExecutionCount == 1);

    [controller ResignAsWindowState:terminal];
    [explorer_panel setTestPath:"/private/tmp/q1-10-second/"];
    [controller requestTerminalExecutionWithFullPath:"/usr/bin/true" andArguments:{}];
    CHECK(terminal.recordedChangedDirectories == std::vector<std::string>{"/private/tmp/q1-10-second/"});
    CHECK(terminal.recordedExecutionCount == 2);

    [controller ResignAsWindowState:terminal];
    [controller toggleExplorerMode:nil];
    [controller windowWillClose:nil];
}

#undef PREFIX
