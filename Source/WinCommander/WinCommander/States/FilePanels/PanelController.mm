// Copyright (C) 2013-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PanelController.h"
#include <Base/algo.h>
#include <Utility/NSView+Sugar.h>
#include <Utility/NSMenu+Hierarchical.h>
#include "../MainWindowController.h"
#include "PanelPreview.h"
#include "MainWindowFilePanelState.h"
#include "Views/BriefSystemOverview.h"
#include <WinCommander/Core/Alert.h>
#include <WinCommander/Core/ActionsShortcutsManager.h>
#include <WinCommander/Core/Errors/FileManagerErrorAdapter.h>
#include <WinCommander/Core/Pane/PanelControllerLifecycle.h>
#include <WinCommander/Bootstrap/Config.h>
#include "PanelViewLayoutSupport.h"
#include <Panel/PanelDataItemVolatileData.h>
#include "PanelDataOptionsPersistence.h"
#include <Base/CommonPaths.h>
#include <VFS/Native.h>
#include "PanelHistory.h"
#include <Base/SerialQueue.h>
#include <Panel/PanelData.h>
#include "PanelView.h"
#include "DragReceiver.h"
#include "ContextMenu.h"
#include <Panel/PanelDataExternalEntryKey.h>
#include "PanelDataPersistency.h"
#include <WinCommander/Core/VFSInstanceManager.h>
#include "Actions/OpenFile.h"
#include "Actions/GoToFolder.h"
#include "Actions/Enter.h"
#include <Operations/Copying.h>
#include <Panel/CursorBackup.h>
#include <Panel/QuickSearch.h>
#include <Panel/Log.h>
#include "PanelViewHeader.h"
#include <Config/RapidJSON.h>
#include <Utility/ObjCpp.h>
#include <Utility/StringExtras.h>
#include <Utility/PathManip.h>
#include <Base/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>

using namespace nc;
using namespace nc::core;
using namespace nc::panel;
using namespace std::literals;

static constexpr size_t g_MaxSizeCalculationCommitBatches = 40;
static constexpr std::chrono::nanoseconds g_FilesystemHintTriggerDelay = std::chrono::milliseconds{500}; // 0.5s

static const auto g_ConfigShowDotDotEntry = "filePanel.general.showDotDotEntry";
static const auto g_ConfigIgnoreDirectoriesOnMaskSelection = "filePanel.general.ignoreDirectoriesOnSelectionWithMask";
static const auto g_ConfigShowLocalizedFilenames = "filePanel.general.showLocalizedFilenames";
static const auto g_ConfigEnableFinderTags = "filePanel.FinderTags.enable";

namespace {

constexpr std::string_view g_PanelNavigationErrorDomain = "PanelController.Navigation";
constexpr std::string_view g_PanelRefreshErrorDomain = "PanelController.Refresh";
static_assert(std::is_nothrow_move_assignable_v<data::Model>);

struct NavigationFetchOutcome {
    VFSListingPtr listing;
    std::optional<Error> error;
    std::exception_ptr exception;
    bool cancelled = false;
};

struct NavigationWorkerSlot {
    PaneRequestId request_id;
    std::shared_ptr<std::atomic_bool> callback_allowed;
    std::shared_ptr<std::atomic_bool> worker_finished;
    bool uses_loading_queue = true;
};

struct NavigationAdmissionState {
    std::optional<PaneRequestId> correlated_worker;
};

[[nodiscard]] bool IsCancellationError(const Error &_error) noexcept;

[[nodiscard]] NavigationFetchOutcome FetchNavigationRequestDetached(
    const std::shared_ptr<DirectoryChangeRequest> &_request,
    const unsigned long _fetch_flags,
    const std::shared_ptr<std::atomic_bool> &_callback_allowed)
{
    NavigationFetchOutcome outcome;
    try {
        const auto canceller = VFSCancelChecker(
            [_callback_allowed] { return !_callback_allowed->load(std::memory_order_acquire); });
        const std::expected<VFSListingPtr, Error> listing =
            _request->VFS->FetchDirectoryListing(_request->RequestedDirectory, _fetch_flags, canceller);

        if( listing )
            outcome.listing = *listing;
        else
            outcome.error = listing.error();
        outcome.cancelled = !_callback_allowed->load(std::memory_order_acquire) ||
                            (outcome.error && IsCancellationError(*outcome.error));
    } catch( ... ) {
        outcome.exception = std::current_exception();
    }
    return outcome;
}

struct ControllerLoadingWorkFacts {
    bool has_external_work = false;
    std::optional<PaneRequestId> correlated_navigation_worker;
};

struct RefreshWorkRequest {
    VFSListingPtr source_listing;
    unsigned long source_generation = 0;
    bool is_uniform = false;
    VFSHostPtr host;
    VFSHostPtr native_host;
    std::string path;
    unsigned long fetch_flags = 0;
    FileManagerErrorContext error_context;
};

struct RefreshRecoveryTarget {
    VFSHostPtr host;
    std::string path;
};

struct RefreshFetchOutcome {
    VFSListingPtr listing;
    std::optional<Error> error;
    std::exception_ptr exception;
    std::optional<RefreshRecoveryTarget> recovery_target;
    bool cancelled = false;
};

struct RefreshWorkerSlot {
    PaneRequestId request_id;
    std::shared_ptr<std::atomic_bool> cancel_requested;
    std::shared_ptr<std::atomic_bool> worker_finished;
};

struct PendingRefreshWork {
    PaneRequestId request_id;
    RefreshWorkRequest request;
    unsigned long content_generation = 0;
    std::shared_ptr<std::atomic_bool> cancel_requested;
};

struct RefreshAdmissionState {
    std::optional<RefreshWorkRequest> work;
};

struct LifecycleMappedException {
    FileManagerError error;
};

struct WeakPanelControllerRef {
    __weak PanelController *panel = nil;
};

[[nodiscard]] bool IsCancellationError(const Error &_error) noexcept
{
    return _error.Domain() == Error::POSIX && _error.Code() == ECANCELED;
}

[[nodiscard]] bool IsInvalidLocationError(const Error &_error) noexcept
{
    return _error.Domain() == Error::POSIX &&
           (_error.Code() == ENOENT || _error.Code() == ENOTDIR || _error.Code() == ESTALE);
}

[[nodiscard]] FileManagerErrorContext NavigationErrorContext(
    const std::shared_ptr<DirectoryChangeRequest> &_request)
{
    FileManagerErrorContext context;
    if( !_request )
        return context;
    if( !_request->RequestedDirectory.empty() )
        context.affected_items.emplace_back(_request->RequestedDirectory);
    if( _request->VFS )
        context.provider_id = _request->VFS->Tag();
    return context;
}

[[nodiscard]] FileManagerError ApplyHostErrorPresentation(FileManagerError _mapped, const VFSHostPtr &_host)
{
    if( _mapped.category == FileManagerErrorCategory::UnknownError && _host ) {
        switch( _host->ClassifyError(_mapped.original_error) ) {
            case vfs::HostErrorKind::Unavailable:
                _mapped.category = FileManagerErrorCategory::NetworkError;
                _mapped.user_message_key = "errors.network";
                break;
            case vfs::HostErrorKind::TimedOut:
                _mapped.category = FileManagerErrorCategory::TimeoutError;
                _mapped.user_message_key = "errors.timeout";
                break;
            default:
                break;
        }
    }
    return _mapped;
}

[[nodiscard]] FileManagerError MapNavigationError(
    Error _error,
    const std::shared_ptr<DirectoryChangeRequest> &_request = {})
{
    return ApplyHostErrorPresentation(
        FileManagerErrorAdapter::FromError(std::move(_error), NavigationErrorContext(_request)),
        _request ? _request->VFS : nullptr);
}

[[nodiscard]] FileManagerError MapNavigationException(
    std::exception_ptr _exception,
    const std::shared_ptr<DirectoryChangeRequest> &_request = {})
{
    try {
        if( _exception )
            std::rethrow_exception(_exception);
    } catch( const LifecycleMappedException &mapped ) {
        return mapped.error;
    } catch( const ErrorException &error_exception ) {
        return MapNavigationError(error_exception.error(), _request);
    } catch( const std::exception &exception ) {
        Error error{g_PanelNavigationErrorDomain, EIO};
        error.LocalizedFailureReason(file_manager_error_messages::UnknownErrorFallback);
        FileManagerError mapped = MapNavigationError(std::move(error), _request);
        mapped.technical_message = exception.what();
        return mapped;
    } catch( ... ) {
    }

    return MapNavigationError(Error{g_PanelNavigationErrorDomain, EIO}, _request);
}

[[nodiscard]] FileManagerError MapRefreshError(Error _error, const RefreshWorkRequest &_request)
{
    return ApplyHostErrorPresentation(FileManagerErrorAdapter::FromError(std::move(_error), _request.error_context),
                                      _request.host);
}

[[nodiscard]] FileManagerError MapRefreshException(std::exception_ptr _exception,
                                                   const RefreshWorkRequest &_request)
{
    try {
        if( _exception )
            std::rethrow_exception(_exception);
    } catch( const ErrorException &error_exception ) {
        return MapRefreshError(error_exception.error(), _request);
    } catch( const std::exception &exception ) {
        Error error{g_PanelRefreshErrorDomain, EIO};
        error.LocalizedFailureReason(file_manager_error_messages::UnknownErrorFallback);
        FileManagerError mapped = MapRefreshError(std::move(error), _request);
        mapped.technical_message = exception.what();
        return mapped;
    } catch( ... ) {
    }

    return MapRefreshError(Error{g_PanelRefreshErrorDomain, EIO}, _request);
}

[[nodiscard]] std::optional<RefreshRecoveryTarget>
FindRefreshRecoveryTarget(const RefreshWorkRequest &_request,
                          const std::shared_ptr<std::atomic_bool> &_cancel_requested)
{
    if( !_request.is_uniform || !_request.host )
        return std::nullopt;

    std::filesystem::path path = EnsureNoTrailingSlash(_request.path);
    while( true ) {
        if( _cancel_requested->load(std::memory_order_acquire) )
            return std::nullopt;
        if( _request.host->IterateDirectoryListing(path.native(), [](const VFSDirEnt &) { return false; }) )
            return RefreshRecoveryTarget{.host = _request.host, .path = path.native()};
        if( path == "/" )
            break;
        path = path.parent_path();
    }

    if( _cancel_requested->load(std::memory_order_acquire) || !_request.native_host )
        return std::nullopt;
    return RefreshRecoveryTarget{
        .host = _request.native_host,
        .path = nc::base::CommonPaths::Home(),
    };
}

[[nodiscard]] RefreshFetchOutcome
FetchRefreshRequest(const RefreshWorkRequest &_request,
                    const std::shared_ptr<std::atomic_bool> &_cancel_requested)
{
    RefreshFetchOutcome outcome;
    const auto is_cancelled = [&] {
        return _cancel_requested->load(std::memory_order_acquire);
    };

    try {
        if( is_cancelled() ) {
            outcome.cancelled = true;
            return outcome;
        }

        if( _request.is_uniform ) {
            const std::expected<VFSListingPtr, Error> listing =
                _request.host->FetchDirectoryListing(_request.path, _request.fetch_flags, is_cancelled);
            if( listing )
                outcome.listing = *listing;
            else
                outcome.error = listing.error();
        }
        else {
            outcome.listing =
                VFSListing::ProduceUpdatedTemporaryPanelListing(*_request.source_listing, is_cancelled);
        }
        outcome.cancelled = is_cancelled() || (outcome.error && IsCancellationError(*outcome.error));
        if( !outcome.cancelled && outcome.error && IsInvalidLocationError(*outcome.error) )
            outcome.recovery_target = FindRefreshRecoveryTarget(_request, _cancel_requested);
    } catch( const ErrorException &error_exception ) {
        outcome.cancelled = IsCancellationError(error_exception.error());
        if( !outcome.cancelled )
            outcome.error = error_exception.error();
    } catch( ... ) {
        outcome.exception = std::current_exception();
    }
    return outcome;
}

void PresentNavigationException(std::exception_ptr _exception)
{
    try {
        if( _exception )
            std::rethrow_exception(_exception);
    } catch( const std::exception &exception ) {
        ShowExceptionAlert(exception);
    } catch( ... ) {
        ShowExceptionAlert();
    }
}

[[nodiscard]] PaneRequestDescriptor NavigationDescriptor(
    const std::shared_ptr<DirectoryChangeRequest> &_request)
{
    PaneRequestDescriptor descriptor{
        .kind = PaneRequestKind::Navigation,
        .initiated_by_user = _request ? _request->InitiatedByUser : false,
    };
    if( _request && _request->VFS && !_request->RequestedDirectory.empty() ) {
        descriptor.target = PaneRequestLocation{
            .host = _request->VFS,
            .path = _request->RequestedDirectory,
        };
    }
    return descriptor;
}

[[nodiscard]] PaneRequestDescriptor RefreshDescriptor(const bool _initiated_by_user)
{
    return PaneRequestDescriptor{
        .kind = PaneRequestKind::Refresh,
        .initiated_by_user = _initiated_by_user,
    };
}

} // namespace

