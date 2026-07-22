// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerSidebarView.h"
#include "../FilePanels/PanelController.h"
#include "../FilePanels/Favorites.h"
#include "../FilePanels/Actions/Helpers.h"
#include "../FilePanels/Actions/OpenNetworkConnection.h"
#include "../FilePanels/Helpers/LocationFormatter.h"
#include "../../Bootstrap/AppDelegate.h"
#include "../../Bootstrap/NativeVFSHostInstance.h"
#include "../../Core/AnyHolder.h"
#include <Utility/NativeFSManager.h>
#include <Panel/NetworkConnectionsManager.h>
#include <VFS/Native.h>
#include <Base/dispatch_cpp.h>

#include <functional>
#include <vector>

using SidebarRenderOptions = nc::panel::loc_fmt::Formatter::RenderOptions;

static const SidebarRenderOptions g_RenderOptions = static_cast<SidebarRenderOptions>(
    nc::panel::loc_fmt::Formatter::RenderMenuTitle | nc::panel::loc_fmt::Formatter::RenderMenuIcon |
    nc::panel::loc_fmt::Formatter::RenderMenuTooltip);

static const CGFloat g_RowSidePadding = 12.0;
static const CGFloat g_SectionSpacing = 16.0;

// Mirrors GoToPopupListActionMediator.handlePersistentLocation: in Actions/ShowGoToPopup.mm - same
// async restore-then-navigate flow, used here for Favorites rows.
static void NavigateToPersistentLocation(PanelController *_panel, const nc::panel::PersistentLocation &_location)
{
    using nc::panel::actions::AsyncPersistentLocationRestorer;
    nc::panel::NetworkConnectionsManager &net_mgr = *NCAppDelegate.me.networkConnectionsManager;
    auto restorer = AsyncPersistentLocationRestorer(_panel, _panel.vfsInstanceManager, net_mgr);
    auto handler = [path = _location.path, panel = _panel](VFSHostPtr _host) {
        dispatch_to_main_queue([=] {
            auto request = std::make_shared<nc::panel::DirectoryChangeRequest>();
            request->RequestedDirectory = path;
            request->VFS = _host;
            request->PerformAsynchronous = true;
            request->InitiatedByUser = true;
            [panel GoToDirWithContext:request];
        });
    };
    restorer.Restore(_location, std::move(handler), nullptr);
}

// Mirrors GoToPopupListActionMediator.performGoTo: (the std::string/plain_path branch) - used here for
// This Mac / Volumes rows, which are always resolved against the native VFS host.
static void NavigateToNativePath(PanelController *_panel, const std::string &_path)
{
    auto request = std::make_shared<nc::panel::DirectoryChangeRequest>();
    request->RequestedDirectory = _path;
    request->VFS = nc::bootstrap::NativeVFSHostInstance().SharedPtr();
    request->PerformAsynchronous = true;
    request->InitiatedByUser = true;
    [_panel GoToDirWithContext:request];
}

// Mirrors Actions/OpenNetworkConnection.mm's OpenExistingNetworkConnection::Perform, which pulls a
// NetworkConnectionsManager::Connection out of an AnyHolder in _sender.representedObject - a dummy
// NSMenuItem is used here purely to carry that representedObject through the existing action.
static void NavigateToConnection(PanelController *_panel,
                                  const std::shared_ptr<nc::panel::NetworkConnectionsManager> &_net_mgr,
                                  const nc::panel::NetworkConnectionsManager::Connection &_connection)
{
    if( !_net_mgr )
        return;
    NSMenuItem *const carrier = [[NSMenuItem alloc] init];
    carrier.representedObject = [[AnyHolder alloc] initWithAny:std::any{_connection}];
    nc::panel::actions::OpenExistingNetworkConnection(*_net_mgr).Perform(_panel, carrier);
}

@implementation NCExplorerSidebarView {
    PanelController *m_Panel;
    NSScrollView *m_ScrollView;
    NSStackView *m_Stack;
    std::vector<std::function<void()>> m_RowActions;
}

