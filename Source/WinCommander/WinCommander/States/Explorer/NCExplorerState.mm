// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerState.h"
#include "NCExplorerToolbarDelegate.h"
#include "NCExplorerSidebarView.h"
#include "NCExplorerCommandBarView.h"
#include "NCExplorerInspectorView.h"
#include "NCExplorerPaneStateView.h"
#include "NCGalleryView.h"
#include <WinCommander/Core/Cloud/GalleryListingSource.h>
#include "NCExplorerOperationProgressView.h"
#include "NCExplorerSearchModeView.h"
#include "NCExplorerCommandPaletteView.h"
#include "ExplorerOperationProgressController.h"
#include "ExplorerSearchController.h"
#include "ExplorerSpotlightSearchBackend.h"
#include "ExplorerViewSettingsBinding.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/PanelController+DataAccess.h"
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
#include <WinCommander/Core/Alert.h>
#include <WinCommander/Core/Commands/CommandRegistry.h>
#include <WinCommander/Core/Compare/FolderComparison.h>
#include <WinCommander/Core/Compare/FolderSyncPlan.h>
#include <WinCommander/Core/Pane/ExplorerTabsModel.h>
#include <WinCommander/Core/VisualState/VisualStateMapper.h>
#include <WinCommander/Bootstrap/NativeVFSHostInstance.h>
#include <Base/CommonPaths.h>
#include <Base/dispatch_cpp.h>
#include <Panel/PanelData.h>
#include <Operations/Pool.h>
#include <Operations/Copying.h>
#include <Operations/CopyingDialog.h>
#include <Operations/Deletion.h>
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
static const CGFloat g_DefaultPaneDividerRatio = 0.5;
static const CGFloat g_MinimumPaneDividerRatio =
    static_cast<CGFloat>(nc::explorer::ExplorerSessionPersistency::MinimumDividerRatio);
static const CGFloat g_MaximumPaneDividerRatio =
    static_cast<CGFloat>(nc::explorer::ExplorerSessionPersistency::MaximumDividerRatio);
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

/** Grouping label shown and searched as a palette row's subtitle. */
std::string CommandPaletteCategoryName(const nc::core::CommandCategory _category)
{
    switch( _category ) {
        case nc::core::CommandCategory::Navigation:
            return "Navigation";
        case nc::core::CommandCategory::Pane:
            return "Pane";
        case nc::core::CommandCategory::File:
            return "File";
        case nc::core::CommandCategory::Edit:
            return "Edit";
        case nc::core::CommandCategory::View:
            return "View";
        case nc::core::CommandCategory::Search:
            return "Search";
        case nc::core::CommandCategory::Operation:
            return "Operation";
        case nc::core::CommandCategory::Archive:
            return "Archive";
        case nc::core::CommandCategory::Remote:
            return "Remote";
        case nc::core::CommandCategory::Sync:
            return "Sync";
        case nc::core::CommandCategory::Developer:
            return "Developer";
        case nc::core::CommandCategory::Settings:
            return "Settings";
        case nc::core::CommandCategory::Window:
            return "Window";
        case nc::core::CommandCategory::Help:
            return "Help";
    }
    return {};
}

/**
 * Resolves a descriptor's localization key. The key doubles as the fallback value, so an untranslated
 * command is still findable by its key rather than appearing as an unselectable blank row.
 */