namespace nc::panel {

ActivityTicket::ActivityTicket() : ticket(0), panel(nil)
{
}

ActivityTicket::ActivityTicket(PanelController *_panel, uint64_t _ticket) : ticket(_ticket), panel(_panel)
{
}

ActivityTicket::ActivityTicket(ActivityTicket &&_rhs) noexcept : ticket(_rhs.ticket), panel(_rhs.panel)
{
    _rhs.panel = nil;
    _rhs.ticket = 0;
}

ActivityTicket::~ActivityTicket()
{
    Reset();
}

ActivityTicket &ActivityTicket::operator=(ActivityTicket &&_rhs) noexcept
{
    Reset();
    panel = _rhs.panel;
    ticket = _rhs.ticket;
    _rhs.panel = nil;
    _rhs.ticket = 0;
    return *this;
}

void ActivityTicket::Reset()
{
    if( ticket )
        if( PanelController *const pc = panel )
            [pc finishExtActivityWithTicket:ticket];
    panel = nil;
    ticket = 0;
}

struct CalculatedSizesBatch {
    std::vector<VFSListingItem> items;
    std::vector<uint64_t> sizes;
};

} // namespace nc::panel

@interface PanelController ()

@property(nonatomic, readonly)
    bool receivesUpdateNotifications; // returns true if underlying vfs will notify controller that content has changed

- (NavigationFetchOutcome)fetchNavigationRequest:(const std::shared_ptr<DirectoryChangeRequest> &)_request
                                      fetchFlags:(unsigned long)_fetch_flags
                               contentGeneration:(unsigned long)_content_generation;
- (void)finishNavigationRequest:(PaneRequestId)_request_id
                         request:(const std::shared_ptr<DirectoryChangeRequest> &)_request
                         outcome:(const NavigationFetchOutcome &)_outcome
               contentGeneration:(unsigned long)_content_generation
               synchronousResult:(std::expected<void, Error> *)_synchronous_result;
- (void)finishRefreshRequest:(PaneRequestId)_request_id
                      request:(const RefreshWorkRequest &)_request
                      outcome:(const RefreshFetchOutcome &)_outcome
            contentGeneration:(unsigned long)_content_generation;
- (void)startRefreshWorker:(const PendingRefreshWork &)_work;
- (void)refreshQueueDidBecomeDry;
- (void)refreshWorkerDidFinish:(PaneRequestId)_request_id
                 finishedToken:(const std::shared_ptr<std::atomic_bool> &)_finished_token;
- (void)cancelBackgroundOperationsForLifecycleReason:(PaneCancellationReason)_reason;
- (void)stopBackgroundQueues;
- (unsigned long)claimContentIntentInvalidatingNavigationAdmission;
- (ControllerLoadingWorkFacts)loadingWorkFactsForLifecycleContext:
    (std::optional<PanelControllerLifecycleProbeContext>)_context;
@end

@implementation PanelController {
    // Main controller's possessions
    data::Model m_Data; // owns
    PanelView *m_View;  // create and owns

    // VFS changes observation
    vfs::HostDirObservationTicket m_UpdatesObservationTicket;

    // VFS listing fetch flags
    unsigned long m_VFSFetchingFlags;

    // background operations' queues
    nc::base::SerialQueue m_DirectorySizeCountingQ;
    std::shared_ptr<nc::base::SerialQueue> m_DirectoryLoadingQ;
    std::shared_ptr<nc::base::SerialQueue> m_DirectoryReLoadingQ;

    NCPanelQuickSearch *m_QuickSearch;
    __weak id<NCPanelQuickSearchPresentation> m_QuickSearchPresentation;

    // navigation support
    History m_History;

    // spinning indicator support
    bool m_IsAnythingWorksInBackground;

    // Tickets to show some external activities on this panel
    uint64_t m_NextActivityTicket;
    std::vector<uint64_t> m_ActivitiesTickets;
    spinlock m_ActivitiesTicketsLock;

    // delayed entry selection support
    struct {
        /**
         * Requested item name to select. Empty filename means that request is invalid.
         */
        std::string filename;

        /**
         * Time after which request is meaningless and should be removed
         */
        std::chrono::nanoseconds request_end;

        /**
         * Called when changed a cursor position
         */
        std::function<void()> done;
    } m_DelayedSelection;

    __weak MainWindowFilePanelState *m_FilePanelState;

    boost::container::static_vector<nc::config::Token, 3> m_ConfigObservers;
    nc::core::VFSInstanceManager *m_VFSInstanceManager;
    nc::panel::DirectoryAccessProvider *m_DirectoryAccessProvider;
    std::shared_ptr<PanelViewLayoutsStorage> m_Layouts;
    int m_ViewLayoutIndex;
    std::shared_ptr<const PanelViewLayout> m_AssignedViewLayout;
    bool m_AssignedViewLayoutUsesConfiguredSlot;
    PanelViewLayoutsStorage::ObservationTicket m_LayoutsObservation;
    ContextMenuProvider m_ContextMenuProvider;
    nc::utility::NativeFSManager *m_NativeFSManager;
    nc::vfs::NativeHost *m_NativeHost;
    nc::config::Config *m_Config;

    nc::core::PaneId m_PaneId;
    std::unique_ptr<nc::core::PanelControllerLifecycle> m_PaneLifecycle;
    std::optional<NavigationWorkerSlot> m_NavigationWorker;
    std::optional<RefreshWorkerSlot> m_RefreshWorker;
    std::optional<PendingRefreshWork> m_PendingRefresh;
    std::shared_ptr<std::atomic_bool> m_NavigationAdmissionCallbackAllowed;
    bool m_AcceptsNavigation;
    unsigned long m_DataGeneration;
    /** Global content-intent epoch captured by every delayed model commit. */
    std::atomic_ulong m_ContentRequestGeneration;
}

@synthesize view = m_View;
@synthesize data = m_Data;
@synthesize paneId = m_PaneId;
@synthesize history = m_History;
@synthesize layoutIndex = m_ViewLayoutIndex;
@synthesize vfsFetchingFlags = m_VFSFetchingFlags;
@synthesize dataGeneration = m_DataGeneration;

- (id<NCPanelQuickSearchPresentation>)quickSearchPresentation
{
    return m_QuickSearchPresentation;
}

- (void)setQuickSearchPresentation:(id<NCPanelQuickSearchPresentation>)_presentation
{
    id<NCPanelQuickSearchPresentation> const old_presentation = m_QuickSearchPresentation;
    if( old_presentation == _presentation )
        return;

    old_presentation.searchRequestChangeCallback = {};
    m_QuickSearchPresentation = _presentation ? _presentation : m_View.headerView;
    m_QuickSearchPresentation.defaultResponder = m_View;

    __weak NCPanelQuickSearch *weak_qs = m_QuickSearch;
    m_QuickSearchPresentation.searchRequestChangeCallback = [weak_qs](NSString *_request) {
        if( NCPanelQuickSearch *const strong_qs = weak_qs )
            strong_qs.searchCriteria = _request;
    };
}

- (instancetype)initWithView:(PanelView *)_panel_view
                      paneId:(const nc::core::PaneId)_pane_id
                     layouts:(std::shared_ptr<nc::panel::PanelViewLayoutsStorage>)_layouts
                      config:(nc::config::Config &)_config
          vfsInstanceManager:(nc::core::VFSInstanceManager &)_vfs_mgr
     directoryAccessProvider:(nc::panel::DirectoryAccessProvider &)_directory_access_provider
         contextMenuProvider:(nc::panel::ContextMenuProvider)_context_menu_provider
             nativeFSManager:(nc::utility::NativeFSManager &)_native_fs_mgr
                  nativeHost:(nc::vfs::NativeHost &)_native_host
{
    assert(_layouts);
    assert(_context_menu_provider);

    self = [super init];
    if( self ) {
        m_PaneId = _pane_id;
        m_PaneLifecycle = std::make_unique<PanelControllerLifecycle>(m_PaneId, [](std::exception_ptr _exception) {
            return MapNavigationException(std::move(_exception));
        });
        m_Layouts = std::move(_layouts);
        m_VFSInstanceManager = &_vfs_mgr;
        m_NativeFSManager = &_native_fs_mgr;
        m_NativeHost = &_native_host;
        m_Config = &_config;
        m_DirectoryAccessProvider = &_directory_access_provider;
        m_ContextMenuProvider = std::move(_context_menu_provider);
        m_History.SetVFSInstanceManager(_vfs_mgr);
        m_VFSFetchingFlags = 0;
        m_NextActivityTicket = 1;
        m_DataGeneration = 0;
        m_ContentRequestGeneration = 0;
        m_AcceptsNavigation = true;
        m_IsAnythingWorksInBackground = false;
        m_DirectoryLoadingQ = std::make_shared<nc::base::SerialQueue>();
        m_DirectoryReLoadingQ = std::make_shared<nc::base::SerialQueue>();
        m_ViewLayoutIndex = m_Layouts->DefaultLayoutIndex();
        m_AssignedViewLayout = m_Layouts->DefaultLayout();
        m_AssignedViewLayoutUsesConfiguredSlot = m_ViewLayoutIndex >= 0;

        const auto weak_panel = std::make_shared<WeakPanelControllerRef>(WeakPanelControllerRef{.panel = self});
        auto on_change = [weak_panel] {
            dispatch_to_main_queue([weak_panel] {
                if( PanelController *const panel = weak_panel->panel )
                    [panel updateSpinningIndicator];
            });
        };
        m_DirectorySizeCountingQ.SetOnChange(on_change);
        m_DirectoryReLoadingQ->SetOnChange(on_change);
        m_DirectoryLoadingQ->SetOnChange(on_change);
        m_DirectoryReLoadingQ->SetOnDry([weak_panel] {
            dispatch_to_main_queue([weak_panel] {
                if( PanelController *const panel = weak_panel->panel )
                    [panel refreshQueueDidBecomeDry];
            });
        });

        m_View = _panel_view;
        m_View.delegate = self;
        m_View.data = &m_Data;
        __weak PanelView *weak_view = m_View;
        m_History.SetNavigationStateChangeCallback([weak_view]() noexcept {
            dispatch_assert_main_queue();
            if( PanelView *const view = weak_view )
                [NSNotificationCenter.defaultCenter
                    postNotificationName:NCPanelViewContextDidChangeNotification
                                  object:view];
        });
        [m_View setPresentationLayout:*m_AssignedViewLayout];

        // wire up config changing notifications
        auto add_co = [&](const char *_path, SEL _sel) {
            m_ConfigObservers.emplace_back(m_Config->Observe(_path, objc_callback_to_main_queue(self, _sel)));
        };
        add_co(g_ConfigShowDotDotEntry, @selector(configVFSFetchFlagsChanged));
        add_co(g_ConfigShowLocalizedFilenames, @selector(configVFSFetchFlagsChanged));
        add_co(g_ConfigEnableFinderTags, @selector(configVFSFetchFlagsChanged));

        m_LayoutsObservation = m_Layouts->ObserveChanges(objc_callback(self, @selector(panelLayoutsChanged)));

        // loading config via simulating it's change
        [self configVFSFetchFlagsChanged];

        m_QuickSearch = [[NCPanelQuickSearch alloc] initWithData:m_Data delegate:self config:*m_Config];
        self.quickSearchPresentation = m_View.headerView;

        [m_View addKeystrokeSink:self];
        [m_View addKeystrokeSink:m_QuickSearch];
    }

    return self;
}

