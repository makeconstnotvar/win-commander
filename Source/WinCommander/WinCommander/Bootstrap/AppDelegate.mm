// Copyright (C) 2013-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#include "AppDelegate.h"
#include "AppDelegateCPP.h"
#include "AppDelegate+Migration.h"
#include "AppDelegate+MainWindowCreation.h"
#include "AppDelegate+ViewerCreation.h"
#include "Config.h"
#include "ConfigWiring.h"
#include "VFSInit.h"
#include "Interactions.h"
#include "NCHelpMenuDelegate.h"
#include "SparkleShim.h"
#include "PFMoveToApplicationsShim.h"
#include "NativeVFSHostInstance.h"
#include "Actions.h"

#include <algorithm>
#include <magic_enum.hpp>
#include <spdlog/sinks/stdout_sinks.h>

#include <Base/CommonPaths.h>
#include <Base/CFDefaultsCPP.h>
#include <Base/algo.h>
#include <Base/debug.h>

#include <Utility/NSMenu+ActionsShortcutsManager.h>
#include <Utility/NSMenu+Hierarchical.h>
#include <Utility/NativeFSManagerImpl.h>
#include <Utility/TemporaryFileStorageImpl.h>
#include <Utility/PathManip.h>
#include <Utility/FunctionKeysPass.h>
#include <Utility/StringExtras.h>
#include <Utility/ObjCpp.h>
#include <Utility/UTIImpl.h>
#include <Utility/SystemInformation.h>
#include <Utility/Log.h>
#include <Utility/FSEventsFileUpdateImpl.h>
#include <Utility/SpdLogWindow.h>
#include <Utility/Tags.h>

#include <RoutedIO/RoutedIO.h>
#include <RoutedIO/Log.h>

#include <WinCommander/Core/ActionsShortcutsManager.h>
#include <WinCommander/Core/Commands/FileCopyCommand.h>
#include <WinCommander/Core/Commands/FileCutCommand.h>
#include <WinCommander/Core/Commands/FileGetInfoCommand.h>
#include <WinCommander/Core/Commands/FileOpenCommand.h>
#include <WinCommander/Core/Commands/FilePreviewCommand.h>
#include <WinCommander/Core/Commands/FileMutationCommands.h>
#include <WinCommander/Core/Commands/FileRenameCommand.h>
#include <WinCommander/Core/Commands/NavigationHistoryCommand.h>
#include <WinCommander/Core/Commands/OperationCancelCommand.h>
#include <WinCommander/Core/Commands/OperationCenterOpenCommand.h>
#include <WinCommander/Core/Commands/PaneNavigationCommand.h>
#include <WinCommander/Core/Commands/ToggleHiddenFilesCommand.h>
#include <WinCommander/Core/Commands/TogglePreviewPaneCommand.h>
#include <WinCommander/Core/Operations/OperationSubmissionGate.h>
#include <WinCommander/Core/Operations/CopyOperationRecoveryCoordinator.h>
#include <WinCommander/Core/SandboxManager.h>
#include <WinCommander/Core/Dock.h>
#include <WinCommander/Core/ServicesHandler.h>
#include <WinCommander/Core/ConfigBackedNetworkConnectionsManager.h>
#include <WinCommander/Core/ConnectionsMenuDelegate.h>
#include <WinCommander/Core/Theming/SystemThemeDetector.h>
#include <WinCommander/Core/Theming/ThemesManager.h>
#include <WinCommander/Core/Theming/Theme.h>
#include <WinCommander/Core/VFSInstanceManagerImpl.h>
#include <WinCommander/Bootstrap/NCEditMenuPresentationDelegate.h>
#include <WinCommander/States/Terminal/ShellState.h>
#include <WinCommander/States/Explorer/NCExplorerCommandBarView.h>
#include <WinCommander/States/Explorer/NCExplorerInspectorPresenting.h>
#include <WinCommander/States/Explorer/ExplorerViewSettingsPersistence.h>
#include <WinCommander/States/MainWindow.h>
#include <WinCommander/States/MainWindowController.h>
#include <WinCommander/States/FilePanels/MainWindowFilePanelState.h>
#include <WinCommander/States/FilePanels/ExternalEditorInfo.h>
#include <WinCommander/States/FilePanels/PanelController.h>
#include <WinCommander/States/FilePanels/PanelView.h>
#include <WinCommander/States/FilePanels/PanelViewLayoutSupport.h>
#include <WinCommander/States/FilePanels/Actions/NavigateHistory.h>
#include <WinCommander/States/FilePanels/Actions/GoToFolder.h>
#include <WinCommander/States/FilePanels/Actions/OpenFile.h>
#include <WinCommander/States/FilePanels/Actions/ShowQuickLook.h>
#include <WinCommander/States/FilePanels/Actions/Delete.h>
#include <WinCommander/States/FilePanels/Actions/InsertFromPasteboard.h>
#include <WinCommander/States/FilePanels/Actions/MakeNew.h>
#include <WinCommander/States/FilePanels/Actions/Select.h>
#include <WinCommander/States/FilePanels/Actions/Compress.h>
#include <WinCommander/States/FilePanels/Actions/ExtractArchive.h>
#include <WinCommander/States/FilePanels/Actions/Duplicate.h>
#include <WinCommander/States/FilePanels/Actions/CopyFilePaths.h>
#include <WinCommander/States/FilePanels/Actions/CalculateSizes.h>
#include <WinCommander/States/FilePanels/Actions/BatchRename.h>
#include <WinCommander/States/FilePanels/Helpers/Pasteboard.h>
#include <WinCommander/States/FilePanels/FavoritesImpl.h>
#include <WinCommander/States/FilePanels/FavoritesWindowController.h>
#include <WinCommander/States/FilePanels/FavoritesMenuDelegate.h>
#include <WinCommander/States/FilePanels/Helpers/ClosedPanelsHistoryImpl.h>
#include <WinCommander/States/FilePanels/Helpers/RecentlyClosedMenuDelegate.h>
#include <WinCommander/Preferences/Preferences.h>

#include <Operations/Pool.h>
#include <Operations/PoolEnqueueFilter.h>
#include <Operations/AggregateProgressTracker.h>
#include <Operations/CopyOperationOrchestrator.h>
#include <Operations/OperationCenterCoordinator.h>
#include <Operations/OperationJournal.h>

#include <Config/ConfigImpl.h>
#include <Config/ObjCBridge.h>
#include <Config/FileOverwritesStorage.h>
#include <Config/Executor.h>
#include <Config/Log.h>

#include <Viewer/History.h>
#include <Viewer/Log.h>
#include <Viewer/ViewerViewController.h>
#include <Viewer/InternalViewerWindowController.h>
#include <Viewer/Highlighting/FileSettingsStorage.h>

#include <Term/Log.h>

#include <VFS/Log.h>

#include <VFSIcon/Log.h>

#include <Panel/Log.h>
#include <Panel/ExternalTools.h>
#include <Panel/TagsStorage.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace std::literals;
using namespace nc::bootstrap;

static std::optional<std::string> Load(const std::string &_filepath);

static auto g_ConfigDirPostfix = "Config/";
static auto g_StateDirPostfix = "State/";

static nc::config::ConfigImpl *g_Config = nullptr;
static nc::config::ConfigImpl *g_State = nullptr;
static nc::config::ConfigImpl *g_NetworkConnectionsConfig = nullptr;
static nc::utility::TemporaryFileStorageImpl *g_TemporaryFileStorage = nullptr;

static const auto g_ConfigForceFn = "general.alwaysUseFnKeysAsFunctional";
static const auto g_ConfigExternalToolsList = "externalTools.tools_v1";
static const auto g_ConfigLayoutsList = "filePanel.layout.layouts_v1";
static const auto g_ConfigExplorerLayoutsList = "filePanel.layout.explorer_layouts_v1";
static const auto g_ConfigSelectedTheme = "general.theme";
static const auto g_ConfigThemes = "themes";
static const auto g_ConfigExtEditorsList = "externalEditors.editors_v1";
static const auto g_ConfigFinderTags = "filePanel.FinderTags.tags";

nc::config::Config &GlobalConfig() noexcept
{
    assert(g_Config);
    return *g_Config;
}

nc::config::Config &StateConfig() noexcept
{
    assert(g_State);
    return *g_State;
}

static void ResetDefaults()
{
    const auto bundle_id = NSBundle.mainBundle.bundleIdentifier;
    [NSUserDefaults.standardUserDefaults removePersistentDomainForName:bundle_id];
    [NSUserDefaults.standardUserDefaults synchronize];
    g_Config->ResetToDefaults();
    g_State->ResetToDefaults();
    g_Config->Commit();
    g_State->Commit();
}

static void CheckDefaultsReset()
{
    const auto erase_mask =
        NSEventModifierFlagCapsLock | NSEventModifierFlagShift | NSEventModifierFlagOption | NSEventModifierFlagCommand;
    if( (NSEvent.modifierFlags & erase_mask) == erase_mask )
        if( AskUserToResetDefaults() ) {
            ResetDefaults();
            exit(0);
        }
}

template <typename Log>
static void AttachToSink(spdlog::level::level_enum _level, std::shared_ptr<spdlog::sinks::sink> _sink)
{
    Log::Set(std::make_shared<spdlog::logger>(Log::Name(), _sink));
    Log::Get().set_level(_level);
}

static std::span<nc::base::SpdLogger *const> Loggers() noexcept
{
    static const auto loggers = std::to_array({&nc::config::Log::Logger(),
                                               &nc::panel::Log::Logger(),
                                               &nc::routedio::Log::Logger(),
                                               &nc::term::Log::Logger(),
                                               &nc::utility::Log::Logger(),
                                               &nc::vfs::Log::Logger(),
                                               &nc::vfsicon::Log::Logger(),
                                               &nc::viewer::Log::Logger()});
    return loggers;
}

