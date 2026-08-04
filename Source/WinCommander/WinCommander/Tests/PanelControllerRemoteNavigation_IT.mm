// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Base/UUID.h>
#include <Config/ConfigImpl.h>
#include <Config/NonPersistentOverwritesStorage.h>
#include <Panel/PanelData.h>
#include <Utility/ActionsShortcutsManager.h>
#include <Utility/FSEventsFileUpdate.h>
#include <Utility/NativeFSManager.h>
#include <VFS/NetFTP.h>
#include <VFS/NetSFTP.h>
#include <VFS/NetWebDAV.h>
#include <VFS/Native.h>
#include <VFS/VFSListingInput.h>
#include <VFSIcon/IconRepository.h>
#include <WinCommander/Core/VFSInstanceManagerImpl.h>
#include <WinCommander/States/FilePanels/PanelController.h>
#include <WinCommander/States/FilePanels/PanelView.h>
#include <WinCommander/States/FilePanels/PanelViewFooter.h>
#include <WinCommander/States/FilePanels/PanelViewHeader.h>
#include <WinCommander/States/FilePanels/PanelViewLayoutSupport.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <tuple>
#include <vector>

namespace {

using namespace std::chrono_literals;
using nc::core::PaneLifecycleCommitted;
using nc::core::PaneLifecycleEvent;
using nc::core::PaneLifecycleFailed;
using nc::core::PaneLifecycleStarted;
using nc::core::PaneRequestKind;
using nc::panel::DirectoryChangeRequest;

constexpr const char *g_FtpAddress = "127.0.0.1";
constexpr const char *g_FtpUser = "ftpuser";
constexpr const char *g_FtpPassword = "ftpuserpasswd";
constexpr long g_FtpPort = 9021;
constexpr const char *g_SftpUser = "user1";
constexpr const char *g_SftpPassword = "Oc6har5tOu34";
constexpr long g_SftpPort = 9022;
constexpr const char *g_WebDAVUser = "r2d2";
constexpr const char *g_WebDAVPassword = "Hello";
constexpr int g_WebDAVPort = 9080;

bool RunMainLoopUntil(const std::function<bool()> &_predicate,
                      const std::chrono::milliseconds _timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + _timeout;
    while( true ) {
        @autoreleasepool {
            if( _predicate() )
                return true;
            if( std::chrono::steady_clock::now() >= deadline )
                return false;
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        }
    }
}

nc::core::VFSInstanceManagerImpl &TestVFSInstanceManager()
{
    // The production contract requires managers to outlive every promise they issue.
    static auto *const manager = new nc::core::VFSInstanceManagerImpl;
    return *manager;
}

class TestHeaderTheme final : public nc::panel::HeaderTheme
{
public:
    NSFont *Font() const override { return [NSFont systemFontOfSize:12.0]; }
    NSColor *TextColor() const override { return NSColor.textColor; }
    NSColor *ActiveTextColor() const override { return NSColor.textColor; }
    NSColor *ActiveBackgroundColor() const override { return NSColor.controlBackgroundColor; }
    NSColor *InactiveBackgroundColor() const override { return NSColor.controlBackgroundColor; }
    NSColor *SeparatorColor() const override { return NSColor.separatorColor; }
    void ObserveChanges(std::function<void()>) override {}
};

class TestFooterTheme final : public nc::panel::FooterTheme
{
public:
    NSFont *Font() const override { return [NSFont systemFontOfSize:12.0]; }
    NSColor *TextColor() const override { return NSColor.textColor; }
    NSColor *ActiveTextColor() const override { return NSColor.textColor; }
    NSColor *ActiveBackgroundColor() const override { return NSColor.controlBackgroundColor; }
    NSColor *InactiveBackgroundColor() const override { return NSColor.controlBackgroundColor; }
    NSColor *SeparatorsColor() const override { return NSColor.separatorColor; }
    void ObserveChanges(std::function<void()>) override {}
};

class TestIconRepository final : public nc::vfsicon::IconRepository
{
public:
    bool IsValidSlot(SlotKey) const override { return false; }
    NSImage *AvailableIconForSlot(SlotKey) const override { return nil; }
    NSImage *AvailableIconForListingItem(const VFSListingItem &) const override { return nil; }
    SlotKey Register(const VFSListingItem &) override { return InvalidKey; }
    std::vector<SlotKey> AllSlots() const override { return {}; }
    void Unregister(SlotKey) override {}
    void ScheduleIconProduction(SlotKey, const VFSListingItem &) override {}
    void SetUpdateCallback(std::function<void(SlotKey, NSImage *)>) override {}
    void SetPxSize(int) override {}
};

class TestActionsShortcutsManager final : public nc::utility::ActionsShortcutsManager
{
public:
    std::optional<int> TagFromAction(std::string_view) const noexcept override { return std::nullopt; }
    std::optional<std::string_view> ActionFromTag(int) const noexcept override { return std::nullopt; }
    std::optional<Shortcuts> ShortcutsFromAction(std::string_view) const noexcept override { return std::nullopt; }
    std::optional<Shortcuts> ShortcutsFromTag(int) const noexcept override { return std::nullopt; }
    std::optional<Shortcuts> DefaultShortcutsFromTag(int) const noexcept override { return std::nullopt; }
    std::optional<ActionTags>
    ActionTagsFromShortcut(Shortcut, std::string_view = {}) const noexcept override
    {
        return std::nullopt;
    }
    std::optional<int>
    FirstOfActionTagsFromShortcut(std::span<const int>, Shortcut, std::string_view = {}) const noexcept override
    {
        return std::nullopt;
    }
    std::vector<std::pair<std::string, int>> AllShortcuts() const override { return {}; }
    void RevertToDefaults() override {}
    bool SetShortcutOverride(std::string_view, Shortcut) override { return false; }
    bool SetShortcutsOverride(std::string_view, std::span<const Shortcut>) override { return false; }
};

class TestNativeFSManager final : public nc::utility::NativeFSManager
{
public:
    std::vector<Info> Volumes() const override { return {}; }
    Info VolumeFromFD(int) const noexcept override { return nullptr; }
    Info VolumeFromPath(std::string_view) const noexcept override { return nullptr; }
    Info VolumeFromPathFast(std::string_view) const noexcept override { return nullptr; }
    Info VolumeFromMountPoint(std::string_view) const noexcept override { return nullptr; }
    void UpdateSpaceInformation(const Info &) override {}
    void EjectVolumeContainingPath(const std::string &) override {}
    bool IsVolumeContainingPathEjectable(const std::string &) override { return false; }
};

class TestFSEventsFileUpdate final : public nc::utility::FSEventsFileUpdate
{
public:
    uint64_t AddWatchPath(const std::filesystem::path &, std::function<void()>) override { return 0; }
    void RemoveWatchPathWithToken(uint64_t) override {}
};

class TestDirectoryAccessProvider final : public nc::panel::DirectoryAccessProvider
{
public:
    bool HasAccess(PanelController *, const std::string &, VFSHost &) override { return true; }
    bool RequestAccessSync(PanelController *, const std::string &, VFSHost &) override { return true; }
};

VFSListingPtr SeedListing(const VFSHostPtr &_host)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/";
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = _host;
    input.filenames.emplace_back("seed.txt");
    input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
    input.unix_types.emplace_back(DT_REG);
    input.sizes.insert(0, 0);
    return VFSListing::Build(std::move(input));
}