- (nc::panel::PaneLifecycleSubscription)subscribeToPaneLifecycle:
    (nc::panel::PaneLifecycleObserver)_observer
{
    dispatch_assert_main_queue();
    if( !m_PaneLifecycle )
        throw std::logic_error("PanelController lifecycle is unavailable");

    return m_PaneLifecycle->Subscribe(std::move(_observer));
}

- (void)dealloc
{
    dispatch_assert_main_queue();
    m_AcceptsNavigation = false;
    if( m_NavigationAdmissionCallbackAllowed )
        m_NavigationAdmissionCallbackAllowed->store(false, std::memory_order_release);
    if( m_NavigationWorker )
        m_NavigationWorker->callback_allowed->store(false, std::memory_order_release);
    if( m_DirectoryLoadingQ )
        m_DirectoryLoadingQ->Stop();
    if( m_RefreshWorker )
        m_RefreshWorker->cancel_requested->store(true, std::memory_order_release);
    if( m_PendingRefresh )
        m_PendingRefresh->cancel_requested->store(true, std::memory_order_release);
    if( m_PaneLifecycle ) {
        try {
            m_PaneLifecycle->Shutdown();
        } catch( ... ) {
            // The coordinator destructor retains the final no-throw shutdown fallback.
        }
        m_PaneLifecycle.reset();
    }
    m_NavigationWorker.reset();
    m_RefreshWorker.reset();
    m_PendingRefresh.reset();
    m_DirectoryLoadingQ.reset();

    // we need to manually set data to nullptr, since PanelView can be destroyed a bit later due
    // to other strong pointers. in that case view will contain a dangling pointer, which can lead
    // to a crash.
    m_View.data = nullptr;
}

- (void)configVFSFetchFlagsChanged
{
    if( !m_Config->GetBool(g_ConfigShowDotDotEntry) )
        m_VFSFetchingFlags |= VFSFlags::F_NoDotDot;
    else
        m_VFSFetchingFlags &= ~VFSFlags::F_NoDotDot;

    if( m_Config->GetBool(g_ConfigShowLocalizedFilenames) )
        m_VFSFetchingFlags |= VFSFlags::F_LoadDisplayNames;
    else
        m_VFSFetchingFlags &= ~VFSFlags::F_LoadDisplayNames;

    if( m_Config->GetBool(g_ConfigEnableFinderTags) )
        m_VFSFetchingFlags |= VFSFlags::F_LoadTags;
    else
        m_VFSFetchingFlags &= ~VFSFlags::F_LoadTags;

    [self refreshPanel];
}

- (void)setState:(MainWindowFilePanelState *)state
{
    m_FilePanelState = state;
}

- (MainWindowFilePanelState *)state
{
    return m_FilePanelState;
}

- (NSWindow *)window
{
    return self.state.window;
}

- (NCMainWindowController *)mainWindowController
{
    return static_cast<NCMainWindowController *>(self.window.delegate);
}

- (bool)isUniform
{
    return m_Data.Listing().IsUniform();
}

- (bool)receivesUpdateNotifications
{
    return static_cast<bool>(m_UpdatesObservationTicket);
}

- (bool)ignoreDirectoriesOnSelectionByMask
{
    return m_Config->GetBool(g_ConfigIgnoreDirectoriesOnMaskSelection);
}

- (void)copyOptionsFromController:(PanelController *)_pc
{
    if( !_pc )
        return;

    data::OptionsImporter{m_Data}.Import(data::OptionsExporter{_pc.data}.Export());
    [self.view dataUpdated];
    [self.view dataSortingHasChanged];
    self.layoutIndex = _pc.layoutIndex;
}

- (bool)isActive
{
    return m_View.active;
}

- (void)changeSortingModeTo:(data::SortMode)_mode
{
    if( _mode != m_Data.SortMode() ) {
        const auto pers = CursorBackup{m_View.curpos, m_Data};

        m_Data.SetSortMode(_mode);

        m_View.curpos = pers.RestoredCursorPosition();

        [m_View dataSortingHasChanged];
        [m_View dataUpdated];
        [self markRestorableStateAsInvalid];
    }
}

- (void)changeHardFilteringTo:(data::HardFilter)_filter
{
    if( _filter != m_Data.HardFiltering() ) {
        const auto pers = CursorBackup{m_View.curpos, m_Data};

        m_Data.SetHardFiltering(_filter);

        m_View.curpos = pers.RestoredCursorPosition();
        [m_View dataUpdated];
        [self markRestorableStateAsInvalid];
    }
}

- (void)finishRefreshRequest:(const PaneRequestId)_request_id
                      request:(const RefreshWorkRequest &)_request
                      outcome:(const RefreshFetchOutcome &)_outcome
            contentGeneration:(const unsigned long)_content_generation
{
    dispatch_assert_main_queue();

    const auto active = m_PaneLifecycle->Active();
    if( !active || active->request_id != _request_id )
        return;

    bool source_matches = _content_generation == m_ContentRequestGeneration.load(std::memory_order_acquire) &&
                          m_Data.IsLoaded() && m_DataGeneration == _request.source_generation &&
                          m_Data.ListingPtr() == _request.source_listing &&
                          m_Data.Listing().IsUniform() == _request.is_uniform;
    if( source_matches && _request.is_uniform ) {
        source_matches = m_Data.Host() == _request.host &&
                         m_Data.DirectoryPathWithTrailingSlash() == _request.path;
    }
    if( !source_matches ) {
        [[maybe_unused]] const auto result =
            m_PaneLifecycle->Cancel(_request_id, PaneCancellationReason::InternalAbort);
        return;
    }

    if( _outcome.exception ) {
        [[maybe_unused]] const auto result =
            m_PaneLifecycle->Fail(_request_id, MapRefreshException(_outcome.exception, _request));
        return;
    }
    if( _outcome.cancelled ) {
        [[maybe_unused]] const auto result =
            m_PaneLifecycle->Cancel(_request_id, PaneCancellationReason::QueueStopped);
        return;
    }
    if( _outcome.error ) {
        const auto result =
            m_PaneLifecycle->Fail(_request_id, MapRefreshError(*_outcome.error, _request));
        if( result == PaneLifecycleProducer::FinishResult::Published && _outcome.recovery_target &&
            _content_generation == m_ContentRequestGeneration.load(std::memory_order_acquire) &&
            !m_PaneLifecycle->Active() ) {
            auto recovery = std::make_shared<DirectoryChangeRequest>();
            recovery->RequestedDirectory = _outcome.recovery_target->path;
            recovery->VFS = _outcome.recovery_target->host;
            recovery->PerformAsynchronous = true;
            [[maybe_unused]] const auto recovery_submission = [self GoToDirWithContext:std::move(recovery)];
        }
        return;
    }
    if( !_outcome.listing ) {
        const auto exception = std::make_exception_ptr(std::logic_error{"Refresh fetch returned no outcome"});
        [[maybe_unused]] const auto result =
            m_PaneLifecycle->Fail(_request_id, MapRefreshException(exception, _request));
        return;
    }

    bool listing_matches = _outcome.listing->IsUniform() == _request.is_uniform;
    if( listing_matches && _request.is_uniform ) {
        listing_matches = _outcome.listing->Host() == _request.host &&
                          _outcome.listing->Directory() == _request.path;
    }
    if( !listing_matches ) {
        const auto exception =
            std::make_exception_ptr(std::logic_error{"Refresh fetch returned an incompatible listing"});
        [[maybe_unused]] const auto result =
            m_PaneLifecycle->Fail(_request_id, MapRefreshException(exception, _request));
        return;
    }

    std::optional<data::Model> prepared;
    try {
        prepared.emplace(m_Data);
        prepared->ReLoad(_outcome.listing);
    } catch( ... ) {
        [[maybe_unused]] const auto result =
            m_PaneLifecycle->Fail(_request_id, MapRefreshException(std::current_exception(), _request));
        return;
    }

    const auto cursor = CursorBackup{m_View.curpos, m_Data};
    m_DirectorySizeCountingQ.Stop();
    const auto commit_result = m_PaneLifecycle->Commit(
        _request_id,
        PaneLifecycleCommitted{
            .controller_generation = _request.source_generation,
            .listing = _outcome.listing,
        },
        [&] { m_Data = std::move(*prepared); });
    if( commit_result != PaneLifecycleProducer::FinishResult::Published ||
        m_DataGeneration != _request.source_generation ||
        m_Data.ListingPtr() != _outcome.listing )
        return;

    [m_View dataUpdated];
    [m_QuickSearch dataUpdated];
    if( [self checkAgainstRequestedFocusing] ) {
        Log::Trace("Cursor position was changed by requested focusing, skipping RestoredCursorPosition()");
    }
    else {
        m_View.curpos = cursor.RestoredCursorPosition();
    }
    [self onCursorChanged];
    [m_View setNeedsDisplay];
}

- (ControllerLoadingWorkFacts)loadingWorkFactsForLifecycleContext:
    (std::optional<PanelControllerLifecycleProbeContext>)_context
{
    const int loading_queue_length = m_DirectoryLoadingQ->Length();
    const bool loading_queue_occupied = loading_queue_length != 0;
    bool navigation_worker_matches =
        m_NavigationWorker && m_NavigationWorker->uses_loading_queue &&
        !m_NavigationWorker->worker_finished->load(std::memory_order_acquire);
    if( navigation_worker_matches && _context ) {
        navigation_worker_matches =
            (_context->lifecycle_active_request &&
             *_context->lifecycle_active_request == m_NavigationWorker->request_id) ||
            (_context->lifecycle_tail_request &&
             *_context->lifecycle_tail_request == m_NavigationWorker->request_id);
    }

    ControllerLoadingWorkFacts facts;
    if( loading_queue_occupied && navigation_worker_matches )
        facts.correlated_navigation_worker = m_NavigationWorker->request_id;
    const bool lifecycle_owns_loading_queue =
        loading_queue_length == 1 && facts.correlated_navigation_worker.has_value();
    const int reload_queue_length = m_DirectoryReLoadingQ->Length();
    const bool lifecycle_owns_reload_queue =
        reload_queue_length == 1 && m_RefreshWorker && !m_DirectoryReLoadingQ->IsStopped();
    facts.has_external_work =
        (loading_queue_occupied && !lifecycle_owns_loading_queue) ||
        (reload_queue_length != 0 && !lifecycle_owns_reload_queue) ||
        m_DirectoryReLoadingQ->IsStopped();
    return facts;
}

