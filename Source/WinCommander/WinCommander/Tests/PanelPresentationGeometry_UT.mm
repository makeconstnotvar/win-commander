// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <Base/dispatch_cpp.h>
#include <Cocoa/Cocoa.h>
#include <CUI/CommandPopover.h>
#include <Config/ConfigImpl.h>
#include <Config/NonPersistentOverwritesStorage.h>
#include <CoreFoundation/CoreFoundation.h>
#include <VFS/VFSListingInput.h>
#include <Utility/ByteCountFormatter.h>
#include <Utility/HexadecimalColor.h>
#include <Panel/UI/PanelTabBarView.h>
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/FileCutCommand.h>
#include <WinCommander/Core/Commands/OperationCancelCommand.h>
#include <WinCommander/Core/Commands/OperationCenterOpenCommand.h>
#include <WinCommander/Core/Commands/ToggleHiddenFilesCommand.h>
#include <WinCommander/Core/Errors/FileManagerErrorAdapter.h>
#include <WinCommander/Core/Pane/PaneSnapshot.h>
#include <WinCommander/Core/Theming/ExplorerPalette.h>
#include <WinCommander/Core/Theming/ThemesManager.h>
#include <WinCommander/States/Explorer/NCExplorerBreadcrumbControl.h>
#include <WinCommander/States/Explorer/NCExplorerCommandBarView.h>
#include <WinCommander/States/Explorer/NCExplorerPanePresentationModel.h>
#include <WinCommander/States/Explorer/NCExplorerToolbarDelegate.h>
#include <WinCommander/States/FilePanels/ContextMenu.h>
#include <WinCommander/States/FilePanels/PanelController.h>
#include <WinCommander/States/FilePanels/PanelControllerActionsDispatcher.h>
#include <WinCommander/States/FilePanels/PanelViewFooter.h>
#include <WinCommander/States/FilePanels/Brief/PanelBriefViewCollectionViewItem.h>
#include <WinCommander/States/FilePanels/Gallery/PanelGalleryCollectionViewItem.h>
#include <WinCommander/States/FilePanels/Gallery/Layout.h>
#include <WinCommander/States/FilePanels/Helpers/Pasteboard.h>
#include <WinCommander/States/FilePanels/List/PanelListViewGeometry.h>
#include <WinCommander/States/FilePanels/List/PanelListViewProjection.h>
#include <WinCommander/States/FilePanels/List/PanelListViewRowView.h>
#include <WinCommander/States/FilePanels/List/PanelListViewTableHeaderView.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <Operations/OperationCenterCoordinator.h>
#include <Operations/OperationJournal.h>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <unistd.h>

using nc::panel::PanelListViewGeometry;
using nc::panel::PanelListViewGroupKey;
using nc::panel::PanelListViewGroupKind;
using nc::panel::PanelListViewProjection;
using nc::panel::PanelListViewProjectionItem;
using nc::panel::PanelListViewProjectionRow;
using nc::panel::PasteboardFileOperation;
using nc::panel::PasteboardSupport;
using nc::panel::gallery::BuildItemLayout;

@interface ExplorerTabFocusableTestView : NSView
@end

@implementation ExplorerTabFocusableTestView
- (BOOL)acceptsFirstResponder
{
    return YES;
}
@end

@interface ExplorerTabFocusHandoffTestDelegate : NSObject <NCPanelTabBarViewDelegate>
@property(nonatomic, weak) NSWindow *window;
@end

@implementation ExplorerTabFocusHandoffTestDelegate
@synthesize window = _window;

- (void)tabView:(NSTabView *) [[maybe_unused]] _tab_view
    didSelectTabViewItem:(NSTabViewItem *)_item
{
    [self.window makeFirstResponder:_item.view];
}
@end

namespace {

class NativePasteboardTestHost final : public nc::vfs::Host
{
public:
    NativePasteboardTestHost() : Host("/", nullptr, "native_pasteboard_test") {}

    bool IsNativeFS() const noexcept override { return true; }
};

class ExplorerFooterTheme final : public nc::panel::FooterTheme
{
public:
    NSFont *Font() const override { return [NSFont systemFontOfSize:12.0]; }
    NSColor *TextColor() const override { return NSColor.textColor; }
    NSColor *ActiveTextColor() const override { return NSColor.textColor; }
    NSColor *SeparatorsColor() const override { return NSColor.separatorColor; }
    NSColor *ActiveBackgroundColor() const override { return NSColor.controlBackgroundColor; }
    NSColor *InactiveBackgroundColor() const override { return NSColor.controlBackgroundColor; }
    void ObserveChanges(std::function<void()>) override {}
};

std::vector<VFSListingItem> NativeItems(const std::vector<std::string> &_filenames)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/";
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = std::make_shared<NativePasteboardTestHost>();
    for( const auto &filename : _filenames ) {
        input.filenames.emplace_back(filename);
        input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
        input.unix_types.emplace_back(DT_REG);
    }

    const auto listing = VFSListing::Build(std::move(input));
    std::vector<VFSListingItem> items;
    items.reserve(_filenames.size());
    for( unsigned index = 0; index < _filenames.size(); ++index )
        items.emplace_back(listing->Item(index));
    return items;
}

void EnsureExplorerItemTheme()
{
    static const auto *const storage =
        new std::shared_ptr<nc::config::NonPersistentOverwritesStorage>(
            std::make_shared<nc::config::NonPersistentOverwritesStorage>(""));
    static auto *const config = new nc::config::ConfigImpl{
        R"({"current":"accessibility-test","themes":{"themes_v1":[{"themeName":"accessibility-test"}]}})",
        *storage};
    static const auto *const themes = new nc::ThemesManager{*config, "current", "themes"};
    static_cast<void>(themes);
}

VFSListingPtr ExplorerPresentationUniformListing(const VFSHostPtr &_host, std::string _directory)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = std::move(_directory);
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = _host;
    input.filenames.emplace_back("entry");
    input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
    input.unix_types.emplace_back(DT_REG);
    return VFSListing::Build(std::move(input));
}

VFSListingPtr ExplorerPresentationNonUniformListing()
{
    nc::vfs::ListingInput input;
    input.title = "Search results";
    input.directories.reset(nc::base::variable_container<>::type::dense);
    input.hosts.reset(nc::base::variable_container<>::type::dense);
    for( size_t index = 0; index != 2; ++index ) {
        input.directories.insert(index, index == 0 ? "/first/" : "/second/");
        input.hosts.insert(index, VFSHost::DummyHost());
        input.filenames.emplace_back(index == 0 ? "first" : "second");
        input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
        input.unix_types.emplace_back(DT_REG);
    }
    return VFSListing::Build(std::move(input));
}

} // namespace

@interface ExplorerBreadcrumbTestPanelController : PanelController
@property(nonatomic, readonly) std::shared_ptr<nc::panel::DirectoryChangeRequest> capturedRequest;
@end

@implementation ExplorerBreadcrumbTestPanelController {
    std::shared_ptr<nc::panel::DirectoryChangeRequest> m_CapturedRequest;
}

- (nc::core::PaneId)paneId
{
    return nc::core::PaneId{91};
}

- (std::expected<void, nc::Error>)GoToDirWithContext:(std::shared_ptr<nc::panel::DirectoryChangeRequest>)_request
{
    m_CapturedRequest = std::move(_request);
    return {};
}

- (std::shared_ptr<nc::panel::DirectoryChangeRequest>)capturedRequest
{
    return m_CapturedRequest;
}

@end

@interface ExplorerToolbarTestActionsDispatcher : NCPanelControllerActionsDispatcher
- (std::optional<nc::core::PaneHistoryAvailability>)lastBackAvailability;
- (std::optional<nc::core::PaneHistoryAvailability>)lastForwardAvailability;
- (std::optional<nc::core::NavigationUpAvailability>)lastUpAvailability;
- (std::optional<nc::core::NavigationRefreshAvailability>)lastRefreshAvailability;
- (nc::core::CommandInvocationSource)lastSource;
- (int)backExecutionCount;
- (int)forwardExecutionCount;
- (int)upExecutionCount;
- (int)refreshExecutionCount;
- (nc::core::CommandInvocationSource)lastBackExecutionSource;
- (nc::core::CommandInvocationSource)lastForwardExecutionSource;
- (nc::core::CommandInvocationSource)lastUpExecutionSource;
- (nc::core::CommandInvocationSource)lastRefreshExecutionSource;
- (id)lastBackSender;
- (id)lastForwardSender;
- (id)lastUpSender;
- (id)lastRefreshSender;
@end

@implementation ExplorerToolbarTestActionsDispatcher {
    std::optional<nc::core::PaneHistoryAvailability> m_LastBackAvailability;
    std::optional<nc::core::PaneHistoryAvailability> m_LastForwardAvailability;
    std::optional<nc::core::NavigationUpAvailability> m_LastUpAvailability;
    std::optional<nc::core::NavigationRefreshAvailability> m_LastRefreshAvailability;
    nc::core::CommandInvocationSource m_LastSource;
    int m_BackExecutionCount;
    int m_ForwardExecutionCount;
    int m_UpExecutionCount;
    int m_RefreshExecutionCount;
    nc::core::CommandInvocationSource m_LastBackExecutionSource;
    nc::core::CommandInvocationSource m_LastForwardExecutionSource;
    nc::core::CommandInvocationSource m_LastUpExecutionSource;
    nc::core::CommandInvocationSource m_LastRefreshExecutionSource;
    __weak id m_LastBackSender;
    __weak id m_LastForwardSender;
    __weak id m_LastUpSender;
    __weak id m_LastRefreshSender;
}

- (nc::core::CommandState)navigationBackCommandStateForAvailability:
                              (std::optional<nc::core::PaneHistoryAvailability>)_availability
                                                             source:(nc::core::CommandInvocationSource)_source
{
    m_LastBackAvailability = _availability;
    m_LastSource = _source;
    nc::core::CommandState state;
    state.enabled = _availability && _availability->can_go_back;
    if( !state.enabled ) {
        state.disabled_reason = nc::core::DisabledReason{
            .code = _availability ? "navigation.backUnavailable" : "context.historyAvailabilityRequired",
            .user_message_key = _availability ? "commands.navigation.back.disabled.historyBoundary"
                                              : "commands.navigation.back.disabled.stateUnavailable",
            .technical_message = "Toolbar back fixture disabled.",
        };
    }
    return state;
}

- (nc::core::CommandState)navigationForwardCommandStateForAvailability:
                              (std::optional<nc::core::PaneHistoryAvailability>)_availability
                                                                source:(nc::core::CommandInvocationSource)_source
{
    m_LastForwardAvailability = _availability;
    m_LastSource = _source;
    nc::core::CommandState state;
    state.enabled = _availability && _availability->can_go_forward;
    if( !state.enabled ) {
        state.disabled_reason = nc::core::DisabledReason{
            .code = _availability ? "navigation.forwardUnavailable" : "context.historyAvailabilityRequired",
            .user_message_key = _availability ? "commands.navigation.forward.disabled.historyBoundary"
                                              : "commands.navigation.forward.disabled.stateUnavailable",
            .technical_message = "Toolbar forward fixture disabled.",
        };
    }
    return state;
}

- (nc::core::CommandState)navigationUpCommandStateForAvailability:
                              (std::optional<nc::core::NavigationUpAvailability>)_availability
                                                           source:(nc::core::CommandInvocationSource)_source
{
    m_LastUpAvailability = _availability;
    m_LastSource = _source;
    nc::core::CommandState state;
    state.enabled = _availability && *_availability == nc::core::NavigationUpAvailability::Available;
    if( !state.enabled ) {
        state.disabled_reason = nc::core::DisabledReason{
            .code = "navigation.upUnavailable",
            .user_message_key = "commands.navigation.up.disabled.stateUnavailable",
            .technical_message = "Toolbar up fixture disabled.",
        };
    }
    return state;
}

- (nc::core::CommandState)navigationRefreshCommandStateForAvailability:
                              (std::optional<nc::core::NavigationRefreshAvailability>)_availability
                                                                source:(nc::core::CommandInvocationSource)_source
{
    m_LastRefreshAvailability = _availability;
    m_LastSource = _source;
    nc::core::CommandState state;
    state.enabled = _availability && *_availability == nc::core::NavigationRefreshAvailability::Available;
    if( !state.enabled ) {
        state.disabled_reason = nc::core::DisabledReason{
            .code = "navigation.refreshUnavailable",
            .user_message_key = "commands.navigation.refresh.disabled.stateUnavailable",
            .technical_message = "Toolbar refresh fixture disabled.",
        };
    }
    return state;
}

- (std::optional<nc::core::PaneHistoryAvailability>)lastBackAvailability
{
    return m_LastBackAvailability;
}

- (std::optional<nc::core::PaneHistoryAvailability>)lastForwardAvailability
{
    return m_LastForwardAvailability;
}

- (std::optional<nc::core::NavigationUpAvailability>)lastUpAvailability
{
    return m_LastUpAvailability;
}

- (std::optional<nc::core::NavigationRefreshAvailability>)lastRefreshAvailability
{
    return m_LastRefreshAvailability;
}

- (nc::core::CommandInvocationSource)lastSource
{
    return m_LastSource;
}

- (void)executeNavigationBackCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    ++m_BackExecutionCount;
    m_LastBackExecutionSource = _source;
    m_LastBackSender = _sender;
}

- (void)executeNavigationForwardCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    ++m_ForwardExecutionCount;
    m_LastForwardExecutionSource = _source;
    m_LastForwardSender = _sender;
}

- (void)executeNavigationUpCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    ++m_UpExecutionCount;
    m_LastUpExecutionSource = _source;
    m_LastUpSender = _sender;
}

- (void)executeNavigationRefreshCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    ++m_RefreshExecutionCount;
    m_LastRefreshExecutionSource = _source;
    m_LastRefreshSender = _sender;
}

- (int)backExecutionCount
{
    return m_BackExecutionCount;
}

- (int)forwardExecutionCount
{
    return m_ForwardExecutionCount;
}

- (int)upExecutionCount
{
    return m_UpExecutionCount;
}

- (int)refreshExecutionCount
{
    return m_RefreshExecutionCount;
}

- (nc::core::CommandInvocationSource)lastBackExecutionSource
{
    return m_LastBackExecutionSource;
}

- (nc::core::CommandInvocationSource)lastForwardExecutionSource
{
    return m_LastForwardExecutionSource;
}

- (nc::core::CommandInvocationSource)lastUpExecutionSource
{
    return m_LastUpExecutionSource;
}

- (nc::core::CommandInvocationSource)lastRefreshExecutionSource
{
    return m_LastRefreshExecutionSource;
}

- (id)lastBackSender
{
    return m_LastBackSender;
}

- (id)lastForwardSender
{
    return m_LastForwardSender;
}

- (id)lastUpSender
{
    return m_LastUpSender;
}

- (id)lastRefreshSender
{
    return m_LastRefreshSender;
}

@end

@interface ExplorerOperationMenuTestActionsDispatcher : NCPanelControllerActionsDispatcher
@end

@implementation ExplorerOperationMenuTestActionsDispatcher

- (nc::core::CommandState)fileCopyCommandStateFromSource:(nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)fileCutCommandStateFromSource:(nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)filePasteCommandStateFromSource:(nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)fileRenameCommandStateFromSource:(nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)fileGetInfoCommandStateFromSource:(nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)filePreviewCommandStateFromSource:(nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)fileNewFolderCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                  [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)fileNewFileCommandStateFromSource:(nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)fileTrashCommandStateFromSource:(nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)archiveCreateCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                  [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)archiveExtractCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                   [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)fileDuplicateCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                  [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)fileCopyPathCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                 [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)fileCalculateSizesCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                       [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)fileBatchRenameCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                    [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)paneSelectAllCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                  [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)paneInvertSelectionCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                        [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)viewToggleHiddenFilesCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                          [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)viewTogglePreviewPaneCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                          [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)navigationRefreshCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                      [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (bool)validateActionBySelector:(SEL) [[maybe_unused]] _selector
{
    return false;
}

@end

@interface ExplorerFileCommandMenuTestActionsDispatcher : ExplorerOperationMenuTestActionsDispatcher
@property(nonatomic) bool commandsEnabled;
@property(nonatomic, readonly) int archiveCreateExecutions;
@property(nonatomic, readonly) int archiveExtractExecutions;
@property(nonatomic, readonly) int duplicateExecutions;
@property(nonatomic, readonly) int copyPathExecutions;
@property(nonatomic, readonly) int calculateSizesExecutions;
@property(nonatomic, readonly) int batchRenameExecutions;
@property(nonatomic, readonly) int previewExecutions;
@property(nonatomic, readonly) int getInfoExecutions;
@property(nonatomic, readonly) nc::core::CommandInvocationSource lastSource;
@property(nonatomic, readonly) nc::core::CommandInvocationSource lastRosterStateSource;
@property(nonatomic, readonly) NSArray<NSString *> *rosterExecutions;
@end

@implementation ExplorerFileCommandMenuTestActionsDispatcher {
    bool m_CommandsEnabled;
    int m_ArchiveCreateExecutions;
    int m_ArchiveExtractExecutions;
    int m_DuplicateExecutions;
    int m_CopyPathExecutions;
    int m_CalculateSizesExecutions;
    int m_BatchRenameExecutions;
    int m_PreviewExecutions;
    int m_GetInfoExecutions;
    nc::core::CommandInvocationSource m_LastSource;
    nc::core::CommandInvocationSource m_LastRosterStateSource;
    NSMutableArray<NSString *> *m_RosterExecutions;
}

@synthesize commandsEnabled = m_CommandsEnabled;

- (nc::core::CommandState)testCommandState
{
    nc::core::CommandState state;
    state.enabled = m_CommandsEnabled;
    if( !state.enabled ) {
        state.disabled_reason = nc::core::DisabledReason{
            .code = "fixture.disabled",
            .user_message_key = "commands.disabled.generic",
            .technical_message = "Explorer file command fixture is disabled.",
        };
    }
    return state;
}

- (nc::core::CommandState)archiveCreateCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                  [[maybe_unused]] _source
{
    return [self testCommandState];
}

- (nc::core::CommandState)archiveExtractCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                   [[maybe_unused]] _source
{
    return [self testCommandState];
}

- (nc::core::CommandState)fileDuplicateCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                  [[maybe_unused]] _source
{
    return [self testCommandState];
}

- (nc::core::CommandState)fileCopyPathCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                 [[maybe_unused]] _source
{
    return [self testCommandState];
}

- (nc::core::CommandState)fileCalculateSizesCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                       [[maybe_unused]] _source
{
    return [self testCommandState];
}

- (nc::core::CommandState)fileBatchRenameCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                    [[maybe_unused]] _source
{
    return [self testCommandState];
}

- (nc::core::CommandState)filePreviewCommandStateFromSource:(nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    return [self testCommandState];
}

- (nc::core::CommandState)fileGetInfoCommandStateFromSource:(nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    return [self testCommandState];
}

- (nc::core::CommandState)filePasteCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    m_LastRosterStateSource = _source;
    return [self testCommandState];
}