class ScopedRemoteDirectory
{
public:
    ScopedRemoteDirectory(VFSHostPtr _host, std::string _path)
        : m_Host(std::move(_host)), m_Path(std::move(_path))
    {
    }

    ~ScopedRemoteDirectory()
    {
        if( !m_Host )
            return;
        std::ignore = m_Host->Unlink(FilePath("first.txt"));
        std::ignore = m_Host->Unlink(FilePath("second.txt"));
        std::ignore = m_Host->RemoveDirectory(m_Path);
    }

    const std::string &Path() const noexcept { return m_Path; }
    std::string DirectoryPath() const { return m_Path + "/"; }
    std::string FilePath(std::string_view _filename) const { return m_Path + "/" + std::string{_filename}; }
    void SetHost(VFSHostPtr _host) { m_Host = std::move(_host); }

private:
    VFSHostPtr m_Host;
    std::string m_Path;
};

bool TryCreateFixtureFile(VFSHost &_host, const std::string &_path)
{
    const std::expected<VFSFilePtr, nc::Error> file_result = _host.CreateFile(_path);
    if( !file_result )
        return false;
    const VFSFilePtr &file = *file_result;
    constexpr std::string_view contents = "remote-navigation-fixture\n";
    return file->Open(VFSFlags::OF_Write | VFSFlags::OF_Create) && file->SetUploadSize(contents.size()) &&
           file->WriteFile(contents.data(), contents.size()) && file->Close();
}

void CreateFixtureFile(VFSHost &_host, const std::string &_path)
{
    REQUIRE(TryCreateFixtureFile(_host, _path));
}

class ScopedRemoteFixtureRestart
{
public:
    explicit ScopedRemoteFixtureRestart(std::string _container_name) : m_ContainerName(std::move(_container_name)) {}

    [[nodiscard]] bool Stop()
    {
        if( std::system(("docker stop " + m_ContainerName + " >/dev/null").c_str()) != 0 )
            return false;
        m_RestartRequired = true;
        return true;
    }

    [[nodiscard]] bool Start()
    {
        if( !m_RestartRequired )
            return true;
        if( std::system(("docker start " + m_ContainerName + " >/dev/null").c_str()) != 0 )
            return false;
        m_RestartRequired = false;
        return true;
    }

    [[nodiscard]] bool Pause()
    {
        if( m_RestartRequired || m_PauseRequired )
            return false;
        if( std::system(("docker pause " + m_ContainerName + " >/dev/null").c_str()) != 0 )
            return false;
        m_PauseRequired = true;
        return true;
    }

    [[nodiscard]] bool Unpause()
    {
        if( !m_PauseRequired )
            return true;
        if( std::system(("docker unpause " + m_ContainerName + " >/dev/null").c_str()) != 0 )
            return false;
        m_PauseRequired = false;
        return true;
    }

