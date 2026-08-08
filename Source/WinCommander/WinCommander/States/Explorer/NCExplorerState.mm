// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerState.h"
#include "NCExplorerToolbarDelegate.h"
#include "NCExplorerSidebarView.h"
#include "NCExplorerCommandBarView.h"
#include "NCExplorerInspectorView.h"
#include "NCExplorerPaneStateView.h"
#include "NCExplorerOperationProgressView.h"
#include "NCExplorerSearchModeView.h"
#include "ExplorerOperationProgressController.h"
#include "ExplorerSearchController.h"
#include "ExplorerSpotlightSearchBackend.h"
#include "ExplorerViewSettingsBinding.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/PanelControllerPaneStoreAdapter.h"
#include "../FilePanels/PanelAux.h"
#include "../FilePanels/PanelView.h"
#include "../FilePanels/PanelViewHeader.h"
#include "../FilePanels/PanelViewLayoutSupport.h"
#include "../FilePanels/PanelControllerActionsDispatcher.h"
#include "../FilePanels/Actions/Helpers.h"
#include "../FilePanels/Helpers/Pasteboard.h"
#include "../FilePanels/Views/FilePanelsTabbedHolder.h"
#include "../FilePanels/Views/QuickLookPanel.h"
#include "../MainWindowController.h"
#include "../../Bootstrap/AppDelegate.h"
#include "../../Bootstrap/AppDelegate+MainWindowCreation.h"
#include "../../Bootstrap/Config.h"
#include <WinCommander/Core/Commands/CommandRegistry.h>
#include <WinCommander/Core/Pane/ExplorerTabsModel.h>
#include <WinCommander/Core/VisualState/VisualStateMapper.h>
#include <WinCommander/Bootstrap/NativeVFSHostInstance.h>
#include <Base/CommonPaths.h>
#include <Base/dispatch_cpp.h>
#include <Panel/PanelData.h>
#include <Operations/Pool.h>
#include <Utility/ObjCpp.h>
#include <Utility/StringExtras.h>
#include <VFS/VFS.h>
#include <VFS/ProviderCapabilities.h>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <sys/mount.h>
#include <unordered_set>
#include <vector>

static const CGFloat g_SidebarWidth = 220.0;
static const CGFloat g_CommandBarHeight = 36.0;
static const CGFloat g_InspectorPreferredWidth = 320.0;
static const CGFloat g_InspectorMinimumWidth = 280.0;
static const CGFloat g_InspectorMaximumWidth = 520.0;
static const CGFloat g_PanelMinimumWidth = 360.0;
static constexpr auto g_QuickLookHazardousExtensionsList = "filePanel.presentation.quickLookHazardousExtensionsList";

@interface NCAppDelegate (NCExplorerInspectorDependencies)
- (nc::panel::QuickLookVFSBridge &)QLVFSBridge;
- (NCPanelQLPanelAdaptor *)QLPanelAdaptor;
@end

namespace {

std::vector<nc::core::FileMetadataSnapshot> LiveInspectorMetadata(const nc::core::PaneSnapshot &_snapshot)
{
    if( _snapshot.state.load_phase != nc::core::PaneLoadPhase::Loaded &&
        _snapshot.state.load_phase != nc::core::PaneLoadPhase::Refreshing )
        return {};

    std::vector<nc::core::FileMetadataSnapshot> metadata;
    const auto append = [&metadata](const nc::vfs::ListingItem &_item) {
        if( _item && !_item.IsDotDot() )
            metadata.emplace_back(nc::core::CopyFileMetadataSnapshot(_item));
    };
    if( !_snapshot.state.selected_items.empty() ) {
        metadata.reserve(_snapshot.state.selected_items.size());
        for( const nc::vfs::ListingItem &item : _snapshot.state.selected_items )
            append(item);
    }
    else {
        append(_snapshot.state.focused_item);
    }
    return metadata;
}

std::optional<nc::vfs::ListingItem> FindExactListingItem(const nc::core::PaneSnapshot &_snapshot,
                                                         const nc::core::FileMetadataSnapshot &_metadata)
{
    if( _snapshot.state.load_phase != nc::core::PaneLoadPhase::Loaded &&
        _snapshot.state.load_phase != nc::core::PaneLoadPhase::Refreshing )
        return std::nullopt;
    const VFSListingPtr &listing = _snapshot.state.listing;
    if( !listing )
        return std::nullopt;

    std::optional<nc::vfs::ListingItem> match;
    for( unsigned index = 0; index < listing->Count(); ++index ) {
        const nc::vfs::ListingItem item = listing->Item(index);
        if( !item || item.IsDotDot() || nc::core::CopyFileMetadataSnapshot(item) != _metadata )
            continue;
        if( match )
            return std::nullopt;
        match = item;
    }
    return match;
}

struct ExplorerTabEntry {
    __strong PanelController *panel = nil;
    __strong ExplorerSearchController *search_controller = nil;
    std::unique_ptr<nc::panel::PanelControllerPaneStoreAdapter> pane_store;
    std::unique_ptr<nc::explorer::ExplorerViewSettingsBindingPolicy> view_settings_binding;
    uint64_t view_settings_observation_sequence = 0;
    bool view_settings_context_sample_scheduled = false;
    std::optional<uint64_t> last_active_snapshot_revision;
};

std::optional<std::string> NativeVolumeRoot(const std::string &_path)
{
    struct statfs volume_info {};
    if( _path.empty() || ::statfs(_path.c_str(), &volume_info) != 0 || volume_info.f_mntonname[0] != '/' )
        return std::nullopt;
    std::string root = volume_info.f_mntonname;
    if( root.back() != '/' )
        root.push_back('/');
    return root;
}

bool IsStructurallyValidSessionLocation(const nc::panel::PersistentLocation &_location) noexcept
{
    return !_location.path.empty() && _location.path.front() == '/' && _location.path.back() == '/';
}

ExplorerTabEntry *FindTabEntry(std::vector<ExplorerTabEntry> &_entries, const nc::core::PaneId _pane_id)
{
    const auto iterator =
        std::ranges::find(_entries, _pane_id, [](const ExplorerTabEntry &_entry) { return _entry.panel.paneId; });
    return iterator == _entries.end() ? nullptr : &*iterator;
}

ExplorerTabEntry *FindTabEntry(std::vector<ExplorerTabEntry> &_entries, PanelController *_panel)
{
    if( !_panel )
        return nullptr;
    ExplorerTabEntry *const entry = FindTabEntry(_entries, _panel.paneId);
    return entry && entry->panel == _panel ? entry : nullptr;
}

nc::core::PaneVisualState ExplorerPaneVisualState(const nc::core::PaneSnapshot &_snapshot,
                                                  PanelController *_panel)
{
    nc::core::PaneVisualState visual = nc::core::VisualStateMapper::MapPane(_snapshot);
    if( _panel && _snapshot.pane_id == _panel.paneId &&
        _snapshot.state.load_phase == nc::core::PaneLoadPhase::Loading &&
        _panel.isPresentingProgressiveNavigationPreview ) {
        visual.content_visible = true;
    }
    return visual;
}

std::optional<nc::core::PaneId> PaneIdFromTabItem(NSTabViewItem *_item)
{
    const NSNumber *const number = nc::objc_cast<NSNumber>(_item.identifier);
    if( !number || number.unsignedLongLongValue == 0 )
        return std::nullopt;
    return nc::core::PaneId{number.unsignedLongLongValue};
}

std::string TabNameForController(PanelController *_controller)
{
    const std::filesystem::path path = _controller.currentDirectoryPath;
    std::string name = path == "/" ? path.native() : path.parent_path().filename().native();
    if( name == "/" && _controller.isUniform && _controller.vfs->Parent() )
        name = std::filesystem::path(_controller.vfs->JunctionPath()).filename().native();
    return name;
}

std::optional<nc::explorer::ExplorerViewSettings>
CaptureExplorerViewSettings(PanelController *_panel, const nc::core::PaneSnapshot &_snapshot)
{
    if( !_panel || _snapshot.pane_id != _panel.paneId ||
        (_snapshot.state.load_phase != nc::core::PaneLoadPhase::Loaded &&
         _snapshot.state.load_phase != nc::core::PaneLoadPhase::Refreshing) ||
        !_snapshot.state.is_uniform || !_snapshot.state.host || _snapshot.state.path.empty() ||
        _snapshot.state.path.back() != '/' || !_snapshot.state.view_state.layout_index ) {
        return std::nullopt;
    }

    const int slot = *_snapshot.state.view_state.layout_index;
    const auto configured_layout = _panel.layoutStorage.GetLayout(slot);
    if( !configured_layout || configured_layout->is_disabled() )
        return std::nullopt;

    nc::panel::PanelViewLayout actual_layout;
    actual_layout.layout = [_panel.view presentationLayout];
    if( !nc::panel::IsValidPanelViewLayout(actual_layout) || actual_layout.type() != configured_layout->type() ||
        nc::panel::ProjectPaneViewState(actual_layout, slot) != _snapshot.state.view_state ) {
        return std::nullopt;
    }

    const auto sort = nc::panel::RestorePanelSortMode(_snapshot.state.sort_state);
    if( !sort || nc::panel::ProjectPaneGroupingState(*sort, _snapshot.state.grouping_state.enabled) !=
                     _snapshot.state.grouping_state ) {
        return std::nullopt;
    }

    return nc::explorer::ExplorerViewSettings{.layout_slot = slot,
                                               .layout = std::move(actual_layout),
                                               .sort = _snapshot.state.sort_state,
                                               .grouping = _snapshot.state.grouping_state};
}

bool ApplyExplorerViewSettings(PanelController *_panel, const nc::explorer::ExplorerViewSettings &_settings)
{
    if( !_panel )
        return false;
    const auto configured_layout = _panel.layoutStorage.GetLayout(_settings.layout_slot);
    const auto sort = nc::panel::RestorePanelSortMode(_settings.sort);
    if( !configured_layout || configured_layout->is_disabled() ||
        configured_layout->type() != _settings.layout.type() || !sort ||
        nc::panel::ProjectPaneGroupingState(*sort, _settings.grouping.enabled) != _settings.grouping ) {
        return false;
    }

    if( ![_panel applyPaneLocalPresentationLayout:_settings.layout atConfiguredSlot:_settings.layout_slot] )
        return false;
    [_panel changeSortingModeTo:*sort];
    _panel.view.explorerDetailsGroupingEnabled = _settings.grouping.enabled;
    return true;
}

void ConfigureExplorerRootAccessibility(NSView *_view)
{
    _view.accessibilityElement = true;
    _view.accessibilityRole = NSAccessibilityGroupRole;
    _view.accessibilityIdentifier = @"wincommander.explorer.root";
    _view.accessibilityLabel = NSLocalizedString(@"File Explorer", "Explorer root accessibility label");
}

} // namespace