- (void)startRefreshWorker:(const PendingRefreshWork &)_work
{
    dispatch_assert_main_queue();
    if( m_RefreshWorker )
        throw std::logic_error("Refresh worker is already running");

    auto worker_finished = std::make_shared<std::atomic_bool>(false);
    m_RefreshWorker = RefreshWorkerSlot{
        .request_id = _work.request_id,
        .cancel_requested = _work.cancel_requested,
        .worker_finished = worker_finished,
    };

    const auto weak_panel = std::make_shared<WeakPanelControllerRef>(WeakPanelControllerRef{.panel = self});
    const auto reload_queue = m_DirectoryReLoadingQ;
    try {
        reload_queue->Run([request_id = _work.request_id,
                           request = _work.request,
                           content_generation = _work.content_generation,
                           cancel_requested = _work.cancel_requested,
                           worker_finished,
                           weak_panel,
                           reload_queue] {
            (void)reload_queue;
            const RefreshFetchOutcome outcome = FetchRefreshRequest(request, cancel_requested);
            worker_finished->store(true, std::memory_order_release);
            dispatch_to_main_queue([request_id,
                                    request,
                                    outcome,
                                    content_generation,
                                    worker_finished,
                                    weak_panel,
                                    reload_queue] {
                (void)reload_queue;
                PanelController *const panel = weak_panel->panel;
                if( !panel )
                    return;
                [panel finishRefreshRequest:request_id
                                    request:request
                                    outcome:outcome
                          contentGeneration:content_generation];
            });
        });
    } catch( ... ) {
        if( m_RefreshWorker && m_RefreshWorker->request_id == _work.request_id &&
            m_RefreshWorker->worker_finished == worker_finished )
            m_RefreshWorker.reset();
        throw;
    }
}

- (void)refreshQueueDidBecomeDry
{
    dispatch_assert_main_queue();
    if( !m_RefreshWorker || !m_RefreshWorker->worker_finished->load(std::memory_order_acquire) )
        return;
    [self refreshWorkerDidFinish:m_RefreshWorker->request_id finishedToken:m_RefreshWorker->worker_finished];
}

- (void)refreshWorkerDidFinish:(const PaneRequestId)_request_id
                 finishedToken:(const std::shared_ptr<std::atomic_bool> &)_finished_token
{
    dispatch_assert_main_queue();
    if( !m_RefreshWorker || m_RefreshWorker->request_id != _request_id ||
        m_RefreshWorker->worker_finished != _finished_token )
        return;
    assert(m_DirectoryReLoadingQ->Empty());
    if( !m_DirectoryReLoadingQ->Empty() )
        return;
    m_RefreshWorker.reset();

    if( !m_PendingRefresh )
        return;
    PendingRefreshWork pending = std::move(*m_PendingRefresh);
    m_PendingRefresh.reset();
    const auto active = m_PaneLifecycle ? m_PaneLifecycle->Active() : std::nullopt;
    if( pending.cancel_requested->load(std::memory_order_acquire) || !active ||
        active->request_id != pending.request_id ||
        pending.content_generation != m_ContentRequestGeneration.load(std::memory_order_acquire) )
        return;

    try {
        [self startRefreshWorker:pending];
    } catch( ... ) {
        [[maybe_unused]] const auto result = m_PaneLifecycle->Fail(
            pending.request_id, MapRefreshException(std::current_exception(), pending.request));
    }
}

- (bool)refreshPanelDiscardingCaches:(const bool)_force
{
    dispatch_assert_main_queue();
    Log::Debug("refreshPanelDiscardingCaches:{} was called", _force);
    if( !m_PaneLifecycle )
        return false;

    auto admission_state = std::make_shared<RefreshAdmissionState>();
    const auto navigation_admission_to_invalidate = m_NavigationAdmissionCallbackAllowed;
    const auto submission = m_PaneLifecycle->SubmitRefresh(
        RefreshDescriptor(_force),
        [=](const PanelControllerLifecycleProbeContext &_context) {
            admission_state->work.reset();
            RefreshWorkRequest work;
            bool valid = false;
            try {
                valid = m_View != nil && m_Data.IsLoaded() &&
                        m_Data.ListingPtr() != VFSListing::EmptyListing();
                if( valid ) {
                    work.source_listing = m_Data.ListingPtr();
                    work.source_generation = m_DataGeneration;
                    work.is_uniform = m_Data.Listing().IsUniform();
                    work.fetch_flags = m_VFSFetchingFlags | (_force ? VFSFlags::F_ForceRefresh : 0);
                    work.native_host = m_NativeHost->SharedPtr();
                    if( work.is_uniform ) {
                        work.host = m_Data.Host();
                        work.path = m_Data.DirectoryPathWithTrailingSlash();
                        work.error_context.affected_items.emplace_back(work.path);
                        if( const char *const provider = work.host->Tag(); provider != nullptr )
                            work.error_context.provider_id = provider;
                    }
                    admission_state->work.emplace(std::move(work));
                }
            } catch( ... ) {
                throw LifecycleMappedException{MapRefreshException(std::current_exception(), work)};
            }

            const auto work_facts = [self loadingWorkFactsForLifecycleContext:_context];
            return PanelControllerLifecycleAdmission{
                .valid = valid,
                .available = m_AcceptsNavigation,
                .has_external_loading_work = work_facts.has_external_work,
            };
        },
        [=](const PaneRequestId _request_id) {
            RefreshWorkRequest work;
            try {
                if( !admission_state->work )
                    throw std::logic_error("Accepted refresh has no admission snapshot");
                work = *admission_state->work;
                if( m_RefreshWorker )
                    m_RefreshWorker->cancel_requested->store(true, std::memory_order_release);
                if( m_PendingRefresh ) {
                    m_PendingRefresh->cancel_requested->store(true, std::memory_order_release);
                    m_PendingRefresh.reset();
                }
                if( navigation_admission_to_invalidate &&
                    m_NavigationAdmissionCallbackAllowed == navigation_admission_to_invalidate ) {
                    navigation_admission_to_invalidate->store(false, std::memory_order_release);
                    m_NavigationAdmissionCallbackAllowed.reset();
                }
                if( m_NavigationWorker )
                    m_NavigationWorker->callback_allowed->store(false, std::memory_order_release);
                const unsigned long content_generation =
                    m_ContentRequestGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
                auto cancel_requested = std::make_shared<std::atomic_bool>(false);
                PendingRefreshWork pending{
                    .request_id = _request_id,
                    .request = work,
                    .content_generation = content_generation,
                    .cancel_requested = std::move(cancel_requested),
                };
                if( m_RefreshWorker )
                    m_PendingRefresh.emplace(std::move(pending));
                else
                    [self startRefreshWorker:pending];
            } catch( ... ) {
                if( m_PendingRefresh && m_PendingRefresh->request_id == _request_id ) {
                    m_PendingRefresh->cancel_requested->store(true, std::memory_order_release);
                    m_PendingRefresh.reset();
                }
                throw LifecycleMappedException{MapRefreshException(std::current_exception(), work)};
            }
        });

    if( submission.status == PanelControllerLifecycleSubmissionStatus::Rejected ) {
        Log::Debug("Refresh request was rejected with reason {}",
                   static_cast<int>(submission.rejection_reason.value_or(PaneRejectionReason::Unavailable)));
    }
    return submission.status == PanelControllerLifecycleSubmissionStatus::Accepted ||
           submission.status == PanelControllerLifecycleSubmissionStatus::Deferred;
}

- (void)refreshPanel
{
    Log::Trace("[Panel refreshPanel] was called");
    [self refreshPanelDiscardingCaches:false];
}

- (void)forceRefreshPanel
{
    Log::Trace("[Panel forceRefreshPanel] was called");
    [self refreshPanelDiscardingCaches:true];
}

- (bool)submitUserRefresh
{
    Log::Trace("[Panel submitUserRefresh] was called");
    return [self refreshPanelDiscardingCaches:true];
}

- (std::optional<PaneNavigationAvailability>)paneNavigationAvailability
{
    dispatch_assert_main_queue();
    if( !m_PaneLifecycle || !m_AcceptsNavigation || m_View == nil )
        return PaneNavigationAvailability{};

    if( [self loadingWorkFactsForLifecycleContext:std::nullopt].has_external_work ) {
        return PaneNavigationAvailability{
            .up = NavigationUpAvailability::Busy,
            .refresh = NavigationRefreshAvailability::Busy,
        };
    }

    PaneState state;
    if( const auto active = m_PaneLifecycle->Active();
        active && active->descriptor.kind == PaneRequestKind::Navigation ) {
        state.load_phase = PaneLoadPhase::Loading;
        return MapPaneNavigationAvailability(state);
    }

    const bool has_listing = m_Data.IsLoaded() && m_Data.ListingPtr() != VFSListing::EmptyListing();
    state.load_phase = has_listing ? PaneLoadPhase::Loaded : PaneLoadPhase::Empty;
    if( const auto active = m_PaneLifecycle->Active();
        active && active->descriptor.kind == PaneRequestKind::Refresh )
        state.load_phase = PaneLoadPhase::Refreshing;
    if( has_listing ) {
        state.listing = m_Data.ListingPtr();
        state.is_uniform = m_Data.Listing().IsUniform();
        if( state.is_uniform ) {
            state.host = m_Data.Host();
            state.path = m_Data.DirectoryPathWithTrailingSlash();
        }
    }
    return MapPaneNavigationAvailability(state);
}

- (int)bidForHandlingKeyDown:(NSEvent *)_event forPanelView:(PanelView *) [[maybe_unused]] _panel_view
{
    // this is doubtful, actually. need to figure out something clearer:
    [self clearFocusingRequest]; // on any key press we clear entry selection request, if any

    const auto keycode = _event.keyCode;
    if( keycode == 53 ) { // Esc button
        const auto active = m_PaneLifecycle ? m_PaneLifecycle->Active() : std::nullopt;
        if( m_IsAnythingWorksInBackground || active )
            return panel::view::BiddingPriority::Default;
        if( self.quickLook || self.briefSystemOverview )
            return panel::view::BiddingPriority::Default;
        ;
    }

    return panel::view::BiddingPriority::Skip;
}

- (void)handleKeyDown:(NSEvent *)_event forPanelView:(PanelView *) [[maybe_unused]] _panel_view
{
    const auto keycode = _event.keyCode;
    if( keycode == 53 ) { // Esc button
        const auto active = m_PaneLifecycle ? m_PaneLifecycle->Active() : std::nullopt;
        if( m_IsAnythingWorksInBackground || active ) {
            [self cancelBackgroundOperationsForLifecycleReason:PaneCancellationReason::User];
            return;
        }
        if( self.quickLook || self.briefSystemOverview ) {
            [self.state closeAttachedUI:self];
            return;
        }
    }
}

- (void)calculateSizesOfItems:(const std::vector<VFSListingItem> &)_items
{
    if( !_items.empty() ) {
        m_DirectorySizeCountingQ.Run([=] { [self doCalculateSizesOfItems:_items]; });
    }
}