    ~ScopedRemoteFixtureRestart()
    {
        if( m_PauseRequired )
            (void)std::system(("docker unpause " + m_ContainerName + " >/dev/null").c_str());
        if( m_RestartRequired )
            (void)std::system(("docker start " + m_ContainerName + " >/dev/null").c_str());
    }

private:
    std::string m_ContainerName;
    bool m_RestartRequired = false;
    bool m_PauseRequired = false;
};

} // namespace

@interface PanelControllerRemoteNavigationTestFooter : NCPanelViewFooter
@end

@implementation PanelControllerRemoteNavigationTestFooter

- (void)updateFocusedItem:(const VFSListingItem &)_item VD:(nc::panel::data::ItemVolatileData)_vd
{
    (void)_item;
    (void)_vd;
}

@end

@interface PanelControllerRemoteNavigationTestView : PanelView

- (instancetype)initWithNativeHost:(nc::vfs::NativeHost &)_native_host
           actionsShortcutsManager:(const nc::utility::ActionsShortcutsManager &)_shortcuts;

@end

@implementation PanelControllerRemoteNavigationTestView {
    NSProgressIndicator *m_TestBusyIndicator;
}

- (instancetype)initWithNativeHost:(nc::vfs::NativeHost &)_native_host
           actionsShortcutsManager:(const nc::utility::ActionsShortcutsManager &)_shortcuts
{
    auto header = [[NCPanelViewHeader alloc] initWithFrame:NSZeroRect
                                                     theme:std::make_unique<TestHeaderTheme>()];
    auto footer = [[PanelControllerRemoteNavigationTestFooter alloc] initWithFrame:NSZeroRect
                                                                              theme:std::make_unique<TestFooterTheme>()];
    const nc::panel::PresentationFactory factory;
    self = [super initWithFrame:NSMakeRect(0, 0, 100, 100)
                 iconRepository:std::make_unique<TestIconRepository>()
        actionsShortcutsManager:_shortcuts
                      nativeVFS:_native_host
                         header:header
                         footer:footer
            presentationFactory:factory];
    if( self )
        m_TestBusyIndicator = [NSProgressIndicator new];
    return self;
}

- (void)setPresentationLayout:(const nc::panel::PanelViewLayout &)_layout
{
    (void)_layout;
}

- (void)savePathState {}
- (void)dataUpdated {}
- (void)panelChangedWithFocusedFilename:(const std::string &)_filename loadPreviousState:(bool)_load_previous
{
    (void)_filename;
    (void)_load_previous;
}
- (NSProgressIndicator *)busyIndicator { return m_TestBusyIndicator; }

@end

namespace {

class PanelControllerRemoteNavigationFixture
{
public:
    explicit PanelControllerRemoteNavigationFixture(const VFSHostPtr &_remote_host)
    {
        m_NativeHost = std::make_shared<nc::vfs::NativeHost>(m_NativeFSManager, m_FSEvents);
        m_Layouts = std::make_shared<nc::panel::PanelViewLayoutsStorage>(
            "tests.panel_controller_remote_navigation.layouts", m_Config);
        m_View = [[PanelControllerRemoteNavigationTestView alloc] initWithNativeHost:*m_NativeHost
                                                               actionsShortcutsManager:m_Shortcuts];
        nc::panel::ContextMenuProvider context_menu_provider =
            [](std::vector<VFSListingItem>, PanelController *) {
                return static_cast<NCPanelContextMenu *>(nil);
            };
        m_Controller = [[PanelController alloc] initWithView:m_View
                                                     paneId:nc::core::PaneId{711}
                                                    layouts:m_Layouts
                                                     config:m_Config
                                         vfsInstanceManager:TestVFSInstanceManager()
                                    directoryAccessProvider:m_AccessProvider
                                        contextMenuProvider:std::move(context_menu_provider)
                                            nativeFSManager:m_NativeFSManager
                                                 nativeHost:*m_NativeHost];
        [m_Controller loadListing:SeedListing(_remote_host)];
    }

    ~PanelControllerRemoteNavigationFixture()
    {
        [m_Controller CancelBackgroundOperations];
        const bool drained = RunMainLoopUntil([&] { return !m_Controller || !m_Controller.isDoingBackgroundLoading; });
        if( !drained ) {
            std::fputs("PanelControllerRemoteNavigationFixture: background queues did not drain\n", stderr);
            std::terminate();
        }
        m_View.delegate = nil;
        m_View.nextResponder = nil;
        __weak PanelController *weak_controller = m_Controller;
        m_Controller = nil;
        const bool controller_released = RunMainLoopUntil([&] { return weak_controller == nil; });
        if( !controller_released ) {
            std::fputs("PanelControllerRemoteNavigationFixture: controller was retained past teardown\n", stderr);
            std::terminate();
        }
        m_View = nil;
    }

    std::shared_ptr<DirectoryChangeRequest> Request(const VFSHostPtr &_remote_host, std::string _path)
    {
        auto request = std::make_shared<DirectoryChangeRequest>();
        request->RequestedDirectory = std::move(_path);
        request->VFS = _remote_host;
        request->PerformAsynchronous = true;
        request->InitiatedByUser = true;
        return request;
    }