std::string CommandPaletteTitle(const std::string &_title_key)
{
    NSString *const key = [NSString stringWithUTF8String:_title_key.c_str()];
    if( !key )
        return _title_key;
    NSString *const localized = [NSBundle.mainBundle localizedStringForKey:key value:key table:nil];
    return localized.UTF8String != nullptr ? std::string{localized.UTF8String} : _title_key;
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

@interface NCExplorerState () <NSSplitViewDelegate, NCExplorerCommandPaletteDelegate>
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
@property(nonatomic, readonly) double paneDividerRatioForTesting;
- (PanelController *)allocateExplorerPanelForSessionRestore;
- (PanelController *)allocateExplorerPanelForDualPane;
- (BOOL)dualPaneCreatesPaneStore;
- (NSView *)dualPaneRightSideTestingContentViewForPanel:(PanelController *)_panel;
- (void)restoreSessionLocation:(const std::optional<nc::panel::PersistentLocation> &)_location
                       forPanel:(PanelController *)_panel;
- (void)loadNativeHomeForSessionPanel:(PanelController *)_panel;
- (void)invalidateExplorerRestorableState;
- (void)bindActivePanel:(PanelController *)_panel focus:(BOOL)_focus;
- (std::optional<nc::core::SearchPlanningFacts>)searchPlanningFactsForPanel:(PanelController *)_panel;
- (void)applySearchSnapshotForPane:(nc::core::PaneId)_pane_id;
- (void)configureSearchViewHandlers;
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

static bool IsPlainTabKey(NSEvent *_event)
{
    const NSEventModifierFlags flags = _event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask &
                                       ~(NSEventModifierFlagShift | NSEventModifierFlagCapsLock |
                                         NSEventModifierFlagNumericPad | NSEventModifierFlagFunction);
    return _event.keyCode == 48 && flags == 0;
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

namespace {

/** Which side of a dual-pane Explorer layout a content bundle belongs to. Left is also the sole
 *  side used while dual-pane is disabled. */
enum class NCExplorerPaneSide : uint8_t { Left = 0, Right = 1 };
constexpr size_t g_ExplorerPaneSideCount = 2;

} // namespace

class NCExplorerPaneContent;

/** One pane's visible listing reduced for comparison, retaining each item's sorted position. */
struct ExplorerCompareSide {
    std::vector<nc::core::FolderCompareItem> items;
    std::vector<int> sorted_positions;
    bool has_all_modification_times = true;
};

// Declared ahead of NCExplorerPaneContent's definition (not just ahead of @implementation) because
// NCExplorerPaneContent's own methods (e.g. BindActiveObservation) call these on their owner, and
// Objective-C resolves a call's selector against declarations already seen at that point in the
// translation unit - a later-textual @interface would compile that call as an implicit/unknown
// selector under this project's -Werror.
@interface NCExplorerState ()
- (void)explorerPaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot
                      forSide:(NCExplorerPaneSide)_side
                        token:(nc::core::ExplorerTabObservationToken)_token;
- (void)explorerPaneViewSettingsContextChangedForSide:(NCExplorerPaneSide)_side
                                                panel:(PanelController *)_panel
                                                token:(nc::core::ExplorerTabObservationToken)_token;
- (void)applyViewSettingsForSnapshot:(const nc::core::PaneSnapshot &)_snapshot
                            tabEntry:(ExplorerTabEntry &)_entry
                          forContent:(NCExplorerPaneContent &)_content;

- (NCExplorerPaneContent &)contentForSide:(NCExplorerPaneSide)_side;
- (NCExplorerPaneContent &)focusedContent;
- (NCExplorerPaneContent *)contentOwningPanel:(PanelController *)_panel;
- (std::optional<NCExplorerPaneSide>)sideOwningPanel:(PanelController *)_panel;
- (std::optional<NCExplorerPaneSide>)sideOwningPaneId:(nc::core::PaneId)_pane_id;
- (std::optional<NCExplorerPaneSide>)sideOwningTabView:(NSTabView *)_tab_view;
- (std::optional<NCExplorerPaneSide>)sideOwningPanelView:(PanelView *)_panel_view;
- (NSView *)currentPaneAreaView;
- (void)buildContentSplitWithPaneAreaView:(NSView *)_pane_area_view inspectorView:(NCExplorerInspectorView *)_inspector;
- (BOOL)dualPaneEnabled;
- (IBAction)onSwitchDualSinglePaneMode:(id)_sender;
/**
 * Shows the active pane's folder as a Gallery, or puts the listing back.
 *
 * Reached through the responder chain rather than a new menu tag, the way the cross-pane commands
 * were added: no shortcut table and no menu file change, so nothing here can drift out of step with
 * either of them.
 */
- (IBAction)onToggleExplorerGalleryMode:(id)_sender;
- (BOOL)setDualPaneEnabled:(BOOL)_enabled;
- (void)replaceCurrentPaneAreaViewWith:(NSView *)_view;
- (BOOL)attachExplorerTabPanel:(PanelController *)_panel
                createPaneStore:(BOOL)_create_pane_store
                      toContent:(NCExplorerPaneContent &)_content;
- (PanelController *)allocateExplorerPanelForDualPane;
- (void)changeFocusedSide;
- (BOOL)focusSide:(NCExplorerPaneSide)_side;
- (BOOL)restoreTabs:(const nc::explorer::ExplorerTabsSession &)_session intoContent:(NCExplorerPaneContent &)_content;
- (nc::explorer::ExplorerTabsSession)captureTabsForContent:(NCExplorerPaneContent &)_content;
- (ExplorerCompareSide)collectCompareSideForPanel:(PanelController *)_panel;
- (BOOL)canCompareDualPaneDirectories;
- (IBAction)OnCompareDirectories:(id)_sender;
- (void)applyCompareMarks:(const std::vector<bool> &)_marks
                positions:(const std::vector<int> &)_positions
                  toPanel:(PanelController *)_panel;
- (BOOL)canOpenCommandPalette;
- (IBAction)OnCommandPaletteOpen:(id)_sender;
- (nc::core::CommandContext)commandPaletteContextWithItems:(const std::vector<VFSListingItem> &)_items;
- (BOOL)canSynchronizeDualPaneDirectories;
- (IBAction)OnSynchronizeDirectories:(id)_sender;
- (void)submitSyncPlan:(const nc::core::FolderSyncPlan &)_plan
                 source:(PanelController *)_source
          sourceListing:(const VFSListingPtr &)_source_listing
       sourcePositions:(const std::vector<int> &)_source_positions
           destination:(PanelController *)_destination
    destinationListing:(const VFSListingPtr &)_destination_listing
  destinationPositions:(const std::vector<int> &)_destination_positions;
- (void)rollbackSessionRestoreForContent:(NCExplorerPaneContent &)_content;
- (void)applyPaneDividerRatio;
- (std::optional<CGFloat>)measuredPaneDividerRatio;
@end

/**
 * Everything a single Explorer side owns: its ordered tabs, the per-tab PanelController/pane-store/
 * search-controller/view-settings-binding bundle, its own tab strip and its own loading/empty/error
 * overlay. Extracted unchanged from the former single-pane NCExplorerState so that Explorer can host
 * either one of these (single-pane) or two of these side by side (dual-pane) without duplicating the
 * tab-lifecycle logic. Never copied or moved: each instance lives at a fixed address for the whole
 * lifetime of the owning NCExplorerState, which is what lets async callbacks capture a weak reference
 * to the owner and safely re-derive `this` after confirming the owner is still alive.
 */
class NCExplorerPaneContent {
public:
    NCExplorerPaneContent() noexcept = default;
    NCExplorerPaneContent(const NCExplorerPaneContent &) = delete;
    NCExplorerPaneContent &operator=(const NCExplorerPaneContent &) = delete;

    /** Builds this side's container view: a tabbed holder (or, in the legacy no-tab-strip testing
     *  path, a bare container) plus its own pane-state overlay and quick-search overlay. */
    void BuildViews(NCAppDelegate *_app, id<NCPanelTabBarViewDelegate> _tab_bar_delegate)
    {
        m_TabbedHolder = [[FilePanelsTabbedHolder alloc] initWithFrame:NSZeroRect
                                               actionsShortcutsManager:_app.actionsShortcutsManager
                                                         themesManager:_app.themesManager];
        m_TabbedHolder.tabBar.delegate = _tab_bar_delegate;
        m_TabbedHolder.tabBarShown = m_TabEntries.size() > 1;
        for( const ExplorerTabEntry &entry : m_TabEntries )
            AttachTabView(entry.panel);

        m_Container = [[NSView alloc] initWithFrame:NSZeroRect];
        m_Container.accessibilityElement = true;
        m_Container.accessibilityRole = NSAccessibilityGroupRole;
        m_Container.accessibilityIdentifier = @"wincommander.explorer.panelContainer";
        m_Container.accessibilityLabel = NSLocalizedString(@"Files", "Explorer content accessibility label");

        m_TabbedHolder.translatesAutoresizingMaskIntoConstraints = false;
        [m_Container addSubview:m_TabbedHolder];
        [NSLayoutConstraint activateConstraints:@[
            [m_TabbedHolder.leadingAnchor constraintEqualToAnchor:m_Container.leadingAnchor],
            [m_TabbedHolder.trailingAnchor constraintEqualToAnchor:m_Container.trailingAnchor],
            [m_TabbedHolder.topAnchor constraintEqualToAnchor:m_Container.topAnchor],
            [m_TabbedHolder.bottomAnchor constraintEqualToAnchor:m_Container.bottomAnchor],
        ]];

        // Above the file view and below the state overlay: Gallery replaces the listing, but a
        // blocking or empty state still has to be able to cover both.
        m_Gallery = [[NCGalleryView alloc] initWithFrame:NSZeroRect];
        m_Gallery.translatesAutoresizingMaskIntoConstraints = false;
        m_Gallery.hidden = true;
        [m_Container addSubview:m_Gallery];
        [NSLayoutConstraint activateConstraints:@[
            [m_Gallery.leadingAnchor constraintEqualToAnchor:m_Container.leadingAnchor],
            [m_Gallery.trailingAnchor constraintEqualToAnchor:m_Container.trailingAnchor],
            [m_Gallery.topAnchor constraintEqualToAnchor:m_Container.topAnchor],
            [m_Gallery.bottomAnchor constraintEqualToAnchor:m_Container.bottomAnchor],
        ]];

        m_PaneStateView = [[NCExplorerPaneStateView alloc] initWithFrame:NSZeroRect];
        m_PaneStateView.translatesAutoresizingMaskIntoConstraints = false;
        [m_Container addSubview:m_PaneStateView];
        [NSLayoutConstraint activateConstraints:@[
            [m_PaneStateView.leadingAnchor constraintEqualToAnchor:m_Container.leadingAnchor],
            [m_PaneStateView.trailingAnchor constraintEqualToAnchor:m_Container.trailingAnchor],
            [m_PaneStateView.topAnchor constraintEqualToAnchor:m_Container.topAnchor],
            [m_PaneStateView.bottomAnchor constraintEqualToAnchor:m_Container.bottomAnchor],
        ]];

        m_QuickSearchOverlay = [[NCExplorerQuickSearchOverlayView alloc] initWithFrame:NSZeroRect];
        m_QuickSearchOverlay.translatesAutoresizingMaskIntoConstraints = false;
        [m_Container addSubview:m_QuickSearchOverlay];
        [NSLayoutConstraint activateConstraints:@[
            [m_QuickSearchOverlay.topAnchor constraintEqualToAnchor:m_Container.topAnchor constant:9.0],
            [m_QuickSearchOverlay.trailingAnchor constraintEqualToAnchor:m_Container.trailingAnchor constant:-12.0],
            [m_QuickSearchOverlay.widthAnchor constraintEqualToConstant:280.0],
            [m_QuickSearchOverlay.heightAnchor constraintEqualToConstant:32.0],
        ]];
    }

    /** Testing-only path: hosts a single bare panel view with no tab strip. */
    void BuildTestingContainer(NSView *_panel_view, NCExplorerQuickSearchOverlayView *_shared_overlay)
    {
        m_Container = [[NSView alloc] initWithFrame:NSZeroRect];
        _panel_view.translatesAutoresizingMaskIntoConstraints = false;
        [m_Container addSubview:_panel_view];
        [NSLayoutConstraint activateConstraints:@[
            [_panel_view.leadingAnchor constraintEqualToAnchor:m_Container.leadingAnchor],
            [_panel_view.trailingAnchor constraintEqualToAnchor:m_Container.trailingAnchor],
            [_panel_view.topAnchor constraintEqualToAnchor:m_Container.topAnchor],
            [_panel_view.bottomAnchor constraintEqualToAnchor:m_Container.bottomAnchor],
        ]];
        m_PaneStateView = [[NCExplorerPaneStateView alloc] initWithFrame:NSZeroRect];
        [m_Container addSubview:m_PaneStateView];
        [NSLayoutConstraint activateConstraints:@[
            [m_PaneStateView.leadingAnchor constraintEqualToAnchor:m_Container.leadingAnchor],
            [m_PaneStateView.trailingAnchor constraintEqualToAnchor:m_Container.trailingAnchor],
            [m_PaneStateView.topAnchor constraintEqualToAnchor:m_Container.topAnchor],
            [m_PaneStateView.bottomAnchor constraintEqualToAnchor:m_Container.bottomAnchor],
        ]];
        m_QuickSearchOverlay = _shared_overlay;
    }

    NSView *Container() const noexcept { return m_Container; }
    NCGalleryView *Gallery() const noexcept { return m_Gallery; }
    bool GalleryMode() const noexcept { return m_GalleryMode; }

    /**
     * Swaps the listing for the Gallery, or back.
     *
     * The file view is hidden rather than unmounted: it keeps its selection, its scroll position and
     * its first responder status, so coming back from Gallery returns the user to where they were
     * rather than to the top of the folder.
     */
    void SetGalleryMode(const bool _on)
    {
        if( m_GalleryMode == _on || m_Gallery == nil )
            return;
        m_GalleryMode = _on;
        m_Gallery.hidden = !_on;
        m_TabbedHolder.hidden = _on;
        if( _on )
            RefreshGallery();
    }

    /** Rebuilds what the Gallery shows from the active panel's current listing. */
    void RefreshGallery()
    {
        if( m_Gallery == nil || !m_GalleryMode || m_Panel == nil )
            return;
        std::vector<nc::core::NativeListingEntry> entries;
        std::string directory;
        try {
            const VFSListingPtr listing = m_Panel.data.ListingPtr();
            if( !listing )
                return;
            directory = m_Panel.currentDirectoryPath;
            if( !directory.empty() && directory.back() == '/' )
                directory.pop_back();
            entries.reserve(listing->Count());
            for( unsigned i = 0; i < listing->Count(); ++i )
                entries.push_back(nc::core::NativeListingEntry{.filename = std::string{listing->Filename(i)},
                                                               .is_directory = listing->IsDir(i)});
        } catch( ... ) {
            // A listing that changed underneath is not something to half-draw: leaving the previous
            // contents is better than showing a folder assembled from two different moments.
            return;
        }
        // The cloud probe only means anything on the native filesystem; elsewhere every item is
        // simply not cloud, and asking would be a per-row filesystem call answering nothing.
        const bool native = m_Panel.vfs != nullptr && m_Panel.vfs->IsNativeFS();
        auto source = nc::core::BuildGalleryListing(
            directory, entries, native ? nc::core::ProbeNativeCloudItem : decltype(&nc::core::ProbeNativeCloudItem){});
        [m_Gallery applyContents:nc::core::BuildGalleryContents(source.Items()) inDirectory:directory];
    }

    FilePanelsTabbedHolder *TabbedHolder() const noexcept { return m_TabbedHolder; }
    NCExplorerPaneStateView *PaneStateView() const noexcept { return m_PaneStateView; }
    NCExplorerQuickSearchOverlayView *QuickSearchOverlay() const noexcept { return m_QuickSearchOverlay; }
    PanelController *ActivePanel() const noexcept { return m_Panel; }
    bool HasTabsModel() const noexcept { return m_TabsModel.has_value(); }
    size_t TabCount() const noexcept { return m_TabEntries.size(); }
    const std::vector<ExplorerTabEntry> &Entries() const noexcept { return m_TabEntries; }
    std::optional<nc::core::PaneId> ActivePaneId() const
    {
        return m_TabsModel ? std::make_optional(m_TabsModel->Active()) : std::nullopt;
    }
    bool RuntimeTabsMutated() const noexcept { return m_RuntimeTabsMutated; }
    bool SessionRestoreApplied() const noexcept { return m_SessionRestoreApplied; }
    const std::optional<nc::core::PaneSnapshot> &LatestPaneSnapshot() const noexcept { return m_LatestPaneSnapshot; }

    ExplorerTabEntry *FindEntry(PanelController *_panel) { return FindTabEntry(m_TabEntries, _panel); }
    ExplorerTabEntry *FindEntry(nc::core::PaneId _pane_id) { return FindTabEntry(m_TabEntries, _pane_id); }
    bool Owns(PanelController *_panel) { return _panel != nil && FindEntry(_panel) != nullptr; }

    void SetSynchronizingTabs(bool _value) noexcept { m_SynchronizingTabs = _value; }
    bool SynchronizingTabs() const noexcept { return m_SynchronizingTabs; }

    std::vector<nc::core::PaneId> TabPaneIDsForTesting() const
    {
        if( !m_TabsModel )
            return {};
        return {m_TabsModel->Panes().begin(), m_TabsModel->Panes().end()};
    }

    /** Mirrors the former NCExplorerState::attachExplorerTabPanel:createPaneStore:. */
    bool AttachTabPanel(PanelController *_panel,
                        bool _create_pane_store,
                        id<NCPanelControllerHostingState> _hosting_state,
                        id<NCPanelViewKeystrokeSink> _keystroke_sink,
                        std::function<void(std::optional<nc::core::SearchSnapshot>)> _search_snapshot_handler)
    {
        if( !_panel || _panel.paneId.value == 0 || FindEntry(_panel) ||
            m_TabEntries.size() >= nc::explorer::ExplorerSessionPersistency::MaximumTabs ||
            (m_TabsModel && m_TabsModel->Size() >= nc::explorer::ExplorerSessionPersistency::MaximumTabs) ) {
            return false;
        }

        std::unique_ptr<nc::panel::PanelControllerPaneStoreAdapter> pane_store;
        std::unique_ptr<nc::explorer::ExplorerViewSettingsBindingPolicy> view_settings_binding;
        __strong ExplorerSearchController *search_controller = nil;
        _panel.state = _hosting_state;
        [_panel.view addKeystrokeSink:_keystroke_sink];
        _panel.view.headerBarVisible = false;
        try {
            if( _create_pane_store )
                pane_store = std::make_unique<nc::panel::PanelControllerPaneStoreAdapter>(_panel);
            view_settings_binding = std::make_unique<nc::explorer::ExplorerViewSettingsBindingPolicy>(_panel.paneId);
            search_controller = [[ExplorerSearchController alloc]
                initWithPanel:_panel
                       paneId:_panel.paneId
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
               snapshotHandler:std::move(_search_snapshot_handler)];
            if( !search_controller )
                throw std::bad_alloc();
        } catch( ... ) {
            [_panel.view removeKeystrokeSink:_keystroke_sink];
            _panel.state = nil;
            return false;
        }

        if( !m_TabsModel ) {
            auto model = nc::core::ExplorerTabsModel::Create(_panel.paneId);
            if( !model ) {
                [_panel.view removeKeystrokeSink:_keystroke_sink];
                _panel.state = nil;
                return false;
            }
            m_TabsModel.emplace(std::move(*model));
        }
        else if( !m_TabsModel->Append(_panel.paneId) ) {
            [_panel.view removeKeystrokeSink:_keystroke_sink];
            _panel.state = nil;
            return false;
        }

        m_TabEntries.push_back(ExplorerTabEntry{
            .panel = _panel,
            .search_controller = search_controller,
            .pane_store = std::move(pane_store),
            .view_settings_binding = std::move(view_settings_binding),
        });
        AttachTabView(_panel);
        return true;
    }

    /** Mirrors the former NCExplorerState::attachExplorerTabViewForPanel:. */
    void AttachTabView(PanelController *_panel)
    {
        if( !_panel || !FindEntry(_panel) )
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
        if( m_Container && _panel != m_Panel && !_panel.view.superview ) {
            _panel.view.frame = m_Container.bounds;
            _panel.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            _panel.view.hidden = true;
            [m_Container addSubview:_panel.view positioned:NSWindowBelow relativeTo:m_QuickSearchOverlay];
        }
    }

    void UpdateTabLabel(PanelController *_panel)
    {
        if( !FindEntry(_panel) || !m_TabbedHolder )
            return;
        if( NSTabViewItem *const item = [m_TabbedHolder tabViewItemForController:_panel] )
            item.label = [NSString stringWithUTF8StdString:TabNameForController(_panel)];
    }

    /** Removes every tab beyond the first, mirroring rollbackSessionRestorePanels. */
    void RollbackSessionRestore(id<NCPanelViewKeystrokeSink> _keystroke_sink)
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
            [panel.view removeKeystrokeSink:_keystroke_sink];
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
    }

    /** Pane-local part of the former applyActivePaneSnapshot:observationToken:. Always runs for this
     *  side's own overlay/tab label/search sync, independent of which side is focused. The caller is
     *  responsible for also pushing the snapshot into shared chrome when this side is focused. */
    bool ApplySnapshotLocal(const nc::core::PaneSnapshot &_snapshot,
                            const nc::core::ExplorerTabObservationToken _token)
    {
        if( !m_TabsModel || !m_Panel || !m_ObservationGate.Accepts(_token, m_TabsModel->Active(), _snapshot.pane_id) ||
            _snapshot.pane_id != m_Panel.paneId )
            return false;

        ExplorerTabEntry *const entry = FindEntry(_snapshot.pane_id);
        if( !entry || entry->panel != m_Panel )
            return false;
        if( entry->last_active_snapshot_revision && _snapshot.revision < *entry->last_active_snapshot_revision )
            return false;
        entry->last_active_snapshot_revision =
            std::max(entry->last_active_snapshot_revision.value_or(0), _snapshot.revision);

        [entry->search_controller synchronizeExternalContentChange];
        m_LatestPaneSnapshot = _snapshot;
        [m_PaneStateView updateWithVisualState:nc::core::VisualStateMapper::MapPane(_snapshot)];
        return true;
    }

    void ClearPaneStateView() { [m_PaneStateView updateWithVisualState:nc::core::PaneVisualState{}]; }

    /**
     * Exchanges every field except the active-panel observation/snapshot state (m_Panel,
     * m_ObservationGate, m_PaneStoreObservation, m_ViewSettingsContextObservation,
     * m_LatestPaneSnapshot). The caller must call DetachActiveObservation() on both sides before
     * this and BindActiveObservation() on both sides (with each side's *new* active panel) after
     * it: an in-flight callback captured the side it was bound under, and only a fresh Bind() call
     * retires it - silently swapping the observation fields too would leave stale callbacks
     * reading whichever panel now occupies their originally-captured side.
     */
    void SwapStaticState(NCExplorerPaneContent &_other) noexcept
    {
        using std::swap;
        swap(m_TabsModel, _other.m_TabsModel);
        swap(m_TabEntries, _other.m_TabEntries);
        swap(m_TabbedHolder, _other.m_TabbedHolder);
        swap(m_PaneStateView, _other.m_PaneStateView);
        swap(m_QuickSearchOverlay, _other.m_QuickSearchOverlay);
        swap(m_Container, _other.m_Container);
        swap(m_SynchronizingTabs, _other.m_SynchronizingTabs);
        swap(m_RuntimeTabsMutated, _other.m_RuntimeTabsMutated);
        swap(m_SessionRestoreApplied, _other.m_SessionRestoreApplied);
    }

    /** Tears down this side's observation of its previously-active panel, mirroring the top of the
     *  former bindActivePanel:focus:. Returns the panel that was active before the call, if any. */
    PanelController *DetachActiveObservation()
    {
        PanelController *const previous = m_Panel;
        m_ObservationGate.Invalidate();
        m_PaneStoreObservation = {};
        if( m_ViewSettingsContextObservation != nil ) {
            [NSNotificationCenter.defaultCenter removeObserver:m_ViewSettingsContextObservation];
            m_ViewSettingsContextObservation = nil;
        }
        m_LatestPaneSnapshot.reset();
        ClearPaneStateView();
        return previous;
    }

    /** Binds this side's active-panel observation (PaneStore + view-settings-context), mirroring the
     *  bottom half of the former bindActivePanel:focus:. `_owner` is captured weakly by every async
     *  block below; `_side` identifies this content so a later callback can re-locate it safely
     *  without ever capturing a raw `this` across an async boundary. */
    void BindActiveObservation(PanelController *_panel, NCExplorerPaneSide _side, __weak NCExplorerState *_owner)
    {
        m_Panel = _panel;
        ExplorerTabEntry *const entry = FindEntry(_panel);
        const auto token = m_ObservationGate.Bind(_panel.paneId);
        if( !entry || !token )
            return;

        if( entry->pane_store ) {
            m_PaneStoreObservation = entry->pane_store->Store().Observe(
                [_owner, _side, token = *token](const nc::core::PaneSnapshot &_snapshot) {
                    NCExplorerState *const owner = _owner;
                    if( owner )
                        [owner explorerPaneSnapshot:_snapshot forSide:_side token:token];
                });
            [_owner explorerPaneSnapshot:entry->pane_store->Store().Snapshot() forSide:_side token:*token];
        }

        m_ViewSettingsContextObservation = [NSNotificationCenter.defaultCenter
            addObserverForName:NCPanelViewContextDidChangeNotification
                        object:_panel.view
                         queue:nil
                    usingBlock:[_owner, _side, _panel, token = *token](NSNotification *) {
                      NCExplorerState *const owner = _owner;
                      if( owner )
                          [owner explorerPaneViewSettingsContextChangedForSide:_side panel:_panel token:token];
                    }];
    }

    /** Mirrors the search-planning subset of the former searchPlanningFactsForPanel:, gated to this
     *  side's own active tab. */
    ExplorerSearchController *ActiveSearchController() const
    {
        if( !m_TabsModel || !m_Panel )
            return nil;
        ExplorerTabEntry *const entry = const_cast<NCExplorerPaneContent *>(this)->FindEntry(m_TabsModel->Active());
        return entry && entry->panel == m_Panel ? entry->search_controller : nil;
    }

    // Raw accessors for the tab-lifecycle orchestration that still lives on NCExplorerState (tab
    // close/reorder/new-tab/session-restore/capture, and the NCPanelTabBarViewDelegate methods).
    // Kept deliberately close to field access so that porting those methods stays a mechanical,
    // low-risk rename rather than a reinterpretation.
    std::optional<nc::core::ExplorerTabsModel> &TabsModel() noexcept { return m_TabsModel; }
    std::vector<ExplorerTabEntry> &MutableEntries() noexcept { return m_TabEntries; }
    bool &RuntimeTabsMutatedRef() noexcept { return m_RuntimeTabsMutated; }
    bool &SessionRestoreAppliedRef() noexcept { return m_SessionRestoreApplied; }
    PanelController *__strong &ActivePanelRef() noexcept { return m_Panel; }
    nc::core::ExplorerTabObservationGate &ObservationGate() noexcept { return m_ObservationGate; }
    nc::core::PaneStoreAdapter::ObservationTicket &PaneStoreObservationRef() noexcept { return m_PaneStoreObservation; }
    std::optional<nc::core::PaneSnapshot> &LatestPaneSnapshotRef() noexcept { return m_LatestPaneSnapshot; }

private:
    PanelController *m_Panel = nil;
    std::optional<nc::core::ExplorerTabsModel> m_TabsModel;
    std::vector<ExplorerTabEntry> m_TabEntries;
    FilePanelsTabbedHolder *m_TabbedHolder = nil;
    NCExplorerPaneStateView *m_PaneStateView = nil;
    NCGalleryView *m_Gallery = nil;
    bool m_GalleryMode = false;
    NCExplorerQuickSearchOverlayView *m_QuickSearchOverlay = nil;
    NSView *m_Container = nil;
    nc::core::PaneStoreAdapter::ObservationTicket m_PaneStoreObservation;
    id m_ViewSettingsContextObservation = nil;
    nc::core::ExplorerTabObservationGate m_ObservationGate;
    bool m_SynchronizingTabs = false;
    bool m_RuntimeTabsMutated = false;
    bool m_SessionRestoreApplied = false;
    std::optional<nc::core::PaneSnapshot> m_LatestPaneSnapshot;
};

@implementation NCExplorerState {
    NCExplorerToolbarDelegate *m_ToolbarDelegate;
    NCExplorerSidebarView *m_Sidebar;
    NCExplorerCommandBarView *m_CommandBar;
    NCExplorerOperationProgressView *m_OperationProgressView;
    ExplorerOperationProgressController *m_OperationProgressController;
    NCExplorerSearchModeView *m_SearchModeView;
    NSSplitView *m_ContentSplitView;
    NSSplitView *m_PaneSplitView;
    NCExplorerInspectorView *m_Inspector;
    NCPanelQLPanelAdaptor *m_QLPanelAdaptor;
    NCExplorerCommandPaletteView *m_CommandPalette;
    CGFloat m_LastInspectorWidth;
    /** Left side's share of the dual-pane split's usable width. Retained across dual-pane toggles
     *  and window sessions; the live split view only exists while dual-pane is on. */
    CGFloat m_PaneDividerRatio;
    std::array<NCExplorerPaneContent, g_ExplorerPaneSideCount> m_Sides;
    NCExplorerPaneSide m_FocusedSide;
    bool m_DualPaneEnabled;
    std::shared_ptr<nc::ops::Pool> m_OperationsPool;
}

- (PanelController *)panelController
{
    return m_Sides[static_cast<size_t>(m_FocusedSide)].ActivePanel();
}

- (NCExplorerPaneContent &)contentForSide:(const NCExplorerPaneSide)_side
{
    return m_Sides[static_cast<size_t>(_side)];
}

- (NCExplorerPaneContent &)focusedContent
{
    return m_Sides[static_cast<size_t>(m_FocusedSide)];
}

- (NCExplorerPaneContent *)contentOwningPanel:(PanelController *)_panel
{
    for( NCExplorerPaneContent &content : m_Sides )
        if( content.Owns(_panel) )
            return &content;
    return nullptr;
}

- (std::optional<NCExplorerPaneSide>)sideOwningPanel:(PanelController *)_panel
{
    for( size_t index = 0; index < g_ExplorerPaneSideCount; ++index )
        if( m_Sides[index].Owns(_panel) )
            return static_cast<NCExplorerPaneSide>(index);
    return std::nullopt;
}

- (std::optional<NCExplorerPaneSide>)sideOwningPaneId:(const nc::core::PaneId)_pane_id
{
    for( size_t index = 0; index < g_ExplorerPaneSideCount; ++index )
        if( m_Sides[index].FindEntry(_pane_id) )
            return static_cast<NCExplorerPaneSide>(index);
    return std::nullopt;
}

- (instancetype)initWithFrame:(NSRect)frameRect operationsPool:(nc::ops::Pool &)_pool
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        ConfigureExplorerRootAccessibility(self);
        m_OperationsPool = _pool.shared_from_this();
        m_FocusedSide = NCExplorerPaneSide::Left;
        m_DualPaneEnabled = false;
        NCAppDelegate *const app = NCAppDelegate.me;
        PanelController *const initial_panel = [app allocateExplorerPanelController];
        if( !initial_panel || ![self attachExplorerTabPanel:initial_panel createPaneStore:true] )
            return nil;
        m_QLPanelAdaptor = [app QLPanelAdaptor];
        m_LastInspectorWidth = g_InspectorPreferredWidth;
        m_PaneDividerRatio = g_DefaultPaneDividerRatio;

        m_ToolbarDelegate = [[NCExplorerToolbarDelegate alloc] initWithPanelController:initial_panel];
        [self buildLayout];

        [self bindActivePanel:initial_panel focus:false];
        [self loadNativeHomeForSessionPanel:initial_panel];
    }
    return self;
}