@interface NCExplorerState () <NSSplitViewDelegate>
- (instancetype)initForTestingWithFrame:(NSRect)_frame
                        panelController:(PanelController *)_panel
                              panelView:(NSView *)_panel_view
                          inspectorView:(NCExplorerInspectorView *)_inspector
                         QLPanelAdaptor:(NCPanelQLPanelAdaptor *)_ql_panel_adaptor;
- (void)applyPaneSnapshotForTesting:(const nc::core::PaneSnapshot &)_snapshot;
- (void)configureTabBindingForTestingWithToolbarDelegate:(NCExplorerToolbarDelegate *)_toolbar
                                                 sidebar:(NCExplorerSidebarView *)_sidebar
                                              commandBar:(NCExplorerCommandBarView *)_command_bar;
- (BOOL)addInactivePanelForTesting:(PanelController *)_panel;
- (std::vector<nc::core::PaneId>)tabPaneIDsForTesting;
- (BOOL)attachExplorerTabPanel:(PanelController *)_panel createPaneStore:(BOOL)_create_pane_store;
- (void)attachExplorerTabViewForPanel:(PanelController *)_panel;
- (void)rollbackSessionRestorePanels;
- (PanelController *)allocateExplorerPanelForSessionRestore;
- (void)restoreSessionLocation:(const std::optional<nc::panel::PersistentLocation> &)_location
                       forPanel:(PanelController *)_panel;
- (void)loadNativeHomeForSessionPanel:(PanelController *)_panel;
- (void)invalidateExplorerRestorableState;
- (void)bindActivePanel:(PanelController *)_panel focus:(BOOL)_focus;
- (std::optional<nc::core::SearchPlanningFacts>)searchPlanningFactsForPanel:(PanelController *)_panel;
- (void)applySearchSnapshotForPane:(nc::core::PaneId)_pane_id;
- (void)configureSearchViewHandlers;
- (void)applyActivePaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot
               observationToken:(nc::core::ExplorerTabObservationToken)_token;
- (void)applyViewSettingsForSnapshot:(const nc::core::PaneSnapshot &)_snapshot
                            tabEntry:(ExplorerTabEntry &)_entry;
- (void)updateTabLabelForPanel:(PanelController *)_panel;
- (void)closeTabViewItem:(NSTabViewItem *)_item;
- (void)reorderOwnedTabViewItem:(NSTabViewItem *)_item toIndex:(NSUInteger)_index;
@end

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
        self.accessibilityElement = true;
        self.accessibilityRole = NSAccessibilityGroupRole;
        self.accessibilityIdentifier = @"wincommander.explorer.quickSearch";
        self.accessibilityLabel = NSLocalizedString(@"Quick Search", "Explorer accessibility label");

        NSImageView *const icon = [NSImageView imageViewWithImage:[NSImage imageWithSystemSymbolName:@"magnifyingglass"
                                                                            accessibilityDescription:nil]];
        icon.contentTintColor = NSColor.secondaryLabelColor;
        m_SearchField = [[NSTextField alloc] initWithFrame:NSZeroRect];
        m_SearchField.bezeled = false;
        m_SearchField.drawsBackground = false;
        m_SearchField.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
        m_SearchField.placeholderString = NSLocalizedString(@"Quick Search", "Explorer quick search");
        m_SearchField.delegate = self;
        m_SearchField.target = self;
        m_SearchField.action = @selector(onSearchAction:);
        m_SearchField.accessibilityIdentifier = @"wincommander.explorer.quickSearch.query";
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
        m_ClearButton.accessibilityIdentifier = @"wincommander.explorer.quickSearch.clear";
        m_ClearButton.accessibilityLabel = NSLocalizedString(@"Clear Quick Search", "Explorer accessibility label");
        m_ClearButton.accessibilityHelp =
            NSLocalizedString(@"Clear the current filename filter", "Explorer accessibility help");

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
    m_MatchesLabel.stringValue =
        [NSString stringWithFormat:NSLocalizedString(@"%d matches", "Explorer quick search"), _matches];
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
    NCExplorerOperationProgressView *m_OperationProgressView;
    ExplorerOperationProgressController *m_OperationProgressController;
    NCExplorerSearchModeView *m_SearchModeView;
    NSSplitView *m_ContentSplitView;
    NSView *m_PanelContainer;
    NCExplorerPaneStateView *m_PaneStateView;
    NCExplorerInspectorView *m_Inspector;
    NCExplorerQuickSearchOverlayView *m_QuickSearchOverlay;
    FilePanelsTabbedHolder *m_TabbedHolder;
    NCPanelQLPanelAdaptor *m_QLPanelAdaptor;
    CGFloat m_LastInspectorWidth;
    std::optional<nc::core::ExplorerTabsModel> m_TabsModel;
    std::vector<ExplorerTabEntry> m_TabEntries;
    nc::core::PaneStoreAdapter::ObservationTicket m_PaneStoreObservation;
    id m_ViewSettingsContextObservation;
    nc::core::ExplorerTabObservationGate m_ObservationGate;
    bool m_SynchronizingTabs;
    bool m_RuntimeTabsMutated;
    bool m_SessionRestoreApplied;
    std::optional<nc::core::PaneSnapshot> m_LatestPaneSnapshot;
    std::shared_ptr<nc::ops::Pool> m_OperationsPool;
}

@synthesize panelController = m_Panel;