    PanelController *const &Controller() const { return m_Controller; }

private:
    nc::config::ConfigImpl m_Config{
        R"json({
            "filePanel": {
                "general": {
                    "showDotDotEntry": true,
                    "showLocalizedFilenames": false,
                    "ignoreDirectoriesOnSelectionWithMask": false
                },
                "FinderTags": {"enable": false},
                "quickSearch": {
                    "whereToFind": 0,
                    "softFiltering": false,
                    "typingView": false,
                    "keyOption": 4,
                    "ignoreCharacters": ""
                }
            },
            "tests": {"panel_controller_remote_navigation": {"layouts": []}}
        })json",
        std::make_shared<nc::config::NonPersistentOverwritesStorage>("")};
    TestNativeFSManager m_NativeFSManager;
    TestFSEventsFileUpdate m_FSEvents;
    TestActionsShortcutsManager m_Shortcuts;
    TestDirectoryAccessProvider m_AccessProvider;
    std::shared_ptr<nc::vfs::NativeHost> m_NativeHost;
    std::shared_ptr<nc::panel::PanelViewLayoutsStorage> m_Layouts;
    PanelControllerRemoteNavigationTestView *m_View = nil;
    PanelController *m_Controller = nil;
};

} // namespace

#define PREFIX "PanelController remote navigation "

enum class RemoteRefreshLifecycle : uint8_t {
    DirectUserRefresh,
    UserRefreshWithSoftSuccessor
};

enum class RemoteEndpointFault : uint8_t {
    Stopped,
    Paused
};

static void VerifyRemoteNavigationAndForcedRefresh(const VFSHostPtr &_controller_host,
                                                   const VFSHostPtr &_shadow_host,
                                                   const std::string_view _parent_directory,
                                                   const std::string_view _provider_name,
                                                   const RemoteRefreshLifecycle _refresh_lifecycle)
{
    REQUIRE(_controller_host);
    REQUIRE(_shadow_host);
    REQUIRE_FALSE(_parent_directory.empty());
    const std::string parent = _parent_directory == "/" ? "" : std::string{_parent_directory};
    ScopedRemoteDirectory directory{
        _shadow_host,
        parent + "/WinCommanderPanelController-" + std::string{_provider_name} + "-" +
            nc::base::UUID::Generate().ToString()};
    REQUIRE(_shadow_host->CreateDirectory(directory.Path(), 0755));
    CreateFixtureFile(*_shadow_host, directory.FilePath("first.txt"));

    PanelControllerRemoteNavigationFixture fixture{_controller_host};
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    const std::string remote_path = directory.DirectoryPath();
    REQUIRE([fixture.Controller() GoToDirWithContext:fixture.Request(_controller_host, remote_path)].has_value());
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload);
    }));

    REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));
    CHECK(events.front().descriptor.kind == PaneRequestKind::Navigation);
    CHECK(events.front().descriptor.initiated_by_user);
    REQUIRE(events.front().descriptor.target);
    CHECK(events.front().descriptor.target->host == _controller_host);
    CHECK(events.front().descriptor.target->path == remote_path);
    const VFSListingPtr first_listing = fixture.Controller().data.ListingPtr();
    REQUIRE(first_listing);
    CHECK(first_listing->IsUniform());
    CHECK(first_listing->Host() == _controller_host);
    CHECK(first_listing->Directory() == remote_path);
    std::string initial_filenames;
    for( const VFSListingItem &item : *first_listing ) {
        if( !initial_filenames.empty() )
            initial_filenames.append(", ");
        initial_filenames.append(item.Filename());
    }
    CAPTURE(_provider_name, remote_path, initial_filenames);
    CHECK(std::ranges::any_of(*first_listing, [](const VFSListingItem &_item) {
        return _item.Filename() == "first.txt";
    }));

    CreateFixtureFile(*_shadow_host, directory.FilePath("second.txt"));
    const size_t refresh_event_begin = events.size();
    const size_t expected_refresh_events =
        _refresh_lifecycle == RemoteRefreshLifecycle::DirectUserRefresh ? 2 : 4;
    REQUIRE([fixture.Controller() submitUserRefresh]);
    REQUIRE(RunMainLoopUntil([&] {
        const VFSListingPtr refreshed_listing = fixture.Controller().data.ListingPtr();
        return events.size() == refresh_event_begin + expected_refresh_events &&
               std::holds_alternative<PaneLifecycleCommitted>(events.back().payload) &&
               refreshed_listing && refreshed_listing != first_listing &&
               std::ranges::any_of(*refreshed_listing, [](const VFSListingItem &_item) {
                   return _item.Filename() == "second.txt";
               });
    }));

    REQUIRE(events.size() >= refresh_event_begin + 2);
    REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events[refresh_event_begin].payload));
    CHECK(events[refresh_event_begin].descriptor.kind == PaneRequestKind::Refresh);
    CHECK(events[refresh_event_begin].descriptor.initiated_by_user);
    for( size_t index = refresh_event_begin + 1; index + 1 < events.size(); ++index )
        CHECK_FALSE(std::holds_alternative<PaneLifecycleCommitted>(events[index].payload));
    REQUIRE(std::holds_alternative<PaneLifecycleCommitted>(events.back().payload));
    CHECK(events.back().descriptor.kind == PaneRequestKind::Refresh);

    if( _refresh_lifecycle == RemoteRefreshLifecycle::DirectUserRefresh ) {
        CHECK(events.back().request_id == events[refresh_event_begin].request_id);
    }
    else {
        REQUIRE(std::holds_alternative<nc::core::PaneLifecycleSuperseded>(events[refresh_event_begin + 1].payload));
        const auto &superseded = std::get<nc::core::PaneLifecycleSuperseded>(events[refresh_event_begin + 1].payload);
        CHECK(events[refresh_event_begin + 1].request_id == events[refresh_event_begin].request_id);
        CHECK(superseded.replacement == events[refresh_event_begin + 2].request_id);
        REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events[refresh_event_begin + 2].payload));
        CHECK(events[refresh_event_begin + 2].descriptor.kind == PaneRequestKind::Refresh);
        CHECK_FALSE(events[refresh_event_begin + 2].descriptor.initiated_by_user);
        CHECK(events.back().request_id == events[refresh_event_begin + 2].request_id);
    }

    const VFSListingPtr refreshed_listing = fixture.Controller().data.ListingPtr();
    REQUIRE(refreshed_listing);
    CHECK(refreshed_listing->IsUniform());
    CHECK(refreshed_listing->Host() == _controller_host);
    CHECK(refreshed_listing->Directory() == remote_path);
}