- (void)doCalculateSizesOfItems:(const std::vector<VFSListingItem> &)_items
{
    dispatch_assert_background_queue();
    assert(!_items.empty());

    // divide all items into maximum of g_MaxSizeCalculationCommitBatches batches as equally as
    // possible
    const size_t items_count = _items.size();
    const size_t batches = std::min(g_MaxSizeCalculationCommitBatches, items_count);
    const size_t items_per_batch = items_count / batches;
    const size_t items_leftover = items_count - (items_per_batch * batches);

    for( size_t batch = 0, items_first = 0, items_last = 0; batch != batches; ++batch ) {
        items_first = items_last;
        items_last += items_per_batch + (batch < items_leftover ? 1 : 0);

        panel::CalculatedSizesBatch calculated;
        calculated.items.reserve(items_last - items_first);
        calculated.sizes.reserve(items_last - items_first);

        for( size_t item_index = items_first; item_index != items_last; ++item_index ) {
            if( m_DirectorySizeCountingQ.IsStopped() )
                return;

            auto &i = _items[item_index];
            if( !i.IsDir() )
                continue;

            const std::expected<uint64_t, Error> result = i.Host()->CalculateDirectorySize(
                !i.IsDotDot() ? i.Path() : i.Directory(), [=] { return m_DirectorySizeCountingQ.IsStopped(); });

            if( !result )
                continue; // silently skip items that caused erros while calculating size

            calculated.items.emplace_back(i);
            calculated.sizes.emplace_back(*result);
        }

        if( calculated.items.empty() )
            continue;

        auto commit_batch = [=, calculated = std::move(calculated)] {
            assert(!calculated.items.empty());

            // may cause re-sorting if current sorting is by size so save the cursor
            const auto pers = CursorBackup{m_View.curpos, m_Data};

            size_t num_set = 0;
            if( &m_Data.Listing() == calculated.items.front().Listing().get() ) {
                // the listing is the same, can use indices directly
                std::vector<unsigned> raw_indices(calculated.items.size());
                std::ranges::transform(calculated.items, raw_indices.begin(), [](auto &i) { return i.Index(); });
                num_set = m_Data.SetCalculatedSizesForDirectories(raw_indices, calculated.sizes);
            }
            else {
                // the listing has changed, need to use indirects: filename and directory
                std::vector<std::string_view> filenames(calculated.items.size());
                std::vector<std::string_view> directories(calculated.items.size());
                std::ranges::transform(
                    calculated.items, filenames.begin(), [](auto &i) { return std::string_view{i.Filename()}; });
                std::ranges::transform(
                    calculated.items, directories.begin(), [](auto &i) { return std::string_view{i.Directory()}; });
                num_set = m_Data.SetCalculatedSizesForDirectories(filenames, directories, calculated.sizes);
            }
            if( num_set != 0 ) {
                [m_View dataUpdated];
                [m_View volatileDataChanged];
                m_View.curpos = pers.RestoredCursorPosition();
            }
        };
        dispatch_to_main_queue(std::move(commit_batch));
    }
}

- (void)stopBackgroundQueues
{
    m_DirectorySizeCountingQ.Stop();
    m_DirectoryLoadingQ->Stop();
    m_DirectoryReLoadingQ->Stop();
}

- (unsigned long)claimContentIntentInvalidatingNavigationAdmission
{
    dispatch_assert_main_queue();
    if( m_NavigationAdmissionCallbackAllowed ) {
        m_NavigationAdmissionCallbackAllowed->store(false, std::memory_order_release);
        m_NavigationAdmissionCallbackAllowed.reset();
    }
    if( m_NavigationWorker )
        m_NavigationWorker->callback_allowed->store(false, std::memory_order_release);
    if( m_RefreshWorker )
        m_RefreshWorker->cancel_requested->store(true, std::memory_order_release);
    if( m_PendingRefresh ) {
        m_PendingRefresh->cancel_requested->store(true, std::memory_order_release);
        m_PendingRefresh.reset();
    }
    return m_ContentRequestGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
}

- (void)cancelBackgroundOperationsForLifecycleReason:(const PaneCancellationReason)_reason
{
    dispatch_assert_main_queue();

    [self claimContentIntentInvalidatingNavigationAdmission];
    const bool accepted_navigation_before_cancel = m_AcceptsNavigation;
    m_AcceptsNavigation = false;
    if( m_PaneLifecycle ) {
        const auto active = m_PaneLifecycle->Active();
        if( active ) {
            const auto request_id = active->request_id;
            if( active->descriptor.kind == PaneRequestKind::Navigation && m_NavigationWorker &&
                m_NavigationWorker->request_id == request_id )
                m_NavigationWorker->callback_allowed->store(false, std::memory_order_release);
            [[maybe_unused]] const auto result = m_PaneLifecycle->Cancel(request_id, _reason);
            if( active->descriptor.kind == PaneRequestKind::Navigation && m_NavigationWorker &&
                m_NavigationWorker->request_id == request_id )
                m_NavigationWorker.reset();
        }
    }
    [self stopBackgroundQueues];
    m_AcceptsNavigation = accepted_navigation_before_cancel;
}

- (void)CancelBackgroundOperations
{
    [self cancelBackgroundOperationsForLifecycleReason:PaneCancellationReason::InternalAbort];
}

- (void)updateSpinningIndicator
{
    dispatch_assert_main_queue();

    size_t ext_activities_no = call_locked(m_ActivitiesTicketsLock, [&] { return m_ActivitiesTickets.size(); });
    bool is_anything_working = !m_DirectorySizeCountingQ.Empty() || !m_DirectoryLoadingQ->Empty() ||
                               !m_DirectoryReLoadingQ->Empty() || ext_activities_no > 0;

    if( is_anything_working == m_IsAnythingWorksInBackground )
        return; // nothing to update;

    if( is_anything_working ) {
        // there should be 100ms of workload before the user gets the spinning indicator
        dispatch_to_main_queue_after(100ms, [=] {
            // need to check if task was already done
            if( m_IsAnythingWorksInBackground )
                [m_View.busyIndicator startAnimation:nil];
        });
    }
    else
        [m_View.busyIndicator stopAnimation:nil];

    m_IsAnythingWorksInBackground = is_anything_working;
}

- (void)selectEntriesWithFilenames:(const std::vector<std::string> &)_filenames
{
    for( auto &i : _filenames )
        m_Data.CustomFlagsSelectSorted(m_Data.SortedIndexForName(i), true);
    [m_View volatileDataChanged];
}

- (void)setEntriesSelection:(const std::vector<bool> &)_selection
{
    if( m_Data.CustomFlagsSelectSorted(_selection) )
        [m_View volatileDataChanged];
}

- (void)setSelectionForItemAtIndex:(int)_index selected:(bool)_selected
{
    if( m_Data.VolatileDataAtSortPosition(_index).is_selected() == _selected )
        return;
    m_Data.CustomFlagsSelectSorted(_index, _selected);
    [m_View volatileDataChanged];
}

- (void)onPathChanged
{
    Log::Trace("[PanelController onPathChanged] was called");
    // update directory changes notification ticket
    __weak PanelController *weakself = self;
    m_UpdatesObservationTicket.reset();
    if( self.isUniform ) {
        const std::string current_directory_path = self.currentDirectoryPath;
        auto dir_change_callback = [=] {
            dispatch_to_main_queue([=] {
                Log::Debug("Got a notification about a directory change: '{}'", current_directory_path);
                if( PanelController *const pc = weakself ) {
                    if( pc.currentDirectoryPath == current_directory_path ) {
                        [pc refreshPanel];
                    }
                    else {
                        Log::Debug("Discarded a stale directory change notification");
                    }
                }
            });
        };
        m_UpdatesObservationTicket =
            self.vfs->ObserveDirectoryChanges(current_directory_path, std::move(dir_change_callback));
    }

    [self clearFocusingRequest];
    [m_QuickSearch setSearchCriteria:nil];

    [self.state PanelPathChanged:self];
    [self onCursorChanged];
    [self updateAttachedBriefSystemOverview];
    m_History.Put(m_Data.Listing());

    [self markRestorableStateAsInvalid];
}

- (void)markRestorableStateAsInvalid
{
    if( auto wc = objc_cast<NCMainWindowController>(self.state.window.delegate) )
        [wc invalidateRestorableState];
}

- (void)onCursorChanged
{
    [self updateAttachedQuickLook];
}

- (void)updateAttachedQuickLook
{
    if( auto ql = self.quickLook )
        if( auto i = self.view.item )
            [ql previewVFSItem:vfs::VFSPath{i.Host(), i.Path()} forPanel:self];
}

- (void)updateAttachedBriefSystemOverview
{
    if( const auto bso = self.briefSystemOverview ) {
        if( auto i = self.view.item )
            [bso UpdateVFSTarget:i.Directory() host:i.Host()];
        else if( self.isUniform )
            [bso UpdateVFSTarget:self.currentDirectoryPath host:self.vfs];
    }
}

- (void)panelViewCursorChanged:(PanelView *) [[maybe_unused]] _view
{
    [self onCursorChanged];
}

- (NCPanelContextMenu *)panelView:(PanelView *)_view requestsContextMenuForItemNo:(int)_sort_pos
{
    dispatch_assert_main_queue();

    const auto clicked_item = m_Data.EntryAtSortPosition(_sort_pos);
    if( !clicked_item || clicked_item.IsDotDot() )
        return nil;

    const auto clicked_item_vd = m_Data.VolatileDataAtSortPosition(_sort_pos);

    std::vector<VFSListingItem> vfs_items;
    if( !clicked_item_vd.is_selected() )
        vfs_items.emplace_back(clicked_item); // only clicked item
    else
        vfs_items = m_Data.SelectedEntriesSorted(); // all selected items

    for( auto &i : vfs_items )
        m_Data.VolatileDataAtRawPosition(i.Index()).toggle_highlight(true);
    [_view volatileDataChanged];

    NCPanelContextMenu *const menu = m_ContextMenuProvider(std::move(vfs_items), self);
    return menu;
}

- (void)contextMenuDidClose:(NSMenu *) [[maybe_unused]] _menu
{
    m_Data.CustomFlagsClearHighlights();
    [m_View volatileDataChanged];
}

static void ShowAlertAboutInvalidFilename(const std::string &_filename)
{
    Alert *const a = [[Alert alloc] init];
    auto fn = [NSString stringWithUTF8StdString:_filename];
    if( fn.length > 256 )
        fn = [[fn substringToIndex:256] stringByAppendingString:@"..."];

    const auto msg =
        NSLocalizedString(@"The name “%@” can’t be used.", "Message text when user is entering an invalid filename");
    a.messageText = [NSString stringWithFormat:msg, fn];
    const auto info = NSLocalizedString(@"Try using a name with fewer characters or without punctuation marks.",
                                        "Informative text when user is entering an invalid filename");
    a.informativeText = info;
    a.alertStyle = NSAlertStyleCritical;
    [a runModal];
}

- (void)requestQuickRenamingOfItem:(VFSListingItem)_item to:(const std::string &)_filename
{
    if( _filename == "." || _filename == ".." || !_item || _item.IsDotDot() || !_item.Host()->IsWritable() ||
        _filename == _item.Filename() )
        return;

    const auto &target_fn = _filename;

    // checking for invalid symbols
    if( !_item.Host()->ValidateFilename(target_fn) ) {
        ShowAlertAboutInvalidFilename(target_fn);
        return;
    }

    nc::ops::CopyingOptions opts;
    opts.docopy = false;

    const auto op = std::make_shared<nc::ops::Copying>(
        std::vector<VFSListingItem>{_item}, _item.Directory() + target_fn, _item.Host(), opts);

    if( self.isUniform && m_View.item && m_View.item.Filename() == _item.Filename() ) {
        std::string curr_path = self.currentDirectoryPath;
        auto curr_vfs = self.vfs;
        op->ObserveUnticketed(nc::ops::Operation::NotifyAboutCompletion, [=] {
            if( self.currentDirectoryPath == curr_path && self.vfs == curr_vfs )
                dispatch_to_main_queue([=] {
                    DelayedFocusing req;
                    req.filename = target_fn;
                    [self scheduleDelayedFocusing:req];
                    [self refreshPanel];
                });
        });
    }

    [self.mainWindowController enqueueOperation:op];
}

- (void)panelViewDidBecomeFirstResponder
{
    [self.state activePanelChangedTo:self];
    [self updateAttachedQuickLook];
    [self updateAttachedBriefSystemOverview];
}

- (void)changeDataOptions:(const std::function<void(nc::panel::data::Model &_data)> &)_workload
{
    assert(dispatch_is_main_queue());
    assert(_workload);

    const auto pers = CursorBackup{m_View.curpos, m_Data};

    _workload(m_Data);

    [m_View dataUpdated];
    [m_View dataSortingHasChanged];
    m_View.curpos = pers.RestoredCursorPosition();
}