- (nc::core::CommandState)fileNewFolderCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    m_LastRosterStateSource = _source;
    return [self testCommandState];
}

- (nc::core::CommandState)paneSelectAllCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    m_LastRosterStateSource = _source;
    return [self testCommandState];
}

- (nc::core::CommandState)paneInvertSelectionCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    m_LastRosterStateSource = _source;
    return [self testCommandState];
}

- (nc::core::CommandState)viewToggleHiddenFilesCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    m_LastRosterStateSource = _source;
    nc::core::CommandState state = [self testCommandState];
    state.check_state = nc::core::CommandCheckState::On;
    return state;
}

- (nc::core::CommandState)viewToggleHiddenFilesCommandStateForVisibility:(std::optional<bool>)
                                                                             [[maybe_unused]] _visibility
                                                                  source:(nc::core::CommandInvocationSource)_source
{
    return [self viewToggleHiddenFilesCommandStateFromSource:_source];
}

- (nc::core::CommandState)viewTogglePreviewPaneCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    m_LastRosterStateSource = _source;
    nc::core::CommandState state = [self testCommandState];
    state.check_state = nc::core::CommandCheckState::On;
    return state;
}

- (nc::core::CommandState)navigationRefreshCommandStateFromSource:(nc::core::CommandInvocationSource)_source
{
    m_LastRosterStateSource = _source;
    return [self testCommandState];
}

- (void)recordRosterExecution:(NSString *)_command source:(nc::core::CommandInvocationSource)_source
{
    if( !m_RosterExecutions )
        m_RosterExecutions = [NSMutableArray new];
    [m_RosterExecutions addObject:_command];
    m_LastSource = _source;
}

- (void)executeFilePasteCommandFromSource:(nc::core::CommandInvocationSource)_source
                                   sender:(id) [[maybe_unused]] _sender
{
    [self recordRosterExecution:@"file.paste" source:_source];
}

- (void)executeFileNewFolderCommandFromSource:(nc::core::CommandInvocationSource)_source
                                       sender:(id) [[maybe_unused]] _sender
{
    [self recordRosterExecution:@"file.newFolder" source:_source];
}

- (void)executePaneSelectAllCommandFromSource:(nc::core::CommandInvocationSource)_source
                                       sender:(id) [[maybe_unused]] _sender
{
    [self recordRosterExecution:@"pane.selectAll" source:_source];
}

- (void)executePaneInvertSelectionCommandFromSource:(nc::core::CommandInvocationSource)_source
                                             sender:(id) [[maybe_unused]] _sender
{
    [self recordRosterExecution:@"pane.invertSelection" source:_source];
}

- (void)executeViewToggleHiddenFilesCommandFromSource:(nc::core::CommandInvocationSource)_source
                                               sender:(id) [[maybe_unused]] _sender
{
    [self recordRosterExecution:@"view.toggleHiddenFiles" source:_source];
}

- (void)executeViewTogglePreviewPaneCommandFromSource:(nc::core::CommandInvocationSource)_source
                                               sender:(id) [[maybe_unused]] _sender
{
    [self recordRosterExecution:@"view.togglePreviewPane" source:_source];
}

- (void)executeNavigationRefreshCommandFromSource:(nc::core::CommandInvocationSource)_source
                                           sender:(id) [[maybe_unused]] _sender
{
    [self recordRosterExecution:@"navigation.refresh" source:_source];
}

- (void)executeArchiveCreateCommandFromSource:(nc::core::CommandInvocationSource)_source
                                       sender:(id) [[maybe_unused]] _sender
{
    ++m_ArchiveCreateExecutions;
    m_LastSource = _source;
}

- (void)executeArchiveExtractCommandFromSource:(nc::core::CommandInvocationSource)_source
                                        sender:(id) [[maybe_unused]] _sender
{
    ++m_ArchiveExtractExecutions;
    m_LastSource = _source;
}

- (void)executeFileDuplicateCommandFromSource:(nc::core::CommandInvocationSource)_source
                                       sender:(id) [[maybe_unused]] _sender
{
    ++m_DuplicateExecutions;
    m_LastSource = _source;
}

- (void)executeFileCopyPathCommandFromSource:(nc::core::CommandInvocationSource)_source
                                      sender:(id) [[maybe_unused]] _sender
{
    ++m_CopyPathExecutions;
    m_LastSource = _source;
}

- (void)executeFileCalculateSizesCommandFromSource:(nc::core::CommandInvocationSource)_source
                                            sender:(id) [[maybe_unused]] _sender
{
    ++m_CalculateSizesExecutions;
    m_LastSource = _source;
}

- (void)executeFileBatchRenameCommandFromSource:(nc::core::CommandInvocationSource)_source
                                         sender:(id) [[maybe_unused]] _sender
{
    ++m_BatchRenameExecutions;
    m_LastSource = _source;
}

- (void)executeFilePreviewCommandFromSource:(nc::core::CommandInvocationSource)_source
                                     sender:(id) [[maybe_unused]] _sender
{
    ++m_PreviewExecutions;
    m_LastSource = _source;
}

- (void)executeFileGetInfoCommandFromSource:(nc::core::CommandInvocationSource)_source
                                     sender:(id) [[maybe_unused]] _sender
{
    ++m_GetInfoExecutions;
    m_LastSource = _source;
}

- (int)archiveCreateExecutions
{
    return m_ArchiveCreateExecutions;
}
- (int)archiveExtractExecutions
{
    return m_ArchiveExtractExecutions;
}
- (int)duplicateExecutions
{
    return m_DuplicateExecutions;
}
- (int)copyPathExecutions
{
    return m_CopyPathExecutions;
}
- (int)calculateSizesExecutions
{
    return m_CalculateSizesExecutions;
}
- (int)batchRenameExecutions
{
    return m_BatchRenameExecutions;
}
- (int)previewExecutions
{
    return m_PreviewExecutions;
}
- (int)getInfoExecutions
{
    return m_GetInfoExecutions;
}
- (nc::core::CommandInvocationSource)lastSource
{
    return m_LastSource;
}
- (nc::core::CommandInvocationSource)lastRosterStateSource
{
    return m_LastRosterStateSource;
}
- (NSArray<NSString *> *)rosterExecutions
{
    return m_RosterExecutions ? [m_RosterExecutions copy] : @[];
}

@end

@interface ExplorerNewPopoverTestActionsDispatcher : ExplorerOperationMenuTestActionsDispatcher
@property(nonatomic) bool newFolderEnabled;
@property(nonatomic) bool newFileEnabled;
@property(nonatomic, readonly) int newFolderExecutionCount;
@property(nonatomic, readonly) int newFileExecutionCount;
@property(nonatomic, readonly) nc::core::CommandInvocationSource newFolderExecutionSource;
@property(nonatomic, readonly) nc::core::CommandInvocationSource newFileExecutionSource;
@property(nonatomic, readonly, weak) id newFolderSender;
@property(nonatomic, readonly, weak) id newFileSender;
@end

@implementation ExplorerNewPopoverTestActionsDispatcher {
    bool m_NewFolderEnabled;
    bool m_NewFileEnabled;
    int m_NewFolderExecutionCount;
    int m_NewFileExecutionCount;
    nc::core::CommandInvocationSource m_NewFolderExecutionSource;
    nc::core::CommandInvocationSource m_NewFileExecutionSource;
    __weak id m_NewFolderSender;
    __weak id m_NewFileSender;
}

@synthesize newFolderEnabled = m_NewFolderEnabled;
@synthesize newFileEnabled = m_NewFileEnabled;

- (nc::core::CommandState)fileNewFolderCommandStateFromSource:(nc::core::CommandInvocationSource)
                                                                  [[maybe_unused]] _source
{
    nc::core::CommandState state;
    state.enabled = self.newFolderEnabled;
    if( !state.enabled ) {
        state.disabled_reason = nc::core::DisabledReason{
            .code = "destination.readOnly",
            .user_message_key = "commands.file.newFolder.disabled.destinationReadOnly",
            .technical_message = "New Folder fixture is disabled.",
        };
    }
    return state;
}

- (void)executeFileNewFolderCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    ++m_NewFolderExecutionCount;
    m_NewFolderExecutionSource = _source;
    m_NewFolderSender = _sender;
}

- (nc::core::CommandState)fileNewFileCommandStateFromSource:(nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    nc::core::CommandState state;
    state.enabled = self.newFileEnabled;
    if( !state.enabled ) {
        state.disabled_reason = nc::core::DisabledReason{
            .code = "destination.readOnly",
            .user_message_key = "commands.file.newFile.disabled.destinationReadOnly",
            .technical_message = "New File fixture is disabled.",
        };
    }
    return state;
}

- (void)executeFileNewFileCommandFromSource:(nc::core::CommandInvocationSource)_source sender:(id)_sender
{
    ++m_NewFileExecutionCount;
    m_NewFileExecutionSource = _source;
    m_NewFileSender = _sender;
}

- (int)newFolderExecutionCount
{
    return m_NewFolderExecutionCount;
}

- (int)newFileExecutionCount
{
    return m_NewFileExecutionCount;
}

- (nc::core::CommandInvocationSource)newFolderExecutionSource
{
    return m_NewFolderExecutionSource;
}

- (nc::core::CommandInvocationSource)newFileExecutionSource
{
    return m_NewFileExecutionSource;
}

- (id)newFolderSender
{
    return m_NewFolderSender;
}

- (id)newFileSender
{
    return m_NewFileSender;
}

@end

// The command bar only needs this `actionsDispatcher` surface during construction. This fixture
// never becomes a PanelView or attaches to an NSWindow, keeping menu checks non-modal.
@interface ExplorerOperationMenuTestPanelView : NSObject
@property(nonatomic, strong) NCPanelControllerActionsDispatcher *actionsDispatcher;
@end

@implementation ExplorerOperationMenuTestPanelView {
    NCPanelControllerActionsDispatcher *m_ActionsDispatcher;
}

@synthesize actionsDispatcher = m_ActionsDispatcher;
@end

@interface ExplorerOperationMenuTestPanelController : PanelController
- (instancetype)initWithActionsDispatcher:(NCPanelControllerActionsDispatcher *)_dispatcher;
@end

@implementation ExplorerOperationMenuTestPanelController {
    ExplorerOperationMenuTestPanelView *m_TestView;
}

- (instancetype)initWithActionsDispatcher:(NCPanelControllerActionsDispatcher *)_dispatcher
{
    self = [super init];
    if( self ) {
        m_TestView = [ExplorerOperationMenuTestPanelView new];
        m_TestView.actionsDispatcher = _dispatcher;
    }
    return self;
}

- (nc::core::PaneId)paneId
{
    return nc::core::PaneId{92};
}

- (PanelView *)view
{
    return static_cast<PanelView *>(m_TestView);
}

- (std::vector<VFSListingItem>)selectedEntriesOrFocusedEntry
{
    return {};
}

@end

@interface NCExplorerBreadcrumbControl (ExplorerPresentationTests)
- (void)navigateToPath:(const std::string &)_path host:(const VFSHostPtr &)_host;
- (void)onPathEditorCommit:(id)_sender;
@end

@interface NCExplorerCommandBarView (ExplorerOperationMenuTests)
- (NSMenu *)buildMoreMenu;
- (void)performMoreMenuAction:(id)_sender;
- (NCCommandPopover *)buildNewPopover;
- (NCCommandPopover *)buildViewPopover;
- (void)performPopoverAction:(id)_sender;
- (void)performOperationCancel:(id)_sender;
- (void)performOperationCenterOpen:(id)_sender;
- (void)performOperationCenterSnapshotCancel:(id)_sender;
@end

@interface NCCommandPopover (ExplorerNewPopoverTests)
- (std::span<NCCommandPopoverItem *const>)commandItems;
@end

@interface NCExplorerOperationCancelMenuTarget : NSObject
- (const nc::core::CommandContext &)context;
@end

namespace {

nc::core::PaneSnapshot ExplorerSnapshot(const nc::core::PaneLoadPhase _phase,
                                        const VFSHostPtr &_host,
                                        std::optional<nc::core::FileManagerError> _error = std::nullopt)
{
    nc::core::PaneSnapshot snapshot;
    snapshot.pane_id = nc::core::PaneId{91};
    snapshot.revision = 1;
    snapshot.state.location_generation = 17;
    snapshot.state.load_phase = _phase;
    snapshot.state.is_uniform = true;
    snapshot.state.path = "/fixture/";
    snapshot.state.display_title = "Fixture";
    snapshot.state.host = _host;
    snapshot.state.item_count = 3;
    snapshot.state.visible_error = std::move(_error);
    return snapshot;
}

bool RunExplorerPresentationMainLoopUntil(const std::function<bool()> &_predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while( !_predicate() && std::chrono::steady_clock::now() < deadline )
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    return _predicate();
}

nc::core::FileManagerError ExplorerFailure()
{
    return nc::core::FileManagerError{
        .code = {.domain = "ExplorerBreadcrumbTest", .value = 1},
        .category = nc::core::FileManagerErrorCategory::PathNotFoundError,
        .severity = nc::core::FileManagerErrorSeverity::BlockingError,
        .user_message_key = "errors.explorer.fixture.missing",
        .user_message = "The fixture folder could not be opened.",
        .technical_message = "INTERNAL EXPLORER TEST DETAIL",
        .original_error = nc::Error{nc::Error::POSIX, ENOENT},
    };
}

struct ExplorerOperationMenuTestDirectory final {
    ExplorerOperationMenuTestDirectory()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "explorer-operation-menu-ut-XXXXXX").string();
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        path = std::filesystem::canonical(pattern).string();
    }

    ~ExplorerOperationMenuTestDirectory() { std::filesystem::remove_all(path); }

    std::string path;
};