TEST_CASE(PREFIX "loads an FTP directory then force-refreshes a shadow-host mutation", "[integration][docker][ftp]")
{
    const VFSHostPtr controller_host =
        std::make_shared<nc::vfs::FTPHost>(g_FtpAddress, g_FtpUser, g_FtpPassword, "/", g_FtpPort);
    const VFSHostPtr shadow_host =
        std::make_shared<nc::vfs::FTPHost>(g_FtpAddress, g_FtpUser, g_FtpPassword, "/", g_FtpPort);
    VerifyRemoteNavigationAndForcedRefresh(
        controller_host, shadow_host, "/", "FTP", RemoteRefreshLifecycle::UserRefreshWithSoftSuccessor);
}

TEST_CASE(PREFIX "loads an SFTP directory then force-refreshes a shadow-host mutation", "[integration][docker][sftp]")
{
    const auto controller_host =
        std::make_shared<nc::vfs::SFTPHost>(g_FtpAddress, g_SftpUser, g_SftpPassword, "", g_SftpPort);
    const auto shadow_host = std::make_shared<nc::vfs::SFTPHost>(g_FtpAddress, g_SftpUser, g_SftpPassword, "", g_SftpPort);
    const std::string home_directory = controller_host->HomeDir();
    VerifyRemoteNavigationAndForcedRefresh(
        controller_host, shadow_host, home_directory, "SFTP", RemoteRefreshLifecycle::DirectUserRefresh);
}

TEST_CASE(PREFIX "loads a WebDAV directory then force-refreshes a shadow-host mutation", "[integration][docker][webdav]")
{
    const VFSHostPtr controller_host =
        std::make_shared<nc::vfs::WebDAVHost>(g_FtpAddress, g_WebDAVUser, g_WebDAVPassword, "webdav", false, g_WebDAVPort);
    const VFSHostPtr shadow_host =
        std::make_shared<nc::vfs::WebDAVHost>(g_FtpAddress, g_WebDAVUser, g_WebDAVPassword, "webdav", false, g_WebDAVPort);
    VerifyRemoteNavigationAndForcedRefresh(
        controller_host, shadow_host, "/", "WebDAV", RemoteRefreshLifecycle::UserRefreshWithSoftSuccessor);
}