- (void)buildLayout
{
    PanelController *const initial_panel = self.panelController;
    m_Sidebar = [[NCExplorerSidebarView alloc] initWithFrame:NSRect() panelController:initial_panel];
    NCAppDelegate *const app = NCAppDelegate.me;
    nc::core::CommandRegistry &command_registry = app.commandRegistry;
    m_CommandBar = [[NCExplorerCommandBarView alloc] initWithFrame:NSRect()
                                                   panelController:initial_panel
                                        operationCenterCoordinator:app.operationCenterCoordinator
                                                   commandRegistry:&command_registry];
    m_OperationProgressView = [[NCExplorerOperationProgressView alloc] initWithFrame:NSZeroRect];
    m_OperationProgressController = [[ExplorerOperationProgressController alloc] initWithPool:*m_OperationsPool
                                                                                          view:m_OperationProgressView];
    m_SearchModeView = [[NCExplorerSearchModeView alloc] initWithFrame:NSZeroRect];
    [self configureSearchViewHandlers];
    m_Sides[static_cast<size_t>(NCExplorerPaneSide::Left)].BuildViews(app, self);
    m_Inspector = [[NCExplorerInspectorView alloc]
                    initWithFrame:NSMakeRect(
                                      0, 0, g_InspectorPreferredWidth, self.bounds.size.height - g_CommandBarHeight)
                           paneID:initial_panel.paneId
                            UTIDB:app.utiDB
        QLHazardousExtensionsList:GlobalConfig().GetString(g_QuickLookHazardousExtensionsList)
                      QLVFSBridge:[app QLVFSBridge]];

    m_Sidebar.translatesAutoresizingMaskIntoConstraints = false;
    m_CommandBar.translatesAutoresizingMaskIntoConstraints = false;
    [self buildContentSplitWithPaneAreaView:[self currentPaneAreaView] inspectorView:m_Inspector];

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

/** The view that should currently occupy the pane area of m_ContentSplitView: one side's own
 *  container in single-pane, or m_PaneSplitView wrapping both containers in dual-pane. */
- (NSView *)currentPaneAreaView
{
    if( !m_DualPaneEnabled )
        return m_Sides[static_cast<size_t>(NCExplorerPaneSide::Left)].Container();
    return m_PaneSplitView;
}

- (void)buildContentSplitWithPaneAreaView:(NSView *)_pane_area_view inspectorView:(NCExplorerInspectorView *)_inspector
{
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

    _pane_area_view.translatesAutoresizingMaskIntoConstraints = true;
    _inspector.translatesAutoresizingMaskIntoConstraints = true;
    const CGFloat inspector_width = std::min(g_InspectorPreferredWidth, split_width);
    const CGFloat divider = m_ContentSplitView.dividerThickness;
    _pane_area_view.frame =
        NSMakeRect(0, 0, std::max<CGFloat>(0.0, split_width - inspector_width - divider), split_height);
    _inspector.frame = NSMakeRect(NSMaxX(_pane_area_view.frame) + divider, 0, inspector_width, split_height);
    [m_ContentSplitView addSubview:_pane_area_view];
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

#pragma mark - Dual Pane layout toggle

/**
 * Places the dual-pane divider at the retained ratio of the split's usable width. AppKit reports the
 * achieved position back through -splitViewDidResizeSubviews:, so the ratio the delegate then keeps
 * is the one actually satisfied by the per-side minimum-width constraints, not the requested one.
 */
- (void)applyPaneDividerRatio
{
    if( !m_PaneSplitView || m_PaneSplitView.subviews.count != 2 )
        return;
    const CGFloat usable = m_PaneSplitView.bounds.size.width - m_PaneSplitView.dividerThickness;
    if( usable <= 0.0 )
        return;
    const CGFloat ratio = std::clamp(m_PaneDividerRatio, g_MinimumPaneDividerRatio, g_MaximumPaneDividerRatio);
    [m_PaneSplitView setPosition:std::clamp(usable * ratio, 0.0, usable) ofDividerAtIndex:0];
}

/**
 * The left side's measured share of the dual-pane split, or nullopt when that measurement cannot
 * express user intent. Below twice the per-side minimum width no divider position satisfies the
 * layout constraints at all, so whatever AppKit settles on there is an artifact of the window being
 * too narrow - recording it would let one cramped layout pass silently destroy the retained ratio
 * the user actually chose. This mirrors how -splitViewDidResizeSubviews: only accepts an inspector
 * width that is already inside its own usable band.
 */
- (std::optional<CGFloat>)measuredPaneDividerRatio
{
    if( !m_PaneSplitView || m_PaneSplitView.subviews.count != 2 )
        return std::nullopt;
    const CGFloat usable = m_PaneSplitView.bounds.size.width - m_PaneSplitView.dividerThickness;
    if( usable < 2.0 * g_PanelMinimumWidth )
        return std::nullopt;
    const CGFloat left_width = m_PaneSplitView.subviews.firstObject.frame.size.width;
    if( left_width <= 0.0 || left_width > usable )
        return std::nullopt;
    return left_width / usable;
}

- (BOOL)dualPaneEnabled
{
    return m_DualPaneEnabled;
}

- (IBAction)onToggleExplorerGalleryMode:(id) [[maybe_unused]] _sender
{
    dispatch_assert_main_queue();
    NCExplorerPaneContent &pane = [self focusedContent];
    pane.SetGalleryMode(!pane.GalleryMode());
}

- (IBAction)onSwitchDualSinglePaneMode:(id) [[maybe_unused]] _sender
{
    [self setDualPaneEnabled:!m_DualPaneEnabled];
}

- (BOOL)setDualPaneEnabled:(BOOL)_enabled
{
    if( static_cast<bool>(_enabled) == m_DualPaneEnabled )
        return true;

    if( _enabled ) {
        NCExplorerPaneContent &right = m_Sides[static_cast<size_t>(NCExplorerPaneSide::Right)];
        NCAppDelegate *const app = NCAppDelegate.me;
        PanelController *const panel = [self allocateExplorerPanelForDualPane];
        if( !panel || ![self attachExplorerTabPanel:panel
                                     createPaneStore:[self dualPaneCreatesPaneStore]
                                           toContent:right] )
            return false;
        if( NSView *const testing_view = [self dualPaneRightSideTestingContentViewForPanel:panel] )
            right.BuildTestingContainer(testing_view, [[NCExplorerQuickSearchOverlayView alloc] initWithFrame:NSZeroRect]);
        else
            right.BuildViews(app, self);

        m_PaneSplitView = [[NSSplitView alloc] initWithFrame:[self currentPaneAreaView].frame];
        m_PaneSplitView.vertical = true;
        m_PaneSplitView.dividerStyle = NSSplitViewDividerStyleThin;
        m_PaneSplitView.delegate = self;
        m_PaneSplitView.accessibilityElement = true;
        m_PaneSplitView.accessibilityRole = NSAccessibilitySplitGroupRole;
        m_PaneSplitView.accessibilityIdentifier = @"wincommander.explorer.paneSplit";
        m_PaneSplitView.accessibilityLabel =
            NSLocalizedString(@"Left and right panes", "Explorer dual pane split accessibility label");

        // m_PaneSplitView is spliced into m_ContentSplitView (detaching left_view from it, since a
        // view can only have one superview) *before* left_view/right_view are added below - doing it
        // in the opposite order would silently reorder m_ContentSplitView's own two subviews once
        // left_view got reparented out from under it first.
        m_DualPaneEnabled = true;
        [self replaceCurrentPaneAreaViewWith:m_PaneSplitView];

        NSView *const left_view = m_Sides[static_cast<size_t>(NCExplorerPaneSide::Left)].Container();
        NSView *const right_view = right.Container();
        left_view.translatesAutoresizingMaskIntoConstraints = true;
        right_view.translatesAutoresizingMaskIntoConstraints = true;
        const CGFloat total_width = m_PaneSplitView.frame.size.width;
        const CGFloat height = m_PaneSplitView.frame.size.height;
        const CGFloat divider = m_PaneSplitView.dividerThickness;
        const CGFloat half = std::max<CGFloat>(0.0, (total_width - divider) / 2.0);
        left_view.frame = NSMakeRect(0, 0, half, height);
        right_view.frame = NSMakeRect(half + divider, 0, std::max<CGFloat>(0.0, total_width - half - divider), height);
        [m_PaneSplitView addSubview:left_view];
        [m_PaneSplitView addSubview:right_view];
        [m_PaneSplitView adjustSubviews];
        [self applyPaneDividerRatio];

        right.BindActiveObservation(panel, NCExplorerPaneSide::Right, self);
        [self loadNativeHomeForSessionPanel:panel];
    }
    else {
        NCExplorerPaneContent &right = m_Sides[static_cast<size_t>(NCExplorerPaneSide::Right)];
        if( m_FocusedSide == NCExplorerPaneSide::Right )
            [self focusSide:NCExplorerPaneSide::Left];
        right.RollbackSessionRestore(self);
        for( const ExplorerTabEntry &entry : right.Entries() ) {
            [entry.search_controller close];
            [entry.panel.view removeKeystrokeSink:self];
            entry.panel.state = nil;
        }
        right.MutableEntries().clear();
        right.TabsModel().reset();
        right.DetachActiveObservation();
        right.ActivePanelRef() = nil;

        m_DualPaneEnabled = false;
        [self replaceCurrentPaneAreaViewWith:m_Sides[static_cast<size_t>(NCExplorerPaneSide::Left)].Container()];
        m_PaneSplitView = nil;
    }

    [self invalidateExplorerRestorableState];
    return true;
}

/** Swaps the pane-area subview of m_ContentSplitView (index 0) in place, preserving the inspector
 *  (index 1) and the current inspector width. */
- (void)replaceCurrentPaneAreaViewWith:(NSView *)_view
{
    if( !m_ContentSplitView || m_ContentSplitView.subviews.count < 1 )
        return;
    NSView *const previous = m_ContentSplitView.subviews.firstObject;
    if( previous == _view )
        return;
    _view.translatesAutoresizingMaskIntoConstraints = true;
    _view.frame = previous.frame;
    [previous removeFromSuperview];
    if( m_Inspector )
        [m_ContentSplitView addSubview:_view positioned:NSWindowBelow relativeTo:m_Inspector];
    else
        [m_ContentSplitView addSubview:_view];
    [m_ContentSplitView adjustSubviews];
    [self restoreInspectorWidth];
}

- (CGFloat)splitView:(NSSplitView *)_split_view
    constrainMinCoordinate:(CGFloat)_proposed_minimum
               ofSubviewAt:(NSInteger)_divider_index
{
    if( _split_view == m_PaneSplitView && _divider_index == 0 )
        return _proposed_minimum + g_PanelMinimumWidth;
    if( _split_view != m_ContentSplitView || _divider_index != 0 )
        return _proposed_minimum;
    const CGFloat width = _split_view.bounds.size.width;
    const CGFloat divider = _split_view.dividerThickness;
    const CGFloat maximum_position = std::max<CGFloat>(0.0, width - g_InspectorMinimumWidth - divider);
    const CGFloat width_bound = std::max<CGFloat>(0.0, width - g_InspectorMaximumWidth - divider);
    return std::max(width_bound, std::min(g_PanelMinimumWidth, maximum_position));
}

- (CGFloat)splitView:(NSSplitView *)_split_view
    constrainMaxCoordinate:(CGFloat)_proposed_maximum
               ofSubviewAt:(NSInteger)_divider_index
{
    if( _split_view == m_PaneSplitView && _divider_index == 0 )
        return _proposed_maximum - g_PanelMinimumWidth;
    if( _split_view != m_ContentSplitView || _divider_index != 0 )
        return _proposed_maximum;
    return std::max<CGFloat>(0.0,
                             _split_view.bounds.size.width - g_InspectorMinimumWidth - _split_view.dividerThickness);
}

- (BOOL)splitView:(NSSplitView *)_split_view canCollapseSubview:(NSView *)_subview
{
    if( _split_view == m_PaneSplitView )
        return false; // Neither side collapses in this increment - see DP-1 explicit non-goals.
    return _split_view == m_ContentSplitView && _subview == m_Inspector;
}

- (void)splitViewDidResizeSubviews:(NSNotification *)_notification
{
    if( m_PaneSplitView != nil && _notification.object == m_PaneSplitView ) {
        if( const std::optional<CGFloat> ratio = [self measuredPaneDividerRatio] )
            m_PaneDividerRatio = std::clamp(*ratio, g_MinimumPaneDividerRatio, g_MaximumPaneDividerRatio);
        return;
    }
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
      if( !state )
          return;
      NCExplorerPaneContent &content = [state focusedContent];
      ExplorerTabEntry *const entry = content.HasTabsModel() ? content.FindEntry(content.TabsModel()->Active()) : nullptr;
      if( entry && entry->panel == content.ActivePanel() && entry->search_controller.isPresented )
          [entry->search_controller startSearch:std::move(_request)];
    }];
    [m_SearchModeView setCancelHandler:[weak_self] {
      NCExplorerState *const state = weak_self;
      if( !state )
          return;
      NCExplorerPaneContent &content = [state focusedContent];
      ExplorerTabEntry *const entry = content.HasTabsModel() ? content.FindEntry(content.TabsModel()->Active()) : nullptr;
      if( entry && entry->panel == content.ActivePanel() )
          [entry->search_controller cancel];
    }];
    [m_SearchModeView setRevealOriginalHandler:[weak_self] {
      NCExplorerState *const state = weak_self;
      if( !state )
          return;
      NCExplorerPaneContent &content = [state focusedContent];
      ExplorerTabEntry *const entry = content.HasTabsModel() ? content.FindEntry(content.TabsModel()->Active()) : nullptr;
      if( entry && entry->panel == content.ActivePanel() )
          [entry->search_controller revealFocusedResult];
    }];
    [m_SearchModeView setCloseHandler:[weak_self] {
      NCExplorerState *const state = weak_self;
      if( !state )
          return;
      NCExplorerPaneContent &content = [state focusedContent];
      ExplorerTabEntry *const entry = content.HasTabsModel() ? content.FindEntry(content.TabsModel()->Active()) : nullptr;
      if( !entry || entry->panel != content.ActivePanel() )
          return;
      [entry->search_controller close];
      entry->panel.quickSearchPresentation = content.QuickSearchOverlay();
      [state.window makeFirstResponder:entry->panel.view];
    }];
}