nc::ops::OperationPlan ExplorerOperationMenuPlan(std::string _plan_id)
{
    auto plan = nc::ops::OperationPlan::Create(
        {.plan_id = std::move(_plan_id),
         .type = nc::ops::OperationPlanType::Copy,
         .sources = {nc::ops::OperationPlanSourceInput{"native", "/source"}},
         .destination = nc::ops::OperationPlanDestinationInput{"native",
                                                               "/destination",
                                                               nc::ops::OperationPlanDestinationKind::Directory},
         .conflict_policy = nc::ops::OperationPlanConflictPolicy{nc::ops::OperationPlanConflictDecision::Ask,
                                                                 nc::ops::OperationPlanConflictScope::ThisItem},
         .created_at = nc::ops::OperationPlan::TimePoint{std::chrono::seconds{1'700'000'000}}});
    REQUIRE(plan);
    return std::move(*plan);
}

nc::ops::OperationJournalItemResult ExplorerOperationMenuSuccess()
{
    return {.item_index = 0,
            .status = nc::ops::OperationJournalItemStatus::Succeeded,
            .error = nc::ops::OperationJournalItemError::None,
            .system_error = 0,
            .prior_error = nc::ops::OperationJournalItemError::None,
            .prior_system_error = 0,
            .bytes = 42,
            .destination_publication = nc::ops::OperationJournalPublicationState::Published,
            .filesystem_sync_status = nc::ops::OperationJournalFilesystemSyncStatus::Confirmed,
            .filesystem_sync_system_error = 0,
            .recovery_action = nc::ops::OperationJournalRecoveryAction::None};
}

std::shared_ptr<nc::ops::OperationCenterCoordinator>
ExplorerOperationMenuCoordinatorWithTerminalRecord(ExplorerOperationMenuTestDirectory &_directory)
{
    auto journal = nc::ops::OperationJournal::Open(_directory.path);
    REQUIRE(journal);
    auto admission = journal->Admit(ExplorerOperationMenuPlan("terminal"));
    REQUIRE(admission);
    auto running = journal->TransitionToRunning(std::move(*admission));
    REQUIRE(running);
    REQUIRE(journal->Finalize(
        std::move(*running), ExplorerOperationMenuSuccess(), nc::ops::OperationJournalState::Completed));

    auto coordinator = nc::ops::OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);
    return std::move(*coordinator);
}

std::shared_ptr<nc::ops::OperationCenterCoordinator>
ExplorerOperationMenuCoordinatorWithQueuedRecord(ExplorerOperationMenuTestDirectory &_directory)
{
    auto journal = nc::ops::OperationJournal::Open(_directory.path);
    REQUIRE(journal);
    auto coordinator = nc::ops::OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);

    auto staging = (*coordinator)->StageAdmission(*journal, ExplorerOperationMenuPlan("queued"));
    REQUIRE(staging);
    const auto committed = (*coordinator)->CommitAdmission(*journal, std::move(*staging));
    REQUIRE(committed);
    return std::move(*coordinator);
}

NSMenuItem *ExplorerOperationsMenuSection(NSMenu *_menu)
{
    NSString *const title = NSLocalizedString(@"explorer.operations.title", "Explorer operation menu");
    for( NSMenuItem *const item in _menu.itemArray )
        if( [item.title isEqual:title] )
            return item;
    return nil;
}

NSMenuItem *ExplorerOperationsMenuItemAfter(NSMenu *_menu, NSMenuItem *_item)
{
    const NSInteger index = [_menu indexOfItem:_item];
    if( index == -1 || index + 1 >= static_cast<NSInteger>(_menu.numberOfItems) )
        return nil;
    return [_menu itemAtIndex:index + 1];
}

NSMenuItem *ExplorerOperationsMenuItemNamed(NSMenu *_menu, NSString *_title)
{
    for( NSMenuItem *const item in _menu.itemArray )
        if( [item.title isEqual:_title] )
            return item;
    return nil;
}

NSMenuItem *ExplorerOperationsMenuRecordItem(NSMenu *_menu, const nc::ops::OperationRecord &_record)
{
    NSString *const operation_id = [NSString stringWithUTF8String:_record.operation_id.ToString().c_str()];
    for( NSMenuItem *const item in _menu.itemArray )
        if( [item.title rangeOfString:operation_id].location != NSNotFound )
            return item;
    return nil;
}

NSString *ExplorerOperationIdentifier(const nc::ops::OperationRecord &_record)
{
    const std::string operation_id = _record.operation_id.ToString();
    return [NSString stringWithUTF8String:operation_id.c_str()];
}

struct ExplorerOperationCenterSnapshotFixture final {
    std::shared_ptr<nc::ops::OperationJournal> journal;
    std::shared_ptr<nc::ops::OperationCenterCoordinator> coordinator;
};

ExplorerOperationCenterSnapshotFixture
ExplorerOperationCenterSnapshotWithTerminalAndQueuedRecords(ExplorerOperationMenuTestDirectory &_directory)
{
    auto opened = nc::ops::OperationJournal::Open(_directory.path);
    REQUIRE(opened);
    auto journal = std::make_shared<nc::ops::OperationJournal>(std::move(*opened));

    auto terminal_admission = journal->Admit(ExplorerOperationMenuPlan("terminal"));
    REQUIRE(terminal_admission);
    auto terminal_running = journal->TransitionToRunning(std::move(*terminal_admission));
    REQUIRE(terminal_running);
    REQUIRE(journal->Finalize(
        std::move(*terminal_running), ExplorerOperationMenuSuccess(), nc::ops::OperationJournalState::Completed));

    auto coordinator = nc::ops::OperationCenterCoordinator::Create(*journal);
    REQUIRE(coordinator);
    auto queued_staging = (*coordinator)->StageAdmission(*journal, ExplorerOperationMenuPlan("queued"));
    REQUIRE(queued_staging);
    REQUIRE((*coordinator)->CommitAdmission(*journal, std::move(*queued_staging)));
    return {.journal = std::move(journal), .coordinator = std::move(*coordinator)};
}

} // namespace

#define PREFIX "Explorer presentation geometry "

TEST_CASE(PREFIX "palette resolves the mockup light tokens without losing dark appearance support")
{
    const auto resolvedRGBA = [](NSColor *_color, NSAppearanceName _appearance_name) {
        NSAppearance *const appearance = [NSAppearance appearanceNamed:_appearance_name];
        __block uint32_t rgba = 0;
        [appearance performAsCurrentDrawingAppearance:^{
          rgba = [_color toRGBA];
        }];
        return rgba;
    };

    const uint32_t light_chrome = [[NSColor colorWithHexString:"#ECEEF1"] toRGBA];
    const uint32_t dark_chrome = [[NSColor colorWithHexString:"#2F2F31"] toRGBA];
    const uint32_t light_command_bar = [[NSColor colorWithHexString:"#F7F8FA"] toRGBA];
    const uint32_t dark_command_bar = [[NSColor colorWithHexString:"#252527"] toRGBA];

    CHECK(resolvedRGBA(nc::explorer::ChromeFillColor(), NSAppearanceNameAqua) == light_chrome);
    CHECK(resolvedRGBA(nc::explorer::ChromeFillColor(), NSAppearanceNameDarkAqua) == dark_chrome);
    CHECK(resolvedRGBA(nc::explorer::ChromeFillColor(), NSAppearanceNameAccessibilityHighContrastAqua) ==
          light_chrome);
    CHECK(resolvedRGBA(nc::explorer::ChromeFillColor(), NSAppearanceNameAccessibilityHighContrastDarkAqua) ==
          dark_chrome);
    CHECK(resolvedRGBA(nc::explorer::CommandBarFillColor(), NSAppearanceNameAqua) == light_command_bar);
    CHECK(resolvedRGBA(nc::explorer::CommandBarFillColor(), NSAppearanceNameDarkAqua) == dark_command_bar);
}

TEST_CASE(PREFIX "footer renders only PaneStore snapshot status")
{
    auto footer = [[NCPanelViewFooter alloc] initWithFrame:NSMakeRect(0, 0, 600, 24)
                                                     theme:std::make_unique<ExplorerFooterTheme>()
                                        explorerAppearance:true];
    NSTextField *const items = [footer valueForKey:@"m_ItemsLabel"];
    NSTextField *const selection = [footer valueForKey:@"m_SelectionLabel"];
    NSTextField *const volume = [footer valueForKey:@"m_VolumeLabel"];
    NSButton *const details = [footer valueForKey:@"m_DetailsButton"];
    NSButton *const icons = [footer valueForKey:@"m_IconsButton"];
    NSButton *const content = [footer valueForKey:@"m_ContentButton"];

    CHECK([footer.accessibilityIdentifier isEqualToString:@"wincommander.explorer.status"]);
    CHECK([footer.accessibilityRole isEqualToString:NSAccessibilityGroupRole]);
    CHECK(footer.accessibilityLabel.length > 0);
    CHECK([items.accessibilityIdentifier isEqualToString:@"wincommander.explorer.status.items"]);
    CHECK([selection.accessibilityIdentifier isEqualToString:@"wincommander.explorer.status.selection"]);
    CHECK([volume.accessibilityIdentifier isEqualToString:@"wincommander.explorer.status.volume"]);
    for( NSButton *button in @[details, icons, content] ) {
        CHECK(button.accessibilityIdentifier.length > 0);
        CHECK(button.accessibilityLabel.length > 0);
        CHECK(button.accessibilityHelp.length > 0);
    }
    [footer updateExplorerLayoutKind:NCPanelViewFooterLayoutKindDetails];
    CHECK([details.accessibilityValue isEqualToString:@"Selected"]);
    CHECK([icons.accessibilityValue isEqualToString:@"Not selected"]);
    CHECK([content.accessibilityValue isEqualToString:@"Not selected"]);

    nc::core::PaneSnapshot snapshot;
    snapshot.pane_id = nc::core::PaneId{91};
    [footer applyExplorerPaneSnapshot:snapshot];
    CHECK(items.stringValue.length == 0);
    CHECK(selection.stringValue.length == 0);

    snapshot.state.load_phase = nc::core::PaneLoadPhase::Loading;
    [footer applyExplorerPaneSnapshot:snapshot];
    CHECK(items.stringValue.length > 0);
    CHECK(selection.stringValue.length == 0);

    snapshot.state.load_phase = nc::core::PaneLoadPhase::Loaded;
    snapshot.state.item_count = 3;
    snapshot.state.selected_count = 2;
    snapshot.state.selected_bytes = 1536;
    [footer applyExplorerPaneSnapshot:snapshot];
    const auto items_format =
        NSLocalizedString(@"%d items", "Explorer status bar, total number of items in the current directory");
    CHECK([items.stringValue isEqual:[NSString stringWithFormat:items_format, 3]]);
    const auto selection_format = NSLocalizedString(
        @"%d selected (%@)", "Explorer status bar, number and total size of currently selected items");
    const auto selection_size = ByteCountFormatter::Instance().ToNSString(1536, ByteCountFormatter::Adaptive6);
    CHECK([selection.stringValue isEqual:[NSString stringWithFormat:selection_format, 2, selection_size]]);
    CHECK([static_cast<NSString *>(footer.accessibilityValue) containsString:items.stringValue]);
    CHECK([static_cast<NSString *>(footer.accessibilityValue) containsString:selection.stringValue]);

    const nc::panel::data::Statistics legacy_stats{
        .total_entries_amount = 99,
        .bytes_in_selected_entries = 4096,
        .selected_entries_amount = 7,
    };
    [footer updateStatistics:legacy_stats];
    [footer updateListing:VFSListingPtr{}];
    CHECK([items.stringValue isEqual:[NSString stringWithFormat:items_format, 3]]);
    CHECK([selection.stringValue isEqual:[NSString stringWithFormat:selection_format, 2, selection_size]]);

    snapshot.state.item_count = 0;
    snapshot.state.selected_count = 0;
    snapshot.state.selected_bytes = 0;
    [footer applyExplorerPaneSnapshot:snapshot];
    CHECK(items.stringValue.length > 0);
    CHECK(selection.stringValue.length == 0);

    snapshot.state.load_phase = nc::core::PaneLoadPhase::Failed;
    snapshot.state.visible_error = ExplorerFailure();
    [footer applyExplorerPaneSnapshot:snapshot];
    CHECK(items.stringValue.length > 0);
    CHECK(selection.stringValue.length == 0);
}

TEST_CASE(PREFIX "tab bar exposes stable VoiceOver controls and selected state")
{
    REQUIRE(nc::dispatch_is_main_queue());
    NSWindow *const window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 160)
                                                          styleMask:NSWindowStyleMaskBorderless
                                                            backing:NSBackingStoreBuffered
                                                              defer:false];
    NCPanelTabBarView *const bar = [[NCPanelTabBarView alloc] initWithFrame:NSMakeRect(0, 0, 480, 24)];
    [window.contentView addSubview:bar];

    NSTabView *const tab_view = [[NSTabView alloc] initWithFrame:NSMakeRect(0, 30, 480, 120)];
    [window.contentView addSubview:tab_view];
    NSTabViewItem *const first = [[NSTabViewItem alloc] initWithIdentifier:@(101)];
    first.label = @"Documents";
    first.view = [[ExplorerTabFocusableTestView alloc] initWithFrame:NSMakeRect(0, 0, 100, 24)];
    NSTabViewItem *const second = [[NSTabViewItem alloc] initWithIdentifier:@(202)];
    second.label = @"Downloads";
    second.view = [[ExplorerTabFocusableTestView alloc] initWithFrame:NSMakeRect(0, 0, 100, 24)];
    [tab_view addTabViewItem:first];
    [tab_view addTabViewItem:second];
    bar.tabView = tab_view;
    tab_view.delegate = bar;
    ExplorerTabFocusHandoffTestDelegate *const focus_handoff =
        [[ExplorerTabFocusHandoffTestDelegate alloc] init];
    focus_handoff.window = window;
    bar.delegate = focus_handoff;
    [bar reloadTabs];
    [bar layoutSubtreeIfNeeded];

    CHECK([bar.accessibilityIdentifier isEqualToString:@"wincommander.tabs"]);
    CHECK([bar.accessibilityRole isEqualToString:NSAccessibilityTabGroupRole]);
    CHECK(bar.accessibilityLabel.length > 0);
    NSButton *const add = bar.addTabButton;
    REQUIRE(add);
    CHECK([add.accessibilityIdentifier isEqualToString:@"wincommander.tabs.add"]);
    CHECK(add.accessibilityLabel.length > 0);
    CHECK(add.accessibilityHelp.length > 0);

    NSButton *const first_close = [bar closeButtonOfTabViewItem:first];
    NSButton *const second_close = [bar closeButtonOfTabViewItem:second];
    REQUIRE(first_close);
    REQUIRE(second_close);
    CHECK_FALSE(first_close.hidden);
    CHECK([first_close.accessibilityIdentifier isEqualToString:@"wincommander.tabs.close.101"]);
    CHECK(first_close.accessibilityLabel.length > 0);
    CHECK(first_close.accessibilityHelp.length > 0);

    NSView *const first_tab = first_close.superview;
    NSView *const second_tab = second_close.superview;
    REQUIRE(first_tab);
    REQUIRE(second_tab);
    CHECK([first_tab.accessibilityIdentifier isEqualToString:@"wincommander.tabs.tab.101"]);
    CHECK([first_tab.accessibilityRole isEqualToString:NSAccessibilityRadioButtonRole]);
    CHECK([first_tab.accessibilityLabel isEqualToString:@"Documents"]);
    CHECK(first_tab.accessibilitySelected);
    CHECK_FALSE(first_tab.accessibilityFocused);
    CHECK(static_cast<NSString *>(first_tab.accessibilityValue).length > 0);

    CHECK([second_tab.accessibilityIdentifier isEqualToString:@"wincommander.tabs.tab.202"]);
    CHECK_FALSE(second_tab.accessibilitySelected);
    CHECK([second_tab accessibilityPerformPress]);
    CHECK(tab_view.selectedTabViewItem == second);
    CHECK(second_tab.accessibilitySelected);
    CHECK_FALSE(second_close.hidden);
    CHECK(window.firstResponder == second.view);
    CHECK_FALSE(second_tab.accessibilityFocused);

    id const first_controller = first_close.target;
    REQUIRE([first_controller isKindOfClass:NSCollectionViewItem.class]);
    [static_cast<NSCollectionViewItem *>(first_controller) prepareForReuse];
    CHECK_FALSE(first_tab.accessibilityElement);
    CHECK(first_tab.accessibilityIdentifier == nil);
    CHECK(first_tab.accessibilityLabel == nil);
    CHECK_FALSE(first_tab.accessibilitySelected);
    CHECK_FALSE(first_tab.accessibilityFocused);
    CHECK(first_tab.accessibilityValue == nil);
    CHECK_FALSE(first_close.accessibilityElement);
    CHECK(first_close.accessibilityIdentifier == nil);
    CHECK(first_close.accessibilityLabel == nil);
}

TEST_CASE(PREFIX "virtualized List items rebind accessibility state without stale selection")
{
    REQUIRE(nc::dispatch_is_main_queue());
    EnsureExplorerItemTheme();
    const std::vector<VFSListingItem> items = NativeItems({"fixture.txt", "replacement.txt"});
    REQUIRE(items.size() == 2);

    PanelListViewRowView *const list = [[PanelListViewRowView alloc] initWithItem:items.front()];
    CHECK([list.accessibilityIdentifier isEqualToString:@"wincommander.explorer.list.item"]);
    CHECK([list.accessibilityRole isEqualToString:NSAccessibilityRowRole]);
    CHECK([list.accessibilityLabel isEqualToString:@"fixture.txt"]);
    CHECK_FALSE(list.accessibilitySelected);
    CHECK_FALSE(list.accessibilityFocused);
    list.panelActive = true;
    list.selected = true;
    CHECK(list.accessibilitySelected);
    CHECK(list.accessibilityFocused);
    CHECK([static_cast<NSString *>(list.accessibilityValue) containsString:@"focused"]);

    list.item = VFSListingItem{};
    CHECK_FALSE(list.accessibilityElement);
    CHECK_FALSE(list.accessibilitySelected);
    CHECK_FALSE(list.accessibilityFocused);
    CHECK(list.accessibilityLabel.length == 0);
    CHECK([list.accessibilityValue isEqualToString:@""]);

    list.item = items.back();
    CHECK(list.accessibilityElement);
    CHECK([list.accessibilityLabel isEqualToString:@"replacement.txt"]);
    CHECK_FALSE(list.accessibilitySelected);
    CHECK_FALSE(list.accessibilityFocused);

    nc::panel::data::ItemVolatileData marked;
    marked.toggle_selected(true);
    list.vd = marked;
    CHECK(list.accessibilitySelected);
    CHECK_FALSE(list.accessibilityFocused);
    CHECK([list.accessibilityValue isEqualToString:@"Selected"]);
}

TEST_CASE(PREFIX "virtualized Brief items clear and rebind production accessibility state")
{
    REQUIRE(nc::dispatch_is_main_queue());
    EnsureExplorerItemTheme();
    const std::vector<VFSListingItem> items = NativeItems({"fixture.txt", "replacement.txt"});
    REQUIRE(items.size() == 2);

    PanelBriefViewItem *const brief = [[PanelBriefViewItem alloc] initWithNibName:nil bundle:nil];
    NSView *const element = brief.view;
    CHECK([element.accessibilityIdentifier isEqualToString:@"wincommander.explorer.brief.item"]);
    CHECK([element.accessibilityRole isEqualToString:NSAccessibilityRowRole]);
    CHECK_FALSE(element.accessibilityElement);

    [brief setItem:items.front()];
    brief.panelActive = true;
    brief.selected = true;
    CHECK([element.accessibilityLabel isEqualToString:@"fixture.txt"]);
    CHECK(element.accessibilityElement);
    CHECK(element.accessibilitySelected);
    CHECK(element.accessibilityFocused);
    CHECK([static_cast<NSString *>(element.accessibilityValue) containsString:@"focused"]);

    [brief prepareForReuse];
    CHECK_FALSE(element.accessibilityElement);
    CHECK_FALSE(element.accessibilitySelected);
    CHECK_FALSE(element.accessibilityFocused);
    CHECK(element.accessibilityLabel.length == 0);
    CHECK([element.accessibilityValue isEqualToString:@""]);

    [brief setItem:items.back()];
    nc::panel::data::ItemVolatileData marked;
    marked.toggle_selected(true);
    [brief setVD:marked];
    CHECK(element.accessibilityElement);
    CHECK([element.accessibilityLabel isEqualToString:@"replacement.txt"]);
    CHECK(element.accessibilitySelected);
    CHECK_FALSE(element.accessibilityFocused);
    CHECK([element.accessibilityValue isEqualToString:@"Selected"]);
}