static void VerifyRemoteEndpointOutageAndReconnect(
    const VFSHostPtr &_controller_host,
    const VFSHostPtr &_shadow_host,
    const std::string_view _parent_directory,
    const std::string_view _provider_name,
    const std::string_view _container_name,
    const RemoteRefreshLifecycle _recovery_lifecycle,
    const RemoteEndpointFault _endpoint_fault,
    const nc::core::FileManagerErrorCategory _expected_error_category,
    const std::string_view _expected_user_message_key,
    const std::function<VFSHostPtr()> &_make_readiness_probe,
    const std::function<void(const nc::core::FileManagerError &)> &_verify_outage_error)
{
    REQUIRE(_controller_host);
    REQUIRE(_shadow_host);
    const std::string parent = _parent_directory == "/" ? "" : std::string{_parent_directory};
    ScopedRemoteDirectory directory{
        _shadow_host,
        parent + "/WinCommanderPanelController-" + std::string{_provider_name} + "-Reconnect-" +
            nc::base::UUID::Generate().ToString()};
    REQUIRE(_shadow_host->CreateDirectory(directory.Path(), 0755));
    CreateFixtureFile(*_shadow_host, directory.FilePath("first.txt"));

    PanelControllerRemoteNavigationFixture fixture{_controller_host};
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    const std::string remote_path = directory.DirectoryPath();
    REQUIRE([fixture.Controller() GoToDirWithContext:fixture.Request(_controller_host, remote_path)].has_value());
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload);
    }));

    const VFSListingPtr initial_listing = fixture.Controller().data.ListingPtr();
    REQUIRE(initial_listing);
    const unsigned long initial_generation = fixture.Controller().dataGeneration;
    const size_t outage_event_begin = events.size();
    ScopedRemoteFixtureRestart endpoint{std::string{_container_name}};
    const bool fault_started = _endpoint_fault == RemoteEndpointFault::Stopped ? endpoint.Stop() : endpoint.Pause();
    REQUIRE(fault_started);

    REQUIRE([fixture.Controller() submitUserRefresh]);
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == outage_event_begin + 2 && std::holds_alternative<PaneLifecycleFailed>(events.back().payload);
    }));

    REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events[outage_event_begin].payload));
    REQUIRE(std::holds_alternative<PaneLifecycleFailed>(events[outage_event_begin + 1].payload));
    CHECK(events[outage_event_begin].descriptor.kind == PaneRequestKind::Refresh);
    CHECK(events[outage_event_begin].descriptor.initiated_by_user);
    CHECK(events[outage_event_begin + 1].request_id == events[outage_event_begin].request_id);
    const auto &outage = std::get<PaneLifecycleFailed>(events[outage_event_begin + 1].payload).error;
    _verify_outage_error(outage);
    CHECK(outage.category == _expected_error_category);
    CHECK(outage.user_message_key == _expected_user_message_key);
    CHECK(outage.affected_items == std::vector<std::string>{remote_path});
    REQUIRE(outage.provider_id);
    REQUIRE(_controller_host->Tag());
    CHECK(*outage.provider_id == _controller_host->Tag());
    CHECK(fixture.Controller().data.ListingPtr() == initial_listing);
    CHECK(fixture.Controller().dataGeneration == initial_generation);

    const bool fault_ended = _endpoint_fault == RemoteEndpointFault::Stopped ? endpoint.Start() : endpoint.Unpause();
    REQUIRE(fault_ended);
    VFSHostPtr readiness_probe;
    REQUIRE(RunMainLoopUntil([&] {
        try {
            readiness_probe = _make_readiness_probe();
            return readiness_probe && readiness_probe->FetchDirectoryListing("/", VFSFlags::F_ForceRefresh).has_value();
        } catch( ... ) {
            readiness_probe.reset();
            return false;
        }
    }));
    VFSHostPtr recovery_mutation_host;
    REQUIRE(RunMainLoopUntil([&] {
        VFSHostPtr candidate;
        try {
            candidate = _make_readiness_probe();
            if( !candidate || !TryCreateFixtureFile(*candidate, directory.FilePath("second.txt")) ) {
                if( candidate )
                    std::ignore = candidate->Unlink(directory.FilePath("second.txt"));
                return false;
            }
        } catch( ... ) {
            return false;
        }
        recovery_mutation_host = std::move(candidate);
        return true;
    }));
    directory.SetHost(recovery_mutation_host);
    REQUIRE(RunMainLoopUntil([&] {
        const std::expected<VFSListingPtr, nc::Error> shadow_listing =
            recovery_mutation_host->FetchDirectoryListing(directory.DirectoryPath(), VFSFlags::F_ForceRefresh);
        return shadow_listing && std::ranges::any_of(**shadow_listing, [](const VFSListingItem &_item) {
            return _item.Filename() == "second.txt";
        });
    }));

    const size_t recovery_event_begin = events.size();
    const size_t expected_recovery_events =
        _recovery_lifecycle == RemoteRefreshLifecycle::DirectUserRefresh ? 2 : 4;
    REQUIRE([fixture.Controller() submitUserRefresh]);
    const bool recovered = RunMainLoopUntil([&] {
        const VFSListingPtr recovered_listing = fixture.Controller().data.ListingPtr();
        return events.size() == recovery_event_begin + expected_recovery_events &&
               std::holds_alternative<PaneLifecycleCommitted>(events.back().payload) && recovered_listing &&
               recovered_listing != initial_listing &&
               std::ranges::any_of(*recovered_listing, [](const VFSListingItem &_item) {
                   return _item.Filename() == "second.txt";
               });
    });
    const VFSListingPtr recovered_listing_at_deadline = fixture.Controller().data.ListingPtr();
    std::string recovered_filenames;
    if( recovered_listing_at_deadline ) {
        for( const VFSListingItem &item : *recovered_listing_at_deadline ) {
            if( !recovered_filenames.empty() )
                recovered_filenames.append(", ");
            recovered_filenames.append(item.Filename());
        }
    }
    CAPTURE(events.size(), recovery_event_begin, expected_recovery_events);
    CAPTURE(recovered_listing_at_deadline == initial_listing, recovered_filenames);
    REQUIRE(recovered);

    REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events[recovery_event_begin].payload));
    CHECK(events[recovery_event_begin].descriptor.kind == PaneRequestKind::Refresh);
    CHECK(events[recovery_event_begin].descriptor.initiated_by_user);
    if( _recovery_lifecycle == RemoteRefreshLifecycle::DirectUserRefresh ) {
        REQUIRE(std::holds_alternative<PaneLifecycleCommitted>(events.back().payload));
        CHECK(events.back().request_id == events[recovery_event_begin].request_id);
    }
    else {
        REQUIRE(std::holds_alternative<nc::core::PaneLifecycleSuperseded>(events[recovery_event_begin + 1].payload));
        const auto &superseded = std::get<nc::core::PaneLifecycleSuperseded>(events[recovery_event_begin + 1].payload);
        CHECK(events[recovery_event_begin + 1].request_id == events[recovery_event_begin].request_id);
        CHECK(superseded.replacement == events[recovery_event_begin + 2].request_id);
        REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events[recovery_event_begin + 2].payload));
        CHECK(events[recovery_event_begin + 2].descriptor.kind == PaneRequestKind::Refresh);
        CHECK_FALSE(events[recovery_event_begin + 2].descriptor.initiated_by_user);
        REQUIRE(std::holds_alternative<PaneLifecycleCommitted>(events.back().payload));
        CHECK(events.back().request_id == events[recovery_event_begin + 2].request_id);
    }
    const VFSListingPtr recovered_listing = fixture.Controller().data.ListingPtr();
    REQUIRE(recovered_listing);
    CHECK(recovered_listing->Host() == _controller_host);
    CHECK(recovered_listing->Directory() == remote_path);
    CHECK(fixture.Controller().dataGeneration == initial_generation);
}

