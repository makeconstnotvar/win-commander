// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerSidebarView.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/Favorites.h"
#include "../FilePanels/PanelDataPersistency.h"
#include "../FilePanels/Actions/ShowGoToPopup.h"
#include "../FilePanels/Helpers/LocationFormatter.h"
#include "../../Bootstrap/AppDelegate.h"
#include "../../Core/Theming/ExplorerPalette.h"
#include <Panel/NetworkConnectionsManager.h>
#include <Panel/PanelData.h>
#include <Panel/TagsStorage.h>
#include <Utility/NativeFSManager.h>
#include <Utility/ObjCpp.h>
#include <Utility/Tags.h>
#include <VFS/Native.h>

#include <any>
#include <functional>
#include <optional>
#include <vector>

using SidebarRenderOptions = nc::panel::loc_fmt::Formatter::RenderOptions;

static const SidebarRenderOptions g_RenderOptions = static_cast<SidebarRenderOptions>(
    nc::panel::loc_fmt::Formatter::RenderMenuTitle | nc::panel::loc_fmt::Formatter::RenderMenuIcon |
    nc::panel::loc_fmt::Formatter::RenderMenuTooltip);

static NSString *const g_ColumnIdentifier = @"NCExplorerSidebarColumn";
static NSString *const g_RowIdentifier = @"NCExplorerSidebarRow";

// The source-list row height is deliberately left to AppKit. It follows the user's system sidebar
// size preference and keeps this navigation surface at the same scale as native macOS sidebars.
static const CGFloat g_VerticalPadding = 14.0;
static const CGFloat g_RowIconSize = 20.0;
static const CGFloat g_RowIconLeading = 8.0;
static const CGFloat g_RowIconLabelGap = 9.0;
static const CGFloat g_RowTrailingInset = 8.0;
static const CGFloat g_HairlineWidth = 1.0;

static std::string NormalizedDirectory(std::string _path)
{
    while( _path.size() > 1 && _path.back() == '/' )
        _path.pop_back();
    return _path;
}

static bool IsPathInsideMount(const std::string &_path, const std::string &_mount_path)
{
    if( _mount_path == "/" )
        return _path.starts_with('/');
    return _path == _mount_path ||
           (_path.size() > _mount_path.size() && _path.starts_with(_mount_path) && _path[_mount_path.size()] == '/');
}

static std::optional<std::string> CurrentNativeDirectory(PanelController *_panel)
{
    // `data` hands back a C++ reference, so a nil controller yields a null one and the first read
    // through it faults. These helpers run from row-highlight tests that outlive any particular
    // controller, so an absent panel is simply an absent location.
    if( !_panel )
        return std::nullopt;

    if( !_panel.data.IsLoaded() || !_panel.isUniform )
        return std::nullopt;

    const auto &host = _panel.data.Host();
    if( !host || dynamic_cast<const nc::vfs::NativeHost *>(host.get()) == nullptr )
        return std::nullopt;

    return NormalizedDirectory(_panel.data.Listing().Directory());
}

static std::optional<std::string> CurrentVerboseLocation(PanelController *_panel)
{
    if( !_panel )
        return std::nullopt;

    if( !_panel.data.IsLoaded() || !_panel.isUniform )
        return std::nullopt;

    const auto &host = _panel.data.Host();
    if( !host )
        return std::nullopt;

    return NCAppDelegate.me.panelDataPersistency.MakeVerbosePathString(*host, _panel.data.Listing().Directory());
}

@interface NCExplorerSidebarNode : NSObject

- (instancetype)initSectionWithTitle:(NSString *)_title
                          identifier:(NSString *)_identifier
                            children:(NSArray *)_children;
- (instancetype)initItemWithTitle:(NSString *)_title
                             icon:(NSImage *)_icon
                          tooltip:(NSString *)_tooltip
                           action:(std::function<void()>)_action
                      currentTest:(std::function<bool()>)_current_test;