TEST_CASE(PREFIX "virtualized Gallery items clear and rebind production accessibility state")
{
    REQUIRE(nc::dispatch_is_main_queue());
    EnsureExplorerItemTheme();
    const std::vector<VFSListingItem> items = NativeItems({"fixture.txt", "replacement.txt"});
    REQUIRE(items.size() == 2);

    NCPanelGalleryCollectionViewItem *const gallery =
        [[NCPanelGalleryCollectionViewItem alloc] initWithNibName:nil bundle:nil];
    NSView *const element = gallery.view;
    CHECK([element.accessibilityIdentifier isEqualToString:@"wincommander.explorer.gallery.item"]);
    CHECK([element.accessibilityRole isEqualToString:NSAccessibilityRowRole]);
    CHECK_FALSE(element.accessibilityElement);

    gallery.item = items.front();
    gallery.panelActive = true;
    gallery.selected = true;
    CHECK([element.accessibilityLabel isEqualToString:@"fixture.txt"]);
    CHECK(element.accessibilityElement);
    CHECK(element.accessibilitySelected);
    CHECK(element.accessibilityFocused);
    CHECK([static_cast<NSString *>(element.accessibilityValue) containsString:@"focused"]);

    [gallery prepareForReuse];
    CHECK_FALSE(element.accessibilityElement);
    CHECK_FALSE(element.accessibilitySelected);
    CHECK_FALSE(element.accessibilityFocused);
    CHECK(element.accessibilityLabel.length == 0);
    CHECK([element.accessibilityValue isEqualToString:@""]);

    gallery.item = items.back();
    nc::panel::data::ItemVolatileData marked;
    marked.toggle_selected(true);
    gallery.vd = marked;
    CHECK(element.accessibilityElement);
    CHECK([element.accessibilityLabel isEqualToString:@"replacement.txt"]);
    CHECK(element.accessibilitySelected);
    CHECK_FALSE(element.accessibilityFocused);
    CHECK([element.accessibilityValue isEqualToString:@"Selected"]);
}

TEST_CASE(PREFIX "Details uses the 38 point mockup row")
{
    const PanelListViewGeometry geometry(
        [NSFont systemFontOfSize:13.0], 1, 19, PanelListViewGeometry::Insets{.left = 12, .right = 12, .icon_gap = 8});

    CHECK(geometry.LineHeight() == 38);
    CHECK(geometry.IconSize() == 16);
    CHECK(geometry.LeftInset() == 12);
    CHECK(geometry.RightInset() == 12);
    // 12pt cell pad, then the 16pt icon, then the mockup's 8pt gap before the name.
    CHECK(geometry.FilenameOffsetInColumn() == 36);
}

TEST_CASE(PREFIX "Commander list geometry is untouched by the Explorer insets")
{
    const PanelListViewGeometry geometry([NSFont systemFontOfSize:13.0], 1, 0);

    CHECK(geometry.LineHeight() == 19);
    CHECK(geometry.TextBaseLine() == 4);
    CHECK(geometry.LeftInset() == 7);
    CHECK(geometry.RightInset() == 5);
    // The old formula was 2 * LeftInset() + IconSize(); the new one is left + icon + gap. With the
    // Commander defaults both give 30, which is what keeps the classic presentation bit-identical.
    CHECK(geometry.FilenameOffsetInColumn() == 30);
}

TEST_CASE(PREFIX "Gallery item grows with icon scale and text lines")
{
    const auto compact = BuildItemLayout(32, 16, 4, 1);
    const auto large = BuildItemLayout(64, 16, 4, 2);

    CHECK(compact.icon_size == 32);
    CHECK(compact.text_lines == 1);
    CHECK(large.icon_size == 64);
    CHECK(large.text_lines == 2);
    CHECK(large.width > compact.width);
    CHECK(large.height > compact.height);
    CHECK(large.icon_left_margin + large.icon_size + large.icon_right_margin == large.width);
}

TEST_CASE(PREFIX "Explorer presentation accepts only its pane Store snapshot")
{
    using namespace nc::core;

    CommandRegistry registry;
    REQUIRE(registry.Register(MakeViewToggleHiddenFilesCommand([](void *, bool) { return true; })) ==
            CommandRegistry::RegisterResult::Registered);

    static int target;
    constexpr PaneId pane_id{91};
    nc::explorer::PanePresentationModel presentation{pane_id};
    const auto state = [&] {
        const CommandContext context{
            .source = CommandInvocationSource::Toolbar,
            .native_target = &target,
            .shows_hidden_files = presentation.HiddenFilesVisibility(),
        };
        return registry.QueryState(CommandId{command_ids::ViewToggleHiddenFiles}, context).state;
    };
    const auto check_missing = [&] {
        const CommandState command_state = state();
        CHECK_FALSE(command_state.enabled);
        CHECK(command_state.check_state == CommandCheckState::Off);
        REQUIRE(command_state.disabled_reason);
        CHECK(command_state.disabled_reason->code == "context.hiddenFilesStateRequired");
        CHECK_FALSE(presentation.SortState());
        CHECK_FALSE(presentation.ActiveSortDirection(PaneSortKey::Name));
        CHECK_FALSE(presentation.GroupingState());
        CHECK_FALSE(presentation.ViewState());
        CHECK_FALSE(presentation.NoGroupingMarkerActive());
        CHECK_FALSE(presentation.GroupingMarkerActive(PaneGroupingKey::Name));
        CHECK_FALSE(presentation.LayoutMarkerActive(0));
        CHECK_FALSE(presentation.CanGoBack());
        CHECK_FALSE(presentation.CanGoForward());
    };

    // Before the Store publishes a snapshot the command bar has no authoritative visibility.
    check_missing();

    PaneSnapshot snapshot;
    snapshot.pane_id = pane_id;
    snapshot.state.shows_hidden_files = false;
    presentation.Apply(snapshot);
    CHECK(state().enabled);
    CHECK(state().check_state == CommandCheckState::Off);
    REQUIRE(presentation.SortState());
    REQUIRE(presentation.GroupingState());
    REQUIRE(presentation.ViewState());
    CHECK(presentation.NoGroupingMarkerActive());
    CHECK_FALSE(presentation.LayoutMarkerActive(0));

    static constexpr std::array sort_keys = {
        PaneSortKey::Name,
        PaneSortKey::Extension,
        PaneSortKey::Size,
        PaneSortKey::ModifiedTime,
        PaneSortKey::CreatedTime,
        PaneSortKey::AddedTime,
        PaneSortKey::AccessedTime,
    };
    static constexpr std::array directions = {
        PaneSortDirection::Ascending,
        PaneSortDirection::Descending,
    };
    for( const PaneSortKey key : sort_keys ) {
        for( const PaneSortDirection direction : directions ) {
            snapshot.state.sort_state = {.key = key, .direction = direction};
            presentation.Apply(snapshot);
            REQUIRE(presentation.SortState());
            CHECK(presentation.SortState()->key == key);
            CHECK(presentation.SortState()->direction == direction);
            for( const PaneSortKey candidate : sort_keys ) {
                const auto marker = presentation.ActiveSortDirection(candidate);
                if( candidate == key ) {
                    REQUIRE(marker);
                    CHECK(*marker == direction);
                }
                else {
                    CHECK_FALSE(marker);
                }
            }
        }
    }

    static constexpr std::array grouping_keys = {
        PaneGroupingKey::Name,
        PaneGroupingKey::Extension,
        PaneGroupingKey::Size,
        PaneGroupingKey::ModifiedTime,
    };
    snapshot.state.grouping_state = {.enabled = false, .key = PaneGroupingKey::Unknown};
    presentation.Apply(snapshot);
    CHECK(presentation.NoGroupingMarkerActive());
    for( const PaneGroupingKey key : grouping_keys )
        CHECK_FALSE(presentation.GroupingMarkerActive(key));

    for( const PaneGroupingKey key : grouping_keys ) {
        snapshot.state.grouping_state = {.enabled = true, .key = key};
        presentation.Apply(snapshot);
        CHECK_FALSE(presentation.NoGroupingMarkerActive());
        for( const PaneGroupingKey candidate : grouping_keys )
            CHECK(presentation.GroupingMarkerActive(candidate) == (candidate == key));
    }

    for( int32_t layout_index = 0; layout_index < 5; ++layout_index ) {
        snapshot.state.view_state = {
            .mode = PaneViewMode::Details,
            .layout_index = layout_index,
        };
        presentation.Apply(snapshot);
        for( int32_t candidate = 0; candidate < 5; ++candidate )
            CHECK(presentation.LayoutMarkerActive(candidate) == (candidate == layout_index));
    }

    for( const bool can_go_back : {false, true} ) {
        for( const bool can_go_forward : {false, true} ) {
            snapshot.state.history_availability = {
                .can_go_back = can_go_back,
                .can_go_forward = can_go_forward,
            };
            presentation.Apply(snapshot);
            CHECK(presentation.CanGoBack() == can_go_back);
            CHECK(presentation.CanGoForward() == can_go_forward);
        }
    }

    snapshot.state.shows_hidden_files = true;
    presentation.Apply(snapshot);
    CHECK(state().enabled);
    CHECK(state().check_state == CommandCheckState::On);

    // A later snapshot owned by another pane invalidates the cached Registry context.
    snapshot.pane_id = PaneId{92};
    presentation.Apply(snapshot);
    check_missing();
}

TEST_CASE(PREFIX "pane model caches navigation availability only for its exact snapshot")
{
    using namespace nc::core;

    constexpr PaneId pane_id{91};
    nc::explorer::PanePresentationModel presentation{pane_id};
    CHECK_FALSE(presentation.NavigationAvailability());

    const VFSHostPtr host = std::make_shared<NativePasteboardTestHost>();
    PaneSnapshot snapshot;
    snapshot.pane_id = pane_id;
    snapshot.state.load_phase = PaneLoadPhase::Loaded;
    snapshot.state.is_uniform = true;
    snapshot.state.path = "/child/";
    snapshot.state.host = host;
    snapshot.state.listing = ExplorerPresentationUniformListing(host, snapshot.state.path);

    CHECK(presentation.Apply(snapshot));
    REQUIRE(presentation.NavigationAvailability());
    CHECK(*presentation.NavigationAvailability() ==
          PaneNavigationAvailability{NavigationUpAvailability::Available, NavigationRefreshAvailability::Available});

    snapshot.state.path = "/";
    snapshot.state.listing = ExplorerPresentationUniformListing(host, snapshot.state.path);
    CHECK(presentation.Apply(snapshot));
    REQUIRE(presentation.NavigationAvailability());
    CHECK(*presentation.NavigationAvailability() ==
          PaneNavigationAvailability{NavigationUpAvailability::AtTop, NavigationRefreshAvailability::Available});

    snapshot.state.is_uniform = false;
    snapshot.state.path.clear();
    snapshot.state.host.reset();
    snapshot.state.listing = ExplorerPresentationNonUniformListing();
    CHECK(presentation.Apply(snapshot));
    REQUIRE(presentation.NavigationAvailability());
    CHECK(*presentation.NavigationAvailability() ==
          PaneNavigationAvailability{NavigationUpAvailability::HierarchyUnavailable,
                                     NavigationRefreshAvailability::Available});

    snapshot.state = {};
    snapshot.state.load_phase = PaneLoadPhase::Loading;
    CHECK(presentation.Apply(snapshot));
    REQUIRE(presentation.NavigationAvailability());
    CHECK(*presentation.NavigationAvailability() ==
          PaneNavigationAvailability{NavigationUpAvailability::Busy, NavigationRefreshAvailability::Busy});

    snapshot.pane_id = PaneId{92};
    CHECK_FALSE(presentation.Apply(snapshot));
    CHECK_FALSE(presentation.NavigationAvailability());
}

// Split across two deliberately non-inlined helpers, and not for style: as one function this
// test's frame crossed the project's 32 KiB limit once a sanitizer instrumented it, failing the
// whole scheme's build - so neither ASAN nor TSan could run on this suite at all. The cut sits
// where it does because it is the one point no local crosses.
/** The toolbar's identity, accessibility and initial state. */
[[clang::noinline]] static void CheckExplorerToolbarIdentity(NSButton *const back, NSButton *const forward, NSButton *const up, NSButton *const refresh, NCExplorerBreadcrumbControl *const breadcrumb, const NSArray<NSButton *> *const buttons, const NSArray<NSString *> *const identifiers, ExplorerToolbarTestActionsDispatcher *const dispatcher)
{
    for( NSUInteger index = 0; index != buttons.count; ++index ) {
        NSButton *const button = buttons[index];
        CHECK([button.accessibilityIdentifier isEqualToString:identifiers[index]]);
        CHECK(button.accessibilityLabel.length > 0);
        CHECK(button.accessibilityHelp.length > 0);
        CHECK(button.toolTip.length > 0);
    }
    CHECK([breadcrumb.accessibilityIdentifier isEqualToString:@"wincommander.explorer.toolbar.path"]);
    CHECK(breadcrumb.accessibilityElement);
    CHECK(breadcrumb.accessibilityLabel.length > 0);
    CHECK(breadcrumb.accessibilityHelp.length > 0);
    CHECK_FALSE(back.enabled);
    CHECK_FALSE(forward.enabled);
    CHECK_FALSE(up.enabled);
    CHECK_FALSE(refresh.enabled);
    CHECK(back.toolTip != nil);
    CHECK(forward.toolTip != nil);
    CHECK(up.toolTip != nil);
    CHECK(refresh.toolTip != nil);
    CHECK(back.accessibilityHelp != nil);
    CHECK(forward.accessibilityHelp != nil);
    CHECK(up.accessibilityHelp != nil);
    CHECK(refresh.accessibilityHelp != nil);
    CHECK_FALSE(dispatcher.lastBackAvailability.has_value());
    CHECK_FALSE(dispatcher.lastForwardAvailability.has_value());
    CHECK_FALSE(dispatcher.lastUpAvailability.has_value());
    CHECK_FALSE(dispatcher.lastRefreshAvailability.has_value());
    CHECK(dispatcher.lastSource == nc::core::CommandInvocationSource::Toolbar);

}

/** How the toolbar answers history snapshots, matching and not. */
/** How availability follows the snapshot's own contents, and what a foreign snapshot does not do. */
[[clang::noinline]] static void CheckExplorerToolbarAvailability(nc::core::PaneSnapshot &snapshot, const VFSHostPtr &host, NCExplorerToolbarDelegate *const delegate, ExplorerToolbarTestActionsDispatcher *const dispatcher, NSButton *const back, NSButton *const forward, NSButton *const up, NSButton *const refresh, NCExplorerBreadcrumbControl *const breadcrumb)
{
    snapshot.state.load_phase = nc::core::PaneLoadPhase::Loaded;
    snapshot.state.is_uniform = true;
    snapshot.state.path = "/";
    snapshot.state.host = host;
    snapshot.state.listing = ExplorerPresentationUniformListing(host, snapshot.state.path);
    [delegate applyPaneSnapshot:snapshot];
    CHECK_FALSE(up.enabled);
    CHECK(refresh.enabled);
    REQUIRE(dispatcher.lastUpAvailability);
    REQUIRE(dispatcher.lastRefreshAvailability);
    CHECK(*dispatcher.lastUpAvailability == nc::core::NavigationUpAvailability::AtTop);
    CHECK(*dispatcher.lastRefreshAvailability == nc::core::NavigationRefreshAvailability::Available);
    CHECK(up.toolTip != nil);
    CHECK(up.accessibilityHelp != nil);
    CHECK(refresh.toolTip.length > 0);
    CHECK(refresh.accessibilityHelp.length > 0);

    snapshot.state.is_uniform = false;
    snapshot.state.path.clear();
    snapshot.state.host.reset();
    snapshot.state.listing = ExplorerPresentationNonUniformListing();
    [delegate applyPaneSnapshot:snapshot];
    CHECK_FALSE(up.enabled);
    CHECK(refresh.enabled);
    REQUIRE(dispatcher.lastUpAvailability);
    REQUIRE(dispatcher.lastRefreshAvailability);
    CHECK(*dispatcher.lastUpAvailability == nc::core::NavigationUpAvailability::HierarchyUnavailable);
    CHECK(*dispatcher.lastRefreshAvailability == nc::core::NavigationRefreshAvailability::Available);
    CHECK(up.toolTip != nil);
    CHECK(up.accessibilityHelp != nil);
    CHECK(refresh.toolTip.length > 0);
    CHECK(refresh.accessibilityHelp.length > 0);

    snapshot.state.is_uniform = true;
    snapshot.state.path = "/fixture/";
    snapshot.state.host = host;
    snapshot.state.listing = ExplorerPresentationUniformListing(host, snapshot.state.path);
    [delegate applyPaneSnapshot:snapshot];
    CHECK(up.enabled);
    CHECK(refresh.enabled);
    REQUIRE(dispatcher.lastUpAvailability);
    REQUIRE(dispatcher.lastRefreshAvailability);
    CHECK(*dispatcher.lastUpAvailability == nc::core::NavigationUpAvailability::Available);
    CHECK(*dispatcher.lastRefreshAvailability == nc::core::NavigationRefreshAvailability::Available);
    CHECK(up.toolTip.length > 0);
    CHECK(refresh.toolTip.length > 0);
    CHECK(up.accessibilityHelp.length > 0);
    CHECK(refresh.accessibilityHelp.length > 0);
    [up performClick:nil];
    [refresh performClick:nil];
    CHECK(dispatcher.upExecutionCount == 1);
    CHECK(dispatcher.refreshExecutionCount == 1);
    CHECK(dispatcher.lastUpExecutionSource == nc::core::CommandInvocationSource::Toolbar);
    CHECK(dispatcher.lastRefreshExecutionSource == nc::core::CommandInvocationSource::Toolbar);
    CHECK(dispatcher.lastUpSender == up);
    CHECK(dispatcher.lastRefreshSender == refresh);

    snapshot.pane_id = nc::core::PaneId{92};
    snapshot.state.path = "/foreign/";
    snapshot.state.history_availability = {.can_go_back = true, .can_go_forward = true};
    [delegate applyPaneSnapshot:snapshot];
    CHECK_FALSE(back.enabled);
    CHECK_FALSE(forward.enabled);
    CHECK_FALSE(up.enabled);
    CHECK_FALSE(refresh.enabled);
    CHECK(back.toolTip != nil);
    CHECK(forward.toolTip != nil);
    CHECK(up.toolTip != nil);
    CHECK(refresh.toolTip != nil);
    CHECK(back.accessibilityHelp != nil);
    CHECK(forward.accessibilityHelp != nil);
    CHECK(up.accessibilityHelp != nil);
    CHECK(refresh.accessibilityHelp != nil);
    CHECK_FALSE(dispatcher.lastBackAvailability.has_value());
    CHECK_FALSE(dispatcher.lastForwardAvailability.has_value());
    CHECK_FALSE(dispatcher.lastUpAvailability.has_value());
    CHECK_FALSE(dispatcher.lastRefreshAvailability.has_value());
    CHECK([breadcrumb.accessibilityValue isEqual:@"/fixture/"]);
    [back performClick:nil];
    [forward performClick:nil];
    [up performClick:nil];
    [refresh performClick:nil];
    CHECK(dispatcher.backExecutionCount == 1);
    CHECK(dispatcher.forwardExecutionCount == 1);
    CHECK(dispatcher.upExecutionCount == 1);
    CHECK(dispatcher.refreshExecutionCount == 1);
}