TEST_CASE(PREFIX "retains WebDAV content through endpoint outage and reconnects on user refresh",
          "[integration][docker][webdav][fault]")
{
    const VFSHostPtr controller_host =
        std::make_shared<nc::vfs::WebDAVHost>(g_FtpAddress, g_WebDAVUser, g_WebDAVPassword, "webdav", false, g_WebDAVPort);
    const VFSHostPtr shadow_host =
        std::make_shared<nc::vfs::WebDAVHost>(g_FtpAddress, g_WebDAVUser, g_WebDAVPassword, "webdav", false, g_WebDAVPort);
    VerifyRemoteEndpointOutageAndReconnect(
        controller_host,
        shadow_host,
        "/",
        "WebDAV",
        "nc_webdav_alpine",
        RemoteRefreshLifecycle::UserRefreshWithSoftSuccessor,
        RemoteEndpointFault::Stopped,
        nc::core::FileManagerErrorCategory::NetworkError,
        "errors.network",
        [] {
            return std::make_shared<nc::vfs::WebDAVHost>(
                g_FtpAddress, g_WebDAVUser, g_WebDAVPassword, "webdav", false, g_WebDAVPort);
        },
        [](const nc::core::FileManagerError &_outage) {
            CHECK(_outage.original_error.Domain() == nc::Error::POSIX);
            CHECK((_outage.original_error.Code() == ECONNREFUSED || _outage.original_error.Code() == ECONNRESET));
        });
}

TEST_CASE(PREFIX "retains FTP content through endpoint outage and reconnects on user refresh",
          "[integration][docker][ftp][fault]")
{
    const VFSHostPtr controller_host =
        std::make_shared<nc::vfs::FTPHost>(g_FtpAddress, g_FtpUser, g_FtpPassword, "/", g_FtpPort);
    const VFSHostPtr shadow_host =
        std::make_shared<nc::vfs::FTPHost>(g_FtpAddress, g_FtpUser, g_FtpPassword, "/", g_FtpPort);
    VerifyRemoteEndpointOutageAndReconnect(
        controller_host,
        shadow_host,
        "/",
        "FTP",
        "nc_ftp_alpine",
        RemoteRefreshLifecycle::UserRefreshWithSoftSuccessor,
        RemoteEndpointFault::Stopped,
        nc::core::FileManagerErrorCategory::NetworkError,
        "errors.network",
        [] { return std::make_shared<nc::vfs::FTPHost>(g_FtpAddress, g_FtpUser, g_FtpPassword, "/", g_FtpPort); },
        [&controller_host](const nc::core::FileManagerError &_outage) {
            CHECK(_outage.original_error == nc::Error{nc::vfs::ftp::ErrorDomain, nc::vfs::ftp::Errors::couldnt_connect});
            CHECK(controller_host->ClassifyError(_outage.original_error) == nc::vfs::HostErrorKind::Unavailable);
        });
}

TEST_CASE(PREFIX "retains SFTP content through endpoint outage and reconnects on user refresh",
          "[integration][docker][sftp][fault]")
{
    const auto controller_host =
        std::make_shared<nc::vfs::SFTPHost>(g_FtpAddress, g_SftpUser, g_SftpPassword, "", g_SftpPort);
    const auto shadow_host =
        std::make_shared<nc::vfs::SFTPHost>(g_FtpAddress, g_SftpUser, g_SftpPassword, "", g_SftpPort);
    const std::string home_directory = controller_host->HomeDir();
    VerifyRemoteEndpointOutageAndReconnect(
        controller_host,
        shadow_host,
        home_directory,
        "SFTP",
        "nc_sftp_alpine",
        RemoteRefreshLifecycle::DirectUserRefresh,
        RemoteEndpointFault::Stopped,
        nc::core::FileManagerErrorCategory::NetworkError,
        "errors.network",
        [] { return std::make_shared<nc::vfs::SFTPHost>(g_FtpAddress, g_SftpUser, g_SftpPassword, "", g_SftpPort); },
        [&controller_host](const nc::core::FileManagerError &_outage) {
            CHECK(_outage.original_error.Domain() == nc::vfs::sftp::ErrorDomain);
            CHECK(controller_host->ClassifyError(_outage.original_error) == nc::vfs::HostErrorKind::Unavailable);
        });
}