@property(nonatomic, readonly) NSString *title;
@property(nonatomic, readonly) NSString *nodeIdentifier;
@property(nonatomic, readonly) NSImage *icon;
@property(nonatomic, readonly) NSString *tooltip;
@property(nonatomic, readonly) NSArray<NCExplorerSidebarNode *> *children;
@property(nonatomic, readonly) bool section;

- (void)performAction;
- (bool)isCurrent;

@end

@implementation NCExplorerSidebarNode {
    NSString *m_Title;
    NSString *m_NodeIdentifier;
    NSImage *m_Icon;
    NSString *m_Tooltip;
    NSArray<NCExplorerSidebarNode *> *m_Children;
    std::function<void()> m_Action;
    std::function<bool()> m_CurrentTest;
    bool m_Section;
}

@synthesize title = m_Title;
@synthesize nodeIdentifier = m_NodeIdentifier;
@synthesize icon = m_Icon;
@synthesize tooltip = m_Tooltip;
@synthesize children = m_Children;
@synthesize section = m_Section;

- (instancetype)initSectionWithTitle:(NSString *)_title identifier:(NSString *)_identifier children:(NSArray *)_children
{
    self = [super init];
    if( self ) {
        m_Title = [_title copy];
        m_NodeIdentifier = [_identifier copy];
        m_Children = [_children copy];
        m_Section = true;
    }
    return self;
}

- (instancetype)initItemWithTitle:(NSString *)_title
                             icon:(NSImage *)_icon
                          tooltip:(NSString *)_tooltip
                           action:(std::function<void()>)_action
                      currentTest:(std::function<bool()>)_current_test
{
    self = [super init];
    if( self ) {
        m_Title = [_title copy];
        m_Icon = _icon;
        m_Tooltip = [_tooltip copy];
        m_Children = @[];
        m_Action = std::move(_action);
        m_CurrentTest = std::move(_current_test);
        m_Section = false;
    }
    return self;
}

- (void)performAction
{
    if( m_Action )
        m_Action();
}

- (bool)isCurrent
{
    return m_CurrentTest && m_CurrentTest();
}

@end

@protocol NCExplorerOutlineViewSelectionDelegate <NSObject>
- (void)outlineViewSelectionDidChange:(NSOutlineView *)_outline_view;
@end

@interface NCExplorerOutlineView : NSOutlineView
@property(nonatomic, weak) id<NCExplorerOutlineViewSelectionDelegate> selectionDelegate;
@end

@implementation NCExplorerOutlineView {
    __weak id<NCExplorerOutlineViewSelectionDelegate> m_SelectionDelegate;
}

@synthesize selectionDelegate = m_SelectionDelegate;

- (void)selectRowIndexes:(NSIndexSet *)_indexes byExtendingSelection:(BOOL)_extend
{
    const NSInteger previousRow = self.selectedRow;
    [super selectRowIndexes:_indexes byExtendingSelection:_extend];
    if( self.selectedRow != previousRow )
        [self.selectionDelegate outlineViewSelectionDidChange:self];
}

- (void)mouseDown:(NSEvent *)_event
{
    const NSPoint location = [self convertPoint:_event.locationInWindow fromView:nil];
    const NSInteger row = [self rowAtPoint:location];
    if( row >= 0 ) {
        id const item = [self itemAtRow:row];
        if( [self isExpandable:item] ) {
            // AppKit sends a second mouseDown with clickCount == 2 for a double-click. Toggle on the
            // first click only, otherwise a double-click would immediately undo its own result.
            if( _event.clickCount == 1 ) {
                if( [self isItemExpanded:item] )
                    [self collapseItem:item];
                else
                    [self expandItem:item];
            }
            return;
        }
    }

    [super mouseDown:_event];
}

@end

/** The sidebar's right edge. The mockup draws a 1pt stroke there, so the flat palette surface needs
 *  an explicit seam against the file area. */
@interface NCExplorerSidebarHairlineView : NSView
@end