[[clang::noinline]] static void CheckExplorerToolbarHistory(NCExplorerToolbarDelegate *const delegate, ExplorerToolbarTestActionsDispatcher *const dispatcher, NSButton *const back, NSButton *const forward, NSButton *const up, NSButton *const refresh, NCExplorerBreadcrumbControl *const breadcrumb)
{
    const VFSHostPtr host = std::make_shared<NativePasteboardTestHost>();
    auto snapshot = ExplorerSnapshot(nc::core::PaneLoadPhase::Loaded, host);
    for( const bool can_go_back : {false, true} ) {
        for( const bool can_go_forward : {false, true} ) {
            snapshot.state.history_availability = {
                .can_go_back = can_go_back,
                .can_go_forward = can_go_forward,
            };
            [delegate applyPaneSnapshot:snapshot];
            CHECK(back.enabled == can_go_back);
            CHECK(forward.enabled == can_go_forward);
            REQUIRE(dispatcher.lastBackAvailability);
            REQUIRE(dispatcher.lastForwardAvailability);
            CHECK(dispatcher.lastBackAvailability->can_go_back == can_go_back);
            CHECK(dispatcher.lastBackAvailability->can_go_forward == can_go_forward);
            CHECK(dispatcher.lastForwardAvailability == dispatcher.lastBackAvailability);
            CHECK(dispatcher.lastSource == nc::core::CommandInvocationSource::Toolbar);
            CHECK(back.toolTip.length > 0);
            CHECK(forward.toolTip.length > 0);
            CHECK(back.accessibilityHelp.length > 0);
            CHECK(forward.accessibilityHelp.length > 0);
        }
    }
    CHECK([breadcrumb.accessibilityValue isEqual:@"/fixture/"]);
    REQUIRE(back.enabled);
    REQUIRE(forward.enabled);
    [back performClick:nil];
    [forward performClick:nil];
    CHECK(dispatcher.backExecutionCount == 1);
    CHECK(dispatcher.forwardExecutionCount == 1);
    CHECK(dispatcher.lastBackExecutionSource == nc::core::CommandInvocationSource::Toolbar);
    CHECK(dispatcher.lastForwardExecutionSource == nc::core::CommandInvocationSource::Toolbar);
    CHECK(dispatcher.lastBackSender == back);
    CHECK(dispatcher.lastForwardSender == forward);

    snapshot.state.load_phase = nc::core::PaneLoadPhase::Loading;
    [delegate applyPaneSnapshot:snapshot];
    CHECK_FALSE(up.enabled);
    CHECK_FALSE(refresh.enabled);
    REQUIRE(dispatcher.lastUpAvailability);
    REQUIRE(dispatcher.lastRefreshAvailability);
    CHECK(*dispatcher.lastUpAvailability == nc::core::NavigationUpAvailability::Busy);
    CHECK(*dispatcher.lastRefreshAvailability == nc::core::NavigationRefreshAvailability::Busy);
    CHECK(up.toolTip != nil);
    CHECK(refresh.toolTip != nil);
    CHECK(up.accessibilityHelp != nil);
    CHECK(refresh.accessibilityHelp != nil);

    CheckExplorerToolbarAvailability(snapshot, host, delegate, dispatcher, back, forward, up, refresh, breadcrumb);
}

TEST_CASE(PREFIX "Explorer toolbar applies only matching history snapshots")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerBreadcrumbTestPanelController *const panel = [ExplorerBreadcrumbTestPanelController new];
    ExplorerToolbarTestActionsDispatcher *const dispatcher = [ExplorerToolbarTestActionsDispatcher new];
    NCExplorerToolbarDelegate *const delegate = [[NCExplorerToolbarDelegate alloc] initWithPanelController:panel
                                                                                         actionsDispatcher:dispatcher];

    NSToolbarItem *const back_item = [delegate toolbar:delegate.toolbar
                                 itemForItemIdentifier:@"explorer_back"
                             willBeInsertedIntoToolbar:true];
    NSToolbarItem *const forward_item = [delegate toolbar:delegate.toolbar
                                    itemForItemIdentifier:@"explorer_forward"
                                willBeInsertedIntoToolbar:true];
    NSToolbarItem *const up_item = [delegate toolbar:delegate.toolbar
                               itemForItemIdentifier:@"explorer_up"
                           willBeInsertedIntoToolbar:true];
    NSToolbarItem *const refresh_item = [delegate toolbar:delegate.toolbar
                                    itemForItemIdentifier:@"explorer_refresh"
                                willBeInsertedIntoToolbar:true];
    NSToolbarItem *const breadcrumb_item = [delegate toolbar:delegate.toolbar
                                       itemForItemIdentifier:@"explorer_breadcrumb"
                                   willBeInsertedIntoToolbar:true];
    NSToolbarItem *const commander_item = [delegate toolbar:delegate.toolbar
                                      itemForItemIdentifier:@"explorer_commander_mode"
                                  willBeInsertedIntoToolbar:true];
    REQUIRE([back_item.view isKindOfClass:NSButton.class]);
    REQUIRE([forward_item.view isKindOfClass:NSButton.class]);
    REQUIRE([up_item.view isKindOfClass:NSButton.class]);
    REQUIRE([refresh_item.view isKindOfClass:NSButton.class]);
    REQUIRE([breadcrumb_item.view isKindOfClass:NCExplorerBreadcrumbControl.class]);
    REQUIRE([commander_item.view isKindOfClass:NSButton.class]);
    NSButton *const back = static_cast<NSButton *>(back_item.view);
    NSButton *const forward = static_cast<NSButton *>(forward_item.view);
    NSButton *const up = static_cast<NSButton *>(up_item.view);
    NSButton *const refresh = static_cast<NSButton *>(refresh_item.view);
    NCExplorerBreadcrumbControl *const breadcrumb = static_cast<NCExplorerBreadcrumbControl *>(breadcrumb_item.view);
    NSButton *const commander = static_cast<NSButton *>(commander_item.view);
    const NSArray<NSButton *> *const buttons = @[back, forward, up, refresh, commander];
    const NSArray<NSString *> *const identifiers = @[
        @"wincommander.explorer.toolbar.back",
        @"wincommander.explorer.toolbar.forward",
        @"wincommander.explorer.toolbar.up",
        @"wincommander.explorer.toolbar.refresh",
        @"wincommander.explorer.toolbar.commanderMode",
    ];
    CheckExplorerToolbarIdentity(back, forward, up, refresh, breadcrumb, buttons, identifiers, dispatcher);
    CheckExplorerToolbarHistory(delegate, dispatcher, back, forward, up, refresh, breadcrumb);
}

TEST_CASE(PREFIX "Explorer toolbar rebind retires old snapshot state before the new pane publishes")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerBreadcrumbTestPanelController *const first = [ExplorerBreadcrumbTestPanelController new];
    ExplorerToolbarTestActionsDispatcher *const first_dispatcher = [ExplorerToolbarTestActionsDispatcher new];
    ExplorerToolbarTestActionsDispatcher *const second_dispatcher = [ExplorerToolbarTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const second =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:second_dispatcher];
    NCExplorerToolbarDelegate *const delegate =
        [[NCExplorerToolbarDelegate alloc] initWithPanelController:first actionsDispatcher:first_dispatcher];

    NSToolbarItem *const back_item = [delegate toolbar:delegate.toolbar
                                 itemForItemIdentifier:@"explorer_back"
                             willBeInsertedIntoToolbar:true];
    NSToolbarItem *const refresh_item = [delegate toolbar:delegate.toolbar
                                    itemForItemIdentifier:@"explorer_refresh"
                                willBeInsertedIntoToolbar:true];
    NSToolbarItem *const breadcrumb_item = [delegate toolbar:delegate.toolbar
                                       itemForItemIdentifier:@"explorer_breadcrumb"
                                   willBeInsertedIntoToolbar:true];
    NSButton *const back = static_cast<NSButton *>(back_item.view);
    NSButton *const refresh = static_cast<NSButton *>(refresh_item.view);
    NCExplorerBreadcrumbControl *const breadcrumb = static_cast<NCExplorerBreadcrumbControl *>(breadcrumb_item.view);

    const VFSHostPtr host = std::make_shared<NativePasteboardTestHost>();
    auto snapshot = ExplorerSnapshot(nc::core::PaneLoadPhase::Loaded, host);
    snapshot.state.history_availability.can_go_back = true;
    snapshot.state.listing = ExplorerPresentationUniformListing(snapshot.state.host, snapshot.state.path);
    [delegate applyPaneSnapshot:snapshot];
    REQUIRE(back.enabled);
    REQUIRE(refresh.enabled);
    REQUIRE([breadcrumb.accessibilityValue isEqual:@"/fixture/"]);

    [delegate rebindToPanelController:second];

    CHECK(delegate.panelController == second);
    CHECK(back.target == second_dispatcher);
    CHECK(refresh.target == second_dispatcher);
    CHECK_FALSE(back.enabled);
    CHECK_FALSE(refresh.enabled);
    CHECK(back.toolTip != nil);
    CHECK(refresh.toolTip != nil);
    CHECK([breadcrumb.accessibilityValue isEqual:@""]);
    CHECK_FALSE([second_dispatcher lastBackAvailability].has_value());
    CHECK_FALSE([second_dispatcher lastRefreshAvailability].has_value());
}

TEST_CASE(PREFIX "command bar uses one native 28 point button metric and aligned content")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerOperationMenuTestActionsDispatcher *const dispatcher = [ExplorerOperationMenuTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const panel =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:dispatcher];
    NCExplorerCommandBarView *const bar = [[NCExplorerCommandBarView alloc] initWithFrame:NSMakeRect(0, 0, 1400, 50)
                                                                          panelController:panel];

    [bar layoutSubtreeIfNeeded];
    REQUIRE(bar.subviews.count == 1);
    REQUIRE([bar.subviews.firstObject isKindOfClass:NSStackView.class]);
    NSStackView *const stack = static_cast<NSStackView *>(bar.subviews.firstObject);
    NSMutableArray<NSButton *> *const buttons = [NSMutableArray new];
    for( NSView *const view in stack.arrangedSubviews ) {
        if( [view isKindOfClass:NSButton.class] )
            [buttons addObject:static_cast<NSButton *>(view)];
    }
    REQUIRE(buttons.count == 10);

    const CGFloat expected_font_size = [NSFont systemFontSizeForControlSize:NSControlSizeLarge];
    const CGFloat expected_mid_y = NSMidY(buttons.firstObject.frame);
    const CGFloat expected_baseline = buttons.firstObject.firstBaselineOffsetFromTop;
    for( NSButton *const button in buttons ) {
        CHECK(button.controlSize == NSControlSizeLarge);
        CHECK(button.bezelStyle == NSBezelStyleAccessoryBarAction);
        CHECK(button.imagePosition == NSImageLeading);
        CHECK(button.imageHugsTitle);
        CHECK(button.image != nil);
        CHECK(button.font.pointSize == expected_font_size);
        CHECK(button.intrinsicContentSize.height == 28.0);
        CHECK(button.frame.size.height == 28.0);
        CHECK(NSMidY(button.frame) == expected_mid_y);
        CHECK(button.firstBaselineOffsetFromTop == expected_baseline);
    }

    REQUIRE(buttons.firstObject.bezelColor != nil);
    CHECK([buttons.firstObject.bezelColor isEqual:NSColor.controlAccentColor]);
    for( NSUInteger index = 1; index < buttons.count; ++index )
        CHECK(buttons[index].bezelColor == nil);
}

TEST_CASE(PREFIX "compact Operations menu fails closed when its services are absent")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerOperationMenuTestActionsDispatcher *const dispatcher = [ExplorerOperationMenuTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const panel =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:dispatcher];
    NCExplorerCommandBarView *const bar = [[NCExplorerCommandBarView alloc] initWithFrame:NSMakeRect(0, 0, 800, 32)
                                                                          panelController:panel];

    NSMenu *const menu = [bar buildMoreMenu];
    NSMenuItem *const section = ExplorerOperationsMenuSection(menu);
    REQUIRE(section);
    NSMenuItem *const unavailable = ExplorerOperationsMenuItemAfter(menu, section);
    REQUIRE(unavailable);
    CHECK_FALSE(unavailable.enabled);
    CHECK_FALSE(unavailable.hidden);
    CHECK(unavailable.toolTip.length > 0);
    CHECK([unavailable.accessibilityHelp isEqual:unavailable.toolTip]);
}