- (instancetype)initWithFrame:(NSRect)frameRect operationsPool:(nc::ops::Pool &)_pool
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        ConfigureExplorerRootAccessibility(self);
        m_OperationsPool = _pool.shared_from_this();
        NCAppDelegate *const app = NCAppDelegate.me;
        m_Panel = [app allocateExplorerPanelController];
        if( !m_Panel || ![self attachExplorerTabPanel:m_Panel createPaneStore:true] )
            return nil;
        m_QLPanelAdaptor = [app QLPanelAdaptor];
        m_LastInspectorWidth = g_InspectorPreferredWidth;

        m_ToolbarDelegate = [[NCExplorerToolbarDelegate alloc] initWithPanelController:m_Panel];
        [self buildLayout];

        [self bindActivePanel:m_Panel focus:false];
        [self loadNativeHomeForSessionPanel:m_Panel];
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
    m_OperationProgressView = [[NCExplorerOperationProgressView alloc] initWithFrame:NSZeroRect];
    m_OperationProgressController = [[ExplorerOperationProgressController alloc] initWithPool:*m_OperationsPool
                                                                                          view:m_OperationProgressView];
    m_SearchModeView = [[NCExplorerSearchModeView alloc] initWithFrame:NSZeroRect];
    [self configureSearchViewHandlers];
    m_QuickSearchOverlay = [[NCExplorerQuickSearchOverlayView alloc] initWithFrame:NSZeroRect];
    m_TabbedHolder = [[FilePanelsTabbedHolder alloc] initWithFrame:NSZeroRect
                                           actionsShortcutsManager:app.actionsShortcutsManager
                                                     themesManager:app.themesManager];
    for( const ExplorerTabEntry &entry : m_TabEntries )
        [self attachExplorerTabViewForPanel:entry.panel];
    m_TabbedHolder.tabBar.delegate = self;
    m_TabbedHolder.tabBarShown = m_TabEntries.size() > 1;
    m_Inspector = [[NCExplorerInspectorView alloc]
                    initWithFrame:NSMakeRect(
                                      0, 0, g_InspectorPreferredWidth, self.bounds.size.height - g_CommandBarHeight)
                           paneID:m_Panel.paneId
                            UTIDB:app.utiDB
        QLHazardousExtensionsList:GlobalConfig().GetString(g_QuickLookHazardousExtensionsList)
                      QLVFSBridge:[app QLVFSBridge]];

    m_Sidebar.translatesAutoresizingMaskIntoConstraints = false;
    m_CommandBar.translatesAutoresizingMaskIntoConstraints = false;
    [self buildContentSplitWithPanelView:m_TabbedHolder inspectorView:m_Inspector];

    [self addSubview:m_Sidebar];
    [self addSubview:m_CommandBar];
    [self addSubview:m_OperationProgressView];
    [self addSubview:m_SearchModeView];
    [self addSubview:m_ContentSplitView];

    [NSLayoutConstraint activateConstraints:@[
        [m_Sidebar.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [m_Sidebar.topAnchor constraintEqualToAnchor:self.topAnchor],
        [m_Sidebar.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        [m_Sidebar.widthAnchor constraintEqualToConstant:g_SidebarWidth],

        [m_CommandBar.leadingAnchor constraintEqualToAnchor:m_Sidebar.trailingAnchor],
        [m_CommandBar.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [m_CommandBar.topAnchor constraintEqualToAnchor:self.topAnchor],
        [m_CommandBar.heightAnchor constraintEqualToConstant:g_CommandBarHeight],

        [m_OperationProgressView.leadingAnchor constraintEqualToAnchor:m_Sidebar.trailingAnchor],
        [m_OperationProgressView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [m_OperationProgressView.topAnchor constraintEqualToAnchor:m_CommandBar.bottomAnchor],

        [m_SearchModeView.leadingAnchor constraintEqualToAnchor:m_Sidebar.trailingAnchor],
        [m_SearchModeView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [m_SearchModeView.topAnchor constraintEqualToAnchor:m_OperationProgressView.bottomAnchor],

        [m_ContentSplitView.leadingAnchor constraintEqualToAnchor:m_Sidebar.trailingAnchor],
        [m_ContentSplitView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [m_ContentSplitView.topAnchor constraintEqualToAnchor:m_SearchModeView.bottomAnchor],
        [m_ContentSplitView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];

    [self layoutSubtreeIfNeeded];
    [self restoreInspectorWidth];
}

- (void)buildContentSplitWithPanelView:(NSView *)_panel_view inspectorView:(NCExplorerInspectorView *)_inspector
{
    m_PanelContainer = [[NSView alloc] initWithFrame:NSZeroRect];
    m_PanelContainer.accessibilityElement = true;
    m_PanelContainer.accessibilityRole = NSAccessibilityGroupRole;
    m_PanelContainer.accessibilityIdentifier = @"wincommander.explorer.panelContainer";
    m_PanelContainer.accessibilityLabel = NSLocalizedString(@"Files", "Explorer content accessibility label");

    _panel_view.translatesAutoresizingMaskIntoConstraints = false;
    [m_PanelContainer addSubview:_panel_view];
    [NSLayoutConstraint activateConstraints:@[
        [_panel_view.leadingAnchor constraintEqualToAnchor:m_PanelContainer.leadingAnchor],
        [_panel_view.trailingAnchor constraintEqualToAnchor:m_PanelContainer.trailingAnchor],
        [_panel_view.topAnchor constraintEqualToAnchor:m_PanelContainer.topAnchor],
        [_panel_view.bottomAnchor constraintEqualToAnchor:m_PanelContainer.bottomAnchor],
    ]];

    m_PaneStateView = [[NCExplorerPaneStateView alloc] initWithFrame:NSZeroRect];
    [m_PanelContainer addSubview:m_PaneStateView];
    [NSLayoutConstraint activateConstraints:@[
        [m_PaneStateView.leadingAnchor constraintEqualToAnchor:m_PanelContainer.leadingAnchor],
        [m_PaneStateView.trailingAnchor constraintEqualToAnchor:m_PanelContainer.trailingAnchor],
        [m_PaneStateView.topAnchor constraintEqualToAnchor:m_PanelContainer.topAnchor],
        [m_PaneStateView.bottomAnchor constraintEqualToAnchor:m_PanelContainer.bottomAnchor],
    ]];

    if( m_QuickSearchOverlay ) {
        m_QuickSearchOverlay.translatesAutoresizingMaskIntoConstraints = false;
        [m_PanelContainer addSubview:m_QuickSearchOverlay];
        [NSLayoutConstraint activateConstraints:@[
            [m_QuickSearchOverlay.topAnchor constraintEqualToAnchor:m_PanelContainer.topAnchor constant:9.0],
            [m_QuickSearchOverlay.trailingAnchor constraintEqualToAnchor:m_PanelContainer.trailingAnchor
                                                                constant:-12.0],
            [m_QuickSearchOverlay.widthAnchor constraintEqualToConstant:280.0],
            [m_QuickSearchOverlay.heightAnchor constraintEqualToConstant:32.0],
        ]];
    }

    const CGFloat split_width = std::max<CGFloat>(0.0, self.bounds.size.width - g_SidebarWidth);
    const CGFloat split_height = std::max<CGFloat>(0.0, self.bounds.size.height - g_CommandBarHeight);
    m_ContentSplitView = [[NSSplitView alloc] initWithFrame:NSMakeRect(0, 0, split_width, split_height)];
    m_ContentSplitView.translatesAutoresizingMaskIntoConstraints = false;
    m_ContentSplitView.vertical = true;
    m_ContentSplitView.dividerStyle = NSSplitViewDividerStyleThin;
    m_ContentSplitView.delegate = self;
    m_ContentSplitView.accessibilityElement = true;
    m_ContentSplitView.accessibilityRole = NSAccessibilitySplitGroupRole;
    m_ContentSplitView.accessibilityIdentifier = @"wincommander.explorer.contentSplit";
    m_ContentSplitView.accessibilityLabel =
        NSLocalizedString(@"File list and inspector", "Explorer split view accessibility label");

    m_PanelContainer.translatesAutoresizingMaskIntoConstraints = true;
    _inspector.translatesAutoresizingMaskIntoConstraints = true;
    const CGFloat inspector_width = std::min(g_InspectorPreferredWidth, split_width);
    const CGFloat divider = m_ContentSplitView.dividerThickness;
    m_PanelContainer.frame =
        NSMakeRect(0, 0, std::max<CGFloat>(0.0, split_width - inspector_width - divider), split_height);
    _inspector.frame = NSMakeRect(NSMaxX(m_PanelContainer.frame) + divider, 0, inspector_width, split_height);
    [m_ContentSplitView addSubview:m_PanelContainer];
    [m_ContentSplitView addSubview:_inspector];
    [m_ContentSplitView setHoldingPriority:NSLayoutPriorityDefaultHigh forSubviewAtIndex:1];
}

- (void)restoreInspectorWidth
{
    if( !m_ContentSplitView || m_Inspector.hidden || m_ContentSplitView.subviews.count != 2 )
        return;
    const CGFloat width = m_ContentSplitView.bounds.size.width;
    if( width <= 0.0 )
        return;
    const CGFloat desired = std::clamp(m_LastInspectorWidth, g_InspectorMinimumWidth, g_InspectorMaximumWidth);
    const CGFloat maximum_position =
        std::max<CGFloat>(0.0, width - g_InspectorMinimumWidth - m_ContentSplitView.dividerThickness);
    const CGFloat position = std::clamp(width - desired - m_ContentSplitView.dividerThickness, 0.0, maximum_position);
    [m_ContentSplitView setPosition:position ofDividerAtIndex:0];
}

- (CGFloat)splitView:(NSSplitView *)_split_view
    constrainMinCoordinate:(CGFloat) [[maybe_unused]] _proposed_minimum
               ofSubviewAt:(NSInteger)_divider_index
{
    if( _split_view != m_ContentSplitView || _divider_index != 0 )
        return _proposed_minimum;
    const CGFloat width = _split_view.bounds.size.width;
    const CGFloat divider = _split_view.dividerThickness;
    const CGFloat maximum_position = std::max<CGFloat>(0.0, width - g_InspectorMinimumWidth - divider);
    const CGFloat width_bound = std::max<CGFloat>(0.0, width - g_InspectorMaximumWidth - divider);
    return std::max(width_bound, std::min(g_PanelMinimumWidth, maximum_position));
}

- (CGFloat)splitView:(NSSplitView *)_split_view
    constrainMaxCoordinate:(CGFloat) [[maybe_unused]] _proposed_maximum
               ofSubviewAt:(NSInteger)_divider_index
{
    if( _split_view != m_ContentSplitView || _divider_index != 0 )
        return _proposed_maximum;
    return std::max<CGFloat>(0.0,
                             _split_view.bounds.size.width - g_InspectorMinimumWidth - _split_view.dividerThickness);
}

- (BOOL)splitView:(NSSplitView *)_split_view canCollapseSubview:(NSView *)_subview
{
    return _split_view == m_ContentSplitView && _subview == m_Inspector;
}

- (void)splitViewDidResizeSubviews:(NSNotification *)_notification
{
    if( _notification.object != m_ContentSplitView || m_Inspector.hidden )
        return;
    const CGFloat width = m_Inspector.frame.size.width;
    if( width >= g_InspectorMinimumWidth && width <= g_InspectorMaximumWidth )
        m_LastInspectorWidth = width;
}

#pragma mark - Explorer Search Mode

- (void)configureSearchViewHandlers
{
    if( !m_SearchModeView )
        return;
    __weak NCExplorerState *weak_self = self;
    [m_SearchModeView setStartHandler:[weak_self](nc::core::SearchRequest _request) {
      NCExplorerState *const state = weak_self;
      if( !state || !state->m_TabsModel || !state->m_Panel )
          return;
      ExplorerTabEntry *const entry = FindTabEntry(state->m_TabEntries, state->m_TabsModel->Active());
      if( entry && entry->panel == state->m_Panel && entry->search_controller.isPresented )
          [entry->search_controller startSearch:std::move(_request)];
    }];
    [m_SearchModeView setCancelHandler:[weak_self] {
      NCExplorerState *const state = weak_self;
      ExplorerTabEntry *const entry = state && state->m_TabsModel
                                          ? FindTabEntry(state->m_TabEntries, state->m_TabsModel->Active())
                                          : nullptr;
      if( entry && entry->panel == state->m_Panel )
          [entry->search_controller cancel];
    }];
    [m_SearchModeView setRevealOriginalHandler:[weak_self] {
      NCExplorerState *const state = weak_self;
      ExplorerTabEntry *const entry = state && state->m_TabsModel
                                          ? FindTabEntry(state->m_TabEntries, state->m_TabsModel->Active())
                                          : nullptr;
      if( entry && entry->panel == state->m_Panel )
          [entry->search_controller revealFocusedResult];
    }];
    [m_SearchModeView setCloseHandler:[weak_self] {
      NCExplorerState *const state = weak_self;
      ExplorerTabEntry *const entry = state && state->m_TabsModel
                                          ? FindTabEntry(state->m_TabEntries, state->m_TabsModel->Active())
                                          : nullptr;
      if( !entry || entry->panel != state->m_Panel )
          return;
      [entry->search_controller close];
      entry->panel.quickSearchPresentation = state->m_QuickSearchOverlay;
      [state.window makeFirstResponder:entry->panel.view];
    }];
}

- (std::optional<nc::core::SearchPlanningFacts>)searchPlanningFactsForPanel:(PanelController *)_panel
{
    if( !_panel || _panel != m_Panel || !m_TabsModel || m_TabsModel->Active() != _panel.paneId ||
        !m_LatestPaneSnapshot || m_LatestPaneSnapshot->pane_id != _panel.paneId ) {
        return std::nullopt;
    }
    const nc::core::PaneSnapshot &snapshot = *m_LatestPaneSnapshot;
    if( (snapshot.state.load_phase != nc::core::PaneLoadPhase::Loaded &&
         snapshot.state.load_phase != nc::core::PaneLoadPhase::Refreshing) ||
        !snapshot.state.is_uniform || !snapshot.state.host || !snapshot.state.listing || snapshot.state.path.empty() ||
        snapshot.state.path.front() != '/' || snapshot.state.path.back() != '/' ||
        snapshot.state.listing != _panel.data.ListingPtr() || snapshot.state.location_generation != _panel.dataGeneration ||
        !snapshot.state.listing->IsUniform() || snapshot.state.listing->Host() != snapshot.state.host ||
        snapshot.state.listing->Directory() != snapshot.state.path ) {
        return std::nullopt;
    }

    const nc::vfs::ProviderCapabilities capabilities =
        nc::vfs::ProviderCapabilitiesResolver::Resolve(*snapshot.state.host, snapshot.state.path);
    const bool native = capabilities.is_native;
    const std::optional<std::string> disk_root = native ? NativeVolumeRoot(snapshot.state.path) : std::nullopt;
    return nc::core::SearchPlanningFacts{
        .current_folder = snapshot.state.path,
        .current_disk_root = disk_root,
        .provider_available = capabilities.can_read,
        .provider_is_native = native,
        .provider_supports_recursive = capabilities.can_read,
        .provider_supports_current_disk = capabilities.can_read && disk_root.has_value(),
        .provider_supports_metadata = capabilities.can_read,
        .provider_supports_content = capabilities.can_read,
        .provider_supports_hidden_items = capabilities.can_read,
        .provider_reports_determinate_progress = false,
        .spotlight_available = native && capabilities.can_read,
        .spotlight_index_available = native && capabilities.can_read,
        .spotlight_supports_content = true,
        .full_disk_access = false,
    };
}

- (void)applySearchSnapshotForPane:(const nc::core::PaneId)_pane_id
{
    if( !m_SearchModeView || !m_TabsModel || !m_Panel || m_TabsModel->Active() != _pane_id ||
        m_Panel.paneId != _pane_id ) {
        return;
    }
    ExplorerTabEntry *const entry = FindTabEntry(m_TabEntries, _pane_id);
    if( !entry || entry->panel != m_Panel || !entry->search_controller )
        return;
    const std::optional<nc::core::SearchSnapshot> snapshot = entry->search_controller.snapshot;
    const bool presented = snapshot.has_value();
    entry->panel.quickSearchPresentation = presented ? nil : m_QuickSearchOverlay;
    if( presented )
        m_QuickSearchOverlay.searchPrompt = nil;
    [m_SearchModeView applySnapshot:snapshot
          resultSelectionEligible:static_cast<bool>(entry->search_controller.canRevealFocusedResult)];
}

- (BOOL)canPresentSearchForPanel:(PanelController *)_panel
{
    const ExplorerTabEntry *const entry = FindTabEntry(m_TabEntries, _panel);
    if( !entry || entry->panel != m_Panel || !m_TabsModel || m_TabsModel->Active() != _panel.paneId ||
        !entry->search_controller ) {
        return NO;
    }
    return entry->search_controller.isPresented || [self searchPlanningFactsForPanel:_panel].has_value();
}

- (BOOL)presentSearchForPanel:(PanelController *)_panel
                initialQuery:(NSString *)_query
              preferredScope:(const NCExplorerSearchPreferredScope)_scope
{
    ExplorerTabEntry *const entry = FindTabEntry(m_TabEntries, _panel);
    if( !entry || entry->panel != m_Panel || !m_TabsModel || m_TabsModel->Active() != _panel.paneId ||
        !entry->search_controller ) {
        return NO;
    }
    if( entry->search_controller.isPresented ) {
        [self applySearchSnapshotForPane:_panel.paneId];
        [m_SearchModeView focusQueryField];
        return YES;
    }

    std::optional<nc::core::SearchPlanningFacts> facts = [self searchPlanningFactsForPanel:_panel];
    if( !facts )
        return NO;
    nc::core::SearchRequest request;
    if( _query.length )
        request.query = _query.UTF8String ? _query.UTF8String : "";
    request.scope = _scope == NCExplorerSearchPreferredScopeSpotlightWholeMac
                        ? nc::core::SearchScope::SpotlightWholeMac
                        : nc::core::SearchScope::CurrentFolder;
    if( ![entry->search_controller presentWithPlanningFacts:std::move(*facts) initialRequest:std::move(request)] )
        return NO;
    [self applySearchSnapshotForPane:_panel.paneId];
    [m_SearchModeView focusQueryField];
    return YES;
}

#pragma mark - Explorer tabs

- (BOOL)attachExplorerTabPanel:(PanelController *)_panel createPaneStore:(const BOOL)_create_pane_store
{
    if( !_panel || _panel.paneId.value == 0 || FindTabEntry(m_TabEntries, _panel) ||
        m_TabEntries.size() >= nc::explorer::ExplorerSessionPersistency::MaximumTabs ||
        (m_TabsModel && m_TabsModel->Size() >= nc::explorer::ExplorerSessionPersistency::MaximumTabs) ) {
        return false;
    }

    std::unique_ptr<nc::panel::PanelControllerPaneStoreAdapter> pane_store;
    std::unique_ptr<nc::explorer::ExplorerViewSettingsBindingPolicy> view_settings_binding;
    __strong ExplorerSearchController *search_controller = nil;
    _panel.state = self;
    [_panel.view addKeystrokeSink:self];
    _panel.view.headerBarVisible = false;
    try {
        if( _create_pane_store )
            pane_store = std::make_unique<nc::panel::PanelControllerPaneStoreAdapter>(_panel);
        view_settings_binding =
            std::make_unique<nc::explorer::ExplorerViewSettingsBindingPolicy>(_panel.paneId);
        __weak NCExplorerState *weak_self = self;
        const nc::core::PaneId pane_id = _panel.paneId;
        search_controller = [[ExplorerSearchController alloc]
            initWithPanel:_panel
                   paneId:pane_id
          backendProvider:[](const nc::core::SearchBackendKind _kind)
              -> std::shared_ptr<nc::explorer::ExplorerSearchBackend> {
            switch( _kind ) {
                case nc::core::SearchBackendKind::DirectScan:
                    return std::make_shared<nc::explorer::ExplorerDirectSearchBackend>();
                case nc::core::SearchBackendKind::Spotlight:
                    return std::make_shared<nc::explorer::ExplorerSpotlightSearchBackend>();
            }
            return {};
          }
           snapshotHandler:[weak_self, pane_id](std::optional<nc::core::SearchSnapshot>) {
             NCExplorerState *const state = weak_self;
             if( state )
                 [state applySearchSnapshotForPane:pane_id];
           }];
        if( !search_controller )
            throw std::bad_alloc();
    } catch( ... ) {
        [_panel.view removeKeystrokeSink:self];
        _panel.state = nil;
        return false;
    }

    if( !m_TabsModel ) {
        auto model = nc::core::ExplorerTabsModel::Create(_panel.paneId);
        if( !model ) {
            [_panel.view removeKeystrokeSink:self];
            _panel.state = nil;
            return false;
        }
        m_TabsModel.emplace(std::move(*model));
    }
    else if( !m_TabsModel->Append(_panel.paneId) ) {
        [_panel.view removeKeystrokeSink:self];
        _panel.state = nil;
        return false;
    }

    m_TabEntries.push_back(ExplorerTabEntry{
        .panel = _panel,
        .search_controller = search_controller,
        .pane_store = std::move(pane_store),
        .view_settings_binding = std::move(view_settings_binding),
    });
    [self attachExplorerTabViewForPanel:_panel];
    [self invalidateExplorerRestorableState];
    return true;
}

- (void)attachExplorerTabViewForPanel:(PanelController *)_panel
{
    if( !_panel || !FindTabEntry(m_TabEntries, _panel) )
        return;
    if( m_TabbedHolder ) {
        if( [m_TabbedHolder tabViewItemForController:_panel] )
            return;
        [m_TabbedHolder addPanel:_panel.view];
        NSTabViewItem *const item = [m_TabbedHolder tabViewItemForController:_panel];
        item.identifier = @(_panel.paneId.value);
        item.label = [NSString stringWithUTF8StdString:TabNameForController(_panel)];
        return;
    }
    if( m_PanelContainer && _panel != m_Panel && !_panel.view.superview ) {
        _panel.view.frame = m_PanelContainer.bounds;
        _panel.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _panel.view.hidden = true;
        [m_PanelContainer addSubview:_panel.view positioned:NSWindowBelow relativeTo:m_QuickSearchOverlay];
    }
}

- (PanelController *)allocateExplorerPanelForSessionRestore
{
    return [NCAppDelegate.me allocateExplorerPanelController];
}

- (void)loadNativeHomeForSessionPanel:(PanelController *)_panel
{
    if( !_panel || !FindTabEntry(m_TabEntries, _panel) )
        return;
    auto request = std::make_shared<nc::panel::DirectoryChangeRequest>();
    request->VFS = nc::bootstrap::NativeVFSHostInstance().SharedPtr();
    request->RequestedDirectory = nc::base::CommonPaths::Home();
    request->PerformAsynchronous = true;
    [[maybe_unused]] const auto submission = [_panel GoToDirWithContext:std::move(request)];
}

- (void)restoreSessionLocation:(const std::optional<nc::panel::PersistentLocation> &)_location
                       forPanel:(PanelController *)_panel
{
    if( !_panel || !FindTabEntry(m_TabEntries, _panel) || !_location ||
        !IsStructurallyValidSessionLocation(*_location) ) {
        [self loadNativeHomeForSessionPanel:_panel];
        return;
    }

    const nc::core::PaneId pane_id = _panel.paneId;
    const std::string path = _location->path;
    __weak NCExplorerState *weak_self = self;
    __weak PanelController *weak_panel = _panel;
    const auto is_owned = [weak_self, weak_panel, pane_id] {
        NCExplorerState *const state = weak_self;
        PanelController *const panel = weak_panel;
        ExplorerTabEntry *const entry = state && panel ? FindTabEntry(state->m_TabEntries, pane_id) : nullptr;
        return entry && entry->panel == panel;
    };

    nc::panel::actions::AsyncPersistentLocationRestorer restorer(
        _panel, _panel.vfsInstanceManager, *NCAppDelegate.me.networkConnectionsManager);
    restorer.Restore(
        *_location,
        [weak_self, weak_panel, path, is_owned](VFSHostPtr _host) {
            NCExplorerState *const state = weak_self;
            PanelController *const panel = weak_panel;
            if( !state || !panel || !_host || !is_owned() )
                return;

            auto request = std::make_shared<nc::panel::DirectoryChangeRequest>();
            request->VFS = std::move(_host);
            request->RequestedDirectory = path;
            request->PerformAsynchronous = true;
            request->LoadingResultCallback = [weak_self, weak_panel, is_owned](
                                                 const std::expected<void, nc::Error> &_result,
                                                 nc::panel::DirectoryChangeResultSource,
                                                 const std::function<bool()> &_is_current) {
                if( _result || !_is_current() )
                    return;
                dispatch_to_main_queue([weak_self, weak_panel, is_owned, is_current = _is_current] {
                    NCExplorerState *const live_state = weak_self;
                    PanelController *const live_panel = weak_panel;
                    if( live_state && live_panel && is_current() && is_owned() )
                        [live_state loadNativeHomeForSessionPanel:live_panel];
                });
            };
            // AsyncPersistentLocationRestorer's commit is already fenced by the controller's
            // content-intent generation. GoToDirWithContext installs the second fence used by the
            // load/fallback callback, so a later user navigation retires both delayed stages.
            [[maybe_unused]] const auto submission = [panel GoToDirWithContext:std::move(request)];
        },
        [weak_self, weak_panel, is_owned](nc::Error) {
            NCExplorerState *const state = weak_self;
            PanelController *const panel = weak_panel;
            if( state && panel && is_owned() )
                [state loadNativeHomeForSessionPanel:panel];
        },
        {.allow_password_ui = false});
}

- (void)invalidateExplorerRestorableState
{
    if( auto controller = nc::objc_cast<NCMainWindowController>(self.window.delegate) )
        [controller invalidateRestorableState];
}

- (void)rollbackSessionRestorePanels
{
    if( m_TabEntries.empty() )
        return;
    PanelController *const initial_panel = m_TabEntries.front().panel;
    for( auto iterator = std::next(m_TabEntries.begin()); iterator != m_TabEntries.end(); ++iterator ) {
        PanelController *const panel = iterator->panel;
        [iterator->search_controller close];
        if( m_TabbedHolder ) {
            if( NSTabViewItem *const item = [m_TabbedHolder tabViewItemForController:panel] )
                [m_TabbedHolder.tabView removeTabViewItem:item];
            else
                [panel.view removeFromSuperview];
        }
        else {
            [panel.view removeFromSuperview];
        }
        [panel.view removeKeystrokeSink:self];
        panel.state = nil;
    }
    m_TabEntries.erase(std::next(m_TabEntries.begin()), m_TabEntries.end());

    auto model = nc::core::ExplorerTabsModel::Create(initial_panel.paneId);
    if( model )
        m_TabsModel.emplace(std::move(*model));
    if( m_TabbedHolder ) {
        m_TabbedHolder.tabBarShown = false;
        if( NSTabViewItem *const item = [m_TabbedHolder tabViewItemForController:initial_panel] )
            [m_TabbedHolder.tabBar selectTabViewItem:item];
    }
    [self invalidateExplorerRestorableState];
}

- (BOOL)restoreTabsFromSession:(const nc::explorer::ExplorerTabsSession &)_session
{
    if( m_SessionRestoreApplied || m_RuntimeTabsMutated || !m_TabsModel || m_TabsModel->Size() != 1 ||
        m_TabEntries.size() != 1 || _session.tabs.empty() ||
        _session.tabs.size() > nc::explorer::ExplorerSessionPersistency::MaximumTabs ||
        _session.active_index >= _session.tabs.size() ) {
        return false;
    }

    std::vector<__strong PanelController *> panels;
    panels.reserve(_session.tabs.size());
    panels.emplace_back(m_TabEntries.front().panel);
    std::unordered_set<uint64_t> pane_ids{panels.front().paneId.value};
    for( size_t index = 1; index < _session.tabs.size(); ++index ) {
        PanelController *const panel = [self allocateExplorerPanelForSessionRestore];
        if( !panel || panel.paneId.value == 0 || !pane_ids.emplace(panel.paneId.value).second )
            return false;
        panels.emplace_back(panel);
    }

    const BOOL create_pane_store = m_TabEntries.front().pane_store != nullptr;
    m_SynchronizingTabs = true;
    for( size_t index = 1; index < panels.size(); ++index ) {
        if( ![self attachExplorerTabPanel:panels[index] createPaneStore:create_pane_store] ) {
            [self rollbackSessionRestorePanels];
            m_SynchronizingTabs = false;
            return false;
        }
    }

    FilePanelsTabbedHolder *const tabbed_holder = m_TabbedHolder;
    if( tabbed_holder &&
        std::ranges::any_of(panels, [tabbed_holder](PanelController *_panel) {
            return [tabbed_holder tabViewItemForController:_panel] == nil;
        }) ) {
        [self rollbackSessionRestorePanels];
        m_SynchronizingTabs = false;
        return false;
    }

    PanelController *const active_panel = panels[_session.active_index];
    if( !m_TabsModel->Activate(active_panel.paneId) ) {
        [self rollbackSessionRestorePanels];
        m_SynchronizingTabs = false;
        return false;
    }
    if( m_TabbedHolder ) {
        NSTabViewItem *const active_item = [m_TabbedHolder tabViewItemForController:active_panel];
        if( !active_item ) {
            [self rollbackSessionRestorePanels];
            m_SynchronizingTabs = false;
            return false;
        }
        m_TabbedHolder.tabBarShown = panels.size() > 1;
        [m_TabbedHolder.tabBar selectTabViewItem:active_item];
    }
    m_SynchronizingTabs = false;

    m_SessionRestoreApplied = true;
    [self bindActivePanel:active_panel focus:false];
    for( size_t index = 0; index < panels.size(); ++index )
        [self restoreSessionLocation:_session.tabs[index].location forPanel:panels[index]];
    [self invalidateExplorerRestorableState];
    return true;
}

- (nc::explorer::ExplorerTabsSession)captureTabsSession
{
    nc::explorer::ExplorerTabsSession session;
    if( !m_TabsModel || m_TabEntries.size() != m_TabsModel->Size() )
        return session;

    session.active_index = m_TabsModel->ActiveIndex();
    session.tabs.reserve(m_TabEntries.size());
    for( const ExplorerTabEntry &entry : m_TabEntries ) {
        nc::explorer::ExplorerSessionTab tab;
        if( entry.panel && entry.pane_store ) {
            const nc::core::PaneSnapshot snapshot = entry.pane_store->Store().Snapshot();
            const bool exact = snapshot.pane_id == entry.panel.paneId && snapshot.state.is_uniform &&
                               snapshot.state.host && !snapshot.state.path.empty() &&
                               snapshot.state.path.front() == '/' && snapshot.state.path.back() == '/' &&
                               snapshot.state.location_generation == entry.panel.dataGeneration &&
                               snapshot.state.listing == entry.panel.data.ListingPtr();
            if( exact ) {
                tab.location = NCAppDelegate.me.panelDataPersistency.EncodeLocation(*snapshot.state.host,
                                                                                    snapshot.state.path);
            }
        }
        session.tabs.emplace_back(std::move(tab));
    }
    return session;
}

- (void)applyActivePaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot
               observationToken:(const nc::core::ExplorerTabObservationToken)_token
{
    if( !m_TabsModel || !m_Panel || !m_ObservationGate.Accepts(_token, m_TabsModel->Active(), _snapshot.pane_id) ||
        _snapshot.pane_id != m_Panel.paneId )
        return;

    ExplorerTabEntry *const entry = FindTabEntry(m_TabEntries, _snapshot.pane_id);
    if( !entry || entry->panel != m_Panel )
        return;
    if( entry->last_active_snapshot_revision && _snapshot.revision < *entry->last_active_snapshot_revision )
        return;
    entry->last_active_snapshot_revision =
        std::max(entry->last_active_snapshot_revision.value_or(0), _snapshot.revision);

    [entry->search_controller synchronizeExternalContentChange];
    m_LatestPaneSnapshot = _snapshot;
    [self applyViewSettingsForSnapshot:_snapshot tabEntry:*entry];
    [m_ToolbarDelegate applyPaneSnapshot:_snapshot];
    [m_CommandBar applyPaneSnapshot:_snapshot];
    [m_Panel.view applyExplorerPaneSnapshot:_snapshot];
    [m_Inspector applyPaneSnapshot:_snapshot];
    [m_PaneStateView updateWithVisualState:ExplorerPaneVisualState(_snapshot, m_Panel)];
}

- (void)applyViewSettingsForSnapshot:(const nc::core::PaneSnapshot &)_snapshot
                            tabEntry:(ExplorerTabEntry &)_entry
{
    if( !_entry.view_settings_binding || _entry.panel != m_Panel || _snapshot.pane_id != _entry.panel.paneId )
        return;

    auto settings = CaptureExplorerViewSettings(_entry.panel, _snapshot);
    nc::explorer::ExplorerViewSettingsObservation observation{
        .pane_id = _snapshot.pane_id,
        .observation_sequence = ++_entry.view_settings_observation_sequence,
        .revision = _snapshot.revision,
        .location_generation = _snapshot.state.location_generation,
        .load_phase = _snapshot.state.load_phase,
        .is_uniform = _snapshot.state.is_uniform,
        .host_identity = _snapshot.state.host.get(),
        .path = _snapshot.state.path,
        .settings = std::move(settings),
    };

    using Action = nc::explorer::ExplorerViewSettingsBindingAction;
    const Action action = _entry.view_settings_binding->Observe(observation);
    if( action == Action::Rejected || action == Action::None || action == Action::RestoreSettled )
        return;

    NCAppDelegate *const app = NCAppDelegate.me;
    const auto location = _snapshot.state.host
                              ? app.panelDataPersistency.EncodeLocation(*_snapshot.state.host, _snapshot.state.path)
                              : std::nullopt;
    if( !location || !observation.settings )
        return;

    auto &persistence = app.explorerViewSettingsPersistence;
    if( action == Action::LoadLocation ) {
        const auto persisted = persistence.Load(*location);
        if( !persisted ) {
            if( !ApplyExplorerViewSettings(_entry.panel, *observation.settings) )
                return;
            if( persistence.Store(*location, *observation.settings) ) {
                [[maybe_unused]] const bool accepted = _entry.view_settings_binding->AcceptCurrent(observation);
            }
            return;
        }
        if( *persisted == *observation.settings ) {
            if( !ApplyExplorerViewSettings(_entry.panel, *observation.settings) )
                return;
            [[maybe_unused]] const bool accepted = _entry.view_settings_binding->AcceptCurrent(observation);
            return;
        }
        if( ApplyExplorerViewSettings(_entry.panel, *persisted) ) {
            [[maybe_unused]] const bool restoring = _entry.view_settings_binding->BeginRestore(*persisted);
            return;
        }

        // The record remains intact when its configured slot/type is no longer applicable. The
        // valid live layout still becomes pane-local so a resize cannot mutate a shared preset.
        [[maybe_unused]] const bool materialized =
            ApplyExplorerViewSettings(_entry.panel, *observation.settings);
        [[maybe_unused]] const bool accepted = _entry.view_settings_binding->AcceptCurrent(observation);
        return;
    }

    if( action == Action::StoreCurrent || action == Action::RestoreDiverged ) {
        if( !ApplyExplorerViewSettings(_entry.panel, *observation.settings) )
            return;
        if( persistence.Store(*location, *observation.settings) ) {
            [[maybe_unused]] const bool accepted = _entry.view_settings_binding->AcceptCurrent(observation);
        }
    }
}

- (void)bindActivePanel:(PanelController *)_panel focus:(const BOOL)_focus
{
    ExplorerTabEntry *const entry = FindTabEntry(m_TabEntries, _panel);
    if( !entry || !m_TabsModel || m_TabsModel->Active() != _panel.paneId )
        return;

    PanelController *const previous = m_Panel;
    m_ObservationGate.Invalidate();
    m_PaneStoreObservation = {};
    if( m_ViewSettingsContextObservation != nil ) {
        [NSNotificationCenter.defaultCenter removeObserver:m_ViewSettingsContextObservation];
        m_ViewSettingsContextObservation = nil;
    }
    m_LatestPaneSnapshot.reset();
    [m_PaneStateView updateWithVisualState:nc::core::PaneVisualState{}];

    if( previous && previous != _panel ) {
        [self closeAttachedUI:previous];
        previous.quickSearchPresentation = nil;
        previous.view.busyIndicatorOverride = nil;
        m_QuickSearchOverlay.searchPrompt = nil;
        if( !m_TabbedHolder ) {
            previous.view.hidden = true;
            _panel.view.hidden = false;
        }
    }

    m_Panel = _panel;
    const auto token = m_ObservationGate.Bind(_panel.paneId);
    if( !token )
        return;
    [m_ToolbarDelegate rebindToPanelController:_panel];
    [m_Sidebar rebindToPanelController:_panel];
    [m_CommandBar rebindToPanelController:_panel];
    [m_Inspector rebindToPaneID:_panel.paneId];
    _panel.view.busyIndicatorOverride = m_ToolbarDelegate.busyIndicator;
    _panel.quickSearchPresentation = m_QuickSearchOverlay;

    if( entry->pane_store ) {
        __weak NCExplorerState *weak_self = self;
        m_PaneStoreObservation =
            entry->pane_store->Store().Observe([weak_self, token = *token](const nc::core::PaneSnapshot &_snapshot) {
                NCExplorerState *const strong_self = weak_self;
                if( strong_self )
                    [strong_self applyActivePaneSnapshot:_snapshot observationToken:token];
            });
        [self applyActivePaneSnapshot:entry->pane_store->Store().Snapshot() observationToken:*token];
    }

    __weak NCExplorerState *weak_self = self;
    __weak PanelController *weak_panel = _panel;
    m_ViewSettingsContextObservation =
        [NSNotificationCenter.defaultCenter addObserverForName:NCPanelViewContextDidChangeNotification
                                                        object:_panel.view
                                                         queue:nil
                                                    usingBlock:^(__unused NSNotification *_notification) {
                                                      NCExplorerState *const strong_self = weak_self;
                                                      PanelController *const strong_panel = weak_panel;
                                                      if( !strong_self || !strong_panel )
                                                          return;
                                                      ExplorerTabEntry *const live_entry =
                                                          FindTabEntry(strong_self->m_TabEntries, strong_panel);
                                                      if( !live_entry )
                                                          return;
                                                      if( strong_self->m_Panel == strong_panel &&
                                                          strong_self->m_LatestPaneSnapshot &&
                                                          strong_self->m_LatestPaneSnapshot->pane_id ==
                                                              strong_panel.paneId ) {
                                                          const nc::core::PaneSnapshot &snapshot =
                                                              *strong_self->m_LatestPaneSnapshot;
                                                          [strong_self->m_PaneStateView
                                                              updateWithVisualState:
                                                                  ExplorerPaneVisualState(snapshot, strong_panel)];
                                                      }
                                                      if( live_entry->view_settings_context_sample_scheduled )
                                                          return;
                                                      live_entry->view_settings_context_sample_scheduled = true;
                                                      dispatch_async(dispatch_get_main_queue(), ^{
                                                        // PaneStore and this observer see the same context notification,
                                                        // but observer order is not contractual. A second hop lets its
                                                        // coalesced rebuild publish sort/group before we combine that
                                                        // snapshot with the concrete layout sampled from PanelView.
                                                        dispatch_async(dispatch_get_main_queue(), ^{
                                                          NCExplorerState *const state = weak_self;
                                                          PanelController *const panel = weak_panel;
                                                          if( !state || !panel )
                                                              return;
                                                          ExplorerTabEntry *const scheduled_entry =
                                                              FindTabEntry(state->m_TabEntries, panel);
                                                          if( !scheduled_entry )
                                                              return;
                                                          scheduled_entry->view_settings_context_sample_scheduled = false;
                                                          if( !state->m_TabsModel ||
                                                              !state->m_ObservationGate.Accepts(
                                                                  *token,
                                                                  state->m_TabsModel->Active(),
                                                                  panel.paneId) ||
                                                              state->m_Panel != panel ||
                                                              !state->m_LatestPaneSnapshot ||
                                                              state->m_LatestPaneSnapshot->pane_id != panel.paneId ) {
                                                              return;
                                                          }
                                                          [state applyViewSettingsForSnapshot:
                                                                     *state->m_LatestPaneSnapshot
                                                                                    tabEntry:*scheduled_entry];
                                                        });
                                                      });
                                                    }];

    [self applySearchSnapshotForPane:_panel.paneId];
    if( _focus )
        [self.window makeFirstResponder:_panel.view];
}

- (void)updateTabLabelForPanel:(PanelController *)_panel
{
    if( !FindTabEntry(m_TabEntries, _panel) || !m_TabbedHolder )
        return;
    NSTabViewItem *const item = [m_TabbedHolder tabViewItemForController:_panel];
    if( item )
        item.label = [NSString stringWithUTF8StdString:TabNameForController(_panel)];
}

- (void)closeTabViewItem:(NSTabViewItem *)_item
{
    const std::optional<nc::core::PaneId> pane_id = PaneIdFromTabItem(_item);
    if( !pane_id || !m_TabsModel || m_TabsModel->Size() <= 1 )
        return;

    ExplorerTabEntry *const entry = FindTabEntry(m_TabEntries, *pane_id);
    if( !entry || _item.view != entry->panel.view )
        return;

    PanelController *const closing_panel = entry->panel;
    [entry->search_controller close];
    const bool closing_active = m_TabsModel->Active() == *pane_id;
    const auto result = m_TabsModel->Close(*pane_id);
    if( !result )
        return;
    m_RuntimeTabsMutated = true;

    if( closing_active ) {
        m_ObservationGate.Invalidate();
        m_PaneStoreObservation = {};
        [self closeAttachedUI:closing_panel];
        closing_panel.quickSearchPresentation = nil;
        closing_panel.view.busyIndicatorOverride = nil;
        m_QuickSearchOverlay.searchPrompt = nil;
    }

    [closing_panel.view removeKeystrokeSink:self];
    closing_panel.state = nil;
    std::erase_if(m_TabEntries, [pane_id](const ExplorerTabEntry &_entry) { return _entry.panel.paneId == *pane_id; });
    m_TabbedHolder.tabBarShown = m_TabsModel->Size() > 1;
    [self invalidateExplorerRestorableState];

    if( closing_active ) {
        ExplorerTabEntry *const active = FindTabEntry(m_TabEntries, m_TabsModel->Active());
        if( !active )
            return;
        NSTabViewItem *const active_item = [m_TabbedHolder tabViewItemForController:active->panel];
        m_SynchronizingTabs = true;
        [m_TabbedHolder.tabBar selectTabViewItem:active_item];
        m_SynchronizingTabs = false;
        [self bindActivePanel:active->panel focus:true];
    }
}

- (IBAction)OnFileNewTab:(id) [[maybe_unused]] _sender
{
    if( !m_TabsModel || !m_TabbedHolder || !m_Panel ||
        m_TabsModel->Size() >= nc::explorer::ExplorerSessionPersistency::MaximumTabs ) {
        return;
    }

    PanelController *const source = m_Panel;
    PanelController *const panel = [NCAppDelegate.me allocateExplorerPanelController];
    if( !panel )
        return;
    [panel copyOptionsFromController:source];
    if( ![self attachExplorerTabPanel:panel createPaneStore:true] )
        return;

    m_SynchronizingTabs = true;
    NSTabViewItem *const item = [m_TabbedHolder tabViewItemForController:panel];
    m_TabbedHolder.tabBarShown = true;
    [m_TabbedHolder.tabBar selectTabViewItem:item];
    m_SynchronizingTabs = false;
    m_RuntimeTabsMutated = true;

    const VFSListingPtr listing = source.data.ListingPtr();
    if( listing )
        [panel loadListing:listing];
    [self updateTabLabelForPanel:panel];
    [self bindActivePanel:panel focus:true];
    if( !listing )
        [self loadNativeHomeForSessionPanel:panel];
}

- (IBAction)performClose:(id)_sender
{
    if( !m_TabsModel || m_TabsModel->Size() == 1 ) {
        [self.window performClose:_sender];
        return;
    }

    ExplorerTabEntry *const active = FindTabEntry(m_TabEntries, m_TabsModel->Active());
    NSTabViewItem *const item = active ? [m_TabbedHolder tabViewItemForController:active->panel] : nil;
    if( !item )
        return;
    m_SynchronizingTabs = true;
    [m_TabbedHolder.tabBar removeTabViewItem:item];
    m_SynchronizingTabs = false;
    [self closeTabViewItem:item];
}

- (IBAction)OnWindowShowPreviousTab:(id) [[maybe_unused]] _sender
{
    [m_TabbedHolder selectPreviousFilePanelTab];
}

- (IBAction)OnWindowShowNextTab:(id) [[maybe_unused]] _sender
{
    [m_TabbedHolder selectNextFilePanelTab];
}

- (BOOL)validateMenuItem:(NSMenuItem *)_item
{
    if( _item.action == @selector(OnFileNewTab:) )
        return m_TabsModel && m_TabsModel->Size() < nc::explorer::ExplorerSessionPersistency::MaximumTabs;
    if( _item.action == @selector(performClose:) ) {
        const bool closes_window = !m_TabsModel || m_TabsModel->Size() == 1;
        _item.title = closes_window ? NSLocalizedString(@"Close Window", "Explorer File menu")
                                    : NSLocalizedString(@"Close Tab", "Explorer File menu");
        return true;
    }
    if( _item.action == @selector(OnWindowShowPreviousTab:) || _item.action == @selector(OnWindowShowNextTab:) )
        return m_TabsModel && m_TabsModel->Size() > 1;
    return false;
}

- (void)addNewTabToTabView:(NSTabView *)_tab_view
{
    if( _tab_view == m_TabbedHolder.tabView )
        [self OnFileNewTab:m_TabbedHolder.tabBar];
}

- (void)tabView:(NSTabView *)_tab_view didSelectTabViewItem:(NSTabViewItem *)_item
{
    if( m_SynchronizingTabs || _tab_view != m_TabbedHolder.tabView || !m_TabsModel )
        return;

    const ExplorerTabEntry *const current = FindTabEntry(m_TabEntries, m_TabsModel->Active());
    if( current && ![m_TabbedHolder tabViewItemForController:current->panel] )
        return;

    const std::optional<nc::core::PaneId> pane_id = PaneIdFromTabItem(_item);
    ExplorerTabEntry *const entry = pane_id ? FindTabEntry(m_TabEntries, *pane_id) : nullptr;
    if( !entry || _item.view != entry->panel.view )
        return;
    if( m_TabsModel->Active() == *pane_id ) {
        [self.window makeFirstResponder:entry->panel.view];
        return;
    }
    if( !m_TabsModel->Activate(*pane_id) )
        return;
    m_RuntimeTabsMutated = true;
    [self invalidateExplorerRestorableState];
    [self bindActivePanel:entry->panel focus:true];
}

- (void)tabView:(NSTabView *)_tab_view receivedClickOnSelectedTabViewItem:(NSTabViewItem *)_item
{
    if( _tab_view != m_TabbedHolder.tabView || _item != _tab_view.selectedTabViewItem )
        return;
    const std::optional<nc::core::PaneId> pane_id = PaneIdFromTabItem(_item);
    ExplorerTabEntry *const entry = pane_id ? FindTabEntry(m_TabEntries, *pane_id) : nullptr;
    if( entry && m_TabsModel && m_TabsModel->Active() == *pane_id && _item.view == entry->panel.view )
        [self.window makeFirstResponder:entry->panel.view];
}

- (void)tabView:(NSTabView *)_tab_view didCloseTabViewItem:(NSTabViewItem *)_item
{
    if( _tab_view == m_TabbedHolder.tabView )
        [self closeTabViewItem:_item];
}

- (void)tabView:(NSTabView *)_tab_view
    didDropTabViewItem:(NSTabViewItem *)_item
          inTabBarView:(NCPanelTabBarView *)_tab_bar
{
    if( _tab_view != m_TabbedHolder.tabView || _tab_bar != m_TabbedHolder.tabBar || !m_TabsModel )
        return;
    const NSUInteger index = [_tab_view indexOfTabViewItem:_item];
    if( index != NSNotFound )
        [self reorderOwnedTabViewItem:_item toIndex:index];
}

- (void)tabView:(NSTabView *)_tab_view didMoveTabViewItem:(NSTabViewItem *)_item toIndex:(NSUInteger)_index
{
    if( _tab_view == m_TabbedHolder.tabView )
        [self reorderOwnedTabViewItem:_item toIndex:_index];
}

- (void)reorderOwnedTabViewItem:(NSTabViewItem *)_item toIndex:(const NSUInteger)_index
{
    if( !m_TabsModel || _index >= m_TabEntries.size() )
        return;

    const std::optional<nc::core::PaneId> pane_id = PaneIdFromTabItem(_item);
    ExplorerTabEntry *const owned = pane_id ? FindTabEntry(m_TabEntries, *pane_id) : nullptr;
    if( !owned || _item.view != owned->panel.view )
        return;

    const auto iterator =
        std::ranges::find(m_TabEntries, *pane_id, [](const ExplorerTabEntry &_entry) { return _entry.panel.paneId; });
    if( iterator == m_TabEntries.end() || !m_TabsModel->Reorder(*pane_id, _index) )
        return;
    ExplorerTabEntry moving = std::move(*iterator);
    m_TabEntries.erase(iterator);
    m_TabEntries.insert(m_TabEntries.begin() + static_cast<std::ptrdiff_t>(_index), std::move(moving));
    m_RuntimeTabsMutated = true;
    [self invalidateExplorerRestorableState];
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

- (void)windowStateDidResign
{
    [self closeAttachedUI:m_Panel];
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

#pragma mark - NCExplorerInspectorPresenting

- (BOOL)presentFileGetInfo:(const nc::core::FileGetInfoPresentation &)_presentation forPanel:(PanelController *)_panel
{
    if( _panel != m_Panel || !m_LatestPaneSnapshot || _presentation.items.empty() )
        return false;

    nc::core::PaneSnapshot presentation_snapshot = *m_LatestPaneSnapshot;
    if( _presentation.items.size() == 1 ) {
        const auto exact_item = FindExactListingItem(presentation_snapshot, _presentation.items.front());
        if( !exact_item || !presentation_snapshot.state.listing ||
            _panel.data.ListingPtr() != presentation_snapshot.state.listing )
            return false;
        const int sort_position = _panel.data.SortPositionOfEntry(*exact_item);
        if( sort_position < 0 )
            return false;

        const uint64_t revision = presentation_snapshot.revision;
        const uint64_t listing_generation = presentation_snapshot.listing_generation;
        _panel.view.curpos = sort_position;

        if( !m_LatestPaneSnapshot || m_LatestPaneSnapshot->revision != revision ||
            m_LatestPaneSnapshot->listing_generation != listing_generation ||
            m_LatestPaneSnapshot->state.listing != presentation_snapshot.state.listing ||
            _panel.data.ListingPtr() != presentation_snapshot.state.listing || _panel.view.curpos != sort_position )
            return false;
        const nc::vfs::ListingItem focused = _panel.view.item;
        if( !focused || focused.Listing() != exact_item->Listing() || focused.Index() != exact_item->Index() )
            return false;

        presentation_snapshot.state.focused_item = focused;
        presentation_snapshot.state.selected_items = {};
        presentation_snapshot.state.selected_count = 0;
        presentation_snapshot.state.selected_bytes = 0;
    }
    else {
        const std::vector<nc::core::FileMetadataSnapshot> live_metadata = LiveInspectorMetadata(presentation_snapshot);
        if( live_metadata.empty() || live_metadata != _presentation.items ||
            presentation_snapshot.state.selected_items.empty() )
            return false;
    }

    if( m_Inspector.hidden ) {
        if( ![self setPreviewPaneVisible:true expected:false forPanel:_panel] )
            return false;
    }
    if( ![m_Inspector applyPaneSnapshot:presentation_snapshot] ) {
        return false;
    }
    return true;
}

- (BOOL)previewPaneVisibleForPanel:(PanelController *)_panel
{
    return _panel == m_Panel && m_Inspector != nil && !m_Inspector.hidden;
}

- (BOOL)setPreviewPaneVisible:(BOOL)_desired expected:(BOOL)_expected forPanel:(PanelController *)_panel
{
    if( _panel != m_Panel || !m_Inspector )
        return false;
    const BOOL current = !m_Inspector.hidden;
    if( current != _expected )
        return false;
    if( current == _desired )
        return true;

    if( _desired ) {
        if( !m_LatestPaneSnapshot || ![m_Inspector applyPaneSnapshot:*m_LatestPaneSnapshot] )
            return false;
        m_Inspector.hidden = false;
        [m_ContentSplitView adjustSubviews];
        [self restoreInspectorWidth];
    }
    else {
        const CGFloat width = m_Inspector.frame.size.width;
        if( width >= g_InspectorMinimumWidth && width <= g_InspectorMaximumWidth )
            m_LastInspectorWidth = width;
        [m_Inspector clearPreview];
        m_Inspector.hidden = true;
        [m_ContentSplitView adjustSubviews];
    }
    return [self previewPaneVisibleForPanel:_panel] == _desired;
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

- (bool)isLeftController:(PanelController *)_controller
{
    return FindTabEntry(m_TabEntries, _controller) != nullptr;
}

- (bool)isRightController:(PanelController *) [[maybe_unused]] _controller
{
    return false;
}

- (void)closeAttachedUI:(PanelController *)_panel
{
    if( _panel != m_Panel || !FindTabEntry(m_TabEntries, _panel) || m_QLPanelAdaptor.owner != self )
        return;
    if( QLPreviewPanel.sharedPreviewPanelExists && QLPreviewPanel.sharedPreviewPanel.isVisible )
        [QLPreviewPanel.sharedPreviewPanel orderOut:nil];
}

- (void)PanelPathChanged:(PanelController *)_panel
{
    ExplorerTabEntry *const entry = FindTabEntry(m_TabEntries, _panel);
    if( !entry )
        return;
    [entry->search_controller synchronizeExternalContentChange];
    [self updateTabLabelForPanel:_panel];
    if( _panel == m_Panel )
        [m_Sidebar panelPathChanged];
    [self invalidateExplorerRestorableState];
}

- (void)activePanelChangedTo:(PanelController *)_controller
{
    if( FindTabEntry(m_TabEntries, _controller) )
        [self ActivatePanelByController:_controller];
}

- (void)ActivatePanelByController:(PanelController *)_controller
{
    ExplorerTabEntry *const entry = FindTabEntry(m_TabEntries, _controller);
    if( !entry || !m_TabsModel )
        return;
    NSTabViewItem *const item = [m_TabbedHolder tabViewItemForController:_controller];
    if( m_TabbedHolder && !item )
        return;
    if( m_TabsModel->Active() == _controller.paneId ) {
        [self.window makeFirstResponder:_controller.view];
        return;
    }
    if( !m_TabsModel->Activate(_controller.paneId) )
        return;
    m_RuntimeTabsMutated = true;
    [self invalidateExplorerRestorableState];
    m_SynchronizingTabs = true;
    if( item )
        [m_TabbedHolder.tabBar selectTabViewItem:item];
    m_SynchronizingTabs = false;
    [self bindActivePanel:entry->panel focus:true];
}

- (BriefSystemOverview *)briefSystemOverviewForPanel:(PanelController *) [[maybe_unused]] _panel
                                                make:(bool) [[maybe_unused]] _make_if_absent
{
    return nil;
}

- (id<NCPanelPreview>)quickLookForPanel:(PanelController *)_panel make:(bool)_make_if_absent
{
    if( _panel != m_Panel || !FindTabEntry(m_TabEntries, _panel) || !m_QLPanelAdaptor )
        return nil;

    if( QLPreviewPanel.sharedPreviewPanelExists && QLPreviewPanel.sharedPreviewPanel.isVisible ) {
        return m_QLPanelAdaptor.owner == self ? m_QLPanelAdaptor : nil;
    }
    if( !_make_if_absent )
        return nil;

    [QLPreviewPanel.sharedPreviewPanel makeKeyAndOrderFront:nil];
    return m_QLPanelAdaptor.owner == self ? m_QLPanelAdaptor : nil;
}

- (void)requestTerminalExecution:(const std::string &) [[maybe_unused]] _filename
                              at:(const std::string &) [[maybe_unused]] _cwd
{
    NSBeep();
}

- (BOOL)acceptsPreviewPanelControl:(QLPreviewPanel *) [[maybe_unused]] _panel
{
    return m_Panel != nil && FindTabEntry(m_TabEntries, m_Panel) != nullptr && m_QLPanelAdaptor != nil;
}

- (void)beginPreviewPanelControl:(QLPreviewPanel *) [[maybe_unused]] _panel
{
    if( [m_QLPanelAdaptor registerExistingQLPreviewPanelFor:self] )
        [m_Panel updateAttachedQuickLook];
}

- (void)endPreviewPanelControl:(QLPreviewPanel *) [[maybe_unused]] _panel
{
    [m_QLPanelAdaptor unregisterExistingQLPreviewPanelFor:self];
}

#pragma mark - NCPanelViewKeystrokeSink

- (int)bidForHandlingKeyDown:(NSEvent *) [[maybe_unused]] _event forPanelView:(PanelView *)_panel_view
{
    if( _panel_view != m_Panel.view )
        return nc::panel::view::BiddingPriority::Skip;
    if( IsFocusAddressShortcut(_event) )
        return nc::panel::view::BiddingPriority::Max;
    if( _event.keyCode == 53 ) {
        NSPasteboard *const pasteboard = NSPasteboard.generalPasteboard;
        if( nc::panel::PasteboardSupport::CurrentCutToken(pasteboard) &&
            !nc::panel::PasteboardSupport::IsCutInFlight(pasteboard) )
            return nc::panel::view::BiddingPriority::High;
        if( m_Panel.isPresentingProgressiveNavigationPreview )
            return nc::panel::view::BiddingPriority::High;
    }
    return nc::panel::view::BiddingPriority::Skip;
}

- (void)handleKeyDown:(NSEvent *)_event forPanelView:(PanelView *)_panel_view
{
    if( _panel_view != m_Panel.view )
        return;
    if( _event.keyCode == 53 ) {
        NSPasteboard *const pasteboard = NSPasteboard.generalPasteboard;
        if( nc::panel::PasteboardSupport::CurrentCutToken(pasteboard) &&
            !nc::panel::PasteboardSupport::IsCutInFlight(pasteboard) ) {
            nc::panel::PasteboardSupport::CancelCut(pasteboard);
            return;
        }
        if( m_Panel.isPresentingProgressiveNavigationPreview )
            [m_Panel CancelBackgroundOperations];
        return;
    }
    [self focusAddressFieldShowingToolbarIfNeeded];
}

- (instancetype)initForTestingWithFrame:(NSRect)_frame
                        panelController:(PanelController *)_panel
                              panelView:(NSView *)_panel_view
                          inspectorView:(NCExplorerInspectorView *)_inspector
                         QLPanelAdaptor:(NCPanelQLPanelAdaptor *)_ql_panel_adaptor
{
    self = [super initWithFrame:_frame];
    if( !self )
        return nil;
    ConfigureExplorerRootAccessibility(self);
    m_Panel = _panel;
    auto tabs = nc::core::ExplorerTabsModel::Create(_panel.paneId);
    if( !tabs )
        return nil;
    m_TabsModel.emplace(std::move(*tabs));
    m_TabEntries.push_back(ExplorerTabEntry{
        .panel = _panel,
        .view_settings_binding =
            std::make_unique<nc::explorer::ExplorerViewSettingsBindingPolicy>(_panel.paneId),
    });
    _panel.state = self;
    if( [_panel.view respondsToSelector:@selector(addKeystrokeSink:)] )
        [_panel.view addKeystrokeSink:self];
    if( [_panel.view respondsToSelector:@selector(setHeaderBarVisible:)] )
        _panel.view.headerBarVisible = false;
    m_Inspector = _inspector;
    m_QLPanelAdaptor = _ql_panel_adaptor;
    m_QuickSearchOverlay = [[NCExplorerQuickSearchOverlayView alloc] initWithFrame:NSZeroRect];
    m_LastInspectorWidth = g_InspectorPreferredWidth;
    [self buildContentSplitWithPanelView:_panel_view inspectorView:_inspector];
    [self addSubview:m_ContentSplitView];
    [NSLayoutConstraint activateConstraints:@[
        [m_ContentSplitView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [m_ContentSplitView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [m_ContentSplitView.topAnchor constraintEqualToAnchor:self.topAnchor],
        [m_ContentSplitView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];
    [self layoutSubtreeIfNeeded];
    [self restoreInspectorWidth];
    return self;
}

- (void)configureTabBindingForTestingWithToolbarDelegate:(NCExplorerToolbarDelegate *)_toolbar
                                                 sidebar:(NCExplorerSidebarView *)_sidebar
                                              commandBar:(NCExplorerCommandBarView *)_command_bar
{
    m_ToolbarDelegate = _toolbar;
    m_Sidebar = _sidebar;
    m_CommandBar = _command_bar;
    [self bindActivePanel:m_Panel focus:false];
}

- (BOOL)addInactivePanelForTesting:(PanelController *)_panel
{
    if( !_panel || !m_TabsModel || FindTabEntry(m_TabEntries, _panel) )
        return false;
    const nc::core::PaneId active = m_TabsModel->Active();
    if( ![self attachExplorerTabPanel:_panel createPaneStore:false] )
        return false;
    return m_TabsModel->Active() == active || m_TabsModel->Activate(active);
}

- (std::vector<nc::core::PaneId>)tabPaneIDsForTesting
{
    if( !m_TabsModel )
        return {};
    return {m_TabsModel->Panes().begin(), m_TabsModel->Panes().end()};
}

- (void)applyPaneSnapshotForTesting:(const nc::core::PaneSnapshot &)_snapshot
{
    m_LatestPaneSnapshot = _snapshot;
    [m_Inspector applyPaneSnapshot:_snapshot];
    [m_PaneStateView updateWithVisualState:ExplorerPaneVisualState(_snapshot, m_Panel)];
}

- (BOOL)setSearchControllerForTesting:(ExplorerSearchController *)_controller forPanel:(PanelController *)_panel
{
    ExplorerTabEntry *const entry = FindTabEntry(m_TabEntries, _panel);
    if( !entry || !_controller )
        return NO;
    [entry->search_controller close];
    entry->search_controller = _controller;
    if( entry->panel == m_Panel )
        [self applySearchSnapshotForPane:entry->panel.paneId];
    return YES;
}

- (void)setSearchModeViewForTesting:(NCExplorerSearchModeView *)_view
{
    m_SearchModeView = _view;
    [self configureSearchViewHandlers];
    if( m_Panel )
        [self applySearchSnapshotForPane:m_Panel.paneId];
}

- (void)dealloc
{
    m_ObservationGate.Invalidate();
    m_PaneStoreObservation = {};
    if( m_ViewSettingsContextObservation != nil )
        [NSNotificationCenter.defaultCenter removeObserver:m_ViewSettingsContextObservation];
    [self closeAttachedUI:m_Panel];
    for( ExplorerTabEntry &entry : m_TabEntries ) {
        [entry.search_controller close];
        PanelController *const panel = entry.panel;
        PanelView *const view = panel.view;
        if( [panel respondsToSelector:@selector(setQuickSearchPresentation:)] )
            panel.quickSearchPresentation = nil;
        if( [view respondsToSelector:@selector(setBusyIndicatorOverride:)] )
            view.busyIndicatorOverride = nil;
        if( [view respondsToSelector:@selector(removeKeystrokeSink:)] )
            [view removeKeystrokeSink:self];
        if( [panel respondsToSelector:@selector(setState:)] )
            panel.state = nil;
    }
}

- (NSSplitView *)contentSplitViewForTesting
{
    return m_ContentSplitView;
}

- (NSView *)panelContainerForTesting
{
    return m_PanelContainer;
}

- (NCExplorerInspectorView *)inspectorViewForTesting
{
    return m_Inspector;
}

- (NCExplorerPaneStateView *)paneStateViewForTesting
{
    return m_PaneStateView;
}

@end