@implementation NCExplorerSidebarHairlineView
- (BOOL)isOpaque
{
    return false;
}
- (void)drawRect:(NSRect) [[maybe_unused]] _dirty_rect
{
    [NSColor.separatorColor setFill];
    NSRectFillUsingOperation(self.bounds, NSCompositingOperationSourceOver);
}
@end

@interface NCExplorerSidebarRowView : NSTableRowView
@end

@implementation NCExplorerSidebarRowView
- (BOOL)isOpaque
{
    return true;
}

- (void)drawBackgroundInRect:(NSRect)_dirty_rect
{
    // Paint both states. Leaving a deselected row transparent lets its previous accent fill survive
    // in the backing store until an ancestor happens to repaint, which can look like multi-selection.
    [nc::explorer::ChromeFillColor() setFill];
    NSRectFill(_dirty_rect);
    if( !self.selected )
        return;
    NSColor *const selection_color = self.emphasized
                                         ? [NSColor.controlAccentColor colorWithAlphaComponent:0.16]
                                         : NSColor.unemphasizedSelectedContentBackgroundColor;
    [selection_color setFill];
    [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 4.0, 1.0) xRadius:5.0 yRadius:5.0] fill];
}
@end

@interface NCExplorerSidebarView () <NSOutlineViewDataSource,
                                     NSOutlineViewDelegate,
                                     NCExplorerOutlineViewSelectionDelegate>
@end

@implementation NCExplorerSidebarView {
    PanelController *m_Panel;
    NSScrollView *m_ScrollView;
    NSOutlineView *m_OutlineView;
    NSArray<NCExplorerSidebarNode *> *m_RootNodes;
    nc::panel::FavoriteLocationsStorage::ObservationTicket m_FavoritesObservation;
    nc::panel::NetworkConnectionsManager::ObservationTicket m_NetworkObservation;
    nc::panel::TagsStorage::ObservationTicket m_TagsObservation;
    nc::config::Token m_TagsConfigObservation;
    bool m_IgnoreSelectionChange;
    NSInteger m_LastActivatedRow;
}

@synthesize panelController = m_Panel;

- (BOOL)isOpaque
{
    return true;
}

- (void)drawRect:(NSRect) [[maybe_unused]] _dirty_rect
{
    [nc::explorer::ChromeFillColor() setFill];
    NSRectFill(self.bounds);
}

- (void)layout
{
    [super layout];

    // The state constructs the sidebar at NSZeroRect and Auto Layout assigns its production width
    // later. NSOutlineView does not reliably grow its only column from that zero-width bootstrap,
    // leaving live, hittable rows whose cell views are clipped to an empty column. Bind both the
    // document and its column to the clip width once layout has the real geometry.
    const CGFloat width = NSWidth(m_ScrollView.contentView.bounds);
    if( width <= 0.0 )
        return;
    NSSize document_size = m_OutlineView.frame.size;
    document_size.width = width;
    [m_OutlineView setFrameSize:document_size];
    if( NSTableColumn *const column = m_OutlineView.tableColumns.firstObject )
        column.width = width;
}

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        m_Panel = _panel;
        m_LastActivatedRow = -1;
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        self.accessibilityElement = true;
        self.accessibilityRole = NSAccessibilityGroupRole;
        self.accessibilityIdentifier = @"wincommander.explorer.sidebar";
        self.accessibilityLabel = NSLocalizedString(@"Locations", "Explorer sidebar accessibility label");

        [self buildScaffold];
        [self startObservingSources];
        [self reloadData];
    }
    return self;
}

- (void)rebindToPanelController:(PanelController *)_panel
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( !_panel || m_Panel == _panel )
        return;
    m_Panel = _panel;
    [self reloadData];
    [self panelPathChanged];
}

- (void)dealloc
{
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self];
}