- (std::optional<nc::core::SearchPlanningFacts>)searchPlanningFactsForPanel:(PanelController *)_panel
{
    NCExplorerPaneContent &content = [self focusedContent];
    if( !_panel || _panel != content.ActivePanel() || !content.HasTabsModel() ||
        content.TabsModel()->Active() != _panel.paneId || !content.LatestPaneSnapshot() ||
        content.LatestPaneSnapshot()->pane_id != _panel.paneId ) {
        return std::nullopt;
    }
    const nc::core::PaneSnapshot &snapshot = *content.LatestPaneSnapshot();
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
    NCExplorerPaneContent &content = [self focusedContent];
    if( !m_SearchModeView || !content.HasTabsModel() || !content.ActivePanel() ||
        content.TabsModel()->Active() != _pane_id || content.ActivePanel().paneId != _pane_id ) {
        return;
    }
    ExplorerTabEntry *const entry = content.FindEntry(_pane_id);
    if( !entry || entry->panel != content.ActivePanel() || !entry->search_controller )
        return;
    const std::optional<nc::core::SearchSnapshot> snapshot = entry->search_controller.snapshot;
    const bool presented = snapshot.has_value();
    entry->panel.quickSearchPresentation = presented ? nil : content.QuickSearchOverlay();
    if( presented )
        content.QuickSearchOverlay().searchPrompt = nil;
    [m_SearchModeView applySnapshot:snapshot
          resultSelectionEligible:static_cast<bool>(entry->search_controller.canRevealFocusedResult)];
}

- (BOOL)canPresentSearchForPanel:(PanelController *)_panel
{
    NCExplorerPaneContent &content = [self focusedContent];
    const ExplorerTabEntry *const entry = content.FindEntry(_panel);
    if( !entry || entry->panel != content.ActivePanel() || !content.HasTabsModel() ||
        content.TabsModel()->Active() != _panel.paneId || !entry->search_controller ) {
        return NO;
    }
    return entry->search_controller.isPresented || [self searchPlanningFactsForPanel:_panel].has_value();
}