@synthesize panelController = m_Panel;

- (instancetype)initWithFrame:(NSRect)frameRect panelController:(PanelController *)_panel
{
    self = [super initWithFrame:frameRect];
    if( self ) {
        m_Panel = _panel;
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        [self buildScaffold];
        [self reloadData];
    }
    return self;
}

- (void)buildScaffold
{
    NSVisualEffectView *const background = [[NSVisualEffectView alloc] initWithFrame:self.bounds];
    background.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    background.material = NSVisualEffectMaterialSidebar;
    background.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    background.state = NSVisualEffectStateActive;
    [self addSubview:background];

    m_ScrollView = [[NSScrollView alloc] initWithFrame:self.bounds];
    m_ScrollView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    m_ScrollView.hasVerticalScroller = true;
    m_ScrollView.hasHorizontalScroller = false;
    m_ScrollView.drawsBackground = false;
    m_ScrollView.borderType = NSNoBorder;
    [self addSubview:m_ScrollView];

    m_Stack = [[NSStackView alloc] initWithFrame:NSRect()];
    m_Stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    m_Stack.alignment = NSLayoutAttributeLeading;
    m_Stack.spacing = 2.0;
    m_Stack.edgeInsets = NSEdgeInsetsMake(10, 0, 10, 0);
    m_Stack.translatesAutoresizingMaskIntoConstraints = false;

    m_ScrollView.documentView = m_Stack;
    [NSLayoutConstraint activateConstraints:@[
        [m_Stack.topAnchor constraintEqualToAnchor:m_ScrollView.contentView.topAnchor],
        [m_Stack.leadingAnchor constraintEqualToAnchor:m_ScrollView.contentView.leadingAnchor],
        [m_Stack.trailingAnchor constraintEqualToAnchor:m_ScrollView.contentView.trailingAnchor],
        [m_Stack.widthAnchor constraintEqualToAnchor:m_ScrollView.contentView.widthAnchor],
    ]];
}