static void SetupLogs()
{
    spdlog::level::level_enum level = spdlog::level::off;
    const auto defaults = NSUserDefaults.standardUserDefaults;
    const auto args = [defaults volatileDomainForName:NSArgumentDomain];
    if( const auto arg_level = nc::objc_cast<NSString>([args objectForKey:@"NCLogLevel"]) ) {
        const auto casted = magic_enum::enum_cast<spdlog::level::level_enum>(arg_level.UTF8String);
        level = casted.value_or(spdlog::level::off);
    }

    if( level < spdlog::level::off ) {
        const auto stdout_sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
        for( auto logger : Loggers() ) {
            logger->Get().sinks().emplace_back(stdout_sink);
            logger->Get().set_level(level);
        }
    }
}

static NCAppDelegate *g_Me = nil;

@interface NCAppDelegate ()

@property(nonatomic, readonly) nc::core::Dock &dock;

@property(nonatomic) IBOutlet NSMenu *recentlyClosedMenu;

- (void)setupCopyOperationRuntime;

@end

@interface NCViewerWindowDelegateBridge : NSObject <NCViewerWindowDelegate>

- (void)viewerWindowWillShow:(InternalViewerWindowController *)_window;
- (void)viewerWindowWillClose:(InternalViewerWindowController *)_window;

@end

@implementation NCAppDelegate {
    std::vector<NCMainWindowController *> m_MainWindows;
    std::vector<InternalViewerWindowController *> m_ViewerWindows;
    nc::spinlock m_ViewerWindowsLock;
    std::filesystem::path m_SupportDirectory;
    std::filesystem::path m_ConfigDirectory;
    std::filesystem::path m_StateDirectory;
    std::vector<nc::config::Token> m_ConfigObservationTickets;
    upward_flag m_FinishedLaunching;
    std::shared_ptr<nc::panel::FavoriteLocationsStorageImpl> m_Favorites;
    NSMutableArray *m_FilesToOpen;
    NCViewerWindowDelegateBridge *m_ViewerWindowDelegateBridge;
    std::unique_ptr<nc::utility::NativeFSManager> m_NativeFSManager;
    std::shared_ptr<nc::vfs::NativeHost> m_NativeHost;
    std::unique_ptr<nc::utility::FSEventsFileUpdateImpl> m_FSEventsFileUpdate;
    nc::ops::PoolEnqueueFilter m_PoolEnqueueFilter;
    std::shared_ptr<nc::core::CopyOperationRecoveryCoordinator> m_CopyOperationRecoveryCoordinator;
    std::shared_ptr<nc::ops::OperationCenterCoordinator> m_OperationCenterCoordinator;
    std::unique_ptr<ConfigWiring> m_ConfigWiring;
    std::unique_ptr<nc::SystemThemeDetector> m_SystemThemeDetector;
    std::unique_ptr<nc::ThemesManager> m_ThemesManager;
    std::unique_ptr<nc::core::CommandRegistry> m_CommandRegistry;
    NCSpdLogWindowController *m_LogWindowController;
}

@synthesize mainWindowControllers = m_MainWindows;
@synthesize configDirectory = m_ConfigDirectory;
@synthesize stateDirectory = m_StateDirectory;
@synthesize supportDirectory = m_SupportDirectory;
@synthesize recentlyClosedMenu;

