// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <Base/dispatch_cpp.h>
#include <Cocoa/Cocoa.h>
#include <CoreFoundation/CoreFoundation.h>
#include <VFS/VFSListingInput.h>
#include <Utility/ByteCountFormatter.h>
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/FileCutCommand.h>
#include <WinCommander/Core/Commands/OperationCancelCommand.h>
#include <WinCommander/Core/Commands/ToggleHiddenFilesCommand.h>
#include <WinCommander/Core/Errors/FileManagerErrorAdapter.h>
#include <WinCommander/Core/Pane/PaneSnapshot.h>
#include <WinCommander/States/Explorer/NCExplorerBreadcrumbControl.h>
#include <WinCommander/States/Explorer/NCExplorerCommandBarView.h>
#include <WinCommander/States/Explorer/NCExplorerPanePresentationModel.h>
#include <WinCommander/States/Explorer/NCExplorerToolbarDelegate.h>
#include <WinCommander/States/FilePanels/PanelController.h>
#include <WinCommander/States/FilePanels/PanelControllerActionsDispatcher.h>
#include <WinCommander/States/FilePanels/PanelViewFooter.h>
#include <WinCommander/States/FilePanels/Gallery/Layout.h>
#include <WinCommander/States/FilePanels/Helpers/Pasteboard.h>
#include <WinCommander/States/FilePanels/List/PanelListViewGeometry.h>
#include <WinCommander/States/FilePanels/List/PanelListViewProjection.h>
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

- (std::expected<void, nc::Error>)GoToDirWithContext:
    (std::shared_ptr<nc::panel::DirectoryChangeRequest>)_request
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

- (nc::core::CommandState)fileCopyCommandStateFromSource:
    (nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)fileCutCommandStateFromSource:
    (nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (nc::core::CommandState)fileRenameCommandStateFromSource:
    (nc::core::CommandInvocationSource) [[maybe_unused]] _source
{
    return {.enabled = false};
}

- (bool)validateActionBySelector:(SEL) [[maybe_unused]] _selector
{
    return false;
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
- (void)performOperationCancel:(id)_sender;
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
        std::string pattern =
            (std::filesystem::temp_directory_path() / "explorer-operation-menu-ut-XXXXXX").string();
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
         .destination = nc::ops::OperationPlanDestinationInput{
             "native", "/destination", nc::ops::OperationPlanDestinationKind::Directory},
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
    REQUIRE(journal->Finalize(std::move(*running), ExplorerOperationMenuSuccess(), nc::ops::OperationJournalState::Completed));

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

NSMenuItem *ExplorerOperationsMenuRecordItem(NSMenu *_menu, const nc::ops::OperationRecord &_record)
{
    NSString *const operation_id = [NSString stringWithUTF8String:_record.operation_id.ToString().c_str()];
    for( NSMenuItem *const item in _menu.itemArray )
        if( [item.title rangeOfString:operation_id].location != NSNotFound )
            return item;
    return nil;
}

} // namespace

#define PREFIX "Explorer presentation geometry "

TEST_CASE(PREFIX "footer renders only PaneStore snapshot status")
{
    auto footer = [[NCPanelViewFooter alloc] initWithFrame:NSMakeRect(0, 0, 600, 24)
                                                     theme:std::make_unique<ExplorerFooterTheme>()
                                         explorerAppearance:true];
    NSTextField *const items = [footer valueForKey:@"m_ItemsLabel"];
    NSTextField *const selection = [footer valueForKey:@"m_SelectionLabel"];

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

TEST_CASE(PREFIX "Details uses a readable 28 point row")
{
    const PanelListViewGeometry geometry([NSFont systemFontOfSize:13.0], 1, 9);

    CHECK(geometry.LineHeight() == 28);
    CHECK(geometry.IconSize() == 16);
    CHECK(geometry.TextBaseLine() == 8);
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
          PaneNavigationAvailability{NavigationUpAvailability::Available,
                                     NavigationRefreshAvailability::Available});

    snapshot.state.path = "/";
    snapshot.state.listing = ExplorerPresentationUniformListing(host, snapshot.state.path);
    CHECK(presentation.Apply(snapshot));
    REQUIRE(presentation.NavigationAvailability());
    CHECK(*presentation.NavigationAvailability() ==
          PaneNavigationAvailability{NavigationUpAvailability::AtTop,
                                     NavigationRefreshAvailability::Available});

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
          PaneNavigationAvailability{NavigationUpAvailability::Busy,
                                     NavigationRefreshAvailability::Busy});

    snapshot.pane_id = PaneId{92};
    CHECK_FALSE(presentation.Apply(snapshot));
    CHECK_FALSE(presentation.NavigationAvailability());
}