TEST_CASE(PREFIX "Explorer More projects payload Registry state with Toolbar execution")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerFileCommandMenuTestActionsDispatcher *const dispatcher = [ExplorerFileCommandMenuTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const panel =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:dispatcher];
    NCExplorerCommandBarView *const bar = [[NCExplorerCommandBarView alloc] initWithFrame:NSMakeRect(0, 0, 800, 32)
                                                                          panelController:panel];

    dispatcher.commandsEnabled = false;
    NSMenu *const disabled_menu = [bar buildMoreMenu];
    NSMenuItem *const disabled_preview = ExplorerOperationsMenuItemNamed(
        disabled_menu, NSLocalizedString(@"commands.file.preview.title", "Preview command title"));
    NSMenuItem *const disabled_get_info = ExplorerOperationsMenuItemNamed(
        disabled_menu, NSLocalizedString(@"commands.file.getInfo.title", "Get Info command title"));
    NSMenuItem *const disabled_archive = ExplorerOperationsMenuItemNamed(
        disabled_menu, NSLocalizedString(@"Compress", "Explorer command bar - More menu item"));
    NSMenuItem *const disabled_extract = ExplorerOperationsMenuItemNamed(
        disabled_menu, NSLocalizedString(@"commands.archive.extract.title", "Extract archive command title"));
    NSMenuItem *const disabled_duplicate = ExplorerOperationsMenuItemNamed(
        disabled_menu, NSLocalizedString(@"Duplicate", "Explorer command bar - More menu item"));
    NSMenuItem *const disabled_copy_path = ExplorerOperationsMenuItemNamed(
        disabled_menu, NSLocalizedString(@"Copy Path", "Explorer command bar - More menu item"));
    NSMenuItem *const disabled_calculate_sizes = ExplorerOperationsMenuItemNamed(
        disabled_menu, NSLocalizedString(@"Calculate Sizes", "Explorer command bar - More menu item"));
    NSMenuItem *const disabled_batch_rename = ExplorerOperationsMenuItemNamed(
        disabled_menu, NSLocalizedString(@"Batch Rename", "Explorer command bar - More menu item"));
    for( NSMenuItem *const &item : std::array{disabled_preview,
                                              disabled_get_info,
                                              disabled_archive,
                                              disabled_extract,
                                              disabled_duplicate,
                                              disabled_copy_path,
                                              disabled_calculate_sizes,
                                              disabled_batch_rename} ) {
        REQUIRE(item);
        CHECK_FALSE(item.enabled);
        CHECK(item.target == nil);
        CHECK(item.action == nil);
        CHECK(item.toolTip.length > 0);
        CHECK([item.accessibilityHelp isEqual:item.toolTip]);
    }

    dispatcher.commandsEnabled = true;
    NSMenu *const enabled_menu = [bar buildMoreMenu];
    NSMenuItem *const preview = ExplorerOperationsMenuItemNamed(
        enabled_menu, NSLocalizedString(@"commands.file.preview.title", "Preview command title"));
    NSMenuItem *const get_info = ExplorerOperationsMenuItemNamed(
        enabled_menu, NSLocalizedString(@"commands.file.getInfo.title", "Get Info command title"));
    NSMenuItem *const archive = ExplorerOperationsMenuItemNamed(
        enabled_menu, NSLocalizedString(@"Compress", "Explorer command bar - More menu item"));
    NSMenuItem *const extract = ExplorerOperationsMenuItemNamed(
        enabled_menu, NSLocalizedString(@"commands.archive.extract.title", "Extract archive command title"));
    NSMenuItem *const duplicate = ExplorerOperationsMenuItemNamed(
        enabled_menu, NSLocalizedString(@"Duplicate", "Explorer command bar - More menu item"));
    NSMenuItem *const copy_path = ExplorerOperationsMenuItemNamed(
        enabled_menu, NSLocalizedString(@"Copy Path", "Explorer command bar - More menu item"));
    NSMenuItem *const calculate_sizes = ExplorerOperationsMenuItemNamed(
        enabled_menu, NSLocalizedString(@"Calculate Sizes", "Explorer command bar - More menu item"));
    NSMenuItem *const batch_rename = ExplorerOperationsMenuItemNamed(
        enabled_menu, NSLocalizedString(@"Batch Rename", "Explorer command bar - More menu item"));
    for( NSMenuItem *const &item :
         std::array{preview, get_info, archive, extract, duplicate, copy_path, calculate_sizes, batch_rename} ) {
        REQUIRE(item);
        CHECK(item.enabled);
        CHECK(item.target == bar);
        CHECK(item.action == @selector(performMoreMenuAction:));
    }
    CHECK([archive.representedObject isEqual:NSStringFromSelector(@selector(onCompressItemsHere:))]);
    CHECK([extract.representedObject isEqual:NSStringFromSelector(@selector(onExtractArchiveHere:))]);

    [bar performMoreMenuAction:preview];
    CHECK(dispatcher.previewExecutions == 1);
    CHECK(dispatcher.lastSource == nc::core::CommandInvocationSource::Toolbar);
    [bar performMoreMenuAction:get_info];
    CHECK(dispatcher.getInfoExecutions == 1);
    CHECK(dispatcher.lastSource == nc::core::CommandInvocationSource::Toolbar);

    [bar performMoreMenuAction:archive];
    CHECK(dispatcher.archiveCreateExecutions == 1);
    CHECK(dispatcher.lastSource == nc::core::CommandInvocationSource::Toolbar);
    [bar performMoreMenuAction:extract];
    CHECK(dispatcher.archiveExtractExecutions == 1);
    CHECK(dispatcher.lastSource == nc::core::CommandInvocationSource::Toolbar);
    [bar performMoreMenuAction:duplicate];
    CHECK(dispatcher.duplicateExecutions == 1);
    CHECK(dispatcher.lastSource == nc::core::CommandInvocationSource::Toolbar);
    [bar performMoreMenuAction:copy_path];
    CHECK(dispatcher.copyPathExecutions == 1);
    CHECK(dispatcher.lastSource == nc::core::CommandInvocationSource::Toolbar);
    [bar performMoreMenuAction:calculate_sizes];
    CHECK(dispatcher.calculateSizesExecutions == 1);
    CHECK(dispatcher.lastSource == nc::core::CommandInvocationSource::Toolbar);
    [bar performMoreMenuAction:batch_rename];
    CHECK(dispatcher.batchRenameExecutions == 1);
    CHECK(dispatcher.lastSource == nc::core::CommandInvocationSource::Toolbar);
}

TEST_CASE(PREFIX "background context menu projects the pane Registry roster without an item payload")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerFileCommandMenuTestActionsDispatcher *const dispatcher = [ExplorerFileCommandMenuTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const panel =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:dispatcher];
    NSArray<NSString *> *const titles = @[
        NSLocalizedString(@"commands.file.paste.title", "Paste command title"),
        NSLocalizedString(@"commands.file.newFolder.title", "New Folder command title"),
        NSLocalizedString(@"commands.pane.selectAll.title", "Select All command title"),
        NSLocalizedString(@"commands.pane.invertSelection.title", "Invert Selection command title"),
        NSLocalizedString(@"commands.view.toggleHiddenFiles.title", "Show hidden files command title"),
        NSLocalizedString(@"commands.navigation.refresh.title", "Refresh command title")
    ];
    dispatcher.commandsEnabled = false;
    NCPanelContextMenu *const disabled_menu = [[NCPanelContextMenu alloc] initForBackgroundOfPanel:panel];
    REQUIRE(disabled_menu);
    CHECK(disabled_menu.items.empty());
    for( NSString *const title in titles ) {
        NSMenuItem *const item = ExplorerOperationsMenuItemNamed(disabled_menu, title);
        REQUIRE(item);
        CHECK_FALSE(item.enabled);
        CHECK(item.target == nil);
        CHECK(item.action == nil);
        CHECK(item.toolTip.length > 0);
        CHECK([item.accessibilityHelp isEqual:item.toolTip]);
    }
    CHECK(dispatcher.lastRosterStateSource == nc::core::CommandInvocationSource::ContextMenu);

    dispatcher.commandsEnabled = true;
    NCPanelContextMenu *const enabled_menu = [[NCPanelContextMenu alloc] initForBackgroundOfPanel:panel];
    REQUIRE(enabled_menu);
    for( NSString *const title in titles ) {
        NSMenuItem *const item = ExplorerOperationsMenuItemNamed(enabled_menu, title);
        REQUIRE(item);
        CHECK(item.enabled);
        CHECK(item.target == enabled_menu);
        CHECK(item.action != nil);
        const IMP action = [item.target methodForSelector:item.action];
        REQUIRE(action);
        reinterpret_cast<void (*)(id, SEL, id)>(action)(item.target, item.action, item);
    }
    NSMenuItem *const hidden_files = ExplorerOperationsMenuItemNamed(enabled_menu, titles[4]);
    REQUIRE(hidden_files);
    CHECK(hidden_files.state == NSControlStateValueOn);
    CHECK([dispatcher.rosterExecutions isEqualToArray:@[
        @"file.paste",
        @"file.newFolder",
        @"pane.selectAll",
        @"pane.invertSelection",
        @"view.toggleHiddenFiles",
        @"navigation.refresh"
    ]]);
    CHECK(dispatcher.lastSource == nc::core::CommandInvocationSource::ContextMenu);
}

TEST_CASE(PREFIX "Explorer More exposes the background Registry roster with Toolbar execution")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerFileCommandMenuTestActionsDispatcher *const dispatcher = [ExplorerFileCommandMenuTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const panel =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:dispatcher];
    NCExplorerCommandBarView *const bar = [[NCExplorerCommandBarView alloc] initWithFrame:NSMakeRect(0, 0, 800, 32)
                                                                          panelController:panel];
    NSArray<NSString *> *const titles = @[
        NSLocalizedString(@"commands.file.paste.title", "Paste command title"),
        NSLocalizedString(@"commands.file.newFolder.title", "New Folder command title"),
        NSLocalizedString(@"commands.pane.selectAll.title", "Select All command title"),
        NSLocalizedString(@"commands.pane.invertSelection.title", "Invert Selection command title"),
        NSLocalizedString(@"commands.view.toggleHiddenFiles.title", "Show hidden files command title"),
        NSLocalizedString(@"commands.view.togglePreviewPane.title", "Show Details Pane command title"),
        NSLocalizedString(@"commands.navigation.refresh.title", "Refresh command title")
    ];
    NSArray<NSString *> *const selectors = @[
        NSStringFromSelector(@selector(paste:)),
        NSStringFromSelector(@selector(OnQuickNewFolder:)),
        NSStringFromSelector(@selector(selectAll:)),
        NSStringFromSelector(@selector(OnMenuInvertSelection:)),
        NSStringFromSelector(@selector(ToggleViewHiddenFiles:)),
        NSStringFromSelector(@selector(OnTogglePreviewPane:)),
        NSStringFromSelector(@selector(OnRefreshPanel:))
    ];

    dispatcher.commandsEnabled = false;
    NSMenu *const disabled_menu = [bar buildMoreMenu];
    for( NSString *const title in titles ) {
        NSMenuItem *const item = ExplorerOperationsMenuItemNamed(disabled_menu, title);
        REQUIRE(item);
        CHECK_FALSE(item.enabled);
        CHECK(item.target == nil);
        CHECK(item.action == nil);
        CHECK(item.toolTip.length > 0);
        CHECK([item.accessibilityHelp isEqual:item.toolTip]);
    }

    dispatcher.commandsEnabled = true;
    NSMenu *const enabled_menu = [bar buildMoreMenu];
    for( NSUInteger index = 0; index < titles.count; ++index ) {
        NSString *const title = titles[index];
        NSMenuItem *const item = ExplorerOperationsMenuItemNamed(enabled_menu, title);
        REQUIRE(item);
        CHECK(item.enabled);
        CHECK(item.target == bar);
        CHECK(item.action == @selector(performMoreMenuAction:));
        CHECK([item.representedObject isEqual:selectors[index]]);
        [bar performMoreMenuAction:item];
    }
    NSMenuItem *const hidden_files = ExplorerOperationsMenuItemNamed(enabled_menu, titles[4]);
    REQUIRE(hidden_files);
    CHECK(hidden_files.state == NSControlStateValueOn);
    CHECK([dispatcher.rosterExecutions isEqualToArray:@[
        @"file.paste",
        @"file.newFolder",
        @"pane.selectAll",
        @"pane.invertSelection",
        @"view.toggleHiddenFiles",
        @"view.togglePreviewPane",
        @"navigation.refresh"
    ]]);
    CHECK(dispatcher.lastRosterStateSource == nc::core::CommandInvocationSource::Toolbar);
    CHECK(dispatcher.lastSource == nc::core::CommandInvocationSource::Toolbar);
}

TEST_CASE(PREFIX "Explorer View projects and executes the details pane Registry command")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerFileCommandMenuTestActionsDispatcher *const dispatcher = [ExplorerFileCommandMenuTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const panel =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:dispatcher];
    NCExplorerCommandBarView *const bar = [[NCExplorerCommandBarView alloc] initWithFrame:NSMakeRect(0, 0, 800, 32)
                                                                          panelController:panel];
    const auto find_details = [](NCCommandPopover *const _popover) -> NCCommandPopoverItem * {
        const auto items = _popover.commandItems;
        const auto it = std::ranges::find_if(items, [](NCCommandPopoverItem *const _item) {
            return [_item.representedObject isEqual:NSStringFromSelector(@selector(OnTogglePreviewPane:))];
        });
        return it != items.end() ? *it : nil;
    };

    dispatcher.commandsEnabled = false;
    NCCommandPopoverItem *const disabled = find_details([bar buildViewPopover]);
    REQUIRE(disabled);
    CHECK([disabled.title
        isEqual:NSLocalizedString(@"commands.view.togglePreviewPane.title", "Show Details Pane command title")]);
    CHECK(disabled.target == nil);
    CHECK(disabled.action == nil);
    CHECK(disabled.toolTip.length > 0);

    dispatcher.commandsEnabled = true;
    NCCommandPopoverItem *const enabled = find_details([bar buildViewPopover]);
    REQUIRE(enabled);
    CHECK(enabled.target == bar);
    CHECK(enabled.action == @selector(performPopoverAction:));
    CHECK(enabled.image != nil);
    [bar performPopoverAction:enabled];
    CHECK([dispatcher.rosterExecutions isEqualToArray:@[@"view.togglePreviewPane"]]);
    CHECK(dispatcher.lastRosterStateSource == nc::core::CommandInvocationSource::Toolbar);
    CHECK(dispatcher.lastSource == nc::core::CommandInvocationSource::Toolbar);
}

TEST_CASE(PREFIX "New popover projects creation Registry states and keeps Toolbar execution source")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerNewPopoverTestActionsDispatcher *const dispatcher = [ExplorerNewPopoverTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const panel =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:dispatcher];
    NCExplorerCommandBarView *const bar = [[NCExplorerCommandBarView alloc] initWithFrame:NSMakeRect(0, 0, 800, 32)
                                                                          panelController:panel];

    dispatcher.newFolderEnabled = false;
    dispatcher.newFileEnabled = false;
    NCCommandPopover *const disabled_popover = [bar buildNewPopover];
    const auto disabled_items = disabled_popover.commandItems;
    REQUIRE(disabled_items.size() == 2);
    const auto disabled_it = std::ranges::find_if(disabled_items, [](NCCommandPopoverItem *const _item) {
        return [_item.representedObject isEqual:NSStringFromSelector(@selector(OnQuickNewFolder:))];
    });
    REQUIRE(disabled_it != disabled_items.end());
    NCCommandPopoverItem *const disabled = *disabled_it;
    CHECK([disabled.title isEqual:NSLocalizedString(@"commands.file.newFolder.title", "New Folder command title")]);
    CHECK(disabled.target == nil);
    CHECK(disabled.action == nil);
    CHECK(disabled.toolTip.length > 0);
    const auto disabled_file_it = std::ranges::find_if(disabled_items, [](NCCommandPopoverItem *const _item) {
        return [_item.representedObject isEqual:NSStringFromSelector(@selector(OnQuickNewFile:))];
    });
    REQUIRE(disabled_file_it != disabled_items.end());
    CHECK([(*disabled_file_it).title
        isEqual:NSLocalizedString(@"commands.file.newFile.title", "New File command title")]);
    CHECK((*disabled_file_it).target == nil);
    CHECK((*disabled_file_it).action == nil);
    CHECK((*disabled_file_it).toolTip.length > 0);

    dispatcher.newFolderEnabled = true;
    dispatcher.newFileEnabled = true;
    NCCommandPopover *const enabled_popover = [bar buildNewPopover];
    const auto enabled_items = enabled_popover.commandItems;
    const auto enabled_it = std::ranges::find_if(enabled_items, [](NCCommandPopoverItem *const _item) {
        return [_item.representedObject isEqual:NSStringFromSelector(@selector(OnQuickNewFolder:))];
    });
    REQUIRE(enabled_it != enabled_items.end());
    NCCommandPopoverItem *const enabled = *enabled_it;
    CHECK(enabled.target == bar);
    CHECK(enabled.action == @selector(performPopoverAction:));
    CHECK(enabled.toolTip == nil);
    [bar performPopoverAction:enabled];
    CHECK(dispatcher.newFolderExecutionCount == 1);
    CHECK(dispatcher.newFolderExecutionSource == nc::core::CommandInvocationSource::Toolbar);
    CHECK(dispatcher.newFolderSender == enabled);

    const auto enabled_file_it = std::ranges::find_if(enabled_items, [](NCCommandPopoverItem *const _item) {
        return [_item.representedObject isEqual:NSStringFromSelector(@selector(OnQuickNewFile:))];
    });
    REQUIRE(enabled_file_it != enabled_items.end());
    NCCommandPopoverItem *const enabled_file = *enabled_file_it;
    CHECK(enabled_file.target == bar);
    CHECK(enabled_file.action == @selector(performPopoverAction:));
    CHECK(enabled_file.toolTip == nil);
    [bar performPopoverAction:enabled_file];
    CHECK(dispatcher.newFileExecutionCount == 1);
    CHECK(dispatcher.newFileExecutionSource == nc::core::CommandInvocationSource::Toolbar);
    CHECK(dispatcher.newFileSender == enabled_file);
}

TEST_CASE(PREFIX "compact Operations menu omits terminal records and reports no active operations")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerOperationMenuTestDirectory directory;
    const auto coordinator = ExplorerOperationMenuCoordinatorWithTerminalRecord(directory);
    REQUIRE(coordinator);
    const auto records = coordinator->Model().Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records.front().state == nc::ops::OperationRecordState::Completed);

    nc::core::CommandRegistry registry;
    ExplorerOperationMenuTestActionsDispatcher *const dispatcher = [ExplorerOperationMenuTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const panel =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:dispatcher];
    NCExplorerCommandBarView *const bar = [[NCExplorerCommandBarView alloc] initWithFrame:NSMakeRect(0, 0, 800, 32)
                                                                          panelController:panel
                                                               operationCenterCoordinator:coordinator
                                                                          commandRegistry:&registry];

    NSMenu *const menu = [bar buildMoreMenu];
    NSMenuItem *const section = ExplorerOperationsMenuSection(menu);
    REQUIRE(section);
    NSMenuItem *const empty = ExplorerOperationsMenuItemNamed(
        menu, NSLocalizedString(@"explorer.operations.noActive", "Explorer operation menu"));
    REQUIRE(empty);
    CHECK_FALSE(empty.enabled);
    CHECK_FALSE(empty.hidden);
    for( NSMenuItem *const item in menu.itemArray )
        CHECK([item.title rangeOfString:@"op-1"].location == NSNotFound);
}