TEST_CASE(PREFIX "retains FTP content through a transport timeout and reconnects on user refresh",
          "[integration][docker][ftp][timeout]")
{
    const VFSHostPtr controller_host =
        std::make_shared<nc::vfs::FTPHost>(g_FtpAddress, g_FtpUser, g_FtpPassword, "/", g_FtpPort, false, 500);
    const VFSHostPtr shadow_host =
        std::make_shared<nc::vfs::FTPHost>(g_FtpAddress, g_FtpUser, g_FtpPassword, "/", g_FtpPort);
    VerifyRemoteEndpointOutageAndReconnect(
        controller_host,
        shadow_host,
        "/",
        "FTP-Timeout",
        "nc_ftp_alpine",
        RemoteRefreshLifecycle::UserRefreshWithSoftSuccessor,
        RemoteEndpointFault::Paused,
        nc::core::FileManagerErrorCategory::TimeoutError,
        "errors.timeout",
        [] { return std::make_shared<nc::vfs::FTPHost>(g_FtpAddress, g_FtpUser, g_FtpPassword, "/", g_FtpPort); },
        [&controller_host](const nc::core::FileManagerError &_outage) {
            CHECK(_outage.original_error ==
                  nc::Error{nc::vfs::ftp::ErrorDomain, nc::vfs::ftp::Errors::operation_timeout});
            CHECK(controller_host->ClassifyError(_outage.original_error) == nc::vfs::HostErrorKind::TimedOut);
        });
}

TEST_CASE(PREFIX "retains SFTP content through a transport timeout and reconnects on user refresh",
          "[integration][docker][sftp][timeout]")
{
    const auto controller_host =
        std::make_shared<nc::vfs::SFTPHost>(g_FtpAddress, g_SftpUser, g_SftpPassword, "", g_SftpPort, "", 500);
    const auto shadow_host =
        std::make_shared<nc::vfs::SFTPHost>(g_FtpAddress, g_SftpUser, g_SftpPassword, "", g_SftpPort);
    const std::string home_directory = controller_host->HomeDir();
    VerifyRemoteEndpointOutageAndReconnect(
        controller_host,
        shadow_host,
        home_directory,
        "SFTP-Timeout",
        "nc_sftp_alpine",
        RemoteRefreshLifecycle::DirectUserRefresh,
        RemoteEndpointFault::Paused,
        nc::core::FileManagerErrorCategory::TimeoutError,
        "errors.timeout",
        [] { return std::make_shared<nc::vfs::SFTPHost>(g_FtpAddress, g_SftpUser, g_SftpPassword, "", g_SftpPort); },
        [&controller_host](const nc::core::FileManagerError &_outage) {
            CHECK(_outage.original_error == nc::Error{nc::vfs::sftp::ErrorDomain, nc::vfs::sftp::Errors::timeout});
            CHECK(controller_host->ClassifyError(_outage.original_error) == nc::vfs::HostErrorKind::TimedOut);
        });
}

TEST_CASE(PREFIX "retains WebDAV content through a transport timeout and reconnects on user refresh",
          "[integration][docker][webdav][timeout]")
{
    const VFSHostPtr controller_host = std::make_shared<nc::vfs::WebDAVHost>(
        g_FtpAddress, g_WebDAVUser, g_WebDAVPassword, "webdav", false, g_WebDAVPort, 500);
    const VFSHostPtr shadow_host =
        std::make_shared<nc::vfs::WebDAVHost>(g_FtpAddress, g_WebDAVUser, g_WebDAVPassword, "webdav", false, g_WebDAVPort);
    VerifyRemoteEndpointOutageAndReconnect(
        controller_host,
        shadow_host,
        "/",
        "WebDAV-Timeout",
        "nc_webdav_alpine",
        RemoteRefreshLifecycle::UserRefreshWithSoftSuccessor,
        RemoteEndpointFault::Paused,
        nc::core::FileManagerErrorCategory::TimeoutError,
        "errors.timeout",
        [] {
            return std::make_shared<nc::vfs::WebDAVHost>(
                g_FtpAddress, g_WebDAVUser, g_WebDAVPassword, "webdav", false, g_WebDAVPort);
        },
        [&controller_host](const nc::core::FileManagerError &_outage) {
            CHECK(_outage.original_error == nc::Error{nc::Error::POSIX, ETIMEDOUT});
            CHECK(controller_host->ClassifyError(_outage.original_error) == nc::vfs::HostErrorKind::TimedOut);
        });
}