- (ActivityTicket)registerExtActivity
{
    auto ticket = call_locked(m_ActivitiesTicketsLock, [&] {
        m_ActivitiesTickets.emplace_back(m_NextActivityTicket);
        return ActivityTicket(self, m_NextActivityTicket++);
    });
    dispatch_to_main_queue([=] { [self updateSpinningIndicator]; });
    return ticket;
}

- (void)finishExtActivityWithTicket:(uint64_t)_ticket
{
    {
        auto lock = std::lock_guard{m_ActivitiesTicketsLock};
        auto i = std::ranges::find(m_ActivitiesTickets, _ticket);
        if( i == end(m_ActivitiesTickets) )
            return;
        m_ActivitiesTickets.erase(i);
    }
    dispatch_to_main_queue([=] { [self updateSpinningIndicator]; });
}

- (void)setLayoutIndex:(int)layoutIndex
{
    if( m_ViewLayoutIndex != layoutIndex ) {
        if( auto l = m_Layouts->GetLayout(layoutIndex) )
            if( !l->is_disabled() ) {
                m_ViewLayoutIndex = layoutIndex;
                m_AssignedViewLayout = l;
                m_AssignedViewLayoutUsesConfiguredSlot = true;
                [m_View setPresentationLayout:*l];
                [self markRestorableStateAsInvalid];
                [NSNotificationCenter.defaultCenter
                    postNotificationName:NCPanelViewContextDidChangeNotification
                                  object:m_View];
            }
    }
}

- (void)panelLayoutsChanged
{
    const auto configured_layout = m_Layouts->GetLayout(m_ViewLayoutIndex);
    const bool uses_configured_slot = configured_layout && !configured_layout->is_disabled();
    const auto next_layout = uses_configured_slot
                                 ? configured_layout
                                 : nc::panel::PanelViewLayoutsStorage::LastResortLayout();
    const bool presentation_changed = !m_AssignedViewLayout || *m_AssignedViewLayout != *next_layout;
    const bool slot_validity_changed =
        m_AssignedViewLayoutUsesConfiguredSlot != uses_configured_slot;
    if( !presentation_changed && !slot_validity_changed )
        return;

    m_AssignedViewLayout = next_layout;
    m_AssignedViewLayoutUsesConfiguredSlot = uses_configured_slot;
    if( presentation_changed )
        [m_View setPresentationLayout:*m_AssignedViewLayout];
    [NSNotificationCenter.defaultCenter postNotificationName:NCPanelViewContextDidChangeNotification object:m_View];
}

- (void)panelViewDidChangePresentationLayout
{
    PanelViewLayout layout;
    layout.name = m_AssignedViewLayout->name;
    layout.layout = [m_View presentationLayout];

    if( layout != *m_AssignedViewLayout )
        m_Layouts->ReplaceLayout(std::move(layout), m_ViewLayoutIndex);
}

- (void)commitCancelableLoadingTask:(std::function<void(const CancelableLoadingTaskContext &)>)_task
{
    dispatch_assert_main_queue();
    const auto content_generation = [self claimContentIntentInvalidatingNavigationAdmission];
    m_DirectoryLoadingQ->Run([task = std::move(_task), sq = m_DirectoryLoadingQ, self, content_generation] {
        const CancelableLoadingTaskContext context{
            .is_cancelled = [sq, self, content_generation] {
                return sq->IsStopped() ||
                       content_generation !=
                           self->m_ContentRequestGeneration.load(std::memory_order_acquire);
            },
            .commit_on_main = [self, content_generation](std::function<void()> _commit) {
                dispatch_to_main_queue([self, content_generation, commit = std::move(_commit)] {
                    if( content_generation !=
                        self->m_ContentRequestGeneration.load(std::memory_order_acquire) )
                        return;
                    commit();
                });
            },
        };
        task(context);
    });
}

- (bool)probeDirectoryAccessForRequest:(DirectoryChangeRequest &)_request
{
    const auto &directory = _request.RequestedDirectory;
    auto &vfs = *_request.VFS;
    auto &access_provider = *m_DirectoryAccessProvider;
    const auto has_access = access_provider.HasAccess(self, directory, vfs);
    if( has_access ) {
        return true;
    }
    else {
        if( _request.InitiatedByUser )
            return access_provider.RequestAccessSync(self, directory, vfs);
        else
            return false;
    }
}

- (std::expected<void, Error>)doGoToDirWithContext:(std::shared_ptr<DirectoryChangeRequest>)_request
                                  contentGeneration:(const unsigned long)_content_generation
{
    assert(_request != nullptr);
    assert(_request->VFS != nullptr);
    Log::Debug("[PanelController doGoToDirWithContext] was called with {}", *_request);
    if( _content_generation != m_ContentRequestGeneration.load(std::memory_order_acquire) )
        return std::unexpected(Error{Error::POSIX, ECANCELED});

    try {
        if( ![self probeDirectoryAccessForRequest:*_request] ) {
            return std::unexpected(Error{Error::POSIX, EPERM});
        }

        auto directory = _request->RequestedDirectory;
        auto &vfs = *_request->VFS;
        const auto canceller = VFSCancelChecker([&] { return m_DirectoryLoadingQ->IsStopped(); });
        const std::expected<VFSListingPtr, Error> listing =
            vfs.FetchDirectoryListing(directory, m_VFSFetchingFlags, canceller);
        if( _request->LoadingResultCallback ) {
            _request->LoadingResultCallback(
                listing ? std::expected<void, Error>{}
                        : std::expected<void, Error>{std::unexpected(listing.error())},
                DirectoryChangeResultSource::Fetch,
                [=] {
                    return _content_generation ==
                           m_ContentRequestGeneration.load(std::memory_order_acquire);
                });
        }

        if( !listing )
            return std::unexpected(listing.error());
        if( m_DirectoryLoadingQ->IsStopped() ||
            _content_generation != m_ContentRequestGeneration.load(std::memory_order_acquire) )
            return std::unexpected(Error{Error::POSIX, ECANCELED});

        // TODO: need an ability to show errors at least

        [self stopBackgroundQueues]; // legacy recovery path; no lifecycle request owns this worker
        dispatch_or_run_in_main_queue([=] {
            if( _content_generation != m_ContentRequestGeneration.load(std::memory_order_acquire) )
                return;
            [m_View savePathState];
            m_Data.Load(*listing, data::Model::PanelType::Directory);
            for( auto &i : _request->RequestSelectedEntries )
                m_Data.CustomFlagsSelectSorted(m_Data.SortedIndexForName(i), true);
            m_DataGeneration++;
            [m_View dataUpdated];
            [m_View panelChangedWithFocusedFilename:_request->RequestFocusedEntry
                                  loadPreviousState:_request->LoadPreviousViewState];
            [self onPathChanged];
        });
    } catch( std::exception &e ) {
        ShowExceptionAlert(e);
    } catch( ... ) {
        ShowExceptionAlert();
    }
    return {};
}

- (NavigationFetchOutcome)fetchNavigationRequest:(const std::shared_ptr<DirectoryChangeRequest> &)_request
                                      fetchFlags:(const unsigned long)_fetch_flags
                               contentGeneration:(const unsigned long)_content_generation
{
    NavigationFetchOutcome outcome;
    try {
        const auto canceller = VFSCancelChecker([&] {
            return m_DirectoryLoadingQ->IsStopped() ||
                   _content_generation != m_ContentRequestGeneration.load(std::memory_order_acquire);
        });
        const std::expected<VFSListingPtr, Error> listing =
            _request->VFS->FetchDirectoryListing(_request->RequestedDirectory, _fetch_flags, canceller);

        if( listing )
            outcome.listing = *listing;
        else
            outcome.error = listing.error();
        outcome.cancelled = m_DirectoryLoadingQ->IsStopped() ||
                            _content_generation != m_ContentRequestGeneration.load(std::memory_order_acquire) ||
                            (outcome.error && IsCancellationError(*outcome.error));
    } catch( ... ) {
        outcome.exception = std::current_exception();
    }
    return outcome;
}

- (void)finishNavigationRequest:(const PaneRequestId)_request_id
                         request:(const std::shared_ptr<DirectoryChangeRequest> &)_request
                         outcome:(const NavigationFetchOutcome &)_outcome
               contentGeneration:(const unsigned long)_content_generation
               synchronousResult:(std::expected<void, Error> *)_synchronous_result
{
    dispatch_assert_main_queue();

    const auto clear_worker_slot = [&] {
        if( m_NavigationWorker && m_NavigationWorker->request_id == _request_id &&
            !m_NavigationWorker->uses_loading_queue )
            m_NavigationWorker.reset();
    };
    const auto set_synchronous_error = [&](const Error &_error) {
        if( _synchronous_result )
            *_synchronous_result = std::unexpected(_error);
    };

    if( _content_generation != m_ContentRequestGeneration.load(std::memory_order_acquire) ) {
        set_synchronous_error(Error{Error::POSIX, ECANCELED});
        [[maybe_unused]] const auto result =
            m_PaneLifecycle->Cancel(_request_id, PaneCancellationReason::InternalAbort);
        clear_worker_slot();
        return;
    }

    if( _outcome.exception ) {
        FileManagerError error = MapNavigationException(_outcome.exception, _request);
        set_synchronous_error(error.original_error);
        const auto result = m_PaneLifecycle->Fail(_request_id, std::move(error));
        clear_worker_slot();
        if( result == PaneLifecycleProducer::FinishResult::Published )
            PresentNavigationException(_outcome.exception);
        return;
    }

    if( _outcome.cancelled ) {
        const Error error = _outcome.error.value_or(Error{Error::POSIX, ECANCELED});
        set_synchronous_error(error);
        [[maybe_unused]] const auto result =
            m_PaneLifecycle->Cancel(_request_id, PaneCancellationReason::QueueStopped);
        clear_worker_slot();
        return;
    }

    if( _outcome.error ) {
        set_synchronous_error(*_outcome.error);
        [[maybe_unused]] const auto result =
            m_PaneLifecycle->Fail(_request_id, MapNavigationError(*_outcome.error, _request));
        clear_worker_slot();
        return;
    }

    if( !_outcome.listing ) {
        const auto exception = std::make_exception_ptr(std::logic_error{"Navigation fetch returned no outcome"});
        FileManagerError error = MapNavigationException(exception, _request);
        set_synchronous_error(error.original_error);
        const auto result = m_PaneLifecycle->Fail(_request_id, std::move(error));
        clear_worker_slot();
        if( result == PaneLifecycleProducer::FinishResult::Published )
            PresentNavigationException(exception);
        return;
    }

    const auto active = m_PaneLifecycle->Active();
    if( !active || active->request_id != _request_id ) {
        set_synchronous_error(Error{Error::POSIX, ECANCELED});
        clear_worker_slot();
        return;
    }

    std::optional<data::Model> prepared;
    try {
        prepared.emplace(m_Data);
        prepared->Load(_outcome.listing, data::Model::PanelType::Directory);
        for( const auto &item : _request->RequestSelectedEntries )
            prepared->CustomFlagsSelectSorted(prepared->SortedIndexForName(item), true);
    } catch( ... ) {
        const auto exception = std::current_exception();
        FileManagerError error = MapNavigationException(exception, _request);
        set_synchronous_error(error.original_error);
        const auto result = m_PaneLifecycle->Fail(_request_id, std::move(error));
        clear_worker_slot();
        if( result == PaneLifecycleProducer::FinishResult::Published )
            PresentNavigationException(exception);
        return;
    }

    const unsigned long next_generation = m_DataGeneration + 1;
    [m_View savePathState];
    m_DirectorySizeCountingQ.Stop();

    PaneLifecycleProducer::FinishResult commit_result;
    try {
        commit_result = m_PaneLifecycle->Commit(
            _request_id,
            PaneLifecycleCommitted{.controller_generation = next_generation, .listing = _outcome.listing},
            [&] {
                m_Data = std::move(*prepared);
                m_DataGeneration = next_generation;
            });
    } catch( ... ) {
        const auto exception = std::current_exception();
        FileManagerError error = MapNavigationException(exception, _request);
        set_synchronous_error(error.original_error);
        const auto failure_result = m_PaneLifecycle->Fail(_request_id, std::move(error));
        clear_worker_slot();
        if( failure_result == PaneLifecycleProducer::FinishResult::Published )
            PresentNavigationException(exception);
        return;
    }
    clear_worker_slot();

    if( commit_result != PaneLifecycleProducer::FinishResult::Published ||
        m_DataGeneration != next_generation || m_Data.ListingPtr() != _outcome.listing ) {
        set_synchronous_error(Error{Error::POSIX, ECANCELED});
        return;
    }

    if( _synchronous_result )
        *_synchronous_result = {};
    [m_View dataUpdated];
    [m_View panelChangedWithFocusedFilename:_request->RequestFocusedEntry
                          loadPreviousState:_request->LoadPreviousViewState];
    [self onPathChanged];
}

