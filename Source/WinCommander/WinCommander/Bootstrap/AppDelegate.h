// Copyright (C) 2013-2025 Michael Kazakov. Subject to GNU General Public License version 3.

#pragma once

#include <VFS/VFS_fwd.h>
#include <Cocoa/Cocoa.h>
#include <filesystem>

@class NCConfigObjCBridge;
@class NCMainWindowController;
@class InternalViewerWindowController;
class ExternalEditorsStorage;

namespace nc {

class ThemesManager;

namespace config {
class Config;
}

namespace utility {
class ActionsShortcutsManager;
class FSEventsFileUpdate;
class NativeFSManager;
class TemporaryFileStorage;
class UTIDB;
} // namespace utility

namespace core {
class CommandRegistry;
class CopyOperationRecoveryCoordinator;
class VFSInstanceManager;
class ServicesHandler;
} // namespace core

namespace explorer {
class ExplorerViewSettingsPersistence;
} // namespace explorer

namespace ops {
class AggregateProgressTracker;
class CopyOperationRunReceiptCustodian;
class OperationCenterCoordinator;
class OperationJournal;
class PoolEnqueueFilter;
} // namespace ops

namespace panel {
class PanelViewLayoutsStorage;
class FavoriteLocationsStorage;
class ClosedPanelsHistory;
class ExternalToolsStorage;
class TagsStorage;
class NetworkConnectionsManager;
class PanelDataPersistency;
} // namespace panel

namespace viewer {
class History;
}
namespace viewer::hl {
class SettingsStorage;
}

namespace vfs {
class NativeHost;
}

} // namespace nc

@interface NCAppDelegate : NSObject <NSApplicationDelegate, NSWindowRestoration, NSMenuItemValidation>

- (InternalViewerWindowController *)findInternalViewerWindowForPath:(const std::string &)_path
                                                              onVFS:(const std::shared_ptr<VFSHost> &)_vfs;
/**
 * Searches for an existing window with corresponding path,
 * if it is not found - allocates a new non-shown one.
 */
- (InternalViewerWindowController *)retrieveInternalViewerWindowForPath:(const std::string &)_path
                                                                  onVFS:(const std::shared_ptr<VFSHost> &)_vfs;

/**
 * Runs a modal dialog window, which asks user if he wants to reset app settings.
 * Returns true if defaults were actually reset.
 */
- (bool)askToResetDefaults;

/** Returns all main windows currently present. */
@property(nonatomic, readonly) const std::vector<NCMainWindowController *> &mainWindowControllers;

/**
 * Equal to (NCAppDelegate*) ((NSApplication*)NSApp).delegate.
 */
+ (NCAppDelegate *)me;

/**
 * Support dir, ~/Library/Application Support/<CFBundleExecutable>/.
 * Is in Containers for Sandboxes versions
 */
@property(nonatomic, readonly) const std::filesystem::path &supportDirectory;

/**
 * By default this dir is ~/Library/Application Support/<CFBundleExecutable>/Config/.
 * May change in the future.
 */
@property(nonatomic, readonly) const std::filesystem::path &configDirectory;

/**
 * This dir is ~/Library/Application Support/<CFBundleExecutable>/State/.
 */
@property(nonatomic, readonly) const std::filesystem::path &stateDirectory;

@property(nonatomic, readonly) NCConfigObjCBridge *config;

@property(nonatomic, readonly) nc::config::Config &globalConfig;

@property(nonatomic, readonly) nc::config::Config &stateConfig;

@property(nonatomic, readonly) nc::panel::ExternalToolsStorage &externalTools;

@property(nonatomic, readonly) const std::shared_ptr<nc::panel::PanelViewLayoutsStorage> &panelLayouts;

// Explorer owns its view presets so resizing or reordering its columns never mutates Commander layouts.
@property(nonatomic, readonly) const std::shared_ptr<nc::panel::PanelViewLayoutsStorage> &explorerPanelLayouts;

@property(nonatomic, readonly) nc::ThemesManager &themesManager;

@property(nonatomic, readonly) ExternalEditorsStorage &externalEditorsStorage;

@property(nonatomic, readonly) const std::shared_ptr<nc::panel::FavoriteLocationsStorage> &favoriteLocationsStorage;

@property(nonatomic, readonly) const std::shared_ptr<nc::panel::NetworkConnectionsManager> &networkConnectionsManager;

@property(nonatomic, readonly) nc::ops::AggregateProgressTracker &operationsProgressTracker;

@property(nonatomic, readonly) const std::shared_ptr<nc::panel::ClosedPanelsHistory> &closedPanelsHistory;

@property(nonatomic, readonly) nc::core::VFSInstanceManager &vfsInstanceManager;

@property(nonatomic, readonly) nc::core::ServicesHandler &servicesHandler;

@property(nonatomic, readonly) nc::utility::NativeFSManager &nativeFSManager;

@property(nonatomic, readonly) nc::utility::TemporaryFileStorage &temporaryFileStorage;

@property(nonatomic, readonly) nc::viewer::History &internalViewerHistory;

@property(nonatomic, readonly) nc::utility::UTIDB &utiDB;

@property(nonatomic, readonly) nc::vfs::NativeHost &nativeHost;

@property(nonatomic, readonly) const std::shared_ptr<nc::vfs::NativeHost> &nativeHostPtr;

@property(nonatomic, readonly) nc::utility::FSEventsFileUpdate &fsEventsFileUpdate;

@property(nonatomic, readonly) nc::ops::PoolEnqueueFilter &poolEnqueueFilter;

/** Current process-owned Copy journal. Empty when durable runtime initialization or reopen failed. */
@property(nonatomic, readonly) std::shared_ptr<nc::ops::OperationJournal> operationJournal;

/** Process-owned Copy run-receipt custody. Empty when durable runtime initialization failed. */
@property(nonatomic, readonly) std::shared_ptr<nc::ops::CopyOperationRunReceiptCustodian>
    copyOperationRunReceiptCustodian;

/** Process-owned value-model admission/lifecycle coordinator for reviewed Copy. */
@property(nonatomic, readonly) std::shared_ptr<nc::ops::OperationCenterCoordinator> operationCenterCoordinator;

/** Query and explicit bounded recovery boundary for Copy journal history and retained receipts. */
@property(nonatomic, readonly)
    const std::shared_ptr<nc::core::CopyOperationRecoveryCoordinator> &copyOperationRecoveryCoordinator;

@property(nonatomic, readonly) nc::panel::TagsStorage &tagsStorage;

@property(nonatomic, readonly) nc::viewer::hl::SettingsStorage &syntaxHighlightingSettingsStorage;

@property(nonatomic, readonly) nc::panel::PanelDataPersistency &panelDataPersistency;

/** Process-owned exact per-location Explorer presentation settings. */
@property(nonatomic, readonly) nc::explorer::ExplorerViewSettingsPersistence &explorerViewSettingsPersistence;

@property(nonatomic, readonly) nc::utility::ActionsShortcutsManager &actionsShortcutsManager;

@property(nonatomic, readonly) nc::core::CommandRegistry &commandRegistry;

@end