TEST_CASE(PREFIX "compact Operations menu keeps an unknown cancel command visibly disabled")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerOperationMenuTestDirectory directory;
    const auto coordinator = ExplorerOperationMenuCoordinatorWithQueuedRecord(directory);
    REQUIRE(coordinator);
    const auto records = coordinator->Model().Snapshot();
    REQUIRE(records.size() == 1);
    REQUIRE(records.front().state == nc::ops::OperationRecordState::Queued);
    REQUIRE(records.front().controls.can_cancel);

    nc::core::CommandRegistry registry;
    ExplorerOperationMenuTestActionsDispatcher *const dispatcher = [ExplorerOperationMenuTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const panel =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:dispatcher];
    NCExplorerCommandBarView *const bar = [[NCExplorerCommandBarView alloc] initWithFrame:NSMakeRect(0, 0, 800, 32)
                                                                          panelController:panel
                                                               operationCenterCoordinator:coordinator
                                                                          commandRegistry:&registry];

    NSMenu *const menu = [bar buildMoreMenu];
    NSMenuItem *const record_item = ExplorerOperationsMenuRecordItem(menu, records.front());
    REQUIRE(record_item);
    NSMenuItem *const cancel = ExplorerOperationsMenuItemAfter(menu, record_item);
    REQUIRE(cancel);
    CHECK_FALSE(cancel.enabled);
    CHECK_FALSE(cancel.hidden);
    CHECK(cancel.target == nil);
    CHECK(cancel.action == nil);
    NSString *const key = @"commands.operation.cancel.disabled.controlUnavailable";
    NSString *const localized = [NSBundle.mainBundle localizedStringForKey:key value:nil table:nil];
    NSString *const expected =
        localized.length && ![localized isEqual:key] ? localized : @"This command is currently unavailable";
    CHECK([cancel.toolTip isEqual:expected]);
    CHECK([cancel.accessibilityHelp isEqual:cancel.toolTip]);
}

TEST_CASE(PREFIX "compact Operations menu binds Cancel to an immutable Menu value target")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerOperationMenuTestDirectory directory;
    const auto coordinator = ExplorerOperationMenuCoordinatorWithQueuedRecord(directory);
    REQUIRE(coordinator);
    const auto records = coordinator->Model().Snapshot();
    REQUIRE(records.size() == 1);
    const nc::ops::OperationRecord record = records.front();

    nc::core::CommandRegistry registry;
    REQUIRE(registry.Register(nc::core::MakeOperationCancelCommand([](nc::ops::OperationId, uint64_t) {
        return nc::ops::OperationCenterCancelResult{.code = nc::ops::OperationCenterCancelResultCode::Accepted};
    })) == nc::core::CommandRegistry::RegisterResult::Registered);
    ExplorerOperationMenuTestActionsDispatcher *const dispatcher = [ExplorerOperationMenuTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const panel =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:dispatcher];
    NCExplorerCommandBarView *const bar = [[NCExplorerCommandBarView alloc] initWithFrame:NSMakeRect(0, 0, 800, 32)
                                                                          panelController:panel
                                                               operationCenterCoordinator:coordinator
                                                                          commandRegistry:&registry];

    NSMenu *const menu = [bar buildMoreMenu];
    NSMenuItem *const record_item = ExplorerOperationsMenuRecordItem(menu, record);
    REQUIRE(record_item);
    NSMenuItem *const cancel = ExplorerOperationsMenuItemAfter(menu, record_item);
    REQUIRE(cancel);
    REQUIRE(cancel.enabled);
    REQUIRE_FALSE(cancel.hidden);
    CHECK(cancel.target == bar);
    CHECK(cancel.action == @selector(performOperationCancel:));
    REQUIRE([cancel.representedObject isKindOfClass:NCExplorerOperationCancelMenuTarget.class]);
    NCExplorerOperationCancelMenuTarget *const target =
        static_cast<NCExplorerOperationCancelMenuTarget *>(cancel.representedObject);
    const nc::core::CommandContext &context = target.context;
    REQUIRE(context.operation_cancel_target);
    CHECK(context.source == nc::core::CommandInvocationSource::Menu);
    CHECK(context.operation_cancel_target->operation_id == record.operation_id);
    CHECK(context.operation_cancel_target->expected_revision == record.revision);
    CHECK(context.operation_cancel_target->can_cancel == record.controls.can_cancel);
}

TEST_CASE(PREFIX "Operation Center opens one copied value snapshot with terminal history and Registry Cancel")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerOperationMenuTestDirectory directory;
    auto fixture = ExplorerOperationCenterSnapshotWithTerminalAndQueuedRecords(directory);
    REQUIRE(fixture.coordinator);
    const auto initial_records = fixture.coordinator->Model().Snapshot();
    REQUIRE(initial_records.size() == 2);
    const auto terminal = std::ranges::find_if(initial_records, [](const nc::ops::OperationRecord &record) {
        return record.state == nc::ops::OperationRecordState::Completed;
    });
    const auto queued = std::ranges::find_if(initial_records, [](const nc::ops::OperationRecord &record) {
        return record.state == nc::ops::OperationRecordState::Queued;
    });
    REQUIRE(terminal != initial_records.end());
    REQUIRE(queued != initial_records.end());
    REQUIRE(queued->controls.can_cancel);

    int cancel_calls = 0;
    std::optional<nc::ops::OperationId> cancelled_id;
    uint64_t cancelled_revision = 0;
    nc::core::CommandRegistry registry;
    REQUIRE(registry.Register(nc::core::MakeOperationCancelCommand([&](const nc::ops::OperationId _operation_id,
                                                                       const uint64_t _expected_revision) {
        ++cancel_calls;
        cancelled_id = _operation_id;
        cancelled_revision = _expected_revision;
        return nc::ops::OperationCenterCancelResult{.code = nc::ops::OperationCenterCancelResultCode::Accepted};
    })) == nc::core::CommandRegistry::RegisterResult::Registered);
    auto provider_snapshot = std::make_shared<std::vector<nc::ops::OperationRecord>>(initial_records);
    REQUIRE(registry.Register(nc::core::MakeOperationCenterOpenCommand(
                [provider_snapshot]() -> std::optional<std::vector<nc::ops::OperationRecord>> {
                    return *provider_snapshot;
                },
                [](void *const _native_target, std::vector<nc::ops::OperationRecord> _snapshot) {
                    if( _native_target == nullptr )
                        return false;
                    NCExplorerCommandBarView *const command_bar = (__bridge NCExplorerCommandBarView *)_native_target;
                    return [command_bar presentOperationCenterSnapshot:std::move(_snapshot)];
                })) == nc::core::CommandRegistry::RegisterResult::Registered);

    ExplorerOperationMenuTestActionsDispatcher *const dispatcher = [ExplorerOperationMenuTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const panel =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:dispatcher];
    NCExplorerCommandBarView *const bar = [[NCExplorerCommandBarView alloc] initWithFrame:NSMakeRect(0, 0, 800, 32)
                                                                          panelController:panel
                                                               operationCenterCoordinator:fixture.coordinator
                                                                          commandRegistry:&registry];

    NSMenu *const menu = [bar buildMoreMenu];
    NSMenuItem *const open = ExplorerOperationsMenuItemNamed(
        menu, NSLocalizedString(@"explorer.operations.openCenter", "Explorer operation menu"));
    REQUIRE(open);
    REQUIRE(open.enabled);
    CHECK(open.target == bar);
    CHECK(open.action == @selector(performOperationCenterOpen:));
    [bar performOperationCenterOpen:open];

    NSPanel *const snapshot_panel = [bar valueForKey:@"m_OperationCenterSnapshotPanel"];
    NSTextView *const snapshot_text = [bar valueForKey:@"m_OperationCenterSnapshotText"];
    NSStackView *const snapshot_controls = [bar valueForKey:@"m_OperationCenterSnapshotControls"];
    REQUIRE(snapshot_panel);
    REQUIRE(snapshot_text);
    REQUIRE(snapshot_controls);
    CHECK(snapshot_panel.visible);
    CHECK([snapshot_text.string rangeOfString:ExplorerOperationIdentifier(*terminal)].location != NSNotFound);
    CHECK([snapshot_text.string rangeOfString:ExplorerOperationIdentifier(*queued)].location != NSNotFound);
    CHECK([snapshot_text.string
              rangeOfString:NSLocalizedString(@"explorer.operations.state.queued", "Explorer operation menu")]
              .location != NSNotFound);

    REQUIRE(snapshot_controls.arrangedSubviews.count == 1);
    NSButton *const cancel = static_cast<NSButton *>(snapshot_controls.arrangedSubviews.firstObject);
    REQUIRE([cancel isKindOfClass:NSButton.class]);
    REQUIRE(cancel.enabled);
    CHECK(cancel.target == bar);
    CHECK(cancel.action == @selector(performOperationCenterSnapshotCancel:));
    [bar performOperationCenterSnapshotCancel:cancel];
    CHECK(cancel_calls == 1);
    REQUIRE(cancelled_id);
    CHECK(*cancelled_id == queued->operation_id);
    CHECK(cancelled_revision == queued->revision);
    CHECK_FALSE(cancel.enabled);
    CHECK(cancel.target == nil);
    CHECK(cancel.action == nil);

    const auto mutable_queued = std::ranges::find_if(*provider_snapshot, [&](const nc::ops::OperationRecord &record) {
        return record.operation_id == queued->operation_id;
    });
    REQUIRE(mutable_queued != provider_snapshot->end());
    mutable_queued->state = nc::ops::OperationRecordState::Running;
    CHECK([snapshot_text.string
              rangeOfString:NSLocalizedString(@"explorer.operations.state.queued", "Explorer operation menu")]
              .location != NSNotFound);
    CHECK([snapshot_text.string
              rangeOfString:NSLocalizedString(@"explorer.operations.state.running", "Explorer operation menu")]
              .location == NSNotFound);
    [snapshot_panel orderOut:nil];
}

TEST_CASE(PREFIX "Operation Center retained Cancel control keeps its original ID and revision after reopen")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerOperationMenuTestDirectory directory;
    auto fixture = ExplorerOperationCenterSnapshotWithTerminalAndQueuedRecords(directory);
    auto replacement_staging =
        fixture.coordinator->StageAdmission(*fixture.journal, ExplorerOperationMenuPlan("replacement"));
    REQUIRE(replacement_staging);
    REQUIRE(fixture.coordinator->CommitAdmission(*fixture.journal, std::move(*replacement_staging)));

    const auto records = fixture.coordinator->Model().Snapshot();
    const auto terminal = std::ranges::find_if(
        records, [](const nc::ops::OperationRecord &record) { return record.plan_id.Value() == "terminal"; });
    const auto queued = std::ranges::find_if(
        records, [](const nc::ops::OperationRecord &record) { return record.plan_id.Value() == "queued"; });
    const auto replacement = std::ranges::find_if(
        records, [](const nc::ops::OperationRecord &record) { return record.plan_id.Value() == "replacement"; });
    REQUIRE(terminal != records.end());
    REQUIRE(queued != records.end());
    REQUIRE(replacement != records.end());
    REQUIRE(queued->controls.can_cancel);
    REQUIRE(replacement->controls.can_cancel);

    auto provider_snapshot = std::make_shared<std::vector<nc::ops::OperationRecord>>(
        std::initializer_list<nc::ops::OperationRecord>{*terminal, *queued});
    std::vector<nc::ops::OperationId> cancelled_ids;
    std::vector<uint64_t> cancelled_revisions;
    nc::core::CommandRegistry registry;
    REQUIRE(registry.Register(nc::core::MakeOperationCancelCommand([&](const nc::ops::OperationId _operation_id,
                                                                       const uint64_t _expected_revision) {
        cancelled_ids.emplace_back(_operation_id);
        cancelled_revisions.emplace_back(_expected_revision);
        return nc::ops::OperationCenterCancelResult{.code = nc::ops::OperationCenterCancelResultCode::Accepted};
    })) == nc::core::CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(nc::core::MakeOperationCenterOpenCommand(
                [provider_snapshot]() -> std::optional<std::vector<nc::ops::OperationRecord>> {
                    return *provider_snapshot;
                },
                [](void *const _native_target, std::vector<nc::ops::OperationRecord> _snapshot) {
                    if( _native_target == nullptr )
                        return false;
                    NCExplorerCommandBarView *const command_bar = (__bridge NCExplorerCommandBarView *)_native_target;
                    return [command_bar presentOperationCenterSnapshot:std::move(_snapshot)];
                })) == nc::core::CommandRegistry::RegisterResult::Registered);

    ExplorerOperationMenuTestActionsDispatcher *const dispatcher = [ExplorerOperationMenuTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const panel =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:dispatcher];
    NCExplorerCommandBarView *const bar = [[NCExplorerCommandBarView alloc] initWithFrame:NSMakeRect(0, 0, 800, 32)
                                                                          panelController:panel
                                                               operationCenterCoordinator:fixture.coordinator
                                                                          commandRegistry:&registry];
    [bar performOperationCenterOpen:nil];
    NSStackView *const controls = [bar valueForKey:@"m_OperationCenterSnapshotControls"];
    REQUIRE(controls);
    REQUIRE(controls.arrangedSubviews.count == 1);
    NSButton *const retained_cancel = static_cast<NSButton *>(controls.arrangedSubviews.firstObject);
    REQUIRE(retained_cancel.enabled);

    *provider_snapshot = {*terminal, *replacement};
    [bar performOperationCenterOpen:nil];
    REQUIRE(controls.arrangedSubviews.count == 1);
    NSButton *const current_cancel = static_cast<NSButton *>(controls.arrangedSubviews.firstObject);
    REQUIRE(current_cancel.enabled);
    CHECK(current_cancel != retained_cancel);

    [bar performOperationCenterSnapshotCancel:retained_cancel];
    REQUIRE(cancelled_ids.size() == 1);
    CHECK(cancelled_ids.front() == queued->operation_id);
    REQUIRE(cancelled_revisions.size() == 1);
    CHECK(cancelled_revisions.front() == queued->revision);
    CHECK_FALSE(retained_cancel.enabled);
    CHECK(current_cancel.enabled);

    [bar performOperationCenterSnapshotCancel:current_cancel];
    REQUIRE(cancelled_ids.size() == 2);
    CHECK(cancelled_ids.back() == replacement->operation_id);
    REQUIRE(cancelled_revisions.size() == 2);
    CHECK(cancelled_revisions.back() == replacement->revision);

    NSPanel *const snapshot_panel = [bar valueForKey:@"m_OperationCenterSnapshotPanel"];
    [snapshot_panel orderOut:nil];
}

TEST_CASE(PREFIX "Operation Center open is disabled when its weak coordinator is unavailable")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerOperationMenuTestDirectory directory;
    auto fixture = ExplorerOperationCenterSnapshotWithTerminalAndQueuedRecords(directory);
    std::weak_ptr<nc::ops::OperationCenterCoordinator> weak_coordinator = fixture.coordinator;
    fixture.coordinator.reset();
    REQUIRE(weak_coordinator.expired());

    int snapshot_calls = 0;
    nc::core::CommandRegistry registry;
    REQUIRE(registry.Register(nc::core::MakeOperationCenterOpenCommand(
                [weak_coordinator, &snapshot_calls]() -> std::optional<std::vector<nc::ops::OperationRecord>> {
                    ++snapshot_calls;
                    const auto coordinator = weak_coordinator.lock();
                    if( !coordinator )
                        return std::nullopt;
                    return coordinator->Model().Snapshot();
                },
                [](void *, std::vector<nc::ops::OperationRecord>) { return true; },
                [weak_coordinator] { return !weak_coordinator.expired(); })) ==
            nc::core::CommandRegistry::RegisterResult::Registered);

    ExplorerOperationMenuTestActionsDispatcher *const dispatcher = [ExplorerOperationMenuTestActionsDispatcher new];
    ExplorerOperationMenuTestPanelController *const panel =
        [[ExplorerOperationMenuTestPanelController alloc] initWithActionsDispatcher:dispatcher];
    NCExplorerCommandBarView *const bar = [[NCExplorerCommandBarView alloc] initWithFrame:NSMakeRect(0, 0, 800, 32)
                                                                          panelController:panel
                                                               operationCenterCoordinator:weak_coordinator
                                                                          commandRegistry:&registry];

    NSMenu *const menu = [bar buildMoreMenu];
    NSMenuItem *const open = ExplorerOperationsMenuItemNamed(
        menu, NSLocalizedString(@"explorer.operations.openCenter", "Explorer operation menu"));
    REQUIRE(open);
    CHECK_FALSE(open.enabled);
    CHECK(open.toolTip.length > 0);
    CHECK(open.accessibilityHelp.length > 0);
    CHECK(open.target == nil);
    CHECK(open.action == nil);

    nc::core::CommandContext context;
    context.source = nc::core::CommandInvocationSource::Menu;
    context.native_target = (__bridge void *)bar;
    const auto execution = registry.Execute(nc::core::CommandId{nc::core::command_ids::OperationCenterOpen}, context);
    CHECK(execution.status == nc::core::CommandRegistry::ExecutionStatus::Disabled);
    REQUIRE(execution.disabled_reason);
    CHECK(execution.disabled_reason->code == "operation.snapshotUnavailable");
    CHECK(snapshot_calls == 0);
}

TEST_CASE(PREFIX "Details identity projection keeps model indices unchanged")
{
    PanelListViewProjection projection;
    projection.RebuildIdentity(3);

    CHECK(projection.IsIdentity());
    CHECK(projection.RowsCount() == 3);
    for( int index = 0; index < 3; ++index ) {
        CHECK(projection.SortedIndexForRow(index) == index);
        CHECK(projection.RowForSortedIndex(index) == index);
    }
    CHECK(projection.SortedIndexForRow(-1) == -1);
    CHECK(projection.SortedIndexForRow(3) == -1);
    CHECK(projection.RowForSortedIndex(-1) == -1);
    CHECK(projection.RowForSortedIndex(3) == -1);
}