- (std::expected<void, Error>)GoToDirWithContext:(std::shared_ptr<DirectoryChangeRequest>)_request
{
    dispatch_assert_main_queue();

    if( m_NavigationAdmissionCallbackAllowed )
        m_NavigationAdmissionCallbackAllowed->store(false, std::memory_order_release);
    auto admission_callback_allowed = std::make_shared<std::atomic_bool>(true);
    m_NavigationAdmissionCallbackAllowed = admission_callback_allowed;

    const bool asynchronous = !_request || _request->PerformAsynchronous;
    if( _request )
        Log::Debug("[PanelController GoToDirWithContext] was called with {}", *_request);

    const bool structurally_valid = _request && _request->VFS && !_request->RequestedDirectory.empty() &&
                                    _request->RequestedDirectory.front() == '/';

    auto synchronous_result = std::make_shared<std::expected<void, Error>>(
        std::unexpected(Error{Error::POSIX, ECANCELED}));
    auto admission_state = std::make_shared<NavigationAdmissionState>();
    __weak PanelController *weak_panel_controller = self;
    const auto rejected_callback_is_current = [admission_callback_allowed, weak_panel_controller] {
        PanelController *const panel = weak_panel_controller;
        return panel && admission_callback_allowed->load(std::memory_order_acquire);
    };
    const auto report_rejection = [_request, rejected_callback_is_current](
                                      Error _error) -> std::expected<void, Error> {
        if( _request && _request->LoadingResultCallback ) {
            try {
                _request->LoadingResultCallback(std::unexpected(_error),
                                                DirectoryChangeResultSource::Admission,
                                                rejected_callback_is_current);
            } catch( ... ) {
                PresentNavigationException(std::current_exception());
            }
        }
        return std::unexpected(std::move(_error));
    };
    const auto submission_error = [](const PanelControllerLifecycleSubmissionResult &_result)
        -> std::optional<Error> {
        if( _result.status == PanelControllerLifecycleSubmissionStatus::Accepted )
            return std::nullopt;
        if( _result.status == PanelControllerLifecycleSubmissionStatus::Rejected ) {
            if( _result.rejection_error )
                return _result.rejection_error->original_error;
            if( _result.rejection_reason == PaneRejectionReason::InvalidRequest )
                return Error{Error::POSIX, EINVAL};
            if( _result.rejection_reason == PaneRejectionReason::Unavailable )
                return Error{Error::POSIX, ENODEV};
            return Error{Error::POSIX, EBUSY};
        }
        if( _result.status == PanelControllerLifecycleSubmissionStatus::SynchronousReentrancyUnsupported )
            return Error{Error::POSIX, EBUSY};
        if( _result.status == PanelControllerLifecycleSubmissionStatus::Shutdown )
            return Error{Error::POSIX, ECANCELED};
        return Error{Error::POSIX, EIO};
    };
    const auto submission = m_PaneLifecycle->SubmitNavigation(
        NavigationDescriptor(_request),
        asynchronous ? PaneNavigationExecution::Asynchronous : PaneNavigationExecution::Synchronous,
        [=](const PanelControllerLifecycleProbeContext &_context) {
            bool external_loading_work = false;
            if( asynchronous ) {
                const auto work_facts = [self loadingWorkFactsForLifecycleContext:_context];
                admission_state->correlated_worker = work_facts.correlated_navigation_worker;
                external_loading_work = work_facts.has_external_work;
            }
            return PanelControllerLifecycleAdmission{
                .valid = structurally_valid,
                .available = m_AcceptsNavigation && m_View != nil && m_DirectoryAccessProvider != nullptr,
                .has_external_loading_work = external_loading_work,
            };
        },
        [=](const PaneRequestId _request_id) {
            try {
                // Admission feedback and accepted-worker feedback have separate validity. A later
                // rejected admission must not invalidate this accepted request's worker callback.
                if( m_NavigationAdmissionCallbackAllowed == admission_callback_allowed )
                    m_NavigationAdmissionCallbackAllowed.reset();
                auto worker_callback_allowed = std::make_shared<std::atomic_bool>(true);
                // Claim the accepted content intent before access checks. RequestAccessSync can
                // run a nested main loop, where every older delayed commit must already be stale.
                // Preserve a different admission token: it belongs to a newer request submitted
                // reentrantly from this request's Started publication.
                if( m_RefreshWorker )
                    m_RefreshWorker->cancel_requested->store(true, std::memory_order_release);
                if( m_PendingRefresh ) {
                    m_PendingRefresh->cancel_requested->store(true, std::memory_order_release);
                    m_PendingRefresh.reset();
                }
                const unsigned long content_generation =
                    m_ContentRequestGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
                if( m_NavigationWorker )
                    m_NavigationWorker->callback_allowed->store(false, std::memory_order_release);
                if( !asynchronous ) {
                    m_NavigationWorker = NavigationWorkerSlot{
                        .request_id = _request_id,
                        .callback_allowed = worker_callback_allowed,
                        .worker_finished = std::make_shared<std::atomic_bool>(true),
                        .uses_loading_queue = false,
                    };
                    m_DirectoryLoadingQ->Stop();
                    m_DirectoryLoadingQ->Wait();
                    m_DirectorySizeCountingQ.Stop();
                }

                if( ![self probeDirectoryAccessForRequest:*_request] ) {
                    const Error access_error{Error::POSIX, EPERM};
                    if( !asynchronous )
                        *synchronous_result = std::unexpected(access_error);
                    [[maybe_unused]] const auto result =
                        m_PaneLifecycle->Fail(_request_id, MapNavigationError(access_error, _request));
                    if( _request->LoadingResultCallback ) {
                        const auto callback_is_current =
                            [worker_callback_allowed, content_generation, weak_panel_controller] {
                                PanelController *const panel = weak_panel_controller;
                                return panel && worker_callback_allowed->load(std::memory_order_acquire) &&
                                       content_generation ==
                                           panel->m_ContentRequestGeneration.load(std::memory_order_acquire);
                            };
                        try {
                            _request->LoadingResultCallback(std::unexpected(access_error),
                                                            DirectoryChangeResultSource::Admission,
                                                            callback_is_current);
                        } catch( ... ) {
                            PresentNavigationException(std::current_exception());
                        }
                    }
                    if( m_NavigationWorker && m_NavigationWorker->request_id == _request_id )
                        m_NavigationWorker.reset();
                    return;
                }

                const auto active = m_PaneLifecycle->Active();
                if( !active || active->request_id != _request_id ) {
                    if( !asynchronous )
                        *synchronous_result = std::unexpected(Error{Error::POSIX, ECANCELED});
                    if( m_NavigationWorker && m_NavigationWorker->request_id == _request_id )
                        m_NavigationWorker.reset();
                    return;
                }

                // Started observers and a modal access prompt can enqueue legacy work after the
                // admission sample. Do not place an accepted lifecycle worker behind that work.
                const int loading_queue_length_after_access =
                    asynchronous ? m_DirectoryLoadingQ->Length() : 0;
                const bool correlated_worker_still_running =
                    admission_state->correlated_worker && m_NavigationWorker &&
                    m_NavigationWorker->request_id == *admission_state->correlated_worker &&
                    m_NavigationWorker->uses_loading_queue &&
                    !m_NavigationWorker->worker_finished->load(std::memory_order_acquire);
                const int reload_queue_length_after_access = m_DirectoryReLoadingQ->Length();
                const bool refresh_queue_owned_after_access =
                    reload_queue_length_after_access == 1 && m_RefreshWorker &&
                    !m_DirectoryReLoadingQ->IsStopped();
                if( asynchronous &&
                    ((loading_queue_length_after_access != 0 &&
                      (loading_queue_length_after_access != 1 || !correlated_worker_still_running)) ||
                     (reload_queue_length_after_access != 0 && !refresh_queue_owned_after_access) ||
                     m_DirectoryReLoadingQ->IsStopped()) ) {
                    [[maybe_unused]] const auto result =
                        m_PaneLifecycle->Cancel(_request_id, PaneCancellationReason::InternalAbort);
                    return;
                }

                if( !asynchronous ) {
                    const auto fetch_flags = m_VFSFetchingFlags;
                    const auto callback_allowed = m_NavigationWorker->callback_allowed;
                    __weak PanelController *weakself = self;
                    const auto callback_is_current = [callback_allowed, content_generation, weakself] {
                        PanelController *const panel = weakself;
                        return panel && callback_allowed->load(std::memory_order_acquire) &&
                               content_generation ==
                                   panel->m_ContentRequestGeneration.load(std::memory_order_acquire);
                    };
                    NavigationFetchOutcome outcome = [self fetchNavigationRequest:_request
                                                                        fetchFlags:fetch_flags
                                                                 contentGeneration:content_generation];
                    const std::expected<void, Error> callback_result = outcome.error
                        ? std::expected<void, Error>{std::unexpected(*outcome.error)}
                        : std::expected<void, Error>{};

                    if( !outcome.exception && !outcome.cancelled && !outcome.error &&
                        _request->LoadingResultCallback ) {
                        try {
                            _request->LoadingResultCallback(callback_result,
                                                            DirectoryChangeResultSource::Fetch,
                                                            callback_is_current);
                        } catch( ... ) {
                            outcome.listing.reset();
                            outcome.exception = std::current_exception();
                        }
                    }

                    [self finishNavigationRequest:_request_id
                                           request:_request
                                           outcome:outcome
                                 contentGeneration:content_generation
                                 synchronousResult:synchronous_result.get()];

                    if( (outcome.cancelled || outcome.error) && _request->LoadingResultCallback ) {
                        try {
                            _request->LoadingResultCallback(callback_result,
                                                            DirectoryChangeResultSource::Fetch,
                                                            callback_is_current);
                        } catch( ... ) {
                            PresentNavigationException(std::current_exception());
                        }
                    }
                    return;
                }

                const auto fetch_flags = m_VFSFetchingFlags;
                auto callback_allowed = worker_callback_allowed;
                auto worker_finished = std::make_shared<std::atomic_bool>(false);
                const auto weak_panel =
                    std::make_shared<WeakPanelControllerRef>(WeakPanelControllerRef{.panel = self});
                const auto loading_queue = m_DirectoryLoadingQ;
                loading_queue->Run([request_id = _request_id,
                                    request = _request,
                                    fetch_flags,
                                    content_generation,
                                    callback_allowed,
                                    worker_finished,
                                    weak_panel,
                                    loading_queue] {
                    (void)loading_queue;
                    const auto mark_worker_finished = at_scope_end(
                        [worker_finished] { worker_finished->store(true, std::memory_order_release); });
                    const auto callback_is_current = [callback_allowed, content_generation, weak_panel] {
                        PanelController *const panel = weak_panel->panel;
                        return panel && callback_allowed->load(std::memory_order_acquire) &&
                               content_generation ==
                                   panel->m_ContentRequestGeneration.load(std::memory_order_acquire);
                    };
                    NavigationFetchOutcome outcome =
                        FetchNavigationRequestDetached(request, fetch_flags, callback_allowed);
                    const std::expected<void, Error> callback_result = outcome.error
                        ? std::expected<void, Error>{std::unexpected(*outcome.error)}
                        : std::expected<void, Error>{};

                    if( !outcome.exception && !outcome.cancelled && !outcome.error &&
                        request->LoadingResultCallback &&
                        callback_is_current() ) {
                        try {
                            request->LoadingResultCallback(
                                callback_result, DirectoryChangeResultSource::Fetch, callback_is_current);
                        } catch( ... ) {
                            outcome.listing.reset();
                            outcome.exception = std::current_exception();
                        }
                    }

                    dispatch_to_main_queue([request_id,
                                            request,
                                            outcome,
                                            content_generation,
                                            weak_panel] {
                        PanelController *const panel = weak_panel->panel;
                        if( !panel )
                            return;
                        [panel finishNavigationRequest:request_id
                                                request:request
                                                outcome:outcome
                                      contentGeneration:content_generation
                                      synchronousResult:nullptr];
                    });

                    if( (outcome.cancelled || outcome.error) && request->LoadingResultCallback &&
                        callback_is_current() ) {
                        try {
                            request->LoadingResultCallback(
                                callback_result, DirectoryChangeResultSource::Fetch, callback_is_current);
                        } catch( ... ) {
                            const auto exception = std::current_exception();
                            dispatch_to_main_queue([exception] { PresentNavigationException(exception); });
                        }
                    }

                    worker_finished->store(true, std::memory_order_release);
                    dispatch_to_main_queue([request_id, worker_finished, weak_panel] {
                        PanelController *const panel = weak_panel->panel;
                        if( !panel )
                            return;
                        if( panel->m_NavigationWorker &&
                            panel->m_NavigationWorker->request_id == request_id &&
                            panel->m_NavigationWorker->worker_finished == worker_finished )
                            panel->m_NavigationWorker.reset();
                    });
                });
                m_NavigationWorker = NavigationWorkerSlot{
                    .request_id = _request_id,
                    .callback_allowed = std::move(callback_allowed),
                    .worker_finished = std::move(worker_finished),
                    .uses_loading_queue = true,
                };
            } catch( ... ) {
                const auto exception = std::current_exception();
                if( !asynchronous )
                    *synchronous_result =
                        std::unexpected(MapNavigationException(exception, _request).original_error);
                if( m_NavigationWorker && m_NavigationWorker->request_id == _request_id )
                    m_NavigationWorker.reset();
                dispatch_to_main_queue([exception] { PresentNavigationException(exception); });
                std::rethrow_exception(exception);
            }
        },
        [submission_error, report_rejection](
            const PanelControllerLifecycleSubmissionResult &_resolution) noexcept {
            try {
                if( const auto error = submission_error(_resolution) )
                    [[maybe_unused]] const auto reported = report_rejection(*error);
            } catch( ... ) {
                PresentNavigationException(std::current_exception());
            }
        });

    switch( submission.status ) {
        case PanelControllerLifecycleSubmissionStatus::Accepted:
            return asynchronous ? std::expected<void, Error>{} : *synchronous_result;
        case PanelControllerLifecycleSubmissionStatus::Rejected:
            return report_rejection(*submission_error(submission));
        case PanelControllerLifecycleSubmissionStatus::Deferred:
            return {};
        case PanelControllerLifecycleSubmissionStatus::SynchronousReentrancyUnsupported:
            return report_rejection(Error{Error::POSIX, EBUSY});
        case PanelControllerLifecycleSubmissionStatus::Shutdown:
            return report_rejection(Error{Error::POSIX, ECANCELED});
    }
    return report_rejection(Error{Error::POSIX, EIO});
}