TEST_CASE(PREFIX "Explorer toolbar applies only matching history snapshots")
{
    REQUIRE(nc::dispatch_is_main_queue());
    ExplorerBreadcrumbTestPanelController *const panel = [ExplorerBreadcrumbTestPanelController new];
    ExplorerToolbarTestActionsDispatcher *const dispatcher = [ExplorerToolbarTestActionsDispatcher new];
    NCExplorerToolbarDelegate *const delegate =
        [[NCExplorerToolbarDelegate alloc] initWithPanelController:panel actionsDispatcher:dispatcher];

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
    REQUIRE([back_item.view isKindOfClass:NSButton.class]);
    REQUIRE([forward_item.view isKindOfClass:NSButton.class]);
    REQUIRE([up_item.view isKindOfClass:NSButton.class]);
    REQUIRE([refresh_item.view isKindOfClass:NSButton.class]);
    REQUIRE([breadcrumb_item.view isKindOfClass:NCExplorerBreadcrumbControl.class]);
    NSButton *const back = static_cast<NSButton *>(back_item.view);
    NSButton *const forward = static_cast<NSButton *>(forward_item.view);
    NSButton *const up = static_cast<NSButton *>(up_item.view);
    NSButton *const refresh = static_cast<NSButton *>(refresh_item.view);
    NCExplorerBreadcrumbControl *const breadcrumb =
        static_cast<NCExplorerBreadcrumbControl *>(breadcrumb_item.view);
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
            CHECK((back.toolTip == nil) == can_go_back);
            CHECK((forward.toolTip == nil) == can_go_forward);
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
    CHECK(refresh.toolTip == nil);
    CHECK(refresh.accessibilityHelp == nil);

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
    CHECK(refresh.toolTip == nil);
    CHECK(refresh.accessibilityHelp == nil);

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
    CHECK(up.toolTip == nil);
    CHECK(refresh.toolTip == nil);
    CHECK(up.accessibilityHelp == nil);
    CHECK(refresh.accessibilityHelp == nil);
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
    NSMenuItem *const empty = ExplorerOperationsMenuItemAfter(menu, section);
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
    NSString *const expected = localized.length && ![localized isEqual:key] ? localized
                                                                              : @"This command is currently unavailable";
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
    REQUIRE(registry.Register(nc::core::MakeOperationCancelCommand(
                         [](nc::ops::OperationId, uint64_t) {
                             return nc::ops::OperationCenterCancelResult{
                                 .code = nc::ops::OperationCenterCancelResultCode::Accepted};
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
        registry.Execute(nc::core::CommandId{nc::core::command_ids::FileCut},
                         nc::core::CommandContext{.items = items}),
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
        [[NCExplorerBreadcrumbControl alloc] initWithFrame:NSMakeRect(0.0, 0.0, 600.0, 27.0)
                                               panelController:panel];
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
    request->LoadingResultCallback(std::unexpected(request_error),
                                   nc::panel::DirectoryChangeResultSource::Fetch,
                                   [] { return true; });
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
    const auto adapted_provider_busy_error =
        nc::core::FileManagerErrorAdapter::FromError(provider_busy_error);
    provider_busy_request->LoadingResultCallback(std::unexpected(provider_busy_error),
                                                 nc::panel::DirectoryChangeResultSource::Fetch,
                                                 [] { return true; });
    REQUIRE(control.requestErrorMessage.length != 0);
    NSString *const provider_busy_fallback =
        [NSString stringWithUTF8String:adapted_provider_busy_error.user_message.c_str()];
    CHECK(([control.requestErrorMessage isEqualToString:@"The item or resource is currently in use."] ||
           [control.requestErrorMessage isEqualToString:provider_busy_fallback]));
    CHECK_FALSE([control.requestErrorMessage
        isEqualToString:@"Another folder request is already in progress."]);

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