TEST_CASE(PREFIX "Details grouped projection maps headers around model items")
{
    const PanelListViewGroupKey group_a{PanelListViewGroupKind::NameInitial, "A"};
    const PanelListViewGroupKey group_b{PanelListViewGroupKind::NameInitial, "B"};
    const std::vector<PanelListViewProjectionItem> items = {
        {.sorted_index = 0, .group = {}, .is_dotdot = true},
        {.sorted_index = 1, .group = group_a},
        {.sorted_index = 2, .group = group_a},
        {.sorted_index = 3, .group = group_b},
    };

    PanelListViewProjection projection;
    projection.RebuildGrouped(items);

    CHECK_FALSE(projection.IsIdentity());
    CHECK(projection.RowsCount() == 6);
    CHECK(projection.SortedIndexForRow(0) == 0);
    CHECK(projection.SortedIndexForRow(1) == -1);
    CHECK(projection.SortedIndexForRow(2) == 1);
    CHECK(projection.SortedIndexForRow(3) == 2);
    CHECK(projection.SortedIndexForRow(4) == -1);
    CHECK(projection.SortedIndexForRow(5) == 3);
    CHECK(projection.RowForSortedIndex(0) == 0);
    CHECK(projection.RowForSortedIndex(1) == 2);
    CHECK(projection.RowForSortedIndex(2) == 3);
    CHECK(projection.RowForSortedIndex(3) == 5);

    REQUIRE(projection.RowAt(1));
    CHECK(projection.RowAt(1)->kind == PanelListViewProjectionRow::Kind::GroupHeader);
    REQUIRE(projection.GroupAt(projection.RowAt(1)->group_index));
    CHECK(projection.GroupAt(projection.RowAt(1)->group_index)->header_row == 1);
    CHECK(projection.GroupAt(projection.RowAt(1)->group_index)->item_count == 2);

    REQUIRE(projection.RowAt(4));
    REQUIRE(projection.GroupAt(projection.RowAt(4)->group_index));
    CHECK(projection.GroupAt(projection.RowAt(4)->group_index)->header_row == 4);
    CHECK(projection.GroupAt(projection.RowAt(4)->group_index)->item_count == 1);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
TEST_CASE(PREFIX "Cut pasteboard marker selects move semantics")
{
    NSPasteboard *const pasteboard = [NSPasteboard pasteboardWithUniqueName];
    REQUIRE(pasteboard);

    [pasteboard clearContents];
    CHECK_FALSE(PasteboardSupport::CanReadFileList(pasteboard));
    CHECK(PasteboardSupport::FileOperation(pasteboard) == PasteboardFileOperation::Copy);

    // A marker without this process's nonce/changeCount/path snapshot must never turn Paste into Move.
    [pasteboard declareTypes:@[@"com.wincommander.file-list.move"] owner:nil];
    REQUIRE([pasteboard setData:NSData.data forType:@"com.wincommander.file-list.move"]);
    CHECK(PasteboardSupport::FileOperation(pasteboard) == PasteboardFileOperation::Copy);

    [pasteboard clearContents];
    [pasteboard declareTypes:@[NSFilenamesPboardType] owner:nil];
    REQUIRE([pasteboard setPropertyList:@[@"/tmp/a", @"/tmp/b"] forType:NSFilenamesPboardType]);
    CHECK(PasteboardSupport::CanReadFileList(pasteboard));
    REQUIRE(PasteboardSupport::MarkCurrentFileListForMove(pasteboard));
    CHECK(PasteboardSupport::FileOperation(pasteboard) == PasteboardFileOperation::Move);
    const auto cut_token = PasteboardSupport::CurrentCutToken(pasteboard);
    REQUIRE(cut_token);
    CHECK(PasteboardSupport::IsCutItem(pasteboard, "/tmp/a"));
    CHECK_FALSE(PasteboardSupport::IsCutItem(pasteboard, "/tmp/other"));

    // A Cut can be owned by only one move operation at a time.
    CHECK(PasteboardSupport::TryClaimCut(pasteboard, *cut_token));
    CHECK(PasteboardSupport::IsCutInFlight(pasteboard));
    CHECK_FALSE(PasteboardSupport::TryClaimCut(pasteboard, *cut_token));
    CHECK_FALSE(PasteboardSupport::CancelCut(pasteboard));
    CHECK(PasteboardSupport::ReleaseCut(pasteboard, *cut_token));
    CHECK_FALSE(PasteboardSupport::IsCutInFlight(pasteboard));

    // Cancelling Cut keeps the standard file list, so a later Paste remains a safe Copy.
    CHECK(PasteboardSupport::CancelCut(pasteboard));
    CHECK(PasteboardSupport::CanReadFileList(pasteboard));
    CHECK(PasteboardSupport::FileOperation(pasteboard) == PasteboardFileOperation::Copy);

    REQUIRE(PasteboardSupport::MarkCurrentFileListForMove(pasteboard));

    // Any clipboard replacement invalidates the process-local move intent.
    [pasteboard clearContents];
    [pasteboard declareTypes:@[NSFilenamesPboardType] owner:nil];
    REQUIRE([pasteboard setPropertyList:@[@"/tmp/c"] forType:NSFilenamesPboardType]);
    CHECK(PasteboardSupport::FileOperation(pasteboard) == PasteboardFileOperation::Copy);

    REQUIRE(PasteboardSupport::MarkCurrentFileListForMove(pasteboard));
    const auto replacement_token = PasteboardSupport::CurrentCutToken(pasteboard);
    REQUIRE(replacement_token);
    CHECK_FALSE(PasteboardSupport::ConsumeCut(pasteboard, *cut_token));
    CHECK(PasteboardSupport::TryClaimCut(pasteboard, *replacement_token));
    CHECK(PasteboardSupport::ConsumeCut(pasteboard, *replacement_token));
    CHECK_FALSE(PasteboardSupport::CanReadFileList(pasteboard));
    CHECK(PasteboardSupport::FileOperation(pasteboard) == PasteboardFileOperation::Copy);
    CHECK_FALSE(PasteboardSupport::ConsumeCut(pasteboard, *replacement_token));

    // Explicit Move reserves a normal file-list generation and consumes it exactly once.
    [pasteboard clearContents];
    [pasteboard declareTypes:@[NSFilenamesPboardType] owner:nil];
    REQUIRE([pasteboard setPropertyList:@[@"/tmp/d"] forType:NSFilenamesPboardType]);
    CHECK(PasteboardSupport::CanReadFileList(pasteboard));
    const auto move_token = PasteboardSupport::TryClaimCurrentFileListForMove(pasteboard);
    REQUIRE(move_token);
    CHECK(PasteboardSupport::IsFileListMoveInFlight(pasteboard));
    CHECK(PasteboardSupport::IsFileListMoveClaimCurrent(pasteboard, *move_token));
    CHECK_FALSE(PasteboardSupport::TryClaimCurrentFileListForMove(pasteboard));

    [pasteboard clearContents];
    [pasteboard declareTypes:@[NSFilenamesPboardType] owner:nil];
    REQUIRE([pasteboard setPropertyList:@[@"/tmp/e"] forType:NSFilenamesPboardType]);
    CHECK_FALSE(PasteboardSupport::IsFileListMoveClaimCurrent(pasteboard, *move_token));
    CHECK(PasteboardSupport::ReleaseFileListMove(pasteboard, *move_token));
    CHECK_FALSE(PasteboardSupport::IsFileListMoveInFlight(pasteboard));
    CHECK(PasteboardSupport::CanReadFileList(pasteboard));

    const auto replacement_move_token = PasteboardSupport::TryClaimCurrentFileListForMove(pasteboard);
    REQUIRE(replacement_move_token);
    CHECK(PasteboardSupport::ConsumeFileListMove(pasteboard, *replacement_move_token));
    CHECK_FALSE(PasteboardSupport::CanReadFileList(pasteboard));
    CHECK_FALSE(PasteboardSupport::TryClaimCurrentFileListForMove(pasteboard));
}

TEST_CASE(PREFIX "File list write rejects an unrepresentable path atomically")
{
    NSPasteboard *const pasteboard = [NSPasteboard pasteboardWithUniqueName];
    REQUIRE(pasteboard);

    [pasteboard clearContents];
    [pasteboard declareTypes:@[NSPasteboardTypeString] owner:nil];
    REQUIRE([pasteboard setString:@"existing clipboard payload" forType:NSPasteboardTypeString]);
    const NSInteger original_change_count = pasteboard.changeCount;

    const std::string invalid_utf8_filename{"invalid-\xFF", 9};
    const auto items = NativeItems({"valid", invalid_utf8_filename});
    CHECK_FALSE(PasteboardSupport::WriteFilesnamesPBoard(items, pasteboard));

    CHECK(pasteboard.changeCount == original_change_count);
    CHECK([[pasteboard stringForType:NSPasteboardTypeString] isEqualToString:@"existing clipboard payload"]);
    CHECK_FALSE(PasteboardSupport::CanReadFileList(pasteboard));
}

TEST_CASE(PREFIX "file.cut propagates a pasteboard staging failure")
{
    NSPasteboard *const pasteboard = [NSPasteboard pasteboardWithUniqueName];
    REQUIRE(pasteboard);

    [pasteboard clearContents];
    [pasteboard declareTypes:@[NSPasteboardTypeString] owner:nil];
    REQUIRE([pasteboard setString:@"existing clipboard payload" forType:NSPasteboardTypeString]);
    const NSInteger original_change_count = pasteboard.changeCount;

    const std::string invalid_utf8_filename{"invalid-\xFF", 9};
    const std::vector<VFSListingItem> items = NativeItems({invalid_utf8_filename});
    nc::core::CommandRegistry registry;
    const auto registration = nc::core::MakeFileCutCommand(
        [pasteboard](const std::span<const VFSListingItem> _items, const nc::core::FileCutIntent _intent) {
            const std::vector<VFSListingItem> owned_items{_items.begin(), _items.end()};
            return _intent == nc::core::FileCutIntent::Move &&
                   PasteboardSupport::WriteFilesnamesPBoard(
                       owned_items, pasteboard, nc::panel::PasteboardFileOperation::Move);
        });
    REQUIRE(registry.Register(registration) == nc::core::CommandRegistry::RegisterResult::Registered);

    CHECK_THROWS_AS(
        registry.Execute(nc::core::CommandId{nc::core::command_ids::FileCut}, nc::core::CommandContext{.items = items}),
        nc::core::FileCutWriteError);
    CHECK(pasteboard.changeCount == original_change_count);
    CHECK([[pasteboard stringForType:NSPasteboardTypeString] isEqualToString:@"existing clipboard payload"]);
    CHECK_FALSE(PasteboardSupport::CurrentCutToken(pasteboard));
}

TEST_CASE(PREFIX "failed Cut marker staging invalidates move intent and remains a safe Copy")
{
    NSPasteboard *const pasteboard = [NSPasteboard pasteboardWithUniqueName];
    REQUIRE(pasteboard);

    [pasteboard clearContents];
    [pasteboard declareTypes:@[NSFilenamesPboardType] owner:nil];
    REQUIRE([pasteboard setPropertyList:@[@"/tmp/staged"] forType:NSFilenamesPboardType]);
    REQUIRE(PasteboardSupport::MarkCurrentFileListForMove(pasteboard));
    const auto stale_token = PasteboardSupport::CurrentCutToken(pasteboard);
    REQUIRE(stale_token);

    [pasteboard clearContents];
    [pasteboard declareTypes:@[NSPasteboardTypeString] owner:nil];
    REQUIRE([pasteboard setString:@"replacement" forType:NSPasteboardTypeString]);
    CHECK_FALSE(PasteboardSupport::MarkCurrentFileListForMove(pasteboard));

    CHECK_FALSE(PasteboardSupport::CurrentCutToken(pasteboard));
    CHECK_FALSE(PasteboardSupport::TryClaimCut(pasteboard, *stale_token));
    CHECK_FALSE(PasteboardSupport::IsCutInFlight(pasteboard));
    CHECK(PasteboardSupport::FileOperation(pasteboard) == PasteboardFileOperation::Copy);
    CHECK_FALSE(PasteboardSupport::CanReadFileList(pasteboard));
}

TEST_CASE(PREFIX "breadcrumb keeps address callbacks current and distinguishes admission from fetch failures")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerBreadcrumbTestPanelController *const panel = [ExplorerBreadcrumbTestPanelController new];
    NCExplorerBreadcrumbControl *const control =
        [[NCExplorerBreadcrumbControl alloc] initWithFrame:NSMakeRect(0.0, 0.0, 600.0, 27.0) panelController:panel];
    const VFSHostPtr host = std::make_shared<NativePasteboardTestHost>();
    [control applyPaneSnapshot:ExplorerSnapshot(nc::core::PaneLoadPhase::Loaded, host)];

    [control focusAddressField];
    [control navigateToPath:"/requested/" host:host];
    const auto request = panel.capturedRequest;
    REQUIRE(request);
    REQUIRE(request->LoadingResultCallback);

    auto loading = ExplorerSnapshot(nc::core::PaneLoadPhase::Loading, host);
    loading.revision = 2;
    [control applyPaneSnapshot:loading];
    [control onPathEditorCommit:nil];
    CHECK(panel.capturedRequest == request);
    const nc::Error request_error{nc::Error::POSIX, ENOENT};
    const auto adapted_request_error = nc::core::FileManagerErrorAdapter::FromError(request_error);
    request->LoadingResultCallback(
        std::unexpected(request_error), nc::panel::DirectoryChangeResultSource::Fetch, [] { return true; });
    REQUIRE(RunExplorerPresentationMainLoopUntil([control] { return control.requestErrorMessage.length != 0; }));
    NSString *const adapter_fallback = [NSString stringWithUTF8String:adapted_request_error.user_message.c_str()];
    CHECK(([control.requestErrorMessage isEqualToString:@"The item or location could not be found."] ||
           [control.requestErrorMessage isEqualToString:adapter_fallback]));
    CHECK([control.errorLabel.stringValue isEqualToString:control.requestErrorMessage]);

    [control navigateToPath:"/newer/" host:host];
    CHECK(control.requestErrorMessage == nil);
    const auto busy_request = panel.capturedRequest;
    REQUIRE(busy_request);
    REQUIRE(busy_request->LoadingResultCallback);

    request->LoadingResultCallback(std::unexpected(nc::Error{nc::Error::POSIX, EBUSY}),
                                   nc::panel::DirectoryChangeResultSource::Admission,
                                   [] { return true; });
    CHECK(control.requestErrorMessage == nil);

    busy_request->LoadingResultCallback(std::unexpected(nc::Error{nc::Error::POSIX, EBUSY}),
                                        nc::panel::DirectoryChangeResultSource::Admission,
                                        [] { return true; });
    CHECK([control.requestErrorMessage isEqualToString:@"Another folder request is already in progress."]);

    [control navigateToPath:"/provider-busy/" host:host];
    CHECK(control.requestErrorMessage == nil);
    const auto provider_busy_request = panel.capturedRequest;
    REQUIRE(provider_busy_request);
    REQUIRE(provider_busy_request->LoadingResultCallback);
    const nc::Error provider_busy_error{nc::Error::POSIX, EBUSY};
    const auto adapted_provider_busy_error = nc::core::FileManagerErrorAdapter::FromError(provider_busy_error);
    provider_busy_request->LoadingResultCallback(
        std::unexpected(provider_busy_error), nc::panel::DirectoryChangeResultSource::Fetch, [] { return true; });
    REQUIRE(control.requestErrorMessage.length != 0);
    NSString *const provider_busy_fallback =
        [NSString stringWithUTF8String:adapted_provider_busy_error.user_message.c_str()];
    CHECK(([control.requestErrorMessage isEqualToString:@"The item or resource is currently in use."] ||
           [control.requestErrorMessage isEqualToString:provider_busy_fallback]));
    CHECK_FALSE([control.requestErrorMessage isEqualToString:@"Another folder request is already in progress."]);

    [control navigateToPath:"/unavailable/" host:host];
    CHECK(control.requestErrorMessage == nil);
    const auto unavailable_request = panel.capturedRequest;
    REQUIRE(unavailable_request);
    REQUIRE(unavailable_request->LoadingResultCallback);
    unavailable_request->LoadingResultCallback(std::unexpected(nc::Error{nc::Error::POSIX, ENODEV}),
                                               nc::panel::DirectoryChangeResultSource::Admission,
                                               [] { return true; });
    CHECK([control.requestErrorMessage isEqualToString:@"Folder navigation is unavailable."]);

    [control navigateToPath:"/cancelled/" host:host];
    const auto cancelled_request = panel.capturedRequest;
    REQUIRE(cancelled_request);
    cancelled_request->LoadingResultCallback(std::unexpected(nc::Error{nc::Error::POSIX, ECANCELED}),
                                             nc::panel::DirectoryChangeResultSource::Fetch,
                                             [] { return true; });
    CHECK(control.requestErrorMessage == nil);
    CHECK(control.errorLabel.hidden);

    auto failed = ExplorerSnapshot(nc::core::PaneLoadPhase::Failed, host, ExplorerFailure());
    failed.revision = 3;
    [control applyPaneSnapshot:failed];
    CHECK_FALSE(control.errorLabel.hidden);
    CHECK([control.errorLabel.stringValue isEqualToString:@"The fixture folder could not be opened."]);
    CHECK([control.errorLabel.toolTip isEqualToString:control.errorLabel.stringValue]);
    CHECK([control.errorLabel.accessibilityValue isEqual:control.errorLabel.stringValue]);
    CHECK([control.errorLabel.accessibilityHelp isEqual:control.errorLabel.stringValue]);
    CHECK_FALSE([control.errorLabel.stringValue containsString:@"INTERNAL"]);
    CHECK([control.accessibilityValue isEqual:@"/fixture/"]);
}
#pragma clang diagnostic pop

#undef PREFIX
#define PREFIX "PanelListViewTableHeaderView "

TEST_CASE(PREFIX "exposes an accessibility identifier and label for the column header row")
{
    PanelListViewTableHeaderView *const header = [[PanelListViewTableHeaderView alloc] init];
    CHECK([header.accessibilityIdentifier isEqualToString:@"wincommander.panel.list.header"]);
    CHECK(header.accessibilityLabel.length > 0);
}

#undef PREFIX