- (void)loadListing:(const VFSListingPtr &)_listing
{
    dispatch_assert_main_queue();
    [self claimContentIntentInvalidatingNavigationAdmission];
    [self CancelBackgroundOperations];
    [m_View savePathState];
    if( _listing->IsUniform() )
        m_Data.Load(_listing, data::Model::PanelType::Directory);
    else
        m_Data.Load(_listing, data::Model::PanelType::Temporary);
    m_DataGeneration++;
    [m_View dataUpdated];
    [m_View panelChangedWithFocusedFilename:"" loadPreviousState:false];
    [self onPathChanged];
}

- (void)recoverFromInvalidDirectory
{
    assert(dispatch_is_main_queue());
    const auto recovery_generation = [self claimContentIntentInvalidatingNavigationAdmission];
    std::filesystem::path initial_path = EnsureNoTrailingSlash(self.currentDirectoryPath);
    auto initial_vfs = self.vfs;
    m_DirectoryLoadingQ->Run([=] {
        const auto is_stale = [=] {
            return m_DirectoryLoadingQ->IsStopped() ||
                   recovery_generation !=
                       m_ContentRequestGeneration.load(std::memory_order_acquire);
        };
        // 1st - try to locate a valid dir in current host
        std::filesystem::path path = initial_path;
        const auto &vfs = initial_vfs;

        while( true ) {
            if( is_stale() )
                return;

            const auto is_accessible =
                vfs->IterateDirectoryListing(path.native(), [](const VFSDirEnt &) { return false; });
            if( is_stale() )
                return;

            if( is_accessible ) {
                auto request = std::make_shared<DirectoryChangeRequest>();
                request->RequestedDirectory = path.native();
                request->VFS = vfs;
                request->PerformAsynchronous = true;
                [self doGoToDirWithContext:request contentGeneration:recovery_generation];
                return;
            }

            if( path == "/" )
                break;

            path = path.parent_path();
        }

        if( is_stale() )
            return;

        // we can't work on this vfs. currently for simplicity - just go home
        auto request = std::make_shared<DirectoryChangeRequest>();
        request->RequestedDirectory = nc::base::CommonPaths::Home();
        request->VFS = m_NativeHost->SharedPtr();
        request->PerformAsynchronous = true;
        [self doGoToDirWithContext:request contentGeneration:recovery_generation];
    });
}

- (void)scheduleDelayedFocusing:(const DelayedFocusing &)request
{
    assert(dispatch_is_main_queue()); // to preserve against fancy threading stuff
    // we assume that _item_name will not contain any forward slashes

    if( request.filename.empty() )
        return;

    nc::panel::Log::Trace("[PanelController scheduleDelayedFocusing] called for '{}'", request.filename);

    m_DelayedSelection.request_end = nc::base::machtime() + request.timeout;
    m_DelayedSelection.filename = request.filename;
    m_DelayedSelection.done = request.done;

    if( request.check_now )
        [self checkAgainstRequestedFocusing];
}

// This function checks if a requested focusing can be satisfied and if so - changes the cursor.
// The check is destructive/has side effects - it clears a focus request if either it was satisfied
// or if it became outdated.
// Returns true if the request was satisfied and the cursor position was changed.
- (bool)checkAgainstRequestedFocusing
{
    assert(dispatch_is_main_queue()); // to preserve against fancy threading stuff
    if( m_DelayedSelection.filename.empty() )
        return false;

    if( nc::base::machtime() > m_DelayedSelection.request_end ) {
        nc::panel::Log::Trace("[PanelController checkAgainstRequestedFocusing] removing a stale request for '{}'",
                              m_DelayedSelection.filename);
        [self clearFocusingRequest];
        return false;
    }

    // now try to find it
    int raw_index = m_Data.RawIndexForName(m_DelayedSelection.filename);
    if( raw_index < 0 )
        return false;
    nc::panel::Log::Trace("[PanelController checkAgainstRequestedFocusing] found an entry for '{}'",
                          m_DelayedSelection.filename);

    // we found this entry. regardless of appearance of this entry in current directory presentation
    // there's no reason to search for it again
    auto done = std::move(m_DelayedSelection.done);

    const int sort_index = m_Data.SortedIndexForRawIndex(raw_index);
    if( sort_index >= 0 ) {
        m_View.curpos = sort_index;
        if( !self.isActive )
            [self.state ActivatePanelByController:self];
        if( done )
            done();
    }

    // focus requests are one-shot
    [self clearFocusingRequest];

    // return 'true' only if the entry was actually focused, regardless if it is present in raw
    // listing.
    return sort_index >= 0;
}

- (void)clearFocusingRequest
{
    m_DelayedSelection.filename.clear();
    m_DelayedSelection.done = nullptr;
}

- (BriefSystemOverview *)briefSystemOverview
{
    return [self.state briefSystemOverviewForPanel:self make:false];
}

- (id<NCPanelPreview>)quickLook
{
    return [self.state quickLookForPanel:self make:false];
}

- (nc::panel::PanelViewLayoutsStorage &)layoutStorage
{
    return *m_Layouts;
}

- (nc::core::VFSInstanceManager &)vfsInstanceManager
{
    return *m_VFSInstanceManager;
}

- (int)quickSearchNeedsCursorPosition:(NCPanelQuickSearch *) [[maybe_unused]] _qs
{
    return m_View.curpos;
}

- (void)quickSearch:(NCPanelQuickSearch *) [[maybe_unused]] _qs wantsToSetCursorPosition:(int)_cursor_position
{
    m_View.curpos = _cursor_position;
}

- (void)quickSearchHasChangedVolatileData:(NCPanelQuickSearch *) [[maybe_unused]] _qs
{
    [m_View volatileDataChanged];
}

- (void)quickSearchHasUpdatedData:(NCPanelQuickSearch *) [[maybe_unused]] _qs
{
    [m_View dataUpdated];
}

- (void)quickSearch:(NCPanelQuickSearch *) [[maybe_unused]] _qs
    wantsToSetSearchPrompt:(NSString *)_prompt
          withMatchesCount:(int)_count
{
    id<NCPanelQuickSearchPresentation> const presentation = m_QuickSearchPresentation ? m_QuickSearchPresentation
                                                                                       : m_View.headerView;
    presentation.searchPrompt = _prompt;
    presentation.searchMatches = _count;
}

- (bool)isDoingBackgroundLoading
{
    return !m_DirectoryLoadingQ->Empty() || !m_DirectoryReLoadingQ->Empty();
}

- (std::unique_ptr<nc::panel::DragReceiver>)panelView:(PanelView *) [[maybe_unused]] _view
                      requestsDragReceiverForDragging:(id<NSDraggingInfo>)_dragging
                                               onItem:(int)_on_sorted_index
{
    return std::make_unique<nc::panel::DragReceiver>(
        self, _dragging, _on_sorted_index, *m_NativeFSManager, *m_NativeHost);
}

- (void)hintAboutFilesystemChange
{
    Log::Trace("[PanelController hintAboutFilesystemChange] was called");
    dispatch_assert_main_queue(); // to preserve against fancy threading stuff
    if( self.receivesUpdateNotifications ) {
        // check in some future that the notification actually came
        const auto timestamp = nc::base::machtime();
        __weak PanelController *weak_me = self;
        dispatch_to_main_queue_after(g_FilesystemHintTriggerDelay, [weak_me, timestamp] {
            if( PanelController *const me = weak_me ) {
                // now check if our listing was created after we were hinted
                if( me->m_Data.Listing().BuildTicksTimestamp() > timestamp )
                    return; // yep, fresh enough.

                // nope, stale -> refresh
                [me refreshPanel];
            }
        });
    }
    else {
        // immediately request a listing reload since a notification won't come anyway
        [self refreshPanel];
    }
}

@end