- (void)reloadData
{
    for( NSView *view in [m_Stack.arrangedSubviews copy] ) {
        [m_Stack removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    m_RowActions.clear();

    [self appendFavoritesSection];
    [self appendVolumesSection];
    [self appendConnectionsSection];
}

- (void)appendFavoritesSection
{
    const std::shared_ptr<nc::panel::FavoriteLocationsStorage> &storage = NCAppDelegate.me.favoriteLocationsStorage;
    if( !storage )
        return;

    const std::vector<nc::panel::FavoriteLocationsStorage::Favorite> favorites = storage->Favorites();
    if( favorites.empty() )
        return;

    [self beginSectionWithTitle:NSLocalizedString(@"Favorites", "Explorer sidebar section title")];

    nc::panel::NetworkConnectionsManager &net_mgr = *NCAppDelegate.me.networkConnectionsManager;
    nc::panel::loc_fmt::FavoriteFormatter formatter{net_mgr};
    PanelController *const panel = m_Panel;
    for( const nc::panel::FavoriteLocationsStorage::Favorite &fav : favorites ) {
        if( !fav.location )
            continue;
        const auto rep = formatter.Render(g_RenderOptions, fav);
        const nc::panel::PersistentLocation location = fav.location->hosts_stack;
        const std::function<void()> action = [panel, location] { NavigateToPersistentLocation(panel, location); };
        [self appendRowWithTitle:rep.menu_title icon:rep.menu_icon tooltip:rep.menu_tooltip action:action];
    }
}

- (void)appendVolumesSection
{
    nc::utility::NativeFSManager &fs_mgr = NCAppDelegate.me.nativeFSManager;
    std::vector<nc::utility::NativeFSManager::Info> volumes;
    for( const nc::utility::NativeFSManager::Info &vol : fs_mgr.Volumes() )
        if( vol && !vol->mount_flags.dont_browse )
            volumes.push_back(vol);
    if( volumes.empty() )
        return;

    [self beginSectionWithTitle:NSLocalizedString(@"This Mac", "Explorer sidebar section title")];

    PanelController *const panel = m_Panel;
    for( const nc::utility::NativeFSManager::Info &vol : volumes ) {
        const auto rep = nc::panel::loc_fmt::VolumeFormatter::Render(g_RenderOptions, *vol);
        const std::string path = vol->mounted_at_path;
        const std::function<void()> action = [panel, path] { NavigateToNativePath(panel, path); };
        [self appendRowWithTitle:rep.menu_title icon:rep.menu_icon tooltip:rep.menu_tooltip action:action];
    }
}

- (void)appendConnectionsSection
{
    const std::shared_ptr<nc::panel::NetworkConnectionsManager> &net_mgr = NCAppDelegate.me.networkConnectionsManager;
    if( !net_mgr )
        return;

    const std::vector<nc::panel::NetworkConnectionsManager::Connection> connections = net_mgr->AllConnectionsByMRU();
    if( connections.empty() )
        return;

    [self beginSectionWithTitle:NSLocalizedString(@"Network", "Explorer sidebar section title")];

    PanelController *const panel = m_Panel;
    for( const nc::panel::NetworkConnectionsManager::Connection &conn : connections ) {
        const auto rep = nc::panel::loc_fmt::NetworkConnectionFormatter::Render(g_RenderOptions, conn);
        const std::function<void()> action = [panel, net_mgr, conn] { NavigateToConnection(panel, net_mgr, conn); };
        [self appendRowWithTitle:rep.menu_title icon:rep.menu_icon tooltip:rep.menu_tooltip action:action];
    }
}

- (void)beginSectionWithTitle:(NSString *)_title
{
    if( m_Stack.arrangedSubviews.count > 0 )
        [m_Stack setCustomSpacing:g_SectionSpacing afterView:m_Stack.arrangedSubviews.lastObject];

    NSTextField *const label = [NSTextField labelWithString:_title];
    label.font = [NSFont boldSystemFontOfSize:NSFont.smallSystemFontSize];
    label.textColor = NSColor.secondaryLabelColor;
    label.translatesAutoresizingMaskIntoConstraints = false;

    [m_Stack addArrangedSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor constraintEqualToAnchor:m_Stack.leadingAnchor constant:g_RowSidePadding],
        [label.trailingAnchor constraintEqualToAnchor:m_Stack.trailingAnchor constant:-g_RowSidePadding],
    ]];
}

- (void)appendRowWithTitle:(NSString *)_title
                      icon:(NSImage *)_icon
                   tooltip:(NSString *)_tooltip
                    action:(std::function<void()>)_action
{
    NSButton *const button = [NSButton buttonWithTitle:_title target:self action:@selector(rowClicked:)];
    button.bordered = false;
    button.alignment = NSTextAlignmentLeft;
    button.font = [NSFont systemFontOfSize:NSFont.systemFontSize];
    button.lineBreakMode = NSLineBreakByTruncatingTail;
    button.toolTip = _tooltip;
    if( _icon ) {
        button.image = _icon;
        button.imagePosition = NSImageLeft;
    }
    button.translatesAutoresizingMaskIntoConstraints = false;
    button.tag = static_cast<NSInteger>(m_RowActions.size());
    m_RowActions.emplace_back(std::move(_action));

    [m_Stack addArrangedSubview:button];
    [NSLayoutConstraint activateConstraints:@[
        [button.leadingAnchor constraintEqualToAnchor:m_Stack.leadingAnchor constant:g_RowSidePadding],
        [button.trailingAnchor constraintEqualToAnchor:m_Stack.trailingAnchor constant:-g_RowSidePadding],
    ]];
}

- (void)rowClicked:(NSButton *)_sender
{
    if( _sender.tag < 0 )
        return;
    const size_t idx = static_cast<size_t>(_sender.tag);
    if( idx >= m_RowActions.size() )
        return;

    // Copy out in case the handler ends up re-entering -reloadData (which would otherwise mutate
    // m_RowActions out from under this call).
    const std::function<void()> action = m_RowActions[idx];
    if( action )
        action();
}

@end