- (BOOL)presentSearchForPanel:(PanelController *)_panel
                initialQuery:(NSString *)_query
              preferredScope:(const NCExplorerSearchPreferredScope)_scope
{
    NCExplorerPaneContent &content = [self focusedContent];
    ExplorerTabEntry *const entry = content.FindEntry(_panel);
    if( !entry || entry->panel != content.ActivePanel() || !content.HasTabsModel() ||
        content.TabsModel()->Active() != _panel.paneId || !entry->search_controller ) {
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
    return [self attachExplorerTabPanel:_panel createPaneStore:_create_pane_store toContent:[self focusedContent]];
}

- (BOOL)attachExplorerTabPanel:(PanelController *)_panel
                createPaneStore:(const BOOL)_create_pane_store
                      toContent:(NCExplorerPaneContent &)_content
{
    __weak NCExplorerState *weak_self = self;
    const nc::core::PaneId pane_id = _panel.paneId;
    const bool attached = _content.AttachTabPanel(
        _panel, _create_pane_store, self, self, [weak_self, pane_id](std::optional<nc::core::SearchSnapshot>) {
            NCExplorerState *const state = weak_self;
            if( state )
                [state applySearchSnapshotForPane:pane_id];
        });
    if( attached )
        [self invalidateExplorerRestorableState];
    return attached;
}

- (void)attachExplorerTabViewForPanel:(PanelController *)_panel
{
    NCExplorerPaneContent *const content = [self contentOwningPanel:_panel];
    if( content )
        content->AttachTabView(_panel);
}

- (PanelController *)allocateExplorerPanelForSessionRestore
{
    return [NCAppDelegate.me allocateExplorerPanelController];
}

- (PanelController *)allocateExplorerPanelForDualPane
{
    return [NCAppDelegate.me allocateExplorerPanelController];
}

- (BOOL)dualPaneCreatesPaneStore
{
    return true;
}

/**
 * Returns non-nil to make the dual-pane toggle build the right side's container the same
 * bare-panel-view way -initForTestingWithFrame:... already builds the Left side for unit tests,
 * instead of a real FilePanelsTabbedHolder. A real FilePanelsTabbedHolder needs an app-wide
 * bootstrapped ThemesManager this test binary does not set up (mirrors the pre-existing g_Config
 * bootstrap gap recorded in Development-Plan.md - see NCExplorerPaneContent::BuildTestingContainer
 * for the Left-side equivalent). Production never overrides this; it always returns nil.
 */
- (NSView *)dualPaneRightSideTestingContentViewForPanel:(PanelController *) [[maybe_unused]] _panel
{
    return nil;
}

- (void)loadNativeHomeForSessionPanel:(PanelController *)_panel
{
    if( !_panel || ![self contentOwningPanel:_panel] )
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
    if( !_panel || ![self contentOwningPanel:_panel] || !_location ||
        !IsStructurallyValidSessionLocation(*_location) ) {
        [self loadNativeHomeForSessionPanel:_panel];
        return;
    }

    const std::string path = _location->path;
    __weak NCExplorerState *weak_self = self;
    __weak PanelController *weak_panel = _panel;
    const auto is_owned = [weak_self, weak_panel] {
        NCExplorerState *const state = weak_self;
        PanelController *const panel = weak_panel;
        return state && panel && [state contentOwningPanel:panel] != nullptr;
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
    // The left side is always the first restore target and is the focused one while it is being
    // restored: dual-pane can only be turned on afterwards, by restorePanesFromSession: itself.
    [self rollbackSessionRestoreForContent:[self focusedContent]];
}

- (void)rollbackSessionRestoreForContent:(NCExplorerPaneContent &)_content
{
    _content.RollbackSessionRestore(self);
    [self invalidateExplorerRestorableState];
}

- (BOOL)restorePanesFromSession:(const nc::explorer::ExplorerPanesSession &)_session
{
    // Dual-pane is never on before a session is applied, so the left side is both the focused one
    // and the only one that exists at this point; its restore keeps the exact pre-DP-3 behaviour.
    if( m_DualPaneEnabled )
        return false;
    if( ![self restoreTabs:_session.left intoContent:[self contentForSide:NCExplorerPaneSide::Left]] )
        return false;
    if( !_session.right )
        return true;

    // From here the left side is already restored and is the window's primary content. A right side
    // that cannot be rebuilt therefore degrades this window to single-pane instead of discarding a
    // session that has already succeeded for the side the user is looking at.
    if( _session.divider_ratio ) {
        m_PaneDividerRatio =
            std::clamp(static_cast<CGFloat>(*_session.divider_ratio), g_MinimumPaneDividerRatio, g_MaximumPaneDividerRatio);
    }
    if( ![self setDualPaneEnabled:YES] )
        return true;
    if( ![self restoreTabs:*_session.right intoContent:[self contentForSide:NCExplorerPaneSide::Right]] ) {
        [self setDualPaneEnabled:NO];
        return true;
    }
    if( _session.right_focused )
        [self focusSide:NCExplorerPaneSide::Right];
    return true;
}

- (BOOL)restoreTabs:(const nc::explorer::ExplorerTabsSession &)_session intoContent:(NCExplorerPaneContent &)_content
{
    NCExplorerPaneContent &content = _content;
    if( content.SessionRestoreApplied() || content.RuntimeTabsMutated() || !content.HasTabsModel() ||
        content.TabsModel()->Size() != 1 || content.MutableEntries().size() != 1 || _session.tabs.empty() ||
        _session.tabs.size() > nc::explorer::ExplorerSessionPersistency::MaximumTabs ||
        _session.active_index >= _session.tabs.size() ) {
        return false;
    }

    std::vector<__strong PanelController *> panels;
    panels.reserve(_session.tabs.size());
    panels.emplace_back(content.MutableEntries().front().panel);
    std::unordered_set<uint64_t> pane_ids{panels.front().paneId.value};
    for( size_t index = 1; index < _session.tabs.size(); ++index ) {
        PanelController *const panel = [self allocateExplorerPanelForSessionRestore];
        if( !panel || panel.paneId.value == 0 || !pane_ids.emplace(panel.paneId.value).second )
            return false;
        panels.emplace_back(panel);
    }

    const BOOL create_pane_store = content.MutableEntries().front().pane_store != nullptr;
    const bool content_is_focused = &content == &[self focusedContent];
    content.SetSynchronizingTabs(true);
    for( size_t index = 1; index < panels.size(); ++index ) {
        // The focused side goes through the overridable 2-arg entry point (which resolves to
        // [self focusedContent], the same `content` as here) rather than the toContent: variant, so
        // that test doubles overriding attachExplorerTabPanel:createPaneStore: (e.g. to simulate a
        // failed attach) still apply. Only the dual-pane right side, restored while the left side
        // still holds focus, has to name its content explicitly.
        const BOOL attached = content_is_focused
                                  ? [self attachExplorerTabPanel:panels[index] createPaneStore:create_pane_store]
                                  : [self attachExplorerTabPanel:panels[index]
                                                 createPaneStore:create_pane_store
                                                       toContent:content];
        if( !attached ) {
            [self rollbackSessionRestoreForContent:content];
            content.SetSynchronizingTabs(false);
            return false;
        }
    }

    FilePanelsTabbedHolder *const tabbed_holder = content.TabbedHolder();
    if( tabbed_holder &&
        std::ranges::any_of(panels, [tabbed_holder](PanelController *_panel) {
            return [tabbed_holder tabViewItemForController:_panel] == nil;
        }) ) {
        [self rollbackSessionRestoreForContent:content];
        content.SetSynchronizingTabs(false);
        return false;
    }

    PanelController *const active_panel = panels[_session.active_index];
    if( !content.TabsModel()->Activate(active_panel.paneId) ) {
        [self rollbackSessionRestoreForContent:content];
        content.SetSynchronizingTabs(false);
        return false;
    }
    if( tabbed_holder ) {
        NSTabViewItem *const active_item = [tabbed_holder tabViewItemForController:active_panel];
        if( !active_item ) {
            [self rollbackSessionRestoreForContent:content];
            content.SetSynchronizingTabs(false);
            return false;
        }
        tabbed_holder.tabBarShown = panels.size() > 1;
        [tabbed_holder.tabBar selectTabViewItem:active_item];
    }
    content.SetSynchronizingTabs(false);

    content.SessionRestoreAppliedRef() = true;
    [self bindActivePanel:active_panel focus:false];
    for( size_t index = 0; index < panels.size(); ++index )
        [self restoreSessionLocation:_session.tabs[index].location forPanel:panels[index]];
    [self invalidateExplorerRestorableState];
    return true;
}

- (nc::explorer::ExplorerPanesSession)capturePanesSession
{
    nc::explorer::ExplorerPanesSession session;
    session.left = [self captureTabsForContent:[self contentForSide:NCExplorerPaneSide::Left]];
    if( !m_DualPaneEnabled )
        return session;
    session.right = [self captureTabsForContent:[self contentForSide:NCExplorerPaneSide::Right]];
    session.right_focused = m_FocusedSide == NCExplorerPaneSide::Right;
    session.divider_ratio = static_cast<double>(m_PaneDividerRatio);
    return session;
}

- (nc::explorer::ExplorerTabsSession)captureTabsForContent:(NCExplorerPaneContent &)_content
{
    nc::explorer::ExplorerTabsSession session;
    NCExplorerPaneContent &content = _content;
    if( !content.HasTabsModel() || content.MutableEntries().size() != content.TabsModel()->Size() )
        return session;

    session.active_index = content.TabsModel()->ActiveIndex();
    session.tabs.reserve(content.MutableEntries().size());
    for( const ExplorerTabEntry &entry : content.MutableEntries() ) {
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

- (void)explorerPaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot
                      forSide:(const NCExplorerPaneSide)_side
                        token:(const nc::core::ExplorerTabObservationToken)_token
{
    NCExplorerPaneContent &content = [self contentForSide:_side];
    if( !content.ApplySnapshotLocal(_snapshot, _token) )
        return;

    ExplorerTabEntry *const entry = content.FindEntry(_snapshot.pane_id);
    if( !entry )
        return;
    [self applyViewSettingsForSnapshot:_snapshot tabEntry:*entry forContent:content];

    if( _side != m_FocusedSide )
        return;
    [m_ToolbarDelegate applyPaneSnapshot:_snapshot];
    [m_CommandBar applyPaneSnapshot:_snapshot];
    [content.ActivePanel().view applyExplorerPaneSnapshot:_snapshot];
    [m_Inspector applyPaneSnapshot:_snapshot];
    [content.PaneStateView() updateWithVisualState:ExplorerPaneVisualState(_snapshot, content.ActivePanel())];
}

- (void)explorerPaneViewSettingsContextChangedForSide:(const NCExplorerPaneSide)_side
                                                panel:(PanelController *)_panel
                                                token:(const nc::core::ExplorerTabObservationToken)_token
{
    NCExplorerPaneContent &content = [self contentForSide:_side];
    ExplorerTabEntry *const live_entry = content.FindEntry(_panel);
    if( !live_entry || live_entry->view_settings_context_sample_scheduled )
        return;
    live_entry->view_settings_context_sample_scheduled = true;

    __weak NCExplorerState *weak_self = self;
    __weak PanelController *weak_panel = _panel;
    dispatch_async(dispatch_get_main_queue(), ^{
      // PaneStore and this observer see the same context notification, but observer order is not
      // contractual. A second hop lets its coalesced rebuild publish sort/group before we combine
      // that snapshot with the concrete layout sampled from PanelView.
      dispatch_async(dispatch_get_main_queue(), ^{
        NCExplorerState *const state = weak_self;
        PanelController *const panel = weak_panel;
        if( !state || !panel )
            return;
        NCExplorerPaneContent &scheduled_content = [state contentForSide:_side];
        ExplorerTabEntry *const scheduled_entry = scheduled_content.FindEntry(panel);
        if( !scheduled_entry )
            return;
        scheduled_entry->view_settings_context_sample_scheduled = false;
        if( !scheduled_content.HasTabsModel() ||
            !scheduled_content.ObservationGate().Accepts(_token, scheduled_content.TabsModel()->Active(), panel.paneId) ||
            scheduled_content.ActivePanel() != panel || !scheduled_content.LatestPaneSnapshot() ||
            scheduled_content.LatestPaneSnapshot()->pane_id != panel.paneId ) {
            return;
        }
        [state applyViewSettingsForSnapshot:*scheduled_content.LatestPaneSnapshot()
                                    tabEntry:*scheduled_entry
                                  forContent:scheduled_content];
      });
    });
}

- (void)applyViewSettingsForSnapshot:(const nc::core::PaneSnapshot &)_snapshot
                            tabEntry:(ExplorerTabEntry &)_entry
                          forContent:(NCExplorerPaneContent &)_content
{
    if( !_entry.view_settings_binding || _entry.panel != _content.ActivePanel() ||
        _snapshot.pane_id != _entry.panel.paneId )
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
    NCExplorerPaneContent *const content_ptr = [self contentOwningPanel:_panel];
    const std::optional<NCExplorerPaneSide> side_opt = [self sideOwningPanel:_panel];
    if( !content_ptr || !side_opt || !content_ptr->HasTabsModel() ||
        content_ptr->TabsModel()->Active() != _panel.paneId )
        return;
    NCExplorerPaneContent &content = *content_ptr;
    const NCExplorerPaneSide side = *side_opt;
    const bool becomes_focused = !m_DualPaneEnabled || _focus || side == m_FocusedSide;

    PanelController *const previous_in_side = content.DetachActiveObservation();
    PanelController *const previous_focused_panel = becomes_focused ? self.panelController : nil;

    if( previous_focused_panel && previous_focused_panel != _panel ) {
        [self closeAttachedUI:previous_focused_panel];
        previous_focused_panel.quickSearchPresentation = nil;
        previous_focused_panel.view.busyIndicatorOverride = nil;
    }
    if( previous_in_side && previous_in_side != _panel && !content.TabbedHolder() ) {
        previous_in_side.view.hidden = true;
        _panel.view.hidden = false;
    }

    content.BindActiveObservation(_panel, side, self);

    if( becomes_focused ) {
        m_FocusedSide = side;
        [m_ToolbarDelegate rebindToPanelController:_panel];
        [m_Sidebar rebindToPanelController:_panel];
        [m_CommandBar rebindToPanelController:_panel];
        [m_Inspector rebindToPaneID:_panel.paneId];
        _panel.view.busyIndicatorOverride = m_ToolbarDelegate.busyIndicator;
    }
    _panel.quickSearchPresentation = content.QuickSearchOverlay();

    [self applySearchSnapshotForPane:_panel.paneId];
    if( _focus )
        [self.window makeFirstResponder:_panel.view];
}

/** Moves window focus (and shared chrome binding) between sides without changing either side's
 *  active tab. The Tab key uses this; tab activation/closure keeps going through bindActivePanel:. */
- (BOOL)focusSide:(const NCExplorerPaneSide)_side
{
    if( !m_DualPaneEnabled || _side == m_FocusedSide )
        return false;
    NCExplorerPaneContent &content = [self contentForSide:_side];
    PanelController *const panel = content.ActivePanel();
    if( !panel )
        return false;

    PanelController *const previous_focused_panel = self.panelController;
    if( previous_focused_panel && previous_focused_panel != panel ) {
        [self closeAttachedUI:previous_focused_panel];
        previous_focused_panel.quickSearchPresentation = nil;
        previous_focused_panel.view.busyIndicatorOverride = nil;
    }

    m_FocusedSide = _side;
    [m_ToolbarDelegate rebindToPanelController:panel];
    [m_Sidebar rebindToPanelController:panel];
    [m_CommandBar rebindToPanelController:panel];
    [m_Inspector rebindToPaneID:panel.paneId];
    panel.view.busyIndicatorOverride = m_ToolbarDelegate.busyIndicator;
    panel.quickSearchPresentation = content.QuickSearchOverlay();

    [self applySearchSnapshotForPane:panel.paneId];
    [self.window makeFirstResponder:panel.view];
    [self invalidateExplorerRestorableState];
    return true;
}

- (void)changeFocusedSide
{
    if( !m_DualPaneEnabled )
        return;
    const NCExplorerPaneSide next =
        m_FocusedSide == NCExplorerPaneSide::Left ? NCExplorerPaneSide::Right : NCExplorerPaneSide::Left;
    [self focusSide:next];
}

#pragma mark - Dual Pane cross-pane commands (DP-2)

/**
 * The other side's active panel, or nil when dual-pane is off, `_panel` is not a known side's
 * active panel, or that other side has no panel yet. Mirrors legacy MakeNew.mm's
 * FindOppositeController, re-derived from this state's own dual-pane primitives instead of the
 * MainWindowFilePanelState-specific ones CopyTo/MoveTo/SwapPanels depend on (NCExplorerState is
 * not that class and exposes no activePanelController/oppositePanelController of its own).
 */
- (PanelController *)dualPaneOppositePanelControllerFor:(PanelController *)_panel
{
    if( !m_DualPaneEnabled )
        return nil;
    const std::optional<NCExplorerPaneSide> side = [self sideOwningPanel:_panel];
    if( !side )
        return nil;
    const NCExplorerPaneSide opposite_side =
        *side == NCExplorerPaneSide::Left ? NCExplorerPaneSide::Right : NCExplorerPaneSide::Left;
    return [self contentForSide:opposite_side].ActivePanel();
}

/**
 * Single source of truth for whether a cross-pane copy/move can proceed from `_active` to
 * `_opposite`, shared by -validateMenuItem: and both action methods so the two can never diverge
 * (an action method that skipped straight past a condition -validateMenuItem: already rejected on
 * is exactly how a "disabled" menu item would still crash if invoked directly, e.g. from a test).
 */
- (BOOL)canCopyOrMoveFromActive:(PanelController *)_active toOpposite:(PanelController *)_opposite
{
    return _active != nil && _opposite != nil && _opposite.isUniform && _opposite.vfs != nullptr &&
           _opposite.vfs->IsWritable() && !_active.selectedEntriesOrFocusedEntry.empty();
}

/**
 * Reduces one pane's visible listing to what the comparison judges, keeping each item's sorted
 * position so a verdict can be marked back onto the exact row it came from. Dot-dot is a navigation
 * entry, never a comparison subject, and is dropped here rather than rejected by the model.
 */
- (ExplorerCompareSide)collectCompareSideForPanel:(PanelController *)_panel
{
    ExplorerCompareSide side;
    const int count = _panel.data.SortedEntriesCount();
    side.items.reserve(static_cast<size_t>(std::max(0, count)));
    side.sorted_positions.reserve(static_cast<size_t>(std::max(0, count)));
    for( int position = 0; position < count; ++position ) {
        const VFSListingItem item = _panel.data.EntryAtSortPosition(position);
        if( !item || item.IsDotDot() )
            continue;
        if( !item.HasMTime() )
            side.has_all_modification_times = false;
        side.items.push_back({.name = item.Filename(),
                              .size = item.Size(),
                              .modification_time = item.HasMTime() ? static_cast<int64_t>(item.MTime()) : 0,
                              .is_directory = item.IsDir()});
        side.sorted_positions.push_back(position);
    }
    return side;
}

/**
 * Single source of truth for whether a dual-pane compare can run, shared by -validateMenuItem: and
 * the action itself for the same reason -canCopyOrMoveFromActive:toOpposite: is (see above).
 */
- (BOOL)canCompareDualPaneDirectories
{
    if( !m_DualPaneEnabled )
        return false;
    PanelController *const left = [self contentForSide:NCExplorerPaneSide::Left].ActivePanel();
    PanelController *const right = [self contentForSide:NCExplorerPaneSide::Right].ActivePanel();
    return left != nil && right != nil && left != right && left.isUniform && right.isUniform;
}

/**
 * Compares the two dual-pane sides and leaves each pane with exactly the entries that differ
 * selected, which composes directly with DP-2's F5/F6: compare, then copy the marked set across.
 */
- (IBAction)OnCompareDirectories:(id) [[maybe_unused]] _sender
{
    if( ![self canCompareDualPaneDirectories] )
        return;
    PanelController *const left = [self contentForSide:NCExplorerPaneSide::Left].ActivePanel();
    PanelController *const right = [self contentForSide:NCExplorerPaneSide::Right].ActivePanel();

    const VFSListingPtr left_listing = left.data.ListingPtr();
    const VFSListingPtr right_listing = right.data.ListingPtr();
    const ExplorerCompareSide left_side = [self collectCompareSideForPanel:left];
    const ExplorerCompareSide right_side = [self collectCompareSideForPanel:right];

    // A provider that publishes no modification time cannot be judged by date; fall back to size
    // alone rather than treating every missing timestamp as the same instant.
    nc::core::FolderCompareOptions options;
    options.compare_modification_time =
        left_side.has_all_modification_times && right_side.has_all_modification_times;

    const nc::core::FolderComparisonResult comparison =
        nc::core::CompareFolders(left_side.items, right_side.items, options);
    if( !comparison )
        return;
    const nc::core::FolderCompareMarks marks =
        nc::core::MarkDifferences(*comparison, left_side.items.size(), right_side.items.size());

    // Both listings are revalidated before either selection is applied, so a refresh that lands on
    // one side mid-comparison cannot leave the two panes marked from inconsistent snapshots.
    if( left.isDoingBackgroundLoading || right.isDoingBackgroundLoading ||
        left.data.ListingPtr() != left_listing || right.data.ListingPtr() != right_listing )
        return;

    [self applyCompareMarks:marks.left positions:left_side.sorted_positions toPanel:left];
    [self applyCompareMarks:marks.right positions:right_side.sorted_positions toPanel:right];
}

/** Expands a per-collected-item mark vector back onto the pane's full sorted selection vector. */
- (void)applyCompareMarks:(const std::vector<bool> &)_marks
                positions:(const std::vector<int> &)_positions
                  toPanel:(PanelController *)_panel
{
    const int count = _panel.data.SortedEntriesCount();
    if( count < 0 )
        return;
    std::vector<bool> selection(static_cast<size_t>(count), false);
    for( size_t index = 0; index < _marks.size() && index < _positions.size(); ++index ) {
        const int position = _positions[index];
        if( _marks[index] && position >= 0 && position < count )
            selection[static_cast<size_t>(position)] = true;
    }
    [_panel setEntriesSelection:selection];
}

#pragma mark - Command palette (Q2-3 CP-2)

/**
 * The one context the palette both queries and executes with.
 *
 * Building it once and reusing it is the safety property of this surface: a row is offered because
 * the registry reported it enabled *for this exact context*, and the command later runs against the
 * same one. Re-deriving a context between listing and running would let a row be offered under one
 * set of facts and executed under another.
 */
- (nc::core::CommandContext)commandPaletteContextWithItems:(const std::vector<VFSListingItem> &)_items
{
    nc::core::CommandContext context;
    context.source = nc::core::CommandInvocationSource::Palette;
    context.native_target = (__bridge void *)self.panelController;
    context.items = _items;
    return context;
}

- (BOOL)canOpenCommandPalette
{
    return NCAppDelegate.me != nil && self.panelController != nil;
}

- (IBAction)OnCommandPaletteOpen:(id) [[maybe_unused]] _sender
{
    if( ![self canOpenCommandPalette] || m_CommandPalette != nil )
        return;
    nc::core::CommandRegistry &registry = NCAppDelegate.me.commandRegistry;
    const std::vector<VFSListingItem> items = self.panelController.selectedEntriesOrFocusedEntry;
    const nc::core::CommandContext context = [self commandPaletteContextWithItems:items];

    std::vector<nc::core::CommandPaletteSource> sources;
    sources.reserve(registry.All().size());
    for( const nc::core::CommandDescriptor &descriptor : registry.All() ) {
        const nc::core::CommandRegistry::StateResult state = registry.QueryState(descriptor.id, context);
        if( state.status != nc::core::CommandRegistry::LookupStatus::Found )
            continue;
        sources.push_back({.id = std::string{descriptor.id.Value()},
                           .title = CommandPaletteTitle(descriptor.title_key),
                           .category = CommandPaletteCategoryName(descriptor.category),
                           .visible = state.state.visible,
                           .enabled = state.state.enabled});
    }

    m_CommandPalette = [[NCExplorerCommandPaletteView alloc] initWithFrame:NSZeroRect];
    m_CommandPalette.paletteDelegate = self;
    m_CommandPalette.translatesAutoresizingMaskIntoConstraints = false;
    [m_CommandPalette setRoster:nc::core::BuildCommandPaletteRoster(sources)];
    [self addSubview:m_CommandPalette positioned:NSWindowAbove relativeTo:nil];
    const NSSize size = [NCExplorerCommandPaletteView preferredSize];
    [NSLayoutConstraint activateConstraints:@[
        [m_CommandPalette.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
        [m_CommandPalette.topAnchor constraintEqualToAnchor:self.topAnchor constant:80.0],
        [m_CommandPalette.widthAnchor constraintEqualToConstant:size.width],
        [m_CommandPalette.heightAnchor constraintEqualToConstant:size.height],
    ]];
    [self layoutSubtreeIfNeeded];
    [m_CommandPalette focusQueryField];
}

- (void)commandPaletteDidDismiss:(NCExplorerCommandPaletteView *) [[maybe_unused]] _palette
{
    if( !m_CommandPalette )
        return;
    [m_CommandPalette removeFromSuperview];
    m_CommandPalette = nil;
    if( PanelController *const panel = self.panelController )
        [self.window makeFirstResponder:panel.view];
}

- (void)commandPalette:(NCExplorerCommandPaletteView *) [[maybe_unused]] _palette
    didChooseCommandId:(const std::string &)_command_id
{
    if( ![self canOpenCommandPalette] )
        return;
    // The items are re-read here rather than captured when the palette opened: the palette is modal
    // to the keyboard but not to the world, and executing against a stale selection would act on
    // files the user is no longer pointing at. The registry re-checks the state either way.
    const std::vector<VFSListingItem> items = self.panelController.selectedEntriesOrFocusedEntry;
    const nc::core::CommandContext context = [self commandPaletteContextWithItems:items];
    [[maybe_unused]] const auto result =
        NCAppDelegate.me.commandRegistry.Execute(nc::core::CommandId{_command_id}, context);
}

/**
 * Whether a one-way sync can run at all: everything Compare needs, plus a writable destination.
 * Shared by -validateMenuItem: and the action, for the same reason the compare/copy predicates are.
 */
- (BOOL)canSynchronizeDualPaneDirectories
{
    if( ![self canCompareDualPaneDirectories] )
        return false;
    PanelController *const destination = [self dualPaneOppositePanelControllerFor:self.panelController];
    return destination.vfs != nullptr && destination.vfs->IsWritable();
}

/** Names currently at each collected position, in the index space the comparison was built from. */
static std::vector<std::string> CollectedNames(const ExplorerCompareSide &_side)
{
    std::vector<std::string> names;
    names.reserve(_side.items.size());
    for( const nc::core::FolderCompareItem &item : _side.items )
        names.push_back(item.name);
    return names;
}

/** Human-readable dry-run body: what would change, with the destructive part called out first. */
static NSString *SyncPreviewText(const nc::core::FolderSyncPlan &_plan,
                                 const nc::core::FolderSyncPlan &_deleting_plan)
{
    const nc::core::FolderSyncSummary summary = _plan.Summarize();
    NSMutableArray<NSString *> *const lines = [NSMutableArray new];
    [lines addObject:[NSString stringWithFormat:NSLocalizedString(@"Copy %lu new item(s).",
                                                                   "Explorer sync preview"),
                                                 static_cast<unsigned long>(summary.create)]];
    [lines addObject:[NSString stringWithFormat:NSLocalizedString(@"Replace %lu changed item(s).",
                                                                   "Explorer sync preview"),
                                                 static_cast<unsigned long>(summary.overwrite)]];
    if( summary.overwrite_newer_destination > 0 ) {
        // The one case a one-way sync silently loses data the user may want: say it plainly.
        [lines addObject:[NSString stringWithFormat:NSLocalizedString(
                                                        @"%lu of those are NEWER in the destination and will be lost.",
                                                        "Explorer sync preview"),
                                                     static_cast<unsigned long>(summary.overwrite_newer_destination)]];
    }
    [lines addObject:[NSString stringWithFormat:NSLocalizedString(@"Leave %lu item(s) untouched.",
                                                                   "Explorer sync preview"),
                                                 static_cast<unsigned long>(summary.skip)]];

    const std::vector<const nc::core::FolderSyncAction *> deletions = _deleting_plan.Deletions();
    if( !deletions.empty() ) {
        [lines addObject:@""];
        [lines addObject:[NSString stringWithFormat:NSLocalizedString(
                                                        @"%lu item(s) exist only in the destination:",
                                                        "Explorer sync preview"),
                                                     static_cast<unsigned long>(deletions.size())]];
        constexpr size_t max_listed = 10;
        for( size_t index = 0; index < deletions.size() && index < max_listed; ++index )
            [lines addObject:[NSString stringWithFormat:@"    %s", deletions[index]->name.c_str()]];
        if( deletions.size() > max_listed ) {
            [lines addObject:[NSString stringWithFormat:NSLocalizedString(@"    …and %lu more.",
                                                                           "Explorer sync preview"),
                                                         static_cast<unsigned long>(deletions.size() - max_listed)]];
        }
    }
    return [lines componentsJoinedByString:@"\n"];
}

/**
 * Compares both sides, shows the dry run, and submits only what the user approved.
 *
 * The plan built here is the dry run - nothing simulates separately - and execution re-binds it to
 * the live listings before submitting, so a plan reviewed against listings that have since moved is
 * abandoned rather than applied to files the user never saw.
 */
- (IBAction)OnSynchronizeDirectories:(id) [[maybe_unused]] _sender
{
    if( ![self canSynchronizeDualPaneDirectories] )
        return;
    PanelController *const source = self.panelController;
    PanelController *const destination = [self dualPaneOppositePanelControllerFor:source];

    const VFSListingPtr source_listing = source.data.ListingPtr();
    const VFSListingPtr destination_listing = destination.data.ListingPtr();
    const ExplorerCompareSide source_side = [self collectCompareSideForPanel:source];
    const ExplorerCompareSide destination_side = [self collectCompareSideForPanel:destination];

    nc::core::FolderCompareOptions compare_options;
    compare_options.compare_modification_time =
        source_side.has_all_modification_times && destination_side.has_all_modification_times;
    // The comparison is built with the source as its left side, so the plan direction is fixed and
    // the "left/right" of the model never has to be reconciled with which pane happens to be focused.
    const nc::core::FolderComparisonResult comparison =
        nc::core::CompareFolders(source_side.items, destination_side.items, compare_options);
    if( !comparison )
        return;

    const nc::core::FolderSyncPlan plan =
        nc::core::PlanOneWaySync(*comparison, nc::core::FolderSyncDirection::LeftToRight);
    const nc::core::FolderSyncPlan deleting_plan = nc::core::PlanOneWaySync(
        *comparison, nc::core::FolderSyncDirection::LeftToRight, {.delete_extraneous = true});

    if( plan.IsEmpty() && !deleting_plan.HasDeletions() ) {
        Alert *const nothing = [[Alert alloc] init];
        nothing.messageText = NSLocalizedString(@"These folders are already synchronized.",
                                                 "Explorer sync preview");
        nothing.informativeText = SyncPreviewText(plan, deleting_plan);
        [nothing beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse){
        }];
        return;
    }

    Alert *const alert = [[Alert alloc] init];
    alert.messageText = [NSString stringWithFormat:NSLocalizedString(@"Synchronize “%s” into “%s”?",
                                                                      "Explorer sync preview"),
                                                    source.currentDirectoryPath.c_str(),
                                                    destination.currentDirectoryPath.c_str()];
    alert.informativeText = SyncPreviewText(plan, deleting_plan);
    [alert addButtonWithTitle:NSLocalizedString(@"Synchronize", "Explorer sync preview")];
    [alert addButtonWithTitle:NSLocalizedString(@"Cancel", "Explorer sync preview")];
    const bool offers_deletion = deleting_plan.HasDeletions() && destination.vfs->IsNativeFS();
    if( offers_deletion ) {
        // Deletion is a separate, explicitly labelled choice rather than a checkbox on the safe
        // one, so it can never be armed by accident (§45: deletion is separate and highlighted).
        [alert addButtonWithTitle:[NSString
                                      stringWithFormat:NSLocalizedString(@"Synchronize and Trash %lu Item(s)",
                                                                          "Explorer sync preview"),
                                                        static_cast<unsigned long>(
                                                            deleting_plan.Deletions().size())]];
    }

    __weak NCExplorerState *weak_self = self;
    __weak PanelController *weak_source = source;
    __weak PanelController *weak_destination = destination;
    [alert beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse _response) {
                    if( _response != NSAlertFirstButtonReturn && _response != NSAlertThirdButtonReturn )
                        return;
                    if( _response == NSAlertThirdButtonReturn && !offers_deletion )
                        return;
                    NCExplorerState *const state = weak_self;
                    if( !state )
                        return;
                    [state submitSyncPlan:_response == NSAlertThirdButtonReturn ? deleting_plan : plan
                                   source:weak_source
                             sourceListing:source_listing
                          sourcePositions:source_side.sorted_positions
                              destination:weak_destination
                       destinationListing:destination_listing
                     destinationPositions:destination_side.sorted_positions];
                  }];
}

/**
 * The mutation gate. Nothing reaches the Pool until both listings are still the exact ones the plan
 * was reviewed against and every referenced name still resolves to itself.
 */
- (void)submitSyncPlan:(const nc::core::FolderSyncPlan &)_plan
                 source:(PanelController *)_source
          sourceListing:(const VFSListingPtr &)_source_listing
       sourcePositions:(const std::vector<int> &)_source_positions
           destination:(PanelController *)_destination
    destinationListing:(const VFSListingPtr &)_destination_listing
  destinationPositions:(const std::vector<int> &)_destination_positions
{
    if( !_source || !_destination || ![self canSynchronizeDualPaneDirectories] )
        return;
    // The panes must still hold the very listings the preview described. A refresh or a navigation
    // between review and confirmation invalidates the whole plan, not part of it.
    if( _source.isDoingBackgroundLoading || _destination.isDoingBackgroundLoading ||
        _source.data.ListingPtr() != _source_listing || _destination.data.ListingPtr() != _destination_listing )
        return;

    const ExplorerCompareSide source_side = [self collectCompareSideForPanel:_source];
    const ExplorerCompareSide destination_side = [self collectCompareSideForPanel:_destination];
    const std::optional<nc::core::FolderSyncSubmission> submission =
        nc::core::BindSyncPlan(_plan, CollectedNames(source_side), CollectedNames(destination_side));
    if( !submission || source_side.sorted_positions != _source_positions ||
        destination_side.sorted_positions != _destination_positions )
        return;

    const auto resolve = [](PanelController *_panel,
                            const std::vector<int> &_positions,
                            const std::vector<size_t> &_indices) -> std::optional<std::vector<VFSListingItem>> {
        std::vector<VFSListingItem> items;
        items.reserve(_indices.size());
        for( const size_t index : _indices ) {
            if( index >= _positions.size() )
                return std::nullopt;
            const VFSListingItem item = _panel.data.EntryAtSortPosition(_positions[index]);
            if( !item )
                return std::nullopt;
            items.push_back(item);
        }
        return items;
    };
    const std::optional<std::vector<VFSListingItem>> copy_items =
        resolve(_source, source_side.sorted_positions, submission->copy_source_indices);
    const std::optional<std::vector<VFSListingItem>> delete_items =
        resolve(_destination, destination_side.sorted_positions, submission->delete_destination_indices);
    if( !copy_items || !delete_items )
        return;

    __weak PanelController *weak_source = _source;
    __weak PanelController *weak_destination = _destination;
    const auto refresh = [weak_source, weak_destination] {
        dispatch_to_main_queue([weak_source, weak_destination] {
          [weak_source refreshPanel];
          [weak_destination refreshPanel];
        });
    };

    if( !copy_items->empty() ) {
        nc::ops::CopyingOptions options;
        // A sync's whole purpose is to make the destination match, so a changed entry is replaced
        // outright instead of raising the interactive conflict sheet for every single file.
        options.exist_behavior = nc::ops::CopyingOptions::ExistBehavior::OverwriteAll;
        const auto copying = std::make_shared<nc::ops::Copying>(
            *copy_items, _destination.currentDirectoryPath, _destination.vfs, options);
        copying->ObserveUnticketed(nc::ops::Operation::NotifyAboutFinish, refresh);
        [_source.mainWindowController enqueueOperation:copying];
    }
    if( !delete_items->empty() ) {
        // Trash rather than permanent removal, per the destructive-action rules; the offer is only
        // made for a native destination, which is the only place a Trash actually exists.
        nc::ops::DeletionOptions options;
        options.type = nc::ops::DeletionType::Trash;
        const auto deletion = std::make_shared<nc::ops::Deletion>(*delete_items, options);
        deletion->ObserveUnticketed(nc::ops::Operation::NotifyAboutFinish, refresh);
        [_source.mainWindowController enqueueOperation:deletion];
    }
}

- (IBAction)OnFileCopyCommand:(id) [[maybe_unused]] _sender
{
    PanelController *const active = self.panelController;
    PanelController *const opposite = [self dualPaneOppositePanelControllerFor:active];
    if( ![self canCopyOrMoveFromActive:active toOpposite:opposite] )
        return;
    const std::vector<VFSListingItem> entries = active.selectedEntriesOrFocusedEntry;
    const bool active_uniform = active.isUniform;

    NCOpsCopyingDialog *const dialog =
        [[NCOpsCopyingDialog alloc] initWithItems:entries
                                        sourceVFS:active_uniform ? active.vfs : nullptr
                                  sourceDirectory:active_uniform ? active.currentDirectoryPath : ""
                               initialDestination:opposite.currentDirectoryPath
                                   destinationVFS:opposite.vfs
                                 operationOptions:nc::ops::CopyingOptions{}];
    __weak PanelController *weak_active = active;
    __weak PanelController *weak_opposite = opposite;
    const auto handler = ^(NSModalResponse _return_code) {
      if( _return_code != NSModalResponseOK )
          return;
      const std::string path = dialog.resultDestination;
      const VFSHostPtr host = dialog.resultHost;
      const nc::ops::CopyingOptions options = dialog.resultOptions;
      if( !host || path.empty() )
          return;
      const auto op = std::make_shared<nc::ops::Copying>(entries, path, host, options);
      op->ObserveUnticketed(nc::ops::Operation::NotifyAboutFinish, [weak_active, weak_opposite] {
        dispatch_to_main_queue([weak_active, weak_opposite] {
          [weak_active refreshPanel];
          [weak_opposite refreshPanel];
        });
      });
      [active.mainWindowController enqueueOperation:op];
    };
    [active.mainWindowController beginSheet:dialog.window completionHandler:handler];
}

- (IBAction)OnFileRenameMoveCommand:(id) [[maybe_unused]] _sender
{
    PanelController *const active = self.panelController;
    PanelController *const opposite = [self dualPaneOppositePanelControllerFor:active];
    if( ![self canCopyOrMoveFromActive:active toOpposite:opposite] )
        return;
    const std::vector<VFSListingItem> entries = active.selectedEntriesOrFocusedEntry;
    const bool active_uniform = active.isUniform;

    NCOpsCopyingDialog *const dialog =
        [[NCOpsCopyingDialog alloc] initWithItems:entries
                                        sourceVFS:active_uniform ? active.vfs : nullptr
                                  sourceDirectory:active_uniform ? active.currentDirectoryPath : ""
                               initialDestination:opposite.currentDirectoryPath
                                   destinationVFS:opposite.vfs
                                 operationOptions:nc::ops::CopyingOptions{.docopy = false}];
    __weak PanelController *weak_active = active;
    __weak PanelController *weak_opposite = opposite;
    const auto handler = ^(NSModalResponse _return_code) {
      if( _return_code != NSModalResponseOK )
          return;
      const std::string path = dialog.resultDestination;
      const VFSHostPtr host = dialog.resultHost;
      const nc::ops::CopyingOptions options = dialog.resultOptions;
      if( !host || path.empty() )
          return;
      const auto op = std::make_shared<nc::ops::Copying>(entries, path, host, options);
      op->ObserveUnticketed(nc::ops::Operation::NotifyAboutFinish, [weak_active, weak_opposite] {
        dispatch_to_main_queue([weak_active, weak_opposite] {
          [weak_active refreshPanel];
          [weak_opposite refreshPanel];
        });
      });
      [active.mainWindowController enqueueOperation:op];
    };
    [active.mainWindowController beginSheet:dialog.window completionHandler:handler];
}

- (IBAction)OnSwapPanels:(id) [[maybe_unused]] _sender
{
    if( !m_DualPaneEnabled )
        return;
    NCExplorerPaneContent &left = [self contentForSide:NCExplorerPaneSide::Left];
    NCExplorerPaneContent &right = [self contentForSide:NCExplorerPaneSide::Right];
    PanelController *const previous_left_panel = left.ActivePanel();
    PanelController *const previous_right_panel = right.ActivePanel();
    if( !previous_left_panel || !previous_right_panel )
        return;

    left.DetachActiveObservation();
    right.DetachActiveObservation();
    left.SwapStaticState(right);
    left.ActivePanelRef() = previous_right_panel;
    right.ActivePanelRef() = previous_left_panel;

    if( m_PaneSplitView.subviews.count == 2 ) {
        NSView *const first_subview = m_PaneSplitView.subviews.firstObject;
        NSView *const second_subview = m_PaneSplitView.subviews.lastObject;
        [first_subview removeFromSuperview];
        [second_subview removeFromSuperview];
        [m_PaneSplitView addSubview:second_subview];
        [m_PaneSplitView addSubview:first_subview];
        [m_PaneSplitView adjustSubviews];
    }

    left.BindActiveObservation(previous_right_panel, NCExplorerPaneSide::Left, self);
    right.BindActiveObservation(previous_left_panel, NCExplorerPaneSide::Right, self);
    [self bindActivePanel:[self focusedContent].ActivePanel() focus:false];
    [self invalidateExplorerRestorableState];
}

- (void)updateTabLabelForPanel:(PanelController *)_panel
{
    NCExplorerPaneContent *const content = [self contentOwningPanel:_panel];
    if( content )
        content->UpdateTabLabel(_panel);
}

- (void)closeTabViewItem:(NSTabViewItem *)_item
{
    const std::optional<nc::core::PaneId> pane_id = PaneIdFromTabItem(_item);
    const std::optional<NCExplorerPaneSide> side_opt = pane_id ? [self sideOwningPaneId:*pane_id] : std::nullopt;
    if( !side_opt )
        return;
    const NCExplorerPaneSide side = *side_opt;
    NCExplorerPaneContent &content = [self contentForSide:side];
    if( !content.HasTabsModel() || content.TabsModel()->Size() <= 1 )
        return;

    ExplorerTabEntry *const entry = content.FindEntry(*pane_id);
    if( !entry || _item.view != entry->panel.view )
        return;

    PanelController *const closing_panel = entry->panel;
    [entry->search_controller close];
    const bool closing_active = content.TabsModel()->Active() == *pane_id;
    const auto result = content.TabsModel()->Close(*pane_id);
    if( !result )
        return;
    content.RuntimeTabsMutatedRef() = true;
    const bool side_was_focused = side == m_FocusedSide;

    if( closing_active ) {
        content.DetachActiveObservation();
        [self closeAttachedUI:closing_panel];
        closing_panel.quickSearchPresentation = nil;
        closing_panel.view.busyIndicatorOverride = nil;
    }

    [closing_panel.view removeKeystrokeSink:self];
    closing_panel.state = nil;
    std::erase_if(content.MutableEntries(),
                 [pane_id](const ExplorerTabEntry &_entry) { return _entry.panel.paneId == *pane_id; });
    content.TabbedHolder().tabBarShown = content.TabsModel()->Size() > 1;
    [self invalidateExplorerRestorableState];

    if( closing_active ) {
        ExplorerTabEntry *const active = content.FindEntry(content.TabsModel()->Active());
        if( !active )
            return;
        NSTabViewItem *const active_item = [content.TabbedHolder() tabViewItemForController:active->panel];
        content.SetSynchronizingTabs(true);
        [content.TabbedHolder().tabBar selectTabViewItem:active_item];
        content.SetSynchronizingTabs(false);
        [self bindActivePanel:active->panel focus:side_was_focused];
    }
}

- (std::optional<NCExplorerPaneSide>)sideOwningTabView:(NSTabView *)_tab_view
{
    for( size_t index = 0; index < g_ExplorerPaneSideCount; ++index )
        if( m_Sides[index].TabbedHolder() && m_Sides[index].TabbedHolder().tabView == _tab_view )
            return static_cast<NCExplorerPaneSide>(index);
    return std::nullopt;
}

- (IBAction)OnFileNewTab:(id) [[maybe_unused]] _sender
{
    NCExplorerPaneContent &content = [self focusedContent];
    if( !content.HasTabsModel() || !content.TabbedHolder() || !content.ActivePanel() ||
        content.TabsModel()->Size() >= nc::explorer::ExplorerSessionPersistency::MaximumTabs ) {
        return;
    }

    PanelController *const source = content.ActivePanel();
    PanelController *const panel = [NCAppDelegate.me allocateExplorerPanelController];
    if( !panel )
        return;
    [panel copyOptionsFromController:source];
    if( ![self attachExplorerTabPanel:panel createPaneStore:true toContent:content] )
        return;

    content.SetSynchronizingTabs(true);
    NSTabViewItem *const item = [content.TabbedHolder() tabViewItemForController:panel];
    content.TabbedHolder().tabBarShown = true;
    [content.TabbedHolder().tabBar selectTabViewItem:item];
    content.SetSynchronizingTabs(false);
    content.RuntimeTabsMutatedRef() = true;

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
    NCExplorerPaneContent &content = [self focusedContent];
    if( !content.HasTabsModel() || content.TabsModel()->Size() == 1 ) {
        if( !m_DualPaneEnabled )
            [self.window performClose:_sender];
        return;
    }

    ExplorerTabEntry *const active = content.FindEntry(content.TabsModel()->Active());
    NSTabViewItem *const item = active ? [content.TabbedHolder() tabViewItemForController:active->panel] : nil;
    if( !item )
        return;
    content.SetSynchronizingTabs(true);
    [content.TabbedHolder().tabBar removeTabViewItem:item];
    content.SetSynchronizingTabs(false);
    [self closeTabViewItem:item];
}

- (IBAction)OnWindowShowPreviousTab:(id) [[maybe_unused]] _sender
{
    [[self focusedContent].TabbedHolder() selectPreviousFilePanelTab];
}

- (IBAction)OnWindowShowNextTab:(id) [[maybe_unused]] _sender
{
    [[self focusedContent].TabbedHolder() selectNextFilePanelTab];
}

- (BOOL)validateMenuItem:(NSMenuItem *)_item
{
    NCExplorerPaneContent &content = [self focusedContent];
    if( _item.action == @selector(OnFileNewTab:) )
        return content.HasTabsModel() &&
               content.TabsModel()->Size() < nc::explorer::ExplorerSessionPersistency::MaximumTabs;
    if( _item.action == @selector(performClose:) ) {
        const bool only_tab = !content.HasTabsModel() || content.TabsModel()->Size() == 1;
        _item.title = (only_tab && !m_DualPaneEnabled) ? NSLocalizedString(@"Close Window", "Explorer File menu")
                                                       : NSLocalizedString(@"Close Tab", "Explorer File menu");
        return !only_tab || !m_DualPaneEnabled;
    }
    if( _item.action == @selector(OnWindowShowPreviousTab:) || _item.action == @selector(OnWindowShowNextTab:) )
        return content.HasTabsModel() && content.TabsModel()->Size() > 1;
    if( _item.action == @selector(onSwitchDualSinglePaneMode:) ) {
        _item.title = m_DualPaneEnabled ? NSLocalizedString(@"Switch to Single Pane", "Explorer View menu")
                                        : NSLocalizedString(@"Switch to Dual Pane", "Explorer View menu");
        return true;
    }
    if( _item.action == @selector(OnSwapPanels:) )
        return m_DualPaneEnabled && [self focusedContent].ActivePanel() != nil &&
               [self contentForSide:m_FocusedSide == NCExplorerPaneSide::Left ? NCExplorerPaneSide::Right
                                                                              : NCExplorerPaneSide::Left]
                       .ActivePanel() != nil;
    if( _item.action == @selector(OnFileCopyCommand:) || _item.action == @selector(OnFileRenameMoveCommand:) ) {
        PanelController *const active = self.panelController;
        PanelController *const opposite = [self dualPaneOppositePanelControllerFor:active];
        return [self canCopyOrMoveFromActive:active toOpposite:opposite];
    }
    if( _item.action == @selector(OnCompareDirectories:) )
        return [self canCompareDualPaneDirectories];
    if( _item.action == @selector(OnSynchronizeDirectories:) )
        return [self canSynchronizeDualPaneDirectories];
    if( _item.action == @selector(OnCommandPaletteOpen:) )
        return [self canOpenCommandPalette];
    return false;
}

- (void)addNewTabToTabView:(NSTabView *)_tab_view
{
    const std::optional<NCExplorerPaneSide> side = [self sideOwningTabView:_tab_view];
    if( side && *side == m_FocusedSide )
        [self OnFileNewTab:[self focusedContent].TabbedHolder().tabBar];
}

- (void)tabView:(NSTabView *)_tab_view didSelectTabViewItem:(NSTabViewItem *)_item
{
    const std::optional<NCExplorerPaneSide> side_opt = [self sideOwningTabView:_tab_view];
    if( !side_opt )
        return;
    NCExplorerPaneContent &content = [self contentForSide:*side_opt];
    if( content.SynchronizingTabs() || !content.HasTabsModel() )
        return;

    const ExplorerTabEntry *const current = content.FindEntry(content.TabsModel()->Active());
    if( current && ![content.TabbedHolder() tabViewItemForController:current->panel] )
        return;

    const std::optional<nc::core::PaneId> pane_id = PaneIdFromTabItem(_item);
    ExplorerTabEntry *const entry = pane_id ? content.FindEntry(*pane_id) : nullptr;
    if( !entry || _item.view != entry->panel.view )
        return;
    if( content.TabsModel()->Active() == *pane_id ) {
        [self.window makeFirstResponder:entry->panel.view];
        return;
    }
    if( !content.TabsModel()->Activate(*pane_id) )
        return;
    content.RuntimeTabsMutatedRef() = true;
    [self invalidateExplorerRestorableState];
    [self bindActivePanel:entry->panel focus:true];
}

- (void)tabView:(NSTabView *)_tab_view receivedClickOnSelectedTabViewItem:(NSTabViewItem *)_item
{
    if( _item != _tab_view.selectedTabViewItem )
        return;
    const std::optional<NCExplorerPaneSide> side_opt = [self sideOwningTabView:_tab_view];
    if( !side_opt )
        return;
    NCExplorerPaneContent &content = [self contentForSide:*side_opt];
    const std::optional<nc::core::PaneId> pane_id = PaneIdFromTabItem(_item);
    ExplorerTabEntry *const entry = pane_id ? content.FindEntry(*pane_id) : nullptr;
    if( entry && content.HasTabsModel() && content.TabsModel()->Active() == *pane_id && _item.view == entry->panel.view ) {
        if( *side_opt != m_FocusedSide )
            [self focusSide:*side_opt];
        [self.window makeFirstResponder:entry->panel.view];
    }
}

- (void)tabView:(NSTabView *)_tab_view didCloseTabViewItem:(NSTabViewItem *)_item
{
    if( [self sideOwningTabView:_tab_view] )
        [self closeTabViewItem:_item];
}

- (void)tabView:(NSTabView *)_tab_view
    didDropTabViewItem:(NSTabViewItem *)_item
          inTabBarView:(NCPanelTabBarView *)_tab_bar
{
    const std::optional<NCExplorerPaneSide> side_opt = [self sideOwningTabView:_tab_view];
    if( !side_opt || [self contentForSide:*side_opt].TabbedHolder().tabBar != _tab_bar )
        return;
    const NSUInteger index = [_tab_view indexOfTabViewItem:_item];
    if( index != NSNotFound )
        [self reorderOwnedTabViewItem:_item toIndex:index];
}

- (void)tabView:(NSTabView *)_tab_view didMoveTabViewItem:(NSTabViewItem *)_item toIndex:(NSUInteger)_index
{
    if( [self sideOwningTabView:_tab_view] )
        [self reorderOwnedTabViewItem:_item toIndex:_index];
}

- (void)reorderOwnedTabViewItem:(NSTabViewItem *)_item toIndex:(const NSUInteger)_index
{
    const std::optional<nc::core::PaneId> pane_id = PaneIdFromTabItem(_item);
    const std::optional<NCExplorerPaneSide> side_opt = pane_id ? [self sideOwningPaneId:*pane_id] : std::nullopt;
    if( !side_opt )
        return;
    NCExplorerPaneContent &content = [self contentForSide:*side_opt];
    if( !content.HasTabsModel() || _index >= content.MutableEntries().size() )
        return;

    ExplorerTabEntry *const owned = content.FindEntry(*pane_id);
    if( !owned || _item.view != owned->panel.view )
        return;

    std::vector<ExplorerTabEntry> &entries = content.MutableEntries();
    const auto iterator =
        std::ranges::find(entries, *pane_id, [](const ExplorerTabEntry &_entry) { return _entry.panel.paneId; });
    if( iterator == entries.end() || !content.TabsModel()->Reorder(*pane_id, _index) )
        return;
    ExplorerTabEntry moving = std::move(*iterator);
    entries.erase(iterator);
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(_index), std::move(moving));
    content.RuntimeTabsMutatedRef() = true;
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
    [self.window makeFirstResponder:self.panelController.view];
}

- (void)windowStateDidResign
{
    [self closeAttachedUI:self.panelController];
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
    NCExplorerPaneContent &content = [self focusedContent];
    if( _panel != content.ActivePanel() || !content.LatestPaneSnapshot() || _presentation.items.empty() )
        return false;

    nc::core::PaneSnapshot presentation_snapshot = *content.LatestPaneSnapshot();
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

        if( !content.LatestPaneSnapshot() || content.LatestPaneSnapshot()->revision != revision ||
            content.LatestPaneSnapshot()->listing_generation != listing_generation ||
            content.LatestPaneSnapshot()->state.listing != presentation_snapshot.state.listing ||
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
    return _panel == self.panelController && m_Inspector != nil && !m_Inspector.hidden;
}

- (BOOL)setPreviewPaneVisible:(BOOL)_desired expected:(BOOL)_expected forPanel:(PanelController *)_panel
{
    if( _panel != self.panelController || !m_Inspector )
        return false;
    const BOOL current = !m_Inspector.hidden;
    if( current != _expected )
        return false;
    if( current == _desired )
        return true;

    if( _desired ) {
        const std::optional<nc::core::PaneSnapshot> &snapshot = [self focusedContent].LatestPaneSnapshot();
        if( !snapshot || ![m_Inspector applyPaneSnapshot:*snapshot] )
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
    // Explorer's dual-pane split is a plain NSSplitView (m_PaneSplitView), not a
    // FilePanelMainSplitView, and DP-1 has no per-side collapse/expand - the only caller of this
    // property (ShowQuickLook's collapse-restore) is guarded by anyPanelCollapsed==false below.
    return nil;
}

- (bool)anyPanelCollapsed
{
    return false; // No per-side collapse in this increment - see DP-1 explicit non-goals.
}

- (bool)bothPanelsAreVisible
{
    return m_DualPaneEnabled;
}

- (PanelController *)leftPanelController
{
    return m_Sides[static_cast<size_t>(NCExplorerPaneSide::Left)].ActivePanel();
}

- (PanelController *)rightPanelController
{
    return m_Sides[static_cast<size_t>(NCExplorerPaneSide::Right)].ActivePanel();
}

- (bool)isLeftController:(PanelController *)_controller
{
    return m_Sides[static_cast<size_t>(NCExplorerPaneSide::Left)].Owns(_controller);
}

- (bool)isRightController:(PanelController *)_controller
{
    return m_Sides[static_cast<size_t>(NCExplorerPaneSide::Right)].Owns(_controller);
}

- (void)closeAttachedUI:(PanelController *)_panel
{
    if( _panel != self.panelController || ![self contentOwningPanel:_panel] || m_QLPanelAdaptor.owner != self )
        return;
    if( QLPreviewPanel.sharedPreviewPanelExists && QLPreviewPanel.sharedPreviewPanel.isVisible )
        [QLPreviewPanel.sharedPreviewPanel orderOut:nil];
}

- (void)PanelPathChanged:(PanelController *)_panel
{
    NCExplorerPaneContent *const content = [self contentOwningPanel:_panel];
    if( !content )
        return;
    ExplorerTabEntry *const entry = content->FindEntry(_panel);
    [entry->search_controller synchronizeExternalContentChange];
    [self updateTabLabelForPanel:_panel];
    if( _panel == self.panelController )
        [m_Sidebar panelPathChanged];
    [self invalidateExplorerRestorableState];
}

- (void)activePanelChangedTo:(PanelController *)_controller
{
    if( [self contentOwningPanel:_controller] )
        [self ActivatePanelByController:_controller];
}

- (void)ActivatePanelByController:(PanelController *)_controller
{
    NCExplorerPaneContent *const content_ptr = [self contentOwningPanel:_controller];
    if( !content_ptr || !content_ptr->HasTabsModel() )
        return;
    NCExplorerPaneContent &content = *content_ptr;
    NSTabViewItem *const item = [content.TabbedHolder() tabViewItemForController:_controller];
    if( content.TabbedHolder() && !item )
        return;
    if( content.TabsModel()->Active() == _controller.paneId ) {
        [self.window makeFirstResponder:_controller.view];
        return;
    }
    if( !content.TabsModel()->Activate(_controller.paneId) )
        return;
    content.RuntimeTabsMutatedRef() = true;
    [self invalidateExplorerRestorableState];
    content.SetSynchronizingTabs(true);
    if( item )
        [content.TabbedHolder().tabBar selectTabViewItem:item];
    content.SetSynchronizingTabs(false);
    [self bindActivePanel:_controller focus:true];
}

- (BriefSystemOverview *)briefSystemOverviewForPanel:(PanelController *) [[maybe_unused]] _panel
                                                make:(bool) [[maybe_unused]] _make_if_absent
{
    return nil;
}

- (id<NCPanelPreview>)quickLookForPanel:(PanelController *)_panel make:(bool)_make_if_absent
{
    if( _panel != self.panelController || ![self contentOwningPanel:_panel] || !m_QLPanelAdaptor )
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
    return self.panelController != nil && [self contentOwningPanel:self.panelController] != nullptr &&
           m_QLPanelAdaptor != nil;
}

- (void)beginPreviewPanelControl:(QLPreviewPanel *) [[maybe_unused]] _panel
{
    if( [m_QLPanelAdaptor registerExistingQLPreviewPanelFor:self] )
        [self.panelController updateAttachedQuickLook];
}

- (void)endPreviewPanelControl:(QLPreviewPanel *) [[maybe_unused]] _panel
{
    [m_QLPanelAdaptor unregisterExistingQLPreviewPanelFor:self];
}

#pragma mark - NCPanelViewKeystrokeSink

- (std::optional<NCExplorerPaneSide>)sideOwningPanelView:(PanelView *)_panel_view
{
    for( size_t index = 0; index < g_ExplorerPaneSideCount; ++index )
        if( m_Sides[index].ActivePanel() && m_Sides[index].ActivePanel().view == _panel_view )
            return static_cast<NCExplorerPaneSide>(index);
    return std::nullopt;
}

- (int)bidForHandlingKeyDown:(NSEvent *)_event forPanelView:(PanelView *)_panel_view
{
    const std::optional<NCExplorerPaneSide> side = [self sideOwningPanelView:_panel_view];
    if( !side )
        return nc::panel::view::BiddingPriority::Skip;
    if( m_DualPaneEnabled && IsPlainTabKey(_event) )
        return nc::panel::view::BiddingPriority::Max;
    if( IsFocusAddressShortcut(_event) )
        return nc::panel::view::BiddingPriority::Max;
    if( _event.keyCode == 53 ) {
        NSPasteboard *const pasteboard = NSPasteboard.generalPasteboard;
        if( nc::panel::PasteboardSupport::CurrentCutToken(pasteboard) &&
            !nc::panel::PasteboardSupport::IsCutInFlight(pasteboard) )
            return nc::panel::view::BiddingPriority::High;
        if( [self contentForSide:*side].ActivePanel().isPresentingProgressiveNavigationPreview )
            return nc::panel::view::BiddingPriority::High;
    }
    return nc::panel::view::BiddingPriority::Skip;
}

- (void)handleKeyDown:(NSEvent *)_event forPanelView:(PanelView *)_panel_view
{
    const std::optional<NCExplorerPaneSide> side = [self sideOwningPanelView:_panel_view];
    if( !side )
        return;
    if( m_DualPaneEnabled && IsPlainTabKey(_event) ) {
        [self changeFocusedSide];
        return;
    }
    if( _event.keyCode == 53 ) {
        NSPasteboard *const pasteboard = NSPasteboard.generalPasteboard;
        if( nc::panel::PasteboardSupport::CurrentCutToken(pasteboard) &&
            !nc::panel::PasteboardSupport::IsCutInFlight(pasteboard) ) {
            nc::panel::PasteboardSupport::CancelCut(pasteboard);
            return;
        }
        PanelController *const panel = [self contentForSide:*side].ActivePanel();
        if( panel.isPresentingProgressiveNavigationPreview )
            [panel CancelBackgroundOperations];
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
    m_FocusedSide = NCExplorerPaneSide::Left;
    m_DualPaneEnabled = false;
    NCExplorerPaneContent &content = m_Sides[static_cast<size_t>(NCExplorerPaneSide::Left)];
    auto tabs = nc::core::ExplorerTabsModel::Create(_panel.paneId);
    if( !tabs )
        return nil;
    content.TabsModel().emplace(std::move(*tabs));
    content.MutableEntries().push_back(ExplorerTabEntry{
        .panel = _panel,
        .view_settings_binding =
            std::make_unique<nc::explorer::ExplorerViewSettingsBindingPolicy>(_panel.paneId),
    });
    content.ActivePanelRef() = _panel;
    _panel.state = self;
    if( [_panel.view respondsToSelector:@selector(addKeystrokeSink:)] )
        [_panel.view addKeystrokeSink:self];
    if( [_panel.view respondsToSelector:@selector(setHeaderBarVisible:)] )
        _panel.view.headerBarVisible = false;
    m_Inspector = _inspector;
    m_QLPanelAdaptor = _ql_panel_adaptor;
    content.BuildTestingContainer(_panel_view, [[NCExplorerQuickSearchOverlayView alloc] initWithFrame:NSZeroRect]);
    m_LastInspectorWidth = g_InspectorPreferredWidth;
    m_PaneDividerRatio = g_DefaultPaneDividerRatio;
    [self buildContentSplitWithPaneAreaView:content.Container() inspectorView:_inspector];
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
    [self bindActivePanel:self.panelController focus:false];
}

- (BOOL)addInactivePanelForTesting:(PanelController *)_panel
{
    NCExplorerPaneContent &content = [self focusedContent];
    if( !_panel || !content.HasTabsModel() || content.FindEntry(_panel) )
        return false;
    const nc::core::PaneId active = content.TabsModel()->Active();
    if( ![self attachExplorerTabPanel:_panel createPaneStore:false] )
        return false;
    return content.TabsModel()->Active() == active || content.TabsModel()->Activate(active);
}

- (std::vector<nc::core::PaneId>)tabPaneIDsForTesting
{
    NCExplorerPaneContent &content = [self focusedContent];
    if( !content.HasTabsModel() )
        return {};
    return content.TabPaneIDsForTesting();
}

- (void)applyPaneSnapshotForTesting:(const nc::core::PaneSnapshot &)_snapshot
{
    NCExplorerPaneContent &content = [self focusedContent];
    content.LatestPaneSnapshotRef() = _snapshot;
    [m_Inspector applyPaneSnapshot:_snapshot];
    [content.PaneStateView() updateWithVisualState:ExplorerPaneVisualState(_snapshot, content.ActivePanel())];
}

- (BOOL)setSearchControllerForTesting:(ExplorerSearchController *)_controller forPanel:(PanelController *)_panel
{
    NCExplorerPaneContent *const content = [self contentOwningPanel:_panel];
    ExplorerTabEntry *const entry = content ? content->FindEntry(_panel) : nullptr;
    if( !entry || !_controller )
        return NO;
    [entry->search_controller close];
    entry->search_controller = _controller;
    if( entry->panel == self.panelController )
        [self applySearchSnapshotForPane:entry->panel.paneId];
    return YES;
}

- (void)setSearchModeViewForTesting:(NCExplorerSearchModeView *)_view
{
    m_SearchModeView = _view;
    [self configureSearchViewHandlers];
    if( self.panelController )
        [self applySearchSnapshotForPane:self.panelController.paneId];
}

- (void)dealloc
{
    [self closeAttachedUI:self.panelController];
    for( NCExplorerPaneContent &content : m_Sides ) {
        content.DetachActiveObservation();
        for( ExplorerTabEntry &entry : content.MutableEntries() ) {
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
}

- (NSSplitView *)contentSplitViewForTesting
{
    return m_ContentSplitView;
}

- (NSView *)panelContainerForTesting
{
    return [self focusedContent].Container();
}

- (NCExplorerInspectorView *)inspectorViewForTesting
{
    return m_Inspector;
}

- (NCExplorerPaneStateView *)paneStateViewForTesting
{
    return [self focusedContent].PaneStateView();
}

- (BOOL)dualPaneEnabledForTesting
{
    return m_DualPaneEnabled;
}

- (PanelController *)rightPanelControllerForTesting
{
    return m_Sides[static_cast<size_t>(NCExplorerPaneSide::Right)].ActivePanel();
}

- (NSView *)rightPanelContainerForTesting
{
    return m_Sides[static_cast<size_t>(NCExplorerPaneSide::Right)].Container();
}

- (double)paneDividerRatioForTesting
{
    return static_cast<double>(m_PaneDividerRatio);
}

@end