- (void)buildScaffold
{
    NCExplorerOutlineView *const outlineView = [[NCExplorerOutlineView alloc] initWithFrame:self.bounds];
    outlineView.selectionDelegate = self;
    m_OutlineView = outlineView;
    m_OutlineView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    m_OutlineView.headerView = nil;
    m_OutlineView.rowSizeStyle = NSTableViewRowSizeStyleDefault;
    m_OutlineView.allowsMultipleSelection = false;
    m_OutlineView.selectionHighlightStyle = NSTableViewSelectionHighlightStyleNone;
    m_OutlineView.indentationPerLevel = 0.0;
    m_OutlineView.floatsGroupRows = false;
    m_OutlineView.delegate = self;
    m_OutlineView.dataSource = self;
    m_OutlineView.accessibilityIdentifier = @"wincommander.explorer.sidebar.locations";
    m_OutlineView.accessibilityLabel = NSLocalizedString(@"Locations", "Explorer sidebar accessibility label");
    m_OutlineView.backgroundColor = nc::explorer::ChromeFillColor();
    if( @available(macOS 11.0, *) )
        m_OutlineView.style = NSTableViewStyleSourceList;

    NSTableColumn *const column = [[NSTableColumn alloc] initWithIdentifier:g_ColumnIdentifier];
    column.resizingMask = NSTableColumnAutoresizingMask;
    [m_OutlineView addTableColumn:column];
    m_OutlineView.outlineTableColumn = column;

    m_ScrollView = [[NSScrollView alloc] initWithFrame:self.bounds];
    m_ScrollView.translatesAutoresizingMaskIntoConstraints = false;
    m_ScrollView.hasVerticalScroller = true;
    m_ScrollView.hasHorizontalScroller = false;
    m_ScrollView.autohidesScrollers = true;
    m_ScrollView.automaticallyAdjustsContentInsets = false;
    m_ScrollView.contentInsets = NSEdgeInsetsMake(g_VerticalPadding, 0.0, g_VerticalPadding, 0.0);
    m_ScrollView.drawsBackground = true;
    m_ScrollView.backgroundColor = nc::explorer::ChromeFillColor();
    m_ScrollView.borderType = NSNoBorder;
    m_ScrollView.documentView = m_OutlineView;
    [self addSubview:m_ScrollView];

    // The sidebar is already constrained below the command bar. Padding belongs to the scroll
    // content, not to this view's safe area: adding it to the safe area lets AppKit fold the window
    // titlebar inset into the nested scroll view and can move the whole outline out of its clip.
    // Explicit edge constraints also matter because this view is born at NSZeroRect and only gets
    // its production size from NCExplorerState's Auto Layout pass.
    [NSLayoutConstraint activateConstraints:@[
        [m_ScrollView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [m_ScrollView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [m_ScrollView.topAnchor constraintEqualToAnchor:self.topAnchor],
        [m_ScrollView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];

    NSView *const hairline = [[NCExplorerSidebarHairlineView alloc] initWithFrame:NSZeroRect];
    hairline.translatesAutoresizingMaskIntoConstraints = false;
    [self addSubview:hairline];
    [NSLayoutConstraint activateConstraints:@[
        [hairline.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [hairline.topAnchor constraintEqualToAnchor:self.topAnchor],
        [hairline.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        [hairline.widthAnchor constraintEqualToConstant:g_HairlineWidth],
    ]];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(selectionChanged:)
                                               name:NSOutlineViewSelectionDidChangeNotification
                                             object:m_OutlineView];
}

- (void)startObservingSources
{
    if( const auto &favorites = NCAppDelegate.me.favoriteLocationsStorage )
        m_FavoritesObservation =
            favorites->ObserveFavoritesChanges(nc::objc_callback_to_main_queue(self, @selector(sourceChanged)));

    if( const auto &network = NCAppDelegate.me.networkConnectionsManager )
        m_NetworkObservation = network->ObserveChanges(nc::objc_callback_to_main_queue(self, @selector(sourceChanged)));

    m_TagsObservation =
        NCAppDelegate.me.tagsStorage.ObserveChanges(nc::objc_callback_to_main_queue(self, @selector(sourceChanged)));
    m_TagsConfigObservation = NCAppDelegate.me.globalConfig.Observe(
        "filePanel.FinderTags.enable", nc::objc_callback_to_main_queue(self, @selector(sourceChanged)));

    NSNotificationCenter *const center = NSWorkspace.sharedWorkspace.notificationCenter;
    [center addObserver:self selector:@selector(volumesChanged:) name:NSWorkspaceDidMountNotification object:nil];
    [center addObserver:self selector:@selector(volumesChanged:) name:NSWorkspaceDidUnmountNotification object:nil];
    [center addObserver:self
               selector:@selector(volumesChanged:)
                   name:NSWorkspaceDidRenameVolumeNotification
                 object:nil];
}

- (void)sourceChanged
{
    [self reloadData];
}

- (void)volumesChanged:(NSNotification *) [[maybe_unused]] _notification
{
    [self reloadData];
}

- (void)reloadData
{
    NSMutableSet<NSString *> *const collapsedSections = [NSMutableSet set];
    for( NCExplorerSidebarNode *node in m_RootNodes )
        if( ![m_OutlineView isItemExpanded:node] )
            [collapsedSections addObject:node.nodeIdentifier];

    NSMutableArray<NCExplorerSidebarNode *> *const sections = [NSMutableArray array];
    [self appendFavoritesSectionTo:sections];
    [self appendVolumesSectionTo:sections];
    [self appendConnectionsSectionTo:sections];
    [self appendTagsSectionTo:sections];
    m_RootNodes = [sections copy];

    [m_OutlineView reloadData];
    for( NCExplorerSidebarNode *node in m_RootNodes )
        if( ![collapsedSections containsObject:node.nodeIdentifier] )
            [m_OutlineView expandItem:node];

    [self panelPathChanged];
}

- (void)appendFavoritesSectionTo:(NSMutableArray<NCExplorerSidebarNode *> *)_sections
{
    const auto &storage = NCAppDelegate.me.favoriteLocationsStorage;
    if( !storage )
        return;

    NSMutableArray<NCExplorerSidebarNode *> *const children = [NSMutableArray array];
    nc::panel::NetworkConnectionsManager &netMgr = *NCAppDelegate.me.networkConnectionsManager;
    nc::panel::loc_fmt::FavoriteFormatter formatter{netMgr};
    PanelController *const panel = m_Panel;

    for( const nc::panel::FavoriteLocationsStorage::Favorite &favorite : storage->Favorites() ) {
        if( !favorite.location )
            continue;

        const auto representation = formatter.Render(g_RenderOptions, favorite);
        const auto location = favorite.location;
        const std::string verbosePath = location->verbose_path;
        const std::any context = location;
        auto action = [panel, network = NCAppDelegate.me.networkConnectionsManager, context] {
            nc::panel::actions::NavigateToLocation(panel, *network, context);
        };
        auto currentTest = [panel, verbosePath] {
            const auto current = CurrentVerboseLocation(panel);
            return current && *current == verbosePath;
        };
        auto node = [[NCExplorerSidebarNode alloc] initItemWithTitle:representation.menu_title ?: @""
                                                                icon:representation.menu_icon
                                                             tooltip:representation.menu_tooltip
                                                              action:std::move(action)
                                                         currentTest:std::move(currentTest)];
        [children addObject:node];
    }

    if( children.count == 0 )
        return;
    auto section = [[NCExplorerSidebarNode alloc]
        initSectionWithTitle:NSLocalizedString(@"Favorites", "Explorer sidebar section title")
                  identifier:@"favorites"
                    children:children];
    [_sections addObject:section];
}

- (void)appendVolumesSectionTo:(NSMutableArray<NCExplorerSidebarNode *> *)_sections
{
    NSMutableArray<NCExplorerSidebarNode *> *const children = [NSMutableArray array];
    PanelController *const panel = m_Panel;
    const auto network = NCAppDelegate.me.networkConnectionsManager;

    std::vector<std::string> mountPaths;
    for( const nc::utility::NativeFSManager::Info &volume : NCAppDelegate.me.nativeFSManager.Volumes() )
        if( volume && !volume->mount_flags.dont_browse )
            mountPaths.emplace_back(NormalizedDirectory(volume->mounted_at_path));

    for( const nc::utility::NativeFSManager::Info &volume : NCAppDelegate.me.nativeFSManager.Volumes() ) {
        if( !volume || volume->mount_flags.dont_browse )
            continue;

        const auto representation = nc::panel::loc_fmt::VolumeFormatter::Render(g_RenderOptions, *volume);
        const std::string path = NormalizedDirectory(volume->mounted_at_path);
        const std::any context = volume->mounted_at_path;
        auto action = [panel, network, context] { nc::panel::actions::NavigateToLocation(panel, *network, context); };
        auto currentTest = [panel, path, mountPaths] {
            const auto current = CurrentNativeDirectory(panel);
            if( !current || !IsPathInsideMount(*current, path) )
                return false;

            std::string_view longestMatch;
            for( const std::string &candidate : mountPaths )
                if( IsPathInsideMount(*current, candidate) && candidate.size() > longestMatch.size() )
                    longestMatch = candidate;
            return path == longestMatch;
        };
        auto node = [[NCExplorerSidebarNode alloc] initItemWithTitle:representation.menu_title ?: @""
                                                                icon:representation.menu_icon
                                                             tooltip:representation.menu_tooltip
                                                              action:std::move(action)
                                                         currentTest:std::move(currentTest)];
        [children addObject:node];
    }

    if( children.count == 0 )
        return;
    auto section = [[NCExplorerSidebarNode alloc]
        initSectionWithTitle:NSLocalizedString(@"This Mac", "Explorer sidebar section title")
                  identifier:@"volumes"
                    children:children];
    [_sections addObject:section];
}

- (void)appendConnectionsSectionTo:(NSMutableArray<NCExplorerSidebarNode *> *)_sections
{
    const auto network = NCAppDelegate.me.networkConnectionsManager;
    if( !network )
        return;

    NSMutableArray<NCExplorerSidebarNode *> *const children = [NSMutableArray array];
    PanelController *const panel = m_Panel;
    for( const nc::panel::NetworkConnectionsManager::Connection &connection : network->AllConnectionsByMRU() ) {
        const auto representation = nc::panel::loc_fmt::NetworkConnectionFormatter::Render(g_RenderOptions, connection);
        const std::any context = connection;
        auto action = [panel, network, context] { nc::panel::actions::NavigateToLocation(panel, *network, context); };
        auto currentTest = [panel, network, connection] {
            if( !panel )
                return false;
            if( !panel.data.IsLoaded() || !panel.isUniform )
                return false;
            const auto &host = panel.data.Host();
            const auto current = host ? network->ConnectionForVFS(*host) : std::nullopt;
            return current && *current == connection;
        };
        auto node = [[NCExplorerSidebarNode alloc] initItemWithTitle:representation.menu_title ?: @""
                                                                icon:representation.menu_icon
                                                             tooltip:representation.menu_tooltip
                                                              action:std::move(action)
                                                         currentTest:std::move(currentTest)];
        [children addObject:node];
    }

    if( children.count == 0 )
        return;
    auto section = [[NCExplorerSidebarNode alloc]
        initSectionWithTitle:NSLocalizedString(@"Network", "Explorer sidebar section title")
                  identifier:@"network"
                    children:children];
    [_sections addObject:section];
}

- (void)appendTagsSectionTo:(NSMutableArray<NCExplorerSidebarNode *> *)_sections
{
    if( !NCAppDelegate.me.globalConfig.GetBool("filePanel.FinderTags.enable") )
        return;

    NSMutableArray<NCExplorerSidebarNode *> *const children = [NSMutableArray array];
    PanelController *const panel = m_Panel;
    const auto network = NCAppDelegate.me.networkConnectionsManager;
    for( const nc::utility::Tags::Tag &tag : NCAppDelegate.me.tagsStorage.Get() ) {
        const auto representation = nc::panel::loc_fmt::VFSFinderTagsFormatter::Render(g_RenderOptions, tag);
        const std::any context = tag;
        auto action = [panel, network, context] { nc::panel::actions::NavigateToLocation(panel, *network, context); };
        auto currentTest = [panel, tag] {
            if( !panel )
                return false;
            return panel.data.IsLoaded() && !panel.isUniform && panel.data.Listing().Title() == tag.Label();
        };
        auto node = [[NCExplorerSidebarNode alloc] initItemWithTitle:representation.menu_title ?: @""
                                                                icon:representation.menu_icon
                                                             tooltip:representation.menu_tooltip
                                                              action:std::move(action)
                                                         currentTest:std::move(currentTest)];
        [children addObject:node];
    }

    if( children.count == 0 )
        return;
    auto section =
        [[NCExplorerSidebarNode alloc] initSectionWithTitle:NSLocalizedString(@"Tags", "Explorer sidebar section title")
                                                 identifier:@"tags"
                                                   children:children];
    [_sections addObject:section];
}

- (void)panelPathChanged
{
    NSInteger matchingRow = -1;
    for( NCExplorerSidebarNode *section in m_RootNodes ) {
        for( NCExplorerSidebarNode *node in section.children ) {
            if( node.isCurrent ) {
                matchingRow = [m_OutlineView rowForItem:node];
                break;
            }
        }
        if( matchingRow >= 0 )
            break;
    }

    m_IgnoreSelectionChange = true;
    if( matchingRow >= 0 )
        [m_OutlineView selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(matchingRow)]
                   byExtendingSelection:false];
    else
        [m_OutlineView deselectAll:self];
    m_LastActivatedRow = matchingRow;
    m_IgnoreSelectionChange = false;
}

- (void)selectionChanged:(NSNotification *)_notification
{
    NSOutlineView *const outlineView = nc::objc_cast<NSOutlineView>(_notification.object);
    if( !outlineView )
        return;
    if( m_IgnoreSelectionChange || outlineView.selectedRow < 0 )
        return;

    [self activateSelectedRow:outlineView];
}

- (void)outlineViewSelectionDidChange:(NSOutlineView *)_outline_view
{
    [self activateSelectedRow:_outline_view];
}

- (void)activateSelectedRow:(NSOutlineView *)_outline_view
{
    if( m_IgnoreSelectionChange || _outline_view.selectedRow < 0 || _outline_view.selectedRow == m_LastActivatedRow )
        return;

    m_LastActivatedRow = _outline_view.selectedRow;

    auto node = nc::objc_cast<NCExplorerSidebarNode>([_outline_view itemAtRow:_outline_view.selectedRow]);
    if( node && !node.section )
        [node performAction];
}

#pragma mark - NSOutlineViewDataSource

- (NSInteger)outlineView:(NSOutlineView *) [[maybe_unused]] _outline_view numberOfChildrenOfItem:(id)_item
{
    if( _item == nil )
        return static_cast<NSInteger>(m_RootNodes.count);
    if( auto node = nc::objc_cast<NCExplorerSidebarNode>(_item) )
        return static_cast<NSInteger>(node.children.count);
    return 0;
}

- (id)outlineView:(NSOutlineView *) [[maybe_unused]] _outline_view child:(NSInteger)_index ofItem:(id)_item
{
    const NSUInteger index = static_cast<NSUInteger>(_index);
    if( auto node = nc::objc_cast<NCExplorerSidebarNode>(_item) )
        return [node.children objectAtIndex:index];
    return [m_RootNodes objectAtIndex:index];
}

- (BOOL)outlineView:(NSOutlineView *) [[maybe_unused]] _outline_view isItemExpandable:(id)_item
{
    if( auto node = nc::objc_cast<NCExplorerSidebarNode>(_item) )
        return node.section && node.children.count > 0;
    return false;
}

#pragma mark - NSOutlineViewDelegate

- (BOOL)outlineView:(NSOutlineView *) [[maybe_unused]] _outline_view isGroupItem:(id)_item
{
    if( auto node = nc::objc_cast<NCExplorerSidebarNode>(_item) )
        return node.section;
    return false;
}

- (BOOL)outlineView:(NSOutlineView *) [[maybe_unused]] _outline_view shouldSelectItem:(id)_item
{
    if( auto node = nc::objc_cast<NCExplorerSidebarNode>(_item) )
        return !node.section;
    return false;
}

- (NSTableRowView *)outlineView:(NSOutlineView *) [[maybe_unused]] _outline_view rowViewForItem:(id)_item
{
    auto node = nc::objc_cast<NCExplorerSidebarNode>(_item);
    if( !node )
        return nil;
    return [[NCExplorerSidebarRowView alloc] initWithFrame:NSZeroRect];
}

- (NSView *)outlineView:(NSOutlineView *)_outline_view
     viewForTableColumn:(NSTableColumn *) [[maybe_unused]] _table_column
                   item:(id)_item
{
    auto node = nc::objc_cast<NCExplorerSidebarNode>(_item);
    if( !node )
        return nil;

    if( node.section ) {
        NSTextField *const label = [NSTextField labelWithString:node.title];
        label.translatesAutoresizingMaskIntoConstraints = false;
        label.font = [NSFont systemFontOfSize:NSFont.systemFontSize weight:NSFontWeightSemibold];
        label.textColor = NSColor.secondaryLabelColor;
        label.accessibilityIdentifier = @"wincommander.explorer.sidebar.section";
        label.accessibilityLabel = node.title;

        NSView *const container = [[NSView alloc] initWithFrame:NSZeroRect];
        [container addSubview:label];
        [NSLayoutConstraint activateConstraints:@[
            [label.leadingAnchor constraintEqualToAnchor:container.leadingAnchor constant:g_RowIconLeading],
            [label.trailingAnchor constraintLessThanOrEqualToAnchor:container.trailingAnchor],
            [label.centerYAnchor constraintEqualToAnchor:container.centerYAnchor],
        ]];
        return container;
    }

    NSTableCellView *cell = [_outline_view makeViewWithIdentifier:g_RowIdentifier owner:self];
    if( !cell ) {
        cell = [[NSTableCellView alloc] initWithFrame:NSRect()];
        cell.identifier = g_RowIdentifier;

        NSImageView *const imageView = [[NSImageView alloc] initWithFrame:NSRect()];
        imageView.translatesAutoresizingMaskIntoConstraints = false;
        imageView.imageScaling = NSImageScaleProportionallyDown;
        cell.imageView = imageView;
        [cell addSubview:imageView];

        NSTextField *const textField = [NSTextField labelWithString:@""];
        textField.translatesAutoresizingMaskIntoConstraints = false;
        textField.lineBreakMode = NSLineBreakByTruncatingTail;
        cell.textField = textField;
        [cell addSubview:textField];

        [NSLayoutConstraint activateConstraints:@[
            [imageView.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:g_RowIconLeading],
            [imageView.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
            [imageView.widthAnchor constraintEqualToConstant:g_RowIconSize],
            [imageView.heightAnchor constraintEqualToConstant:g_RowIconSize],
            [textField.leadingAnchor constraintEqualToAnchor:imageView.trailingAnchor
                                                    constant:g_RowIconLabelGap],
            [textField.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor
                                                     constant:-g_RowTrailingInset],
            [textField.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }

    cell.textField.stringValue = node.title;
    cell.imageView.image = node.icon;
    cell.toolTip = node.tooltip;
    cell.accessibilityIdentifier = @"wincommander.explorer.sidebar.location";
    cell.accessibilityLabel = node.title;
    cell.accessibilityHelp = node.tooltip;
    return cell;
}

@end