- (id)init
{
    self = [super init];
    if( self ) {
        SetupLogs();
        g_Me = self;
        m_FilesToOpen = [[NSMutableArray alloc] init];
        m_ViewerWindowDelegateBridge = [[NCViewerWindowDelegateBridge alloc] init];
        m_FSEventsFileUpdate = std::make_unique<nc::utility::FSEventsFileUpdateImpl>();
        m_NativeFSManager = std::make_unique<nc::utility::NativeFSManagerImpl>();
        m_NativeHost = std::make_shared<nc::vfs::NativeHost>(*m_NativeFSManager, *m_FSEventsFileUpdate);
        CheckDefaultsReset();
        m_SupportDirectory = nc::AppDelegate::SupportDirectory();
        [self setupConfigs];
        m_SystemThemeDetector = std::make_unique<nc::SystemThemeDetector>();
        m_CommandRegistry = std::make_unique<nc::core::CommandRegistry>();
        auto *const file_opener = &self.fileOpener;
        const auto file_open_registration = nc::core::MakeFileOpenCommand(
            [file_opener](void *_native_target, const std::span<const nc::vfs::ListingItem> _items) {
                if( _native_target == nullptr )
                    return false;
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return nc::panel::actions::SubmitOpenItemsWithDefaultHandler(_items, panel, *file_opener);
            });
        [[maybe_unused]] const auto file_open_result = m_CommandRegistry->Register(file_open_registration);
        assert(file_open_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto file_get_info_registration = nc::core::MakeFileGetInfoCommand(
            [](void *_native_target, const nc::core::FileGetInfoPresentation _presentation) {
                if( _native_target == nullptr )
                    return false;
                PanelController *const panel = (__bridge PanelController *)_native_target;
                id<NCExplorerInspectorPresenting> const presenter =
                    [panel.state conformsToProtocol:@protocol(NCExplorerInspectorPresenting)]
                        ? static_cast<id<NCExplorerInspectorPresenting>>(panel.state)
                        : nil;
                return presenter && [presenter presentFileGetInfo:_presentation forPanel:panel];
            });
        [[maybe_unused]] const auto file_get_info_result = m_CommandRegistry->Register(file_get_info_registration);
        assert(file_get_info_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto file_preview_registration = nc::core::MakeFilePreviewCommand([](void *_native_target,
                                                                                   const nc::vfs::ListingItem &_item,
                                                                                   nc::core::FilePreviewIntent) {
            if( _native_target == nullptr )
                return false;
            PanelController *const panel = (__bridge PanelController *)_native_target;
            try {
                if( !_item || _item.IsDotDot() )
                    return false;
                const int sort_position = panel.data.SortPositionOfEntry(_item);
                if( sort_position < 0 || !panel.data.IsValidSortPosition(sort_position) )
                    return false;
                PanelView *const view = panel.view;
                if( view == nil )
                    return false;
                view.curpos = sort_position;
                const VFSListingItem focused_item = view.item;
                if( !focused_item || focused_item.Listing() != _item.Listing() ||
                    focused_item.Index() != _item.Index() )
                    return false;
                const nc::panel::actions::ShowQuickLook action;
                if( !action.Predicate(panel) )
                    return false;
                action.Perform(panel, nil);
                return true;
            } catch( ... ) {
                return false;
            }
        });
        [[maybe_unused]] const auto file_preview_result = m_CommandRegistry->Register(file_preview_registration);
        assert(file_preview_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto file_copy_registration =
            nc::core::MakeFileCopyCommand([](const std::span<const nc::vfs::ListingItem> _items) {
                const std::vector<VFSListingItem> items{_items.begin(), _items.end()};
                if( !nc::panel::PasteboardSupport::WriteFilesnamesPBoard(items, NSPasteboard.generalPasteboard) )
                    NSBeep();
            });
        [[maybe_unused]] const auto file_copy_result = m_CommandRegistry->Register(file_copy_registration);
        assert(file_copy_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto file_cut_registration = nc::core::MakeFileCutCommand(
            [](const std::span<const nc::vfs::ListingItem> _items, const nc::core::FileCutIntent _intent) {
                if( _intent != nc::core::FileCutIntent::Move )
                    return false;
                const std::vector<VFSListingItem> items{_items.begin(), _items.end()};
                return nc::panel::PasteboardSupport::WriteFilesnamesPBoard(
                    items, NSPasteboard.generalPasteboard, nc::panel::PasteboardFileOperation::Move);
            });
        [[maybe_unused]] const auto file_cut_result = m_CommandRegistry->Register(file_cut_registration);
        assert(file_cut_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const nc::core::FilePasteAvailabilityProvider file_paste_availability = [](void *_native_target) {
            if( !_native_target )
                return nc::core::FilePasteAvailability::PaneUnavailable;
            PanelController *const panel = (__bridge PanelController *)_native_target;
            try {
                if( !panel.mainWindowController )
                    return nc::core::FilePasteAvailability::WindowUnavailable;
                if( panel.isDoingBackgroundLoading || !panel.isUniform || !panel.vfs )
                    return nc::core::FilePasteAvailability::DestinationUnavailable;
                if( !nc::vfs::ProviderCapabilitiesResolver::Resolve(*panel.vfs, panel.currentDirectoryPath).can_write )
                    return nc::core::FilePasteAvailability::DestinationReadOnly;
            } catch( ... ) {
                return nc::core::FilePasteAvailability::DestinationUnavailable;
            }

            NSPasteboard *const pasteboard = NSPasteboard.generalPasteboard;
            if( !nc::panel::PasteboardSupport::CanReadFileList(pasteboard) )
                return nc::core::FilePasteAvailability::ClipboardUnavailable;
            if( nc::panel::PasteboardSupport::IsCutInFlight(pasteboard) ||
                nc::panel::PasteboardSupport::IsFileListMoveInFlight(pasteboard) )
                return nc::core::FilePasteAvailability::ClipboardBusy;
            return nc::core::FilePasteAvailability::Available;
        };
        nc::vfs::NativeHost *const native_host = m_NativeHost.get();
        const auto file_paste_registration = nc::core::MakeFilePasteCommand(
            file_paste_availability, [file_paste_availability, native_host](void *_native_target, const void *) {
                const auto live = file_paste_availability(_native_target);
                if( live != nc::core::FilePasteAvailability::Available )
                    return live;
                PanelController *const panel = (__bridge PanelController *)_native_target;
                const nc::panel::actions::PasteFromPasteboard action{*native_host};
                using Submission = nc::panel::actions::PasteSubmissionResult;
                switch( action.Execute(panel) ) {
                    case Submission::Submitted:
                        return nc::core::FilePasteAvailability::Available;
                    case Submission::PaneUnavailable:
                        return nc::core::FilePasteAvailability::PaneUnavailable;
                    case Submission::WindowUnavailable:
                        return nc::core::FilePasteAvailability::WindowUnavailable;
                    case Submission::DestinationUnavailable:
                        return nc::core::FilePasteAvailability::DestinationUnavailable;
                    case Submission::DestinationReadOnly:
                        return nc::core::FilePasteAvailability::DestinationReadOnly;
                    case Submission::ClipboardUnavailable:
                        return nc::core::FilePasteAvailability::ClipboardUnavailable;
                    case Submission::ClipboardBusy:
                        return nc::core::FilePasteAvailability::ClipboardBusy;
                    case Submission::ClipboardChanged:
                        return nc::core::FilePasteAvailability::ClipboardChanged;
                    case Submission::SourceUnavailable:
                        return nc::core::FilePasteAvailability::SourceUnavailable;
                }
                return nc::core::FilePasteAvailability::DestinationUnavailable;
            });
        [[maybe_unused]] const auto file_paste_result = m_CommandRegistry->Register(file_paste_registration);
        assert(file_paste_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const nc::core::FileDeletionExecutor file_deletion_executor =
            [](void *_native_target,
               const std::span<const nc::vfs::ListingItem> _items,
               const nc::core::FileDeletionIntent _intent,
               const void *) {
                if( !_native_target )
                    return false;
                PanelController *const panel = (__bridge PanelController *)_native_target;
                if( _intent == nc::core::FileDeletionIntent::Trash )
                    return nc::panel::actions::SubmitItemsToTrash(_items, panel);
                return nc::panel::actions::PresentPermanentDeletion(_items, panel);
            };
        const auto file_trash_registration = nc::core::MakeFileTrashCommand(file_deletion_executor);
        [[maybe_unused]] const auto file_trash_result = m_CommandRegistry->Register(file_trash_registration);
        assert(file_trash_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto file_delete_registration = nc::core::MakeFileDeleteCommand(file_deletion_executor);
        [[maybe_unused]] const auto file_delete_result = m_CommandRegistry->Register(file_delete_registration);
        assert(file_delete_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto map_quick_new_file_result = [](const nc::panel::actions::QuickNewFileSubmissionResult _result) {
            using Submission = nc::panel::actions::QuickNewFileSubmissionResult;
            switch( _result ) {
                case Submission::Submitted:
                    return nc::core::FileCreationAvailability::Available;
                case Submission::PaneUnavailable:
                    return nc::core::FileCreationAvailability::PaneUnavailable;
                case Submission::WindowUnavailable:
                    return nc::core::FileCreationAvailability::WindowUnavailable;
                case Submission::Loading:
                    return nc::core::FileCreationAvailability::Loading;
                case Submission::DestinationUnavailable:
                    return nc::core::FileCreationAvailability::DestinationUnavailable;
                case Submission::DestinationReadOnly:
                    return nc::core::FileCreationAvailability::DestinationReadOnly;
                case Submission::ProviderUnsupported:
                    return nc::core::FileCreationAvailability::ProviderUnsupported;
                case Submission::StaleDestination:
                    return nc::core::FileCreationAvailability::StaleDestination;
                case Submission::NameUnavailable:
                    return nc::core::FileCreationAvailability::NameUnavailable;
            }
            return nc::core::FileCreationAvailability::StaleDestination;
        };
        const nc::core::FileCreationAvailabilityProvider file_creation_availability =
            [map_quick_new_file_result](void *_native_target, const nc::core::FileCreationIntent _intent) {
                if( !_native_target )
                    return nc::core::FileCreationAvailability::PaneUnavailable;
                PanelController *const panel = (__bridge PanelController *)_native_target;
                if( _intent == nc::core::FileCreationIntent::File )
                    return map_quick_new_file_result(nc::panel::actions::EvaluateQuickNewFileSubmission(panel));
                try {
                    if( !panel.mainWindowController )
                        return nc::core::FileCreationAvailability::WindowUnavailable;
                    if( panel.isDoingBackgroundLoading )
                        return nc::core::FileCreationAvailability::Loading;
                    if( !panel.isUniform || !panel.vfs || !panel.data.ListingPtr() )
                        return nc::core::FileCreationAvailability::DestinationUnavailable;
                    const std::string path = panel.currentDirectoryPath;
                    if( !panel.vfs->IsWritableAtPath(path) )
                        return nc::core::FileCreationAvailability::DestinationReadOnly;
                    if( !nc::vfs::ProviderCapabilitiesResolver::Resolve(*panel.vfs, path).can_create_folder )
                        return nc::core::FileCreationAvailability::ProviderUnsupported;
                    return nc::core::FileCreationAvailability::Available;
                } catch( ... ) {
                    return nc::core::FileCreationAvailability::DestinationUnavailable;
                }
            };
        const auto file_new_folder_registration = nc::core::MakeFileNewFolderCommand(
            file_creation_availability,
            [file_creation_availability](
                void *_native_target, const nc::core::FileCreationIntent _intent, const void *) {
                const auto live = file_creation_availability(_native_target, _intent);
                if( live != nc::core::FileCreationAvailability::Available )
                    return live;
                PanelController *const panel = (__bridge PanelController *)_native_target;
                using Submission = nc::panel::actions::QuickNewFolderSubmissionResult;
                switch( nc::panel::actions::SubmitQuickNewFolder(panel) ) {
                    case Submission::Submitted:
                        return nc::core::FileCreationAvailability::Available;
                    case Submission::PaneUnavailable:
                        return nc::core::FileCreationAvailability::PaneUnavailable;
                    case Submission::WindowUnavailable:
                        return nc::core::FileCreationAvailability::WindowUnavailable;
                    case Submission::Loading:
                        return nc::core::FileCreationAvailability::Loading;
                    case Submission::DestinationUnavailable:
                        return nc::core::FileCreationAvailability::DestinationUnavailable;
                    case Submission::DestinationReadOnly:
                        return nc::core::FileCreationAvailability::DestinationReadOnly;
                    case Submission::ProviderUnsupported:
                        return nc::core::FileCreationAvailability::ProviderUnsupported;
                    case Submission::StaleDestination:
                        return nc::core::FileCreationAvailability::StaleDestination;
                    case Submission::NameUnavailable:
                        return nc::core::FileCreationAvailability::NameUnavailable;
                }
                return nc::core::FileCreationAvailability::StaleDestination;
            });
        [[maybe_unused]] const auto file_new_folder_result = m_CommandRegistry->Register(file_new_folder_registration);
        assert(file_new_folder_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto file_new_file_registration = nc::core::MakeFileNewFileCommand(
            file_creation_availability,
            [map_quick_new_file_result](void *_native_target, const nc::core::FileCreationIntent, const void *) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_quick_new_file_result(nc::panel::actions::SubmitQuickNewFile(panel));
            });
        [[maybe_unused]] const auto file_new_file_result = m_CommandRegistry->Register(file_new_file_registration);
        assert(file_new_file_result == nc::core::CommandRegistry::RegisterResult::Registered);

        const auto map_selection_result = [](const nc::panel::actions::PaneSelectionActionResult _result) {
            using Action = nc::panel::actions::PaneSelectionActionResult;
            switch( _result ) {
                case Action::Available:
                    return nc::core::PaneSelectionAvailability::Available;
                case Action::PaneUnavailable:
                    return nc::core::PaneSelectionAvailability::PaneUnavailable;
                case Action::Loading:
                    return nc::core::PaneSelectionAvailability::Loading;
                case Action::ListingUnavailable:
                    return nc::core::PaneSelectionAvailability::ListingUnavailable;
                case Action::Empty:
                    return nc::core::PaneSelectionAvailability::Empty;
            }
            return nc::core::PaneSelectionAvailability::ListingUnavailable;
        };
        const nc::core::PaneSelectionAvailabilityProvider selection_availability =
            [map_selection_result](void *_native_target, const nc::core::PaneSelectionIntent) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_selection_result(nc::panel::actions::EvaluatePaneSelectionAction(panel));
            };
        const nc::core::PaneSelectionExecutor selection_executor =
            [map_selection_result](void *_native_target, const nc::core::PaneSelectionIntent _intent, const void *) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                const auto result = _intent == nc::core::PaneSelectionIntent::SelectAll
                                        ? nc::panel::actions::ApplySelectAll(panel)
                                        : nc::panel::actions::ApplyInvertSelection(panel);
                return map_selection_result(result);
            };
        const auto pane_select_all_registration =
            nc::core::MakePaneSelectAllCommand(selection_availability, selection_executor);
        [[maybe_unused]] const auto pane_select_all_result = m_CommandRegistry->Register(pane_select_all_registration);
        assert(pane_select_all_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto pane_invert_selection_registration =
            nc::core::MakePaneInvertSelectionCommand(selection_availability, selection_executor);
        [[maybe_unused]] const auto pane_invert_selection_result =
            m_CommandRegistry->Register(pane_invert_selection_registration);
        assert(pane_invert_selection_result == nc::core::CommandRegistry::RegisterResult::Registered);

        const auto map_archive_create_result = [](const nc::panel::actions::ArchiveCreateSubmissionResult _result) {
            using Action = nc::panel::actions::ArchiveCreateSubmissionResult;
            switch( _result ) {
                case Action::Presented:
                    return nc::core::ArchiveCreateAvailability::Available;
                case Action::PaneUnavailable:
                    return nc::core::ArchiveCreateAvailability::PaneUnavailable;
                case Action::WindowUnavailable:
                    return nc::core::ArchiveCreateAvailability::WindowUnavailable;
                case Action::Loading:
                    return nc::core::ArchiveCreateAvailability::Loading;
                case Action::SelectionUnavailable:
                    return nc::core::ArchiveCreateAvailability::SelectionUnavailable;
                case Action::ParentEntryUnsupported:
                    return nc::core::ArchiveCreateAvailability::ParentEntryUnsupported;
                case Action::SourceUnreadable:
                    return nc::core::ArchiveCreateAvailability::SourceUnreadable;
                case Action::SourceNameCollision:
                    return nc::core::ArchiveCreateAvailability::SourceNameCollision;
                case Action::DestinationUnavailable:
                    return nc::core::ArchiveCreateAvailability::DestinationUnavailable;
                case Action::DestinationReadOnly:
                    return nc::core::ArchiveCreateAvailability::DestinationReadOnly;
                case Action::ProviderUnsupported:
                    return nc::core::ArchiveCreateAvailability::ProviderUnsupported;
                case Action::StaleContext:
                    return nc::core::ArchiveCreateAvailability::StaleContext;
            }
            return nc::core::ArchiveCreateAvailability::StaleContext;
        };
        const auto archive_create_registration = nc::core::MakeArchiveCreateCommand(
            [map_archive_create_result](void *_native_target, const std::span<const nc::vfs::ListingItem> _items) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_archive_create_result(nc::panel::actions::EvaluateArchiveCreateSubmission(_items, panel));
            },
            [map_archive_create_result](
                void *_native_target, const std::span<const nc::vfs::ListingItem> _items, const void *) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_archive_create_result(
                    nc::panel::actions::PresentArchiveCreate(_items, panel, GlobalConfig()));
            });
        [[maybe_unused]] const auto archive_create_result = m_CommandRegistry->Register(archive_create_registration);
        assert(archive_create_result == nc::core::CommandRegistry::RegisterResult::Registered);

        const auto map_archive_extract_result =
            [](const nc::panel::actions::ArchiveExtractionSubmissionResult _result) {
                using Action = nc::panel::actions::ArchiveExtractionSubmissionResult;
                switch( _result ) {
                    case Action::Submitted:
                        return nc::core::ArchiveExtractAvailability::Available;
                    case Action::PaneUnavailable:
                        return nc::core::ArchiveExtractAvailability::PaneUnavailable;
                    case Action::WindowUnavailable:
                        return nc::core::ArchiveExtractAvailability::WindowUnavailable;
                    case Action::Loading:
                        return nc::core::ArchiveExtractAvailability::Loading;
                    case Action::SelectionUnavailable:
                        return nc::core::ArchiveExtractAvailability::SelectionUnavailable;
                    case Action::ParentEntryUnsupported:
                        return nc::core::ArchiveExtractAvailability::ParentEntryUnsupported;
                    case Action::SourceUnsupported:
                        return nc::core::ArchiveExtractAvailability::SourceUnsupported;
                    case Action::SourceUnreadable:
                        return nc::core::ArchiveExtractAvailability::SourceUnreadable;
                    case Action::DestinationUnavailable:
                        return nc::core::ArchiveExtractAvailability::DestinationUnavailable;
                    case Action::DestinationReadOnly:
                        return nc::core::ArchiveExtractAvailability::DestinationReadOnly;
                    case Action::ProviderUnsupported:
                        return nc::core::ArchiveExtractAvailability::ProviderUnsupported;
                    case Action::CaseSensitivityUnavailable:
                        return nc::core::ArchiveExtractAvailability::CaseSensitivityUnavailable;
                    case Action::StaleContext:
                        return nc::core::ArchiveExtractAvailability::StaleContext;
                }
                return nc::core::ArchiveExtractAvailability::StaleContext;
            };
        const auto archive_extract_registration = nc::core::MakeArchiveExtractCommand(
            [map_archive_extract_result](void *_native_target, const std::span<const nc::vfs::ListingItem> _items) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_archive_extract_result(
                    nc::panel::actions::EvaluateArchiveExtractionSubmission(_items, panel));
            },
            [map_archive_extract_result](
                void *_native_target, const std::span<const nc::vfs::ListingItem> _items, const void *) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_archive_extract_result(nc::panel::actions::SubmitArchiveExtraction(_items, panel));
            });
        [[maybe_unused]] const auto archive_extract_result = m_CommandRegistry->Register(archive_extract_registration);
        assert(archive_extract_result == nc::core::CommandRegistry::RegisterResult::Registered);

        const auto map_duplicate_result = [](const nc::panel::actions::DuplicateSubmissionResult _result) {
            using Action = nc::panel::actions::DuplicateSubmissionResult;
            switch( _result ) {
                case Action::Submitted:
                    return nc::core::FileDuplicateAvailability::Available;
                case Action::PaneUnavailable:
                    return nc::core::FileDuplicateAvailability::PaneUnavailable;
                case Action::WindowUnavailable:
                    return nc::core::FileDuplicateAvailability::WindowUnavailable;
                case Action::Loading:
                    return nc::core::FileDuplicateAvailability::Loading;
                case Action::SelectionUnavailable:
                    return nc::core::FileDuplicateAvailability::SelectionUnavailable;
                case Action::ParentEntryUnsupported:
                    return nc::core::FileDuplicateAvailability::ParentEntryUnsupported;
                case Action::SourceUnreadable:
                    return nc::core::FileDuplicateAvailability::SourceUnreadable;
                case Action::DestinationUnavailable:
                    return nc::core::FileDuplicateAvailability::DestinationUnavailable;
                case Action::DestinationReadOnly:
                    return nc::core::FileDuplicateAvailability::DestinationReadOnly;
                case Action::ProviderUnsupported:
                    return nc::core::FileDuplicateAvailability::ProviderUnsupported;
                case Action::NameUnavailable:
                    return nc::core::FileDuplicateAvailability::NameUnavailable;
                case Action::StaleContext:
                    return nc::core::FileDuplicateAvailability::StaleContext;
            }
            return nc::core::FileDuplicateAvailability::StaleContext;
        };
        const auto file_duplicate_registration = nc::core::MakeFileDuplicateCommand(
            [map_duplicate_result](void *_native_target, const std::span<const nc::vfs::ListingItem> _items) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_duplicate_result(nc::panel::actions::EvaluateDuplicateSubmission(_items, panel));
            },
            [map_duplicate_result](
                void *_native_target, const std::span<const nc::vfs::ListingItem> _items, const void *) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_duplicate_result(nc::panel::actions::SubmitDuplicateItems(_items, panel, GlobalConfig()));
            });
        [[maybe_unused]] const auto file_duplicate_result = m_CommandRegistry->Register(file_duplicate_registration);
        assert(file_duplicate_result == nc::core::CommandRegistry::RegisterResult::Registered);

        const auto map_copy_path_result = [](const nc::panel::actions::CopyPathSubmissionResult _result) {
            using Action = nc::panel::actions::CopyPathSubmissionResult;
            switch( _result ) {
                case Action::Submitted:
                    return nc::core::FileCopyPathAvailability::Available;
                case Action::PaneUnavailable:
                    return nc::core::FileCopyPathAvailability::PaneUnavailable;
                case Action::Loading:
                    return nc::core::FileCopyPathAvailability::Loading;
                case Action::SelectionUnavailable:
                    return nc::core::FileCopyPathAvailability::SelectionUnavailable;
                case Action::ParentEntryUnsupported:
                    return nc::core::FileCopyPathAvailability::ParentEntryUnsupported;
                case Action::StaleContext:
                    return nc::core::FileCopyPathAvailability::StaleContext;
                case Action::ClipboardUnavailable:
                    return nc::core::FileCopyPathAvailability::ClipboardUnavailable;
            }
            return nc::core::FileCopyPathAvailability::StaleContext;
        };
        const auto file_copy_path_registration = nc::core::MakeFileCopyPathCommand(
            [map_copy_path_result](void *_native_target, const std::span<const nc::vfs::ListingItem> _items) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_copy_path_result(nc::panel::actions::EvaluateCopyPathSubmission(_items, panel));
            },
            [map_copy_path_result](
                void *_native_target, const std::span<const nc::vfs::ListingItem> _items, const void *) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_copy_path_result(nc::panel::actions::SubmitCopyPaths(_items, panel));
            });
        [[maybe_unused]] const auto file_copy_path_result = m_CommandRegistry->Register(file_copy_path_registration);
        assert(file_copy_path_result == nc::core::CommandRegistry::RegisterResult::Registered);

        const auto map_calculate_sizes_result = [](const nc::panel::actions::CalculateSizesSubmissionResult _result) {
            using Action = nc::panel::actions::CalculateSizesSubmissionResult;
            switch( _result ) {
                case Action::Submitted:
                    return nc::core::FileCalculateSizesAvailability::Available;
                case Action::PaneUnavailable:
                    return nc::core::FileCalculateSizesAvailability::PaneUnavailable;
                case Action::Loading:
                    return nc::core::FileCalculateSizesAvailability::Loading;
                case Action::ListingUnavailable:
                    return nc::core::FileCalculateSizesAvailability::ListingUnavailable;
                case Action::SelectionUnavailable:
                    return nc::core::FileCalculateSizesAvailability::SelectionUnavailable;
                case Action::ParentEntryUnsupported:
                    return nc::core::FileCalculateSizesAvailability::ParentEntryUnsupported;
                case Action::NoDirectories:
                    return nc::core::FileCalculateSizesAvailability::DirectoryRequired;
                case Action::SourceUnreadable:
                    return nc::core::FileCalculateSizesAvailability::SourceUnreadable;
                case Action::CalculationBusy:
                    return nc::core::FileCalculateSizesAvailability::Busy;
                case Action::StaleContext:
                    return nc::core::FileCalculateSizesAvailability::StaleContext;
            }
            return nc::core::FileCalculateSizesAvailability::StaleContext;
        };
        const auto file_calculate_sizes_registration = nc::core::MakeFileCalculateSizesCommand(
            [map_calculate_sizes_result](void *_native_target, const std::span<const nc::vfs::ListingItem> _items) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_calculate_sizes_result(nc::panel::actions::EvaluateCalculateSizesSubmission(_items, panel));
            },
            [map_calculate_sizes_result](
                void *_native_target, const std::span<const nc::vfs::ListingItem> _items, const void *) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_calculate_sizes_result(nc::panel::actions::SubmitCalculateSizes(_items, panel));
            });
        [[maybe_unused]] const auto file_calculate_sizes_result =
            m_CommandRegistry->Register(file_calculate_sizes_registration);
        assert(file_calculate_sizes_result == nc::core::CommandRegistry::RegisterResult::Registered);

        const auto map_batch_rename_result = [](const nc::panel::actions::BatchRenameSubmissionResult _result) {
            using Action = nc::panel::actions::BatchRenameSubmissionResult;
            switch( _result ) {
                case Action::Presented:
                    return nc::core::FileBatchRenameAvailability::Available;
                case Action::PaneUnavailable:
                    return nc::core::FileBatchRenameAvailability::PaneUnavailable;
                case Action::WindowUnavailable:
                    return nc::core::FileBatchRenameAvailability::WindowUnavailable;
                case Action::Loading:
                    return nc::core::FileBatchRenameAvailability::Loading;
                case Action::ListingUnavailable:
                    return nc::core::FileBatchRenameAvailability::ListingUnavailable;
                case Action::SelectionUnavailable:
                    return nc::core::FileBatchRenameAvailability::SelectionUnavailable;
                case Action::ParentEntryUnsupported:
                    return nc::core::FileBatchRenameAvailability::ParentEntryUnsupported;
                case Action::MixedProviders:
                    return nc::core::FileBatchRenameAvailability::MixedProviders;
                case Action::ProviderUnsupported:
                    return nc::core::FileBatchRenameAvailability::ProviderUnsupported;
                case Action::InvalidPlan:
                    return nc::core::FileBatchRenameAvailability::InvalidPlan;
                case Action::DestinationConflict:
                    return nc::core::FileBatchRenameAvailability::DestinationConflict;
                case Action::StaleContext:
                    return nc::core::FileBatchRenameAvailability::StaleContext;
            }
            return nc::core::FileBatchRenameAvailability::StaleContext;
        };
        const auto file_batch_rename_registration = nc::core::MakeFileBatchRenameCommand(
            [map_batch_rename_result](void *_native_target, const std::span<const nc::vfs::ListingItem> _items) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_batch_rename_result(nc::panel::actions::EvaluateBatchRenameSubmission(_items, panel));
            },
            [map_batch_rename_result](
                void *_native_target, const std::span<const nc::vfs::ListingItem> _items, const void *) {
                PanelController *const panel = (__bridge PanelController *)_native_target;
                return map_batch_rename_result(nc::panel::actions::PresentBatchRename(_items, panel));
            });
        [[maybe_unused]] const auto file_batch_rename_result =
            m_CommandRegistry->Register(file_batch_rename_registration);
        assert(file_batch_rename_result == nc::core::CommandRegistry::RegisterResult::Registered);

        const auto file_rename_registration = nc::core::MakeFileRenameCommand([](void *_native_target,
                                                                                 const nc::vfs::ListingItem &_item) {
            if( _native_target == nullptr )
                return false;

            PanelController *const panel = (__bridge PanelController *)_native_target;
            PanelView *const view = panel.view;
            if( view == nil )
                return false;

            const int sort_position = panel.data.SortPositionOfEntry(_item);
            if( sort_position < 0 || !panel.data.IsValidSortPosition(sort_position) )
                return false;

            view.curpos = sort_position;
            const VFSListingItem focused_item = view.item;
            if( !focused_item || focused_item.Listing() != _item.Listing() || focused_item.Index() != _item.Index() )
                return false;

            return [view startFieldEditorRenaming];
        });
        [[maybe_unused]] const auto file_rename_result = m_CommandRegistry->Register(file_rename_registration);
        assert(file_rename_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto view_toggle_hidden_files_registration =
            nc::core::MakeViewToggleHiddenFilesCommand([](void *_native_target, const bool _shows_hidden_files) {
                if( _native_target == nullptr )
                    return false;

                PanelController *const panel = (__bridge PanelController *)_native_target;
                auto filtering = panel.data.HardFiltering();
                filtering.show_hidden = _shows_hidden_files;
                [panel changeHardFilteringTo:filtering];
                return panel.data.HardFiltering().show_hidden == _shows_hidden_files;
            });
        [[maybe_unused]] const auto view_toggle_hidden_files_result =
            m_CommandRegistry->Register(view_toggle_hidden_files_registration);
        assert(view_toggle_hidden_files_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto view_toggle_preview_pane_registration = nc::core::MakeViewTogglePreviewPaneCommand(
            [](void *_native_target, const bool _expected_visible, const bool _desired_visible) {
                if( _native_target == nullptr )
                    return false;
                PanelController *const panel = (__bridge PanelController *)_native_target;
                id<NCExplorerInspectorPresenting> const presenter =
                    [panel.state conformsToProtocol:@protocol(NCExplorerInspectorPresenting)]
                        ? static_cast<id<NCExplorerInspectorPresenting>>(panel.state)
                        : nil;
                return presenter && [presenter setPreviewPaneVisible:_desired_visible
                                                            expected:_expected_visible
                                                             forPanel:panel];
            });
        [[maybe_unused]] const auto view_toggle_preview_pane_result =
            m_CommandRegistry->Register(view_toggle_preview_pane_registration);
        assert(view_toggle_preview_pane_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const nc::core::NavigationHistoryExecutor navigation_history_executor =
            [](void *_native_target, const nc::core::NavigationHistoryDirection _direction) {
                if( _native_target == nullptr )
                    return false;

                PanelController *const panel = (__bridge PanelController *)_native_target;
                switch( _direction ) {
                    case nc::core::NavigationHistoryDirection::Back: {
                        const nc::panel::actions::GoBack action;
                        if( !action.Predicate(panel) )
                            return false;
                        action.Perform(panel, nil);
                        return true;
                    }
                    case nc::core::NavigationHistoryDirection::Forward: {
                        const nc::panel::actions::GoForward action;
                        if( !action.Predicate(panel) )
                            return false;
                        action.Perform(panel, nil);
                        return true;
                    }
                }
                return false;
            };
        const auto navigation_back_registration = nc::core::MakeNavigationBackCommand(navigation_history_executor);
        [[maybe_unused]] const auto navigation_back_result = m_CommandRegistry->Register(navigation_back_registration);
        assert(navigation_back_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto navigation_forward_registration =
            nc::core::MakeNavigationForwardCommand(navigation_history_executor);
        [[maybe_unused]] const auto navigation_forward_result =
            m_CommandRegistry->Register(navigation_forward_registration);
        assert(navigation_forward_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto navigation_up_registration = nc::core::MakeNavigationUpCommand([](void *_native_target) {
            if( _native_target == nullptr )
                return false;
            PanelController *const panel = (__bridge PanelController *)_native_target;
            return nc::panel::actions::SubmitExplicitGoToEnclosingFolder(panel);
        });
        [[maybe_unused]] const auto navigation_up_result = m_CommandRegistry->Register(navigation_up_registration);
        assert(navigation_up_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto navigation_refresh_registration = nc::core::MakeNavigationRefreshCommand([](void *_native_target) {
            if( _native_target == nullptr )
                return false;
            PanelController *const panel = (__bridge PanelController *)_native_target;
            return [panel submitUserRefresh];
        });
        [[maybe_unused]] const auto navigation_refresh_result =
            m_CommandRegistry->Register(navigation_refresh_registration);
        assert(navigation_refresh_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const std::weak_ptr<nc::ops::OperationCenterCoordinator> operation_center = m_OperationCenterCoordinator;
        const auto operation_cancel_registration = nc::core::MakeOperationCancelCommand(
            [operation_center](const nc::ops::OperationId _operation_id, const uint64_t _expected_revision) {
                const auto coordinator = operation_center.lock();
                if( !coordinator ) {
                    return nc::ops::OperationCenterCancelResult{
                        .code = nc::ops::OperationCenterCancelResultCode::ResidencyUnavailable};
                }
                return coordinator->Cancel(_operation_id, _expected_revision);
            });
        [[maybe_unused]] const auto operation_cancel_result =
            m_CommandRegistry->Register(operation_cancel_registration);
        assert(operation_cancel_result == nc::core::CommandRegistry::RegisterResult::Registered);
        const auto operation_center_open_registration = nc::core::MakeOperationCenterOpenCommand(
            [operation_center]() -> std::optional<std::vector<nc::ops::OperationRecord>> {
                const auto coordinator = operation_center.lock();
                if( !coordinator )
                    return std::nullopt;
                return coordinator->Model().Snapshot();
            },
            [](void *const _native_target, std::vector<nc::ops::OperationRecord> _snapshot) -> bool {
                if( _native_target == nullptr )
                    return false;
                NCExplorerCommandBarView *const command_bar = (__bridge NCExplorerCommandBarView *)_native_target;
                return [command_bar presentOperationCenterSnapshot:std::move(_snapshot)];
            },
            [operation_center] { return !operation_center.expired(); });
        [[maybe_unused]] const auto operation_center_open_result =
            m_CommandRegistry->Register(operation_center_open_registration);
        assert(operation_center_open_result == nc::core::CommandRegistry::RegisterResult::Registered);
    }
    return self;
}

+ (NCAppDelegate *)me
{
    return g_Me;
}

- (void)applicationWillFinishLaunching:(NSNotification *) [[maybe_unused]] _notification
{
    RegisterAvailableVFS();

    // Init themes manager
    m_ThemesManager = std::make_unique<nc::ThemesManager>(GlobalConfig(), g_ConfigSelectedTheme, g_ConfigThemes);
    // also hook up the appearance change notification with the global application appearance
    auto update_tm_appearance = [self] {
        m_ThemesManager->NotifyAboutSystemAppearanceChange(m_SystemThemeDetector->SystemAppearance());
    };
    auto update_app_appearance = [self] { [NSApp setAppearance:m_ThemesManager->SelectedTheme().Appearance()]; };
    update_tm_appearance();
    update_app_appearance();
    // observe forever
    [[clang::no_destroy]] static auto token =
        m_ThemesManager->ObserveChanges(nc::ThemesManager::Notifications::Appearance, update_app_appearance);
    [[clang::no_destroy]] static auto token1 = m_SystemThemeDetector->ObserveChanges(update_tm_appearance);

    [self themesManager];
    [self favoriteLocationsStorage];
    [self tagsStorage]; // might kickstart a background scanning of the finder tags

    [self updateMainMenuFeaturesByVersionAndState];

    // update menu with current shortcuts layout
    [NSApp.mainMenu nc_setMenuItemShortcutsWithActionsShortcutsManager:self.actionsShortcutsManager];
    [self wireMenuDelegates];

    if( nc::base::AmISandboxed() ) {
        auto &sm = SandboxManager::Instance();
        if( sm.Empty() ) {
            sm.AskAccessForPathSync(nc::base::CommonPaths::Home(), false);
            if( self.mainWindowControllers.empty() ) {
                auto ctrl = [self allocateDefaultMainWindow];
                [ctrl showWindow:self];
            }
        }
    }
}

- (void)wireMenuDelegates
{
    // set up menu delegates. do this via DI to reduce links to AppDelegate in whole codebase
    auto item_for_action = [&](const char *_action) -> NSMenuItem * {
        const std::optional<int> tag = self.actionsShortcutsManager.TagFromAction(_action);
        if( tag == std::nullopt )
            return nil;
        return [NSApp.mainMenu itemWithTagHierarchical:*tag];
    };

    // Layout titles are resolved by ToggleLayout validation against the active panel's storage.
    // Commander and Explorer intentionally use separate layout sets.

    NSMenuItem *const copy_item = item_for_action("menu.edit.copy");
    NSMenuItem *const paste_item = item_for_action("menu.edit.paste");
    NSMenuItem *const select_all_item = item_for_action("menu.edit.select_all");
    NSMenuItem *cut_item = nil;
    for( NSMenuItem *const item in copy_item.menu.itemArray ) {
        if( item.action == @selector(cut:) ) {
            cut_item = item;
            break;
        }
    }
    static const auto edit_menu_presentation_delegate =
        [[NCEditMenuPresentationDelegate alloc] initWithCutMenuItem:cut_item
                                                       copyMenuItem:copy_item
                                                      pasteMenuItem:paste_item
                                                  selectAllMenuItem:select_all_item];
    if( edit_menu_presentation_delegate != nil )
        copy_item.menu.delegate = edit_menu_presentation_delegate;

    auto manage_fav_item = item_for_action("menu.go.favorites.manage");
    static auto favorites_delegate =
        [[FavoriteLocationsMenuDelegate alloc] initWithStorage:*self.favoriteLocationsStorage
                                             andManageMenuItem:manage_fav_item];
    manage_fav_item.menu.delegate = favorites_delegate;

    auto clear_freq_item = [NSApp.mainMenu itemWithTagHierarchical:14220];
    static auto frequent_delegate =
        [[FrequentlyVisitedLocationsMenuDelegate alloc] initWithStorage:*self.favoriteLocationsStorage
                                                       andClearMenuItem:clear_freq_item];
    clear_freq_item.menu.delegate = frequent_delegate;

    const auto connections_menu_item = item_for_action("menu.go.connect.network_server");
    static const auto conn_delegate = [[ConnectionsMenuDelegate alloc]
        initWithManager:[]() -> nc::panel::NetworkConnectionsManager & { return *g_Me.networkConnectionsManager; }];
    connections_menu_item.menu.delegate = conn_delegate;

    auto panels_locator = []() -> MainWindowFilePanelState * {
        if( auto wnd = nc::objc_cast<NCMainWindow>(NSApp.keyWindow) )
            if( auto ctrl = nc::objc_cast<NCMainWindowController>(wnd.delegate) )
                return ctrl.filePanelsState;
        return nil;
    };
    static const auto recently_closed_delegate =
        [[NCPanelsRecentlyClosedMenuDelegate alloc] initWithMenu:self.recentlyClosedMenu
                                                         storage:self.closedPanelsHistory
                                                   panelsLocator:panels_locator];
    (void)recently_closed_delegate;

    // These menus will have a submenu generated on the fly by according actions.
    // However, it's required for these menu items to always have submenus so that
    // Preferences can detect it and mark its hotkeys as readonly.
    // This solution is horrible but I can find a better one right now.
    item_for_action("menu.file.open_with_submenu").submenu = [NSMenu new];
    item_for_action("menu.file.always_open_with_submenu").submenu = [NSMenu new];

    // Set up a delegate for the Help menu
    static const auto help_delegate = [[NCHelpMenuDelegate alloc] init];
    auto help_menu_item = [NSApp.mainMenu itemWithTagHierarchical:17'000].menu;
    help_menu_item.delegate = help_delegate;
}

- (void)updateMainMenuFeaturesByVersionAndState
{
    // disable some features available in menu by configuration limitation
    auto tag_from_lit = [&](const char *s) { return self.actionsShortcutsManager.TagFromAction(s).value(); };
    auto current_menuitem = [&](const char *s) { return [NSApp.mainMenu itemWithTagHierarchical:tag_from_lit(s)]; };
    auto hide = [&](const char *s) {
        auto item = current_menuitem(s);
        item.alternate = false;
        item.hidden = true;
    };
    // one-way items hiding
    if( nc::base::AmISandboxed() ) {
        hide("menu.view.show_terminal");
        hide("menu.view.panels_position.move_up");
        hide("menu.view.panels_position.move_down");
        hide("menu.view.panels_position.showpanels");
        hide("menu.view.panels_position.focusterminal");
        hide("menu.file.feed_filename_to_terminal");
        hide("menu.file.feed_filenames_to_terminal");
        hide("menu.win_commander.toggle_admin_mode");
        hide("menu.go.connect.lanshare");
    }
}

- (void)applicationDidFinishLaunching:(NSNotification *) [[maybe_unused]] _notification
{
    m_FinishedLaunching.toggle();

    if( self.mainWindowControllers.empty() )
        [self applicationOpenUntitledFile:NSApp]; // if there's no restored windows - we'll create a
                                                  // freshly new one

    NSApp.servicesProvider = self;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [NSApp registerServicesMenuSendTypes:@[NSFilenamesPboardType, (__bridge NSString *)kUTTypeFileURL]
                             returnTypes:@[]]; // pasteboard types provided by PanelController
#pragma clang diagnostic pop
    NSUpdateDynamicServices();

    [self temporaryFileStorage]; // implicitly runs the background temp storage purging

    // Non-MAS version extended logic below:
    if( !nc::base::AmISandboxed() ) {
#ifndef __NC_CODEX_DEV__
        // setup Sparkle updater stuff
        NSMenuItem *item = [[NSMenuItem alloc] init];
        item.title = NSLocalizedString(@"Check for Updates...",
                                       "Menu item title for check if any Duck Commander updates are available");
        item.target = NCBootstrapSharedSUUpdaterInstance();
        item.action = NCBootstrapSUUpdaterAction();
        [[NSApp.mainMenu itemAtIndex:0].submenu insertItem:item atIndex:1];
#endif

        if( GlobalConfig().GetBool(g_ConfigForceFn) )
            nc::utility::FunctionalKeysPass::Instance().Enable(); // accessibility - remapping functional keys FnXX

#ifndef __NC_CODEX_DEV__
        PFMoveToApplicationsFolderIfNecessary();
#endif
    }

    m_ConfigWiring = std::make_unique<ConfigWiring>(GlobalConfig(), m_PoolEnqueueFilter);
    m_ConfigWiring->Wire();

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(windowWillClose:)
                                                 name:NSWindowWillCloseNotification
                                               object:nil];
}

- (void)setupConfigs
{
    assert(g_Config == nullptr && g_State == nullptr);

    m_ConfigDirectory = m_SupportDirectory / g_ConfigDirPostfix;
    if( !std::filesystem::exists(m_ConfigDirectory) )
        std::filesystem::create_directories(m_ConfigDirectory);

    m_StateDirectory = m_SupportDirectory / g_StateDirPostfix;
    if( !std::filesystem::exists(m_StateDirectory) )
        std::filesystem::create_directories(m_StateDirectory);
    [self setupCopyOperationRuntime];

    const auto bundle = NSBundle.mainBundle;
    const auto config_defaults_path = [bundle pathForResource:@"Config" ofType:@"json"].fileSystemRepresentationSafe;
    const auto config_defaults = Load(config_defaults_path);
    if( config_defaults == std::nullopt ) {
        std::cerr << "Failed to read the main config file: " << config_defaults_path << '\n';
        exit(-1);
    }

    const auto state_defaults_path = [bundle pathForResource:@"State" ofType:@"json"].fileSystemRepresentationSafe;
    const auto state_defaults = Load(state_defaults_path);
    if( state_defaults == std::nullopt ) {
        std::cerr << "Failed to read the state config file: " << state_defaults_path << '\n';
        exit(-1);
    }

    const auto write_delay = std::chrono::seconds{30};
    const auto reload_delay = std::chrono::seconds{1};

    g_Config = new nc::config::ConfigImpl(
        *config_defaults,
        std::make_shared<nc::config::FileOverwritesStorage>(self.configDirectory / "Config.json"),
        std::make_shared<nc::config::DelayedAsyncExecutor>(write_delay),
        std::make_shared<nc::config::DelayedAsyncExecutor>(reload_delay));

    g_State = new nc::config::ConfigImpl(
        *state_defaults,
        std::make_shared<nc::config::FileOverwritesStorage>(self.stateDirectory / "State.json"),
        std::make_shared<nc::config::DelayedAsyncExecutor>(write_delay),
        std::make_shared<nc::config::DelayedAsyncExecutor>(reload_delay));

    g_NetworkConnectionsConfig = new nc::config::ConfigImpl(
        "",
        std::make_shared<nc::config::FileOverwritesStorage>(self.configDirectory / "NetworkConnections.json"),
        std::make_shared<nc::config::DelayedAsyncExecutor>(write_delay),
        std::make_shared<nc::config::DelayedAsyncExecutor>(reload_delay));

    atexit([] {
        // this callback is quite brutal, but works well. may need to find some more gentle approach
        g_Config->Commit();
        g_State->Commit();
        g_NetworkConnectionsConfig->Commit();
    });
}

- (void)setupCopyOperationRuntime
{
    assert(!m_CopyOperationRecoveryCoordinator);

    try {
        auto opened = nc::ops::OperationJournal::Open(m_StateDirectory.native());
        if( !opened ) {
            std::cerr << "Failed to open the Copy operation journal: " << magic_enum::enum_name(opened.error().code)
                      << ", system error " << opened.error().system_error << ".\n";
            return;
        }

        auto journal = std::make_shared<nc::ops::OperationJournal>(std::move(*opened));
        auto operation_center = nc::ops::OperationCenterCoordinator::Create(*journal);
        if( !operation_center ) {
            std::cerr << "Failed to initialize the Copy operation center: "
                      << magic_enum::enum_name(operation_center.error().code) << ".\n";
            return;
        }
        auto custodian = std::make_shared<nc::ops::CopyOperationRunReceiptCustodian>();
        auto coordinator = std::make_shared<nc::core::CopyOperationRecoveryCoordinator>(
            std::move(journal), std::move(custodian), m_StateDirectory.native());
        for( const auto &entry : coordinator->StartupInterruptedHistory() )
            std::cerr << "Copy operation journal contains an Interrupted startup entry for plan "
                      << entry.plan.Id().Value() << ".\n";
        m_CopyOperationRecoveryCoordinator = std::move(coordinator);
        m_OperationCenterCoordinator = std::move(*operation_center);
    } catch( const std::exception &error ) {
        std::cerr << "Failed to initialize the Copy operation runtime: " << error.what() << ".\n";
    } catch( ... ) {
        std::cerr << "Failed to initialize the Copy operation runtime due to an unknown error.\n";
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *) [[maybe_unused]] _app
{
    return NO;
}

+ (void)restoreWindowWithIdentifier:(NSString *)identifier
                              state:(NSCoder *) [[maybe_unused]] _state
                  completionHandler:(void (^)(NSWindow *, NSError *))completionHandler
{
    NSWindow *window = nil;
    if( [identifier isEqualToString:NCMainWindow.defaultIdentifier] )
        window = [g_Me allocateMainWindowRestoredBySystem].window;
    completionHandler(window, nil);
}

- (IBAction)onMainMenuNewWindow:(id) [[maybe_unused]] _sender
{
    auto ctrl = [self allocateMainWindowRestoredManually];
    [ctrl showWindow:self];
}

- (void)addMainWindow:(NCMainWindowController *)_wnd
{
    m_MainWindows.push_back(_wnd);
}

- (void)removeMainWindow:(NCMainWindowController *)_wnd
{
    auto it = std::ranges::find(m_MainWindows, _wnd);
    if( it != end(m_MainWindows) )
        m_MainWindows.erase(it);
}

- (void)windowWillClose:(NSNotification *)aNotification
{
    if( auto main_wnd = nc::objc_cast<NCMainWindow>(aNotification.object) )
        if( auto main_ctrl = nc::objc_cast<NCMainWindowController>(main_wnd.delegate) ) {
            dispatch_to_main_queue([=] { [self removeMainWindow:main_ctrl]; });
        }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *) [[maybe_unused]] _sender
{
    bool has_running_ops = false;
    auto controllers = self.mainWindowControllers;
    for( const auto &wincont : controllers )
        if( !wincont.operationsPool.Empty() || wincont.filePanelsState.operationSubmissionGate.HasPending() ||
            (wincont.terminalState && wincont.terminalState.isAnythingRunning) ) {
            has_running_ops = true;
            break;
        }

    if( has_running_ops ) {
        if( !AskToExitWithRunningOperations() )
            return NSTerminateCancel;

        for( const auto &wincont : controllers ) {
            wincont.filePanelsState.operationSubmissionGate.CancelAndWait();
            wincont.operationsPool.StopAndWaitForShutdown();
            [wincont.terminalState terminate];
        }
    }

    // last cleanup before shutting down here:
    if( m_Favorites )
        m_Favorites->StoreData(StateConfig(), "filePanel.favorites");

    return NSTerminateNow;
}

- (BOOL)applicationShouldOpenUntitledFile:(NSApplication *) [[maybe_unused]] _sender
{
    return true;
}

- (BOOL)applicationOpenUntitledFile:(NSApplication *)sender
{
    if( !m_FinishedLaunching )
        return false;

    if( !self.mainWindowControllers.empty() )
        return true;

    [self onMainMenuNewWindow:sender];

    return true;
}

- (void)drainFilesToOpen
{
    if( m_FilesToOpen.count == 0 )
        return;
    self.servicesHandler.OpenFiles(m_FilesToOpen);
    [m_FilesToOpen removeAllObjects];
}

- (BOOL)application:(NSApplication *) [[maybe_unused]] _sender openFile:(NSString *)filename
{
    [m_FilesToOpen addObjectsFromArray:@[filename]];
    dispatch_to_main_queue_after(250ms, [] { [g_Me drainFilesToOpen]; });
    return true;
}

- (void)application:(NSApplication *) [[maybe_unused]] _sender openFiles:(NSArray<NSString *> *)filenames
{
    [m_FilesToOpen addObjectsFromArray:filenames];
    dispatch_to_main_queue_after(250ms, [] { [g_Me drainFilesToOpen]; });
    [NSApp replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

- (void)openFolderService:(NSPasteboard *)pboard userData:(NSString *)data error:(__strong NSString **)error
{
    self.servicesHandler.OpenFolder(pboard, data, error);
}

- (void)revealItemService:(NSPasteboard *)pboard userData:(NSString *)data error:(__strong NSString **)error
{
    self.servicesHandler.RevealItem(pboard, data, error);
}

- (void)OnPreferencesCommand:(id) [[maybe_unused]] _sender
{
    ShowPreferencesWindow();
}

- (IBAction)OnShowHelp:(id) [[maybe_unused]] _sender
{
    const auto url = [NSBundle.mainBundle URLForResource:@"Help" withExtension:@"pdf"];
    [NSWorkspace.sharedWorkspace openURL:url];
}

- (IBAction)OnMenuToggleAdminMode:(id) [[maybe_unused]] _sender
{
    using nc::routedio::RoutedIO;
    if( RoutedIO::Instance().Enabled() )
        RoutedIO::Instance().TurnOff();
    else {
        const auto turned_on = RoutedIO::Instance().TurnOn();
        if( !turned_on )
            WarnAboutFailingToAccessPrivilegedHelper();
    }

    self.dock.SetAdminBadge(RoutedIO::Instance().Enabled());
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    static const int admin_mode_tag =
        self.actionsShortcutsManager.TagFromAction("menu.win_commander.toggle_admin_mode").value();
    const long tag = item.tag;

    if( tag == admin_mode_tag ) {
        bool enabled = nc::routedio::RoutedIO::Instance().Enabled();
        item.title = enabled ? NSLocalizedString(@"Disable Admin Mode", "Menu item title for disabling an admin mode")
                             : NSLocalizedString(@"Enable Admin Mode", "Menu item title for enabling an admin mode");
        return true;
    }

    return true;
}

- (NCConfigObjCBridge *)config
{
    static auto global_config_bridge = [[NCConfigObjCBridge alloc] initWithConfig:*g_Config];
    return global_config_bridge;
}

- (nc::config::Config &)globalConfig
{
    assert(g_Config);
    return *g_Config;
}

- (nc::config::Config &)stateConfig
{
    assert(g_State);
    return *g_State;
}

- (nc::panel::ExternalToolsStorage &)externalTools
{
    [[clang::no_destroy]] static //
        nc::panel::ExternalToolsStorage storage{g_ConfigExternalToolsList, self.globalConfig};
    return storage;
}

- (const std::shared_ptr<nc::panel::PanelViewLayoutsStorage> &)panelLayouts
{
    [[clang::no_destroy]] static auto i = std::make_shared<nc::panel::PanelViewLayoutsStorage>(g_ConfigLayoutsList);
    return i;
}

- (const std::shared_ptr<nc::panel::PanelViewLayoutsStorage> &)explorerPanelLayouts
{
    [[clang::no_destroy]] static auto i =
        std::make_shared<nc::panel::PanelViewLayoutsStorage>(g_ConfigExplorerLayoutsList);
    return i;
}

- (nc::ThemesManager &)themesManager
{
    assert(m_ThemesManager);
    return *m_ThemesManager;
}

- (ExternalEditorsStorage &)externalEditorsStorage
{
    static auto i = new ExternalEditorsStorage(g_ConfigExtEditorsList);
    return *i;
}

- (const std::shared_ptr<nc::panel::FavoriteLocationsStorage> &)favoriteLocationsStorage
{
    static std::once_flag once;
    std::call_once(once, [&] {
        using t = nc::panel::FavoriteLocationsStorageImpl;
        m_Favorites = std::make_shared<t>(StateConfig(), "filePanel.favorites", self.panelDataPersistency);
    });

    [[clang::no_destroy]] static const std::shared_ptr<nc::panel::FavoriteLocationsStorage> inst = m_Favorites;
    return inst;
}

- (bool)askToResetDefaults
{
    if( AskUserToResetDefaults() ) {
        ResetDefaults();
        return true;
    }
    return false;
}

- (void)addInternalViewerWindow:(InternalViewerWindowController *)_wnd
{
    auto lock = std::lock_guard{m_ViewerWindowsLock};
    m_ViewerWindows.emplace_back(_wnd);
}

- (void)removeInternalViewerWindow:(InternalViewerWindowController *)_wnd
{
    auto lock = std::lock_guard{m_ViewerWindowsLock};
    auto i = std::ranges::find(m_ViewerWindows, _wnd);
    if( i != std::end(m_ViewerWindows) )
        m_ViewerWindows.erase(i);
}

- (InternalViewerWindowController *)findInternalViewerWindowForPath:(const std::string &)_path
                                                              onVFS:(const VFSHostPtr &)_vfs
{
    auto lock = std::lock_guard{m_ViewerWindowsLock};
    auto i = std::ranges::find_if(m_ViewerWindows, [&](auto v) {
        return v.internalViewerController.filePath == _path && v.internalViewerController.fileVFS == _vfs;
    });
    return i != std::end(m_ViewerWindows) ? *i : nil;
    return nil;
}

- (InternalViewerWindowController *)retrieveInternalViewerWindowForPath:(const std::string &)_path
                                                                  onVFS:(const std::shared_ptr<VFSHost> &)_vfs
{
    dispatch_assert_main_queue();
    if( auto window = [self findInternalViewerWindowForPath:_path onVFS:_vfs] )
        return window;
    auto viewer_factory = [](NSRect rc) { return [NCAppDelegate.me makeViewerWithFrame:rc]; };
    auto ctrl = [self makeViewerController];
    auto window = [[InternalViewerWindowController alloc] initWithFilepath:_path
                                                                        at:_vfs
                                                             viewerFactory:viewer_factory
                                                                controller:ctrl];
    window.delegate = m_ViewerWindowDelegateBridge;

    return window;
}

- (IBAction)onMainMenuPerformShowFavorites:(id) [[maybe_unused]] _sender
{
    static __weak FavoritesWindowController *existing_window = nil;
    if( auto w = static_cast<FavoritesWindowController *>(existing_window) ) {
        [w show];
        return;
    }
    auto storage = []() -> nc::panel::FavoriteLocationsStorage & { return *NCAppDelegate.me.favoriteLocationsStorage; };
    FavoritesWindowController *window = [[FavoritesWindowController alloc] initWithFavoritesStorage:storage];
    auto provide_panel = []() -> std::vector<std::pair<VFSHostPtr, std::string>> {
        std::vector<std::pair<VFSHostPtr, std::string>> panel_paths;
        for( const auto &ctr : NCAppDelegate.me.mainWindowControllers ) {
            auto state = ctr.filePanelsState;
            auto paths = state.filePanelsCurrentPaths;
            for( const auto &p : paths )
                panel_paths.emplace_back(std::get<1>(p), std::get<0>(p));
        }
        return panel_paths;
    };
    window.provideCurrentUniformPaths = provide_panel;

    [window show];
    existing_window = window;
}

- (const std::shared_ptr<nc::panel::NetworkConnectionsManager> &)networkConnectionsManager
{
    [[clang::no_destroy]] static const auto mgr =
        std::make_shared<ConfigBackedNetworkConnectionsManager>(*g_NetworkConnectionsConfig, self.nativeFSManager);
    [[clang::no_destroy]] static const std::shared_ptr<nc::panel::NetworkConnectionsManager> int_ptr = mgr;
    return int_ptr;
}

- (nc::ops::AggregateProgressTracker &)operationsProgressTracker
{
    [[clang::no_destroy]] static const auto apt = [] {
        const auto apt = std::make_shared<nc::ops::AggregateProgressTracker>();
        apt->SetProgressCallback([](double _progress) { g_Me.dock.SetProgress(_progress); });
        return apt;
    }();
    return *apt;
}

- (nc::core::Dock &)dock
{
    static const auto instance = new nc::core::Dock;
    return *instance;
}

- (nc::core::VFSInstanceManager &)vfsInstanceManager
{
    static const auto instance = new nc::core::VFSInstanceManagerImpl;
    return *instance;
}

- (const std::shared_ptr<nc::panel::ClosedPanelsHistory> &)closedPanelsHistory
{
    [[clang::no_destroy]] static const auto impl = std::make_shared<nc::panel::ClosedPanelsHistoryImpl>();
    [[clang::no_destroy]] static const std::shared_ptr<nc::panel::ClosedPanelsHistory> history = impl;
    return history;
}

- (NCMainWindowController *)windowForExternalRevealRequest
{
    NCMainWindowController *target_window = nil;
    for( NSWindow *wnd in NSApplication.sharedApplication.orderedWindows )
        if( auto wc = nc::objc_cast<NCMainWindowController>(wnd.windowController) )
            if( wc.visibleActivePanelController ) {
                target_window = wc;
                break;
            }

    if( !target_window )
        target_window = [self allocateDefaultMainWindow];

    if( target_window )
        [target_window.window makeKeyAndOrderFront:self];

    return target_window;
}

- (nc::core::ServicesHandler &)servicesHandler
{
    auto window_locator = [] { return [g_Me windowForExternalRevealRequest]; };
    [[clang::no_destroy]] static nc::core::ServicesHandler handler(window_locator, self.nativeHostPtr);
    return handler;
}

- (nc::utility::NativeFSManager &)nativeFSManager
{
    return *m_NativeFSManager;
}

static void DoTemporaryFileStoragePurge()
{
    assert(g_TemporaryFileStorage != nullptr);
    const auto deadline = time(nullptr) - (60l * 60l * 24l); // 24 hours back
    g_TemporaryFileStorage->Purge(deadline);

    dispatch_after(6h, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_BACKGROUND, 0), DoTemporaryFileStoragePurge);
}

- (nc::utility::TemporaryFileStorage &)temporaryFileStorage
{
    const auto instance = [] {
        const std::string &base_dir = nc::base::CommonPaths::AppTemporaryDirectory();
        const auto prefix = nc::utility::GetBundleID() + ".tmp.";
        g_TemporaryFileStorage = new nc::utility::TemporaryFileStorageImpl(base_dir, prefix);
        dispatch_to_background(DoTemporaryFileStoragePurge);
        return g_TemporaryFileStorage;
    }();

    return *instance;
}

- (nc::viewer::History &)internalViewerHistory
{
    static const auto history_state_path = "viewer.history";
    static const auto instance = [] {
        auto inst = new nc::viewer::History(*g_Config, *g_State, history_state_path);
        auto center = NSNotificationCenter.defaultCenter;
        // Save the history upon application shutdown
        [center addObserverForName:NSApplicationWillTerminateNotification
                            object:nil
                             queue:nil
                        usingBlock:^([[maybe_unused]] NSNotification *_Nonnull note) {
                          inst->SaveToStateConfig();
                        }];
        return inst;
    }();
    return *instance;
}

- (nc::utility::UTIDB &)utiDB
{
    [[clang::no_destroy]] static nc::utility::UTIDBImpl uti_db;
    return uti_db;
}

- (nc::vfs::NativeHost &)nativeHost
{
    return *m_NativeHost;
}

- (const std::shared_ptr<nc::vfs::NativeHost> &)nativeHostPtr
{
    return m_NativeHost;
}

- (nc::utility::FSEventsFileUpdate &)fsEventsFileUpdate
{
    return *m_FSEventsFileUpdate;
}

- (nc::ops::PoolEnqueueFilter &)poolEnqueueFilter
{
    return m_PoolEnqueueFilter;
}

- (std::shared_ptr<nc::ops::OperationJournal>)operationJournal
{
    if( !m_CopyOperationRecoveryCoordinator )
        return {};
    return m_CopyOperationRecoveryCoordinator->CurrentJournal();
}

- (std::shared_ptr<nc::ops::CopyOperationRunReceiptCustodian>)copyOperationRunReceiptCustodian
{
    if( !m_CopyOperationRecoveryCoordinator )
        return {};
    return m_CopyOperationRecoveryCoordinator->CurrentRunReceiptCustodian();
}

- (std::shared_ptr<nc::ops::OperationCenterCoordinator>)operationCenterCoordinator
{
    return m_OperationCenterCoordinator;
}

- (const std::shared_ptr<nc::core::CopyOperationRecoveryCoordinator> &)copyOperationRecoveryCoordinator
{
    return m_CopyOperationRecoveryCoordinator;
}

- (IBAction)onMainMenuShowLogs:(id)_sender
{
    if( m_LogWindowController == nil )
        m_LogWindowController = [[NCSpdLogWindowController alloc] initWithLogs:Loggers()];
    [m_LogWindowController showWindow:self];
}

- (nc::panel::TagsStorage &)tagsStorage
{
    [[clang::no_destroy]] static nc::panel::TagsStorage storage(GlobalConfig(), g_ConfigFinderTags);
    static std::once_flag once;
    std::call_once(once, [] {
        if( !storage.Initialized() ) {
            dispatch_to_background([] {
                auto tags = nc::utility::Tags::GatherAllItemsTags();
                storage.Set(tags);
            });
        }
    });
    return storage;
}

- (nc::viewer::hl::SettingsStorage &)syntaxHighlightingSettingsStorage
{
    // if the overrides directory doesn't exist - create it. Check it only once per run
    static std::once_flag once;
    std::call_once(once, [self] {
        const std::filesystem::path overrides_dir = self.supportDirectory / "SyntaxHighlighting";
        std::error_code ec = {};
        if( !std::filesystem::exists(overrides_dir, ec) ) {
            std::filesystem::create_directory(overrides_dir, ec);
        }
    });

    [[clang::no_destroy]] static nc::viewer::hl::FileSettingsStorage storage{
        [NSBundle.mainBundle pathForResource:@"SyntaxHighlighting" ofType:@""].fileSystemRepresentation,
        self.supportDirectory / "SyntaxHighlighting"};

    return storage;
}

- (nc::panel::PanelDataPersistency &)panelDataPersistency
{
    [[clang::no_destroy]] static nc::panel::PanelDataPersistency persistency{*self.networkConnectionsManager};
    return persistency;
}

- (nc::explorer::ExplorerViewSettingsPersistence &)explorerViewSettingsPersistence
{
    [[clang::no_destroy]] static nc::explorer::ExplorerViewSettingsPersistence persistence{
        StateConfig(), self.panelDataPersistency};
    return persistence;
}

- (nc::utility::ActionsShortcutsManager &)actionsShortcutsManager
{
    [[clang::no_destroy]] static nc::core::ActionsShortcutsManager manager(
        g_ActionsTags, g_DefaultActionShortcuts, GlobalConfig());
    return manager;
}

- (nc::core::CommandRegistry &)commandRegistry
{
    assert(m_CommandRegistry);
    return *m_CommandRegistry;
}

@end

static std::optional<std::string> Load(const std::string &_filepath)
{
    std::ifstream in(_filepath, std::ios::in | std::ios::binary);
    if( !in )
        return std::nullopt;

    std::string contents;
    in.seekg(0, std::ios::end);
    contents.resize(in.tellg());
    in.seekg(0, std::ios::beg);
    in.read(contents.data(), contents.size());
    in.close();
    return contents;
}

@implementation NCViewerWindowDelegateBridge

- (void)viewerWindowWillShow:(InternalViewerWindowController *)_window
{
    [NCAppDelegate.me addInternalViewerWindow:_window];
}

- (void)viewerWindowWillClose:(InternalViewerWindowController *)_window
{
    [NCAppDelegate.me removeInternalViewerWindow:_window];
}

@end

namespace nc::bootstrap {

nc::vfs::NativeHost &NativeVFSHostInstance() noexcept
{
    assert(g_Me != nil);
    return NCAppDelegate.me.nativeHost;
}

} // namespace nc::bootstrap
