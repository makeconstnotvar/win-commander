// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/Core/SandboxManager.h>

#include <CoreFoundation/CoreFoundation.h>
#include <Config/ConfigImpl.h>
#include <Config/NonPersistentOverwritesStorage.h>
#include <Panel/PanelData.h>
#include <Panel/PanelViewFieldEditor.h>
#include <Utility/ActionsShortcutsManager.h>
#include <Utility/FSEventsFileUpdate.h>
#include <Utility/NativeFSManager.h>
#include <VFS/Native.h>
#include <VFS/NetFTP.h>
#include <VFS/NetSFTP.h>
#include <VFS/NetWebDAV.h>
#include <VFS/VFSListingInput.h>
#include <VFSIcon/IconRepository.h>
#include <WinCommander/Core/VFSInstanceManagerImpl.h>
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/CommandRegistry.h>
#include <WinCommander/Core/Commands/FileGetInfoCommand.h>
#include <WinCommander/Core/Commands/FilePreviewCommand.h>
#include <WinCommander/Core/Commands/TogglePreviewPaneCommand.h>
#include <WinCommander/States/FilePanels/FilesDraggingSource.h>
#include <WinCommander/States/FilePanels/PanelController.h>
#include <WinCommander/States/FilePanels/PanelControllerActions.h>
#include <WinCommander/States/FilePanels/PanelControllerActionsDispatcher.h>
#include <WinCommander/States/FilePanels/PanelHistory.h>
#include <WinCommander/States/FilePanels/PanelView.h>
#include <WinCommander/States/FilePanels/PanelViewDummyPresentation.h>
#include <WinCommander/States/FilePanels/PanelViewFooter.h>
#include <WinCommander/States/FilePanels/PanelViewHeader.h>
#include <WinCommander/States/FilePanels/PanelViewLayoutSupport.h>
#include <WinCommander/States/FilePanels/Actions/Enter.h>
#include <WinCommander/States/FilePanels/Actions/GoToFolder.h>
#include <WinCommander/States/FilePanels/Actions/InsertFromPasteboard.h>
#include <WinCommander/States/FilePanels/Actions/MakeNew.h>
#include <WinCommander/States/FilePanels/Actions/Select.h>
#include <WinCommander/States/FilePanels/Helpers/Pasteboard.h>
#include <WinCommander/States/Explorer/NCExplorerInspectorPresenting.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;
using nc::core::PaneCancellationReason;
using nc::core::PaneLifecycleCancelled;
using nc::core::PaneLifecycleCommitted;
using nc::core::PaneLifecycleEvent;
using nc::core::PaneLifecycleFailed;
using nc::core::PaneLifecycleRejected;
using nc::core::PaneLifecycleStarted;
using nc::core::PaneLifecycleSuperseded;
using nc::core::PaneRejectionReason;
using nc::core::PaneRequestKind;
using nc::panel::DirectoryChangeRequest;
using nc::panel::DirectoryChangeResultSource;

bool RunMainLoopUntil(const std::function<bool()> &_predicate,
                      const std::chrono::milliseconds _timeout = 2s)
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
    NSColor *SeparatorsColor() const override { return NSColor.separatorColor; }
    NSColor *ActiveBackgroundColor() const override { return NSColor.controlBackgroundColor; }
    NSColor *InactiveBackgroundColor() const override { return NSColor.controlBackgroundColor; }
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
    std::optional<Shortcuts> ShortcutsFromAction(std::string_view) const noexcept override
    {
        return std::nullopt;
    }
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

class PaneNavigationShortcutManager final : public nc::utility::ActionsShortcutsManager
{
public:
    std::optional<int> TagFromAction(std::string_view) const noexcept override { return std::nullopt; }
    std::optional<std::string_view> ActionFromTag(int) const noexcept override { return std::nullopt; }
    std::optional<Shortcuts> ShortcutsFromAction(const std::string_view _action) const noexcept override
    {
        if( _action == "menu.go.enclosing_folder" )
            return Shortcuts{nc::utility::ActionShortcut{"^u"}};
        if( _action == "panel.go_into_enclosing_folder" )
            return Shortcuts{nc::utility::ActionShortcut{"\b"}};
        if( _action == "menu.view.refresh" )
            return Shortcuts{nc::utility::ActionShortcut{"^r"}};
        return std::nullopt;
    }
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

class FileOpenShortcutManager final : public nc::utility::ActionsShortcutsManager
{
public:
    explicit FileOpenShortcutManager(const int _action_tag = 2) : m_ActionTag(_action_tag) {}

    std::optional<int> TagFromAction(const std::string_view _action) const noexcept override
    {
        if( _action == "menu.file.enter" )
            return 1;
        if( _action == "menu.file.open" )
            return 2;
        if( _action == "panel.go_root" )
            return 3;
        if( _action == "panel.go_home" )
            return 4;
        if( _action == "panel.show_preview" )
            return 5;
        if( _action == "panel.go_into_folder" )
            return 6;
        if( _action == "panel.go_into_enclosing_folder" )
            return 7;
        if( _action == "panel.show_context_menu" )
            return 8;
        return std::nullopt;
    }
    std::optional<std::string_view> ActionFromTag(int) const noexcept override { return std::nullopt; }
    std::optional<Shortcuts> ShortcutsFromAction(std::string_view) const noexcept override
    {
        return std::nullopt;
    }
    std::optional<Shortcuts> ShortcutsFromTag(int) const noexcept override { return std::nullopt; }
    std::optional<Shortcuts> DefaultShortcutsFromTag(int) const noexcept override { return std::nullopt; }
    std::optional<ActionTags>
    ActionTagsFromShortcut(Shortcut, std::string_view = {}) const noexcept override
    {
        return ActionTags{m_ActionTag};
    }
    std::optional<int>
    FirstOfActionTagsFromShortcut(std::span<const int>, Shortcut, std::string_view = {}) const noexcept override
    {
        return m_ActionTag;
    }
    std::vector<std::pair<std::string, int>> AllShortcuts() const override { return {}; }
    void RevertToDefaults() override {}
    bool SetShortcutOverride(std::string_view, Shortcut) override { return false; }
    bool SetShortcutsOverride(std::string_view, std::span<const Shortcut>) override { return false; }

private:
    int m_ActionTag;
};

class TestNativeFSManager final : public nc::utility::NativeFSManager
{
public:
    std::vector<Info> Volumes() const override
    {
        if( m_VolumesThrow )
            throw std::runtime_error{"scripted volume enumeration failure"};
        return m_Volumes;
    }
    Info VolumeFromFD(int) const noexcept override { return nullptr; }
    Info VolumeFromPath(std::string_view) const noexcept override { return m_PathVolume; }
    Info VolumeFromPathFast(std::string_view) const noexcept override { return m_PathVolume; }
    Info VolumeFromMountPoint(std::string_view) const noexcept override { return m_PathVolume; }
    void UpdateSpaceInformation(const Info &) override {}
    void EjectVolumeContainingPath(const std::string &) override {}
    bool IsVolumeContainingPathEjectable(const std::string &) override { return false; }

    void SetVolumes(std::vector<Info> _volumes) { m_Volumes = std::move(_volumes); }
    void SetPathVolume(Info _volume) { m_PathVolume = std::move(_volume); }
    void SetVolumesThrow(const bool _throw) { m_VolumesThrow = _throw; }

private:
    std::vector<Info> m_Volumes;
    Info m_PathVolume;
    bool m_VolumesThrow = false;
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
    bool HasAccess(PanelController *, const std::string &_directory, VFSHost &) override
    {
        ++has_access_calls;
        checked_directories.emplace_back(_directory);
        return has_access;
    }

    bool RequestAccessSync(PanelController *, const std::string &_directory, VFSHost &) override
    {
        ++request_access_calls;
        requested_directories.emplace_back(_directory);
        return grants_access;
    }

    bool has_access{true};
    bool grants_access{true};
    int has_access_calls{0};
    int request_access_calls{0};
    std::vector<std::string> checked_directories;
    std::vector<std::string> requested_directories;
};

class ControllableNavigationHost final : public nc::vfs::Host
{
public:
    struct Plan {
        std::mutex mutex;
        std::condition_variable changed;
        VFSListingPtr listing;
        std::optional<nc::Error> error;
        std::optional<nc::Error> exception_error;
        bool released = true;
        bool entered = false;
        bool completed = false;
        bool honor_cancellation = true;
        unsigned long fetch_flags = 0;
    };

    ControllableNavigationHost() : Host("/", nullptr, "panel_controller_navigation_test")
    {
        AddFeatures(nc::vfs::HostFeatures::Read | nc::vfs::HostFeatures::Rename);
    }

    bool IsWritableAtPath(std::string_view _path) const override { return _path == "/seed/"; }

    nc::vfs::HostErrorKind ClassifyError(const nc::Error &_error) const noexcept override
    {
        if( _error.Domain() == "panel_controller_navigation_timeout" )
            return nc::vfs::HostErrorKind::TimedOut;
        return Host::ClassifyError(_error);
    }

    std::shared_ptr<Plan> ScriptSuccess(std::string _path,
                                        VFSListingPtr _listing,
                                        const bool _gated,
                                        const bool _honor_cancellation = true)
    {
        auto plan = std::make_shared<Plan>();
        plan->listing = std::move(_listing);
        plan->released = !_gated;
        plan->honor_cancellation = _honor_cancellation;
        const std::lock_guard lock{m_PlansMutex};
        m_Plans[std::move(_path)].emplace_back(plan);
        return plan;
    }

    std::shared_ptr<Plan> ScriptError(std::string _path, nc::Error _error, const bool _gated = false)
    {
        auto plan = std::make_shared<Plan>();
        plan->error = std::move(_error);
        plan->released = !_gated;
        const std::lock_guard lock{m_PlansMutex};
        m_Plans[std::move(_path)].emplace_back(plan);
        return plan;
    }

    std::shared_ptr<Plan> ScriptThrownError(std::string _path, nc::Error _error)
    {
        auto plan = std::make_shared<Plan>();
        plan->exception_error = std::move(_error);
        const std::lock_guard lock{m_PlansMutex};
        m_Plans[std::move(_path)].emplace_back(plan);
        return plan;
    }

    static bool WaitEntered(const std::shared_ptr<Plan> &_plan,
                            const std::chrono::milliseconds _timeout = 1s)
    {
        std::unique_lock lock{_plan->mutex};
        return _plan->changed.wait_for(lock, _timeout, [&] { return _plan->entered; });
    }

    static void Release(const std::shared_ptr<Plan> &_plan)
    {
        {
            const std::lock_guard lock{_plan->mutex};
            _plan->released = true;
        }
        _plan->changed.notify_all();
    }

    static bool Completed(const std::shared_ptr<Plan> &_plan)
    {
        const std::lock_guard lock{_plan->mutex};
        return _plan->completed;
    }

    static unsigned long FetchFlags(const std::shared_ptr<Plan> &_plan)
    {
        const std::lock_guard lock{_plan->mutex};
        return _plan->fetch_flags;
    }

    int FetchCount() const noexcept { return m_FetchCount.load(std::memory_order_acquire); }

    void SetNativeFilesystem(const bool _native) noexcept { m_IsNativeFilesystem = _native; }

    bool IsNativeFS() const noexcept override { return m_IsNativeFilesystem; }

    void SetAccessibleDirectory(std::string _path)
    {
        const std::lock_guard lock{m_AccessibleDirectoriesMutex};
        m_AccessibleDirectories.emplace_back(std::move(_path));
    }

    std::expected<void, nc::Error>
    IterateDirectoryListing(std::string_view _path,
                            const std::function<bool(const VFSDirEnt &)> &) override
    {
        const std::lock_guard lock{m_AccessibleDirectoriesMutex};
        if( std::ranges::find(m_AccessibleDirectories, _path) != m_AccessibleDirectories.end() )
            return {};
        return std::unexpected(nc::Error{nc::Error::POSIX, ENOENT});
    }

    std::expected<VFSListingPtr, nc::Error>
    FetchDirectoryListing(std::string_view _path,
                          const unsigned long _flags,
                          const VFSCancelChecker &_cancel_checker = {}) override
    {
        m_FetchCount.fetch_add(1, std::memory_order_acq_rel);
        std::shared_ptr<Plan> plan;
        {
            const std::lock_guard lock{m_PlansMutex};
            const auto it = m_Plans.find(std::string{_path});
            if( it == m_Plans.end() || it->second.empty() )
                return std::unexpected(nc::Error{nc::Error::POSIX, ENOENT});
            plan = std::move(it->second.front());
            it->second.pop_front();
            if( it->second.empty() )
                m_Plans.erase(it);
        }

        std::unique_lock lock{plan->mutex};
        plan->fetch_flags = _flags;
        plan->entered = true;
        plan->changed.notify_all();
        while( !plan->released ) {
            if( plan->honor_cancellation && _cancel_checker && _cancel_checker() ) {
                plan->completed = true;
                lock.unlock();
                plan->changed.notify_all();
                return std::unexpected(nc::Error{nc::Error::POSIX, ECANCELED});
            }
            plan->changed.wait_for(lock, 2ms);
        }

        if( plan->honor_cancellation && _cancel_checker && _cancel_checker() ) {
            plan->completed = true;
            lock.unlock();
            plan->changed.notify_all();
            return std::unexpected(nc::Error{nc::Error::POSIX, ECANCELED});
        }

        auto listing = plan->listing;
        auto error = plan->error;
        auto exception_error = plan->exception_error;
        plan->completed = true;
        lock.unlock();
        plan->changed.notify_all();
        if( exception_error )
            throw nc::ErrorException{std::move(*exception_error)};
        if( error )
            return std::unexpected(std::move(*error));
        return listing;
    }

private:
    std::mutex m_PlansMutex;
    std::map<std::string, std::deque<std::shared_ptr<Plan>>, std::less<>> m_Plans;
    std::mutex m_AccessibleDirectoriesMutex;
    std::vector<std::string> m_AccessibleDirectories;
    std::atomic_int m_FetchCount = 0;
    bool m_IsNativeFilesystem = false;
};

nc::utility::NativeFSManager::Info TestNativeVolume(std::string _mount_path)
{
    auto info = std::make_shared<nc::utility::NativeFileSystemInfo>();
    info->mounted_at_path = std::move(_mount_path);
    return info;
}

VFSListingPtr UniformListing(const VFSHostPtr &_host,
                             std::string _directory,
                             std::string _filename,
                             const mode_t _mode = S_IFREG | S_IRUSR,
                             const uint8_t _type = DT_REG)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = std::move(_directory);
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = _host;

    const std::vector<std::tuple<std::string, mode_t, uint8_t, uint64_t>> entries = {
        {"..", S_IFDIR | S_IRUSR, DT_DIR, 0},
        {std::move(_filename), _mode, _type, 17},
    };
    for( const auto &[name, mode, type, size] : entries ) {
        const size_t index = input.filenames.size();
        input.filenames.emplace_back(name);
        input.unix_modes.emplace_back(mode);
        input.unix_types.emplace_back(type);
        input.sizes.insert(index, size);
    }
    return VFSListing::Build(std::move(input));
}

VFSListingPtr LargeUniformListing(const VFSHostPtr &_host,
                                  std::string _directory,
                                  const size_t _items_count)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = std::move(_directory);
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = _host;
    input.filenames.reserve(_items_count + 2);
    input.unix_modes.reserve(_items_count + 2);
    input.unix_types.reserve(_items_count + 2);

    input.filenames.emplace_back("..");
    input.unix_modes.emplace_back(S_IFDIR | S_IRUSR);
    input.unix_types.emplace_back(DT_DIR);
    for( size_t index = 0; index != _items_count; ++index ) {
        std::string digits = std::to_string(index);
        std::string name = "item-";
        name.append(6 - digits.size(), '0');
        name.append(digits);
        name.append(".txt");
        input.filenames.emplace_back(std::move(name));
        input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
        input.unix_types.emplace_back(DT_REG);
        input.sizes.insert(index + 1, index + 1);
    }
    input.filenames.emplace_back(".hidden-large-test");
    input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
    input.unix_types.emplace_back(DT_REG);
    input.sizes.insert(_items_count + 1, 1);
    return VFSListing::Build(std::move(input));
}

VFSListingPtr NonUniformListing(const std::shared_ptr<ControllableNavigationHost> &_host)
{
    nc::vfs::ListingInput input;
    input.title = "Search results";
    input.directories.reset(nc::base::variable_container<>::type::dense);
    input.hosts.reset(nc::base::variable_container<>::type::dense);
    for( size_t index = 0; index != 2; ++index ) {
        input.directories.insert(index, index == 0 ? "/first/" : "/second/");
        input.hosts.insert(index, _host);
        input.filenames.emplace_back(index == 0 ? "first.txt" : "second.txt");
        input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
        input.unix_types.emplace_back(DT_REG);
    }
    return VFSListing::Build(std::move(input));
}

struct CallbackCall {
    bool succeeded = false;
    std::optional<nc::Error> error;
    DirectoryChangeResultSource source = DirectoryChangeResultSource::Admission;
    bool current = false;
};

class CallbackRecorder
{
public:
    void Record(const std::expected<void, nc::Error> &_result,
                const DirectoryChangeResultSource _source,
                const std::function<bool()> &_is_current)
    {
        const std::lock_guard lock{m_Mutex};
        m_Calls.emplace_back(CallbackCall{
            .succeeded = _result.has_value(),
            .error = _result ? std::nullopt : std::optional{_result.error()},
            .source = _source,
            .current = _is_current(),
        });
    }

    std::vector<CallbackCall> Calls() const
    {
        const std::lock_guard lock{m_Mutex};
        return m_Calls;
    }

private:
    mutable std::mutex m_Mutex;
    std::vector<CallbackCall> m_Calls;
};

} // namespace

@interface PanelControllerNavigationTestFooter : NCPanelViewFooter
@end

@interface PanelControllerNavigationSnapshotFooter : NCPanelViewFooter
@property(nonatomic, readonly) int appliedSnapshotCount;
@property(nonatomic, readonly) int itemCount;
@property(nonatomic, readonly) int selectedCount;
@property(nonatomic, readonly) int64_t selectedBytes;
@end

@interface PanelControllerInspectorPresenterTestState : NSObject <NCPanelControllerHostingState,
                                                                  NCExplorerInspectorPresenting>
@end

@interface NCPanelControllerActionsDispatcher (NavigationShortcutSourceTests)
- (nc::core::CommandInvocationSource)commandInvocationSourceForSender:(id)_sender
                                                   commandId:(std::string_view)_command_id
                                                currentEvent:(NSEvent *)_event;
@end

@implementation PanelControllerNavigationTestFooter

- (void)updateFocusedItem:(const VFSListingItem &)_item VD:(nc::panel::data::ItemVolatileData)_vd
{
    (void)_item;
    (void)_vd;
}

@end

@implementation PanelControllerInspectorPresenterTestState

- (NSWindow *)window { return nil; }
- (FilePanelMainSplitView *)splitView { return nil; }
- (bool)anyPanelCollapsed { return false; }
- (bool)bothPanelsAreVisible { return false; }
- (PanelController *)leftPanelController { return nil; }
- (PanelController *)rightPanelController { return nil; }
- (void)closeAttachedUI:(PanelController *)_panel { (void)_panel; }
- (void)PanelPathChanged:(PanelController *)_panel { (void)_panel; }
- (void)activePanelChangedTo:(PanelController *)_panel { (void)_panel; }
- (void)ActivatePanelByController:(PanelController *)_panel { (void)_panel; }
- (BriefSystemOverview *)briefSystemOverviewForPanel:(PanelController *)_panel make:(bool)_make_if_absent
{
    (void)_panel;
    (void)_make_if_absent;
    return nil;
}
- (id<NCPanelPreview>)quickLookForPanel:(PanelController *)_panel make:(bool)_make_if_absent
{
    (void)_panel;
    (void)_make_if_absent;
    return nil;
}
- (void)requestTerminalExecution:(const std::string &)_filename at:(const std::string &)_cwd
{
    (void)_filename;
    (void)_cwd;
}
- (bool)isLeftController:(PanelController *)_panel
{
    (void)_panel;
    return false;
}
- (bool)isRightController:(PanelController *)_panel
{
    (void)_panel;
    return false;
}

- (BOOL)presentFileGetInfo:(const nc::core::FileGetInfoPresentation &)_presentation
                  forPanel:(PanelController *)_panel
{
    return _panel != nil && !_presentation.items.empty();
}

- (BOOL)previewPaneVisibleForPanel:(PanelController *)_panel
{
    return _panel != nil;
}

- (BOOL)setPreviewPaneVisible:(BOOL)_desired
                     expected:(BOOL)_expected
                     forPanel:(PanelController *)_panel
{
    return _panel != nil && _expected && !_desired;
}

@end

@implementation PanelControllerNavigationSnapshotFooter {
    int m_AppliedSnapshotCount;
    int m_ItemCount;
    int m_SelectedCount;
    int64_t m_SelectedBytes;
}

- (void)applyExplorerPaneSnapshot:(const nc::core::PaneSnapshot &)_snapshot
{
    ++m_AppliedSnapshotCount;
    m_ItemCount = _snapshot.state.item_count;
    m_SelectedCount = _snapshot.state.selected_count;
    m_SelectedBytes = _snapshot.state.selected_bytes;
}

- (int)appliedSnapshotCount
{
    return m_AppliedSnapshotCount;
}

- (int)itemCount
{
    return m_ItemCount;
}

- (int)selectedCount
{
    return m_SelectedCount;
}

- (int64_t)selectedBytes
{
    return m_SelectedBytes;
}

@end

@interface PanelControllerRenamePresentationProbe : NCPanelViewDummyPresentation

@property(nonatomic) bool attachEditors;
@property(nonatomic, copy) dispatch_block_t onEditorAttached;
@property(nonatomic, readonly) int setupCallCount;
- (NCPanelViewFieldEditor *)editorAtCall:(NSUInteger)_index;

@end

@implementation PanelControllerRenamePresentationProbe {
    NSMutableArray<NCPanelViewFieldEditor *> *m_Editors;
    bool m_AttachEditors;
    dispatch_block_t m_OnEditorAttached;
}

@synthesize attachEditors = m_AttachEditors;
@synthesize onEditorAttached = m_OnEditorAttached;

- (instancetype)init
{
    self = [super initWithFrame:NSMakeRect(0, 0, 100, 100)];
    if( self )
        m_Editors = [NSMutableArray new];
    return self;
}

- (bool)isItemVisible:(int)_sorted_item_index
{
    return _sorted_item_index >= 0;
}

- (void)setupFieldEditor:(NCPanelViewFieldEditor *)_editor forItemAtIndex:(int)_sorted_item_index
{
    (void)_sorted_item_index;
    [m_Editors addObject:_editor];
    if( self.attachEditors ) {
        [self addSubview:_editor];
        if( self.onEditorAttached )
            self.onEditorAttached();
    }
}

- (int)setupCallCount
{
    return static_cast<int>(m_Editors.count);
}

- (NCPanelViewFieldEditor *)editorAtCall:(NSUInteger)_index
{
    return m_Editors[_index];
}

@end


@interface PanelControllerRenameRejectingWindow : NSObject

@property(nonatomic, readonly) int makeFirstResponderCallCount;

@end


@implementation PanelControllerRenameRejectingWindow {
    int m_MakeFirstResponderCallCount;
}

- (NSResponder *)firstResponder
{
    return nil;
}

- (BOOL)makeFirstResponder:(NSResponder *)_responder
{
    (void)_responder;
    ++m_MakeFirstResponderCallCount;
    return NO;
}

- (int)makeFirstResponderCallCount
{
    return m_MakeFirstResponderCallCount;
}

@end

@interface PanelControllerNavigationTestView : PanelView

- (instancetype)initWithNativeHost:(nc::vfs::NativeHost &)_native_host
           actionsShortcutsManager:(const nc::utility::ActionsShortcutsManager &)_shortcuts;

@property(nonatomic, readonly) int dataUpdateCount;
@property(nonatomic, readonly) int panelChangeCount;
@property(nonatomic, readonly) int presentationLayoutSetCount;
@property(nonatomic, readonly) std::string lastPanelChangeFocus;
@property(nonatomic, readonly) bool lastPanelChangeLoadedPreviousState;
@property(nonatomic, strong) PanelControllerRenameRejectingWindow *renameWindowOverride;
@property(nonatomic) bool forwardsDataUpdatedToProduction;

- (void)simulatePresentationLayoutChange:(const nc::panel::PanelViewLayout::LayoutVariant &)_layout;

@end

@interface PanelView (PanelControllerNavigationPresentationTestHook)
- (void)notifyAboutPresentationLayoutChange;
@end

@implementation PanelControllerNavigationTestView {
    NSProgressIndicator *m_TestBusyIndicator;
    int m_DataUpdateCount;
    int m_PanelChangeCount;
    int m_PresentationLayoutSetCount;
    nc::panel::PanelViewLayout::LayoutVariant m_TestPresentationLayout;
    std::string m_LastPanelChangeFocus;
    bool m_LastPanelChangeLoadedPreviousState;
    PanelControllerRenameRejectingWindow *m_RenameWindowOverride;
    bool m_ForwardsDataUpdatedToProduction;
}

@synthesize renameWindowOverride = m_RenameWindowOverride;
@synthesize forwardsDataUpdatedToProduction = m_ForwardsDataUpdatedToProduction;

- (instancetype)initWithNativeHost:(nc::vfs::NativeHost &)_native_host
           actionsShortcutsManager:(const nc::utility::ActionsShortcutsManager &)_shortcuts
{
    auto header = [[NCPanelViewHeader alloc] initWithFrame:NSZeroRect theme:std::make_unique<TestHeaderTheme>()];
    auto footer = [[PanelControllerNavigationTestFooter alloc] initWithFrame:NSZeroRect
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
    m_TestPresentationLayout = _layout.layout;
    ++m_PresentationLayoutSetCount;
}

- (nc::panel::PanelViewLayout::LayoutVariant)presentationLayout
{
    return m_TestPresentationLayout;
}

- (void)simulatePresentationLayoutChange:(const nc::panel::PanelViewLayout::LayoutVariant &)_layout
{
    m_TestPresentationLayout = _layout;
    [self notifyAboutPresentationLayoutChange];
}

- (void)savePathState
{
}

- (void)dataUpdated
{
    ++m_DataUpdateCount;
    if( m_ForwardsDataUpdatedToProduction )
        [super dataUpdated];
}

- (void)volatileDataChanged
{
    // This fixture verifies controller/model contracts without the production footer's global
    // formatting configuration. Selection assertions read the model directly.
}

- (void)panelChangedWithFocusedFilename:(const std::string &)_filename
                      loadPreviousState:(bool)_load_previous
{
    m_LastPanelChangeFocus = _filename;
    m_LastPanelChangeLoadedPreviousState = _load_previous;
    ++m_PanelChangeCount;
}

- (NSProgressIndicator *)busyIndicator
{
    return m_TestBusyIndicator;
}

- (NSWindow *)window
{
    if( m_RenameWindowOverride )
        return reinterpret_cast<NSWindow *>(m_RenameWindowOverride);
    return super.window;
}

- (int)dataUpdateCount
{
    return m_DataUpdateCount;
}

- (int)panelChangeCount
{
    return m_PanelChangeCount;
}

- (int)presentationLayoutSetCount
{
    return m_PresentationLayoutSetCount;
}

- (std::string)lastPanelChangeFocus
{
    return m_LastPanelChangeFocus;
}

- (bool)lastPanelChangeLoadedPreviousState
{
    return m_LastPanelChangeLoadedPreviousState;
}

@end

namespace {

class ScopedItemsViewOverride
{
public:
    ScopedItemsViewOverride(PanelView *_view, NSView<NCPanelViewPresentationProtocol> *_replacement)
        : m_View(_view), m_Original([_view valueForKey:@"m_ItemsView"])
    {
        [_view setValue:_replacement forKey:@"m_ItemsView"];
    }

    ~ScopedItemsViewOverride() { [m_View setValue:m_Original forKey:@"m_ItemsView"]; }

    ScopedItemsViewOverride(const ScopedItemsViewOverride &) = delete;
    ScopedItemsViewOverride &operator=(const ScopedItemsViewOverride &) = delete;

private:
    PanelView *m_View;
    NSView<NCPanelViewPresentationProtocol> *m_Original;
};

class ScopedWindowContentViewAttachment final
{
public:
    ScopedWindowContentViewAttachment(NSWindow *_window, NSView *_content) : m_Window{_window}
    {
        m_Window.contentView = _content;
    }

    ~ScopedWindowContentViewAttachment()
    {
        [m_Window makeFirstResponder:nil];
        m_Window.contentView = nil;
    }

    ScopedWindowContentViewAttachment(const ScopedWindowContentViewAttachment &) = delete;
    ScopedWindowContentViewAttachment &operator=(const ScopedWindowContentViewAttachment &) = delete;

private:
    __strong NSWindow *m_Window;
};

class PanelControllerNavigationFixture
{
public:
    explicit PanelControllerNavigationFixture(const bool _with_configured_layouts = false)
    {
        if( !_with_configured_layouts ) {
            const nc::config::Value empty_layouts{rapidjson::kArrayType};
            m_Config.Set("tests.panel_controller_navigation.layouts", empty_layouts);
        }
        m_NativeHost = std::make_shared<nc::vfs::NativeHost>(m_NativeFSManager, m_FSEvents);
        m_Layouts =
            std::make_shared<nc::panel::PanelViewLayoutsStorage>("tests.panel_controller_navigation.layouts", m_Config);
        m_View = [[PanelControllerNavigationTestView alloc] initWithNativeHost:*m_NativeHost
                                                       actionsShortcutsManager:m_Shortcuts];
        nc::panel::ContextMenuProvider context_menu_provider = [](std::vector<VFSListingItem>, PanelController *) {
            return static_cast<NCPanelContextMenu *>(nil);
        };
        m_Controller = [[PanelController alloc] initWithView:m_View
                                                      paneId:nc::core::PaneId{701}
                                                     layouts:m_Layouts
                                                      config:m_Config
                                          vfsInstanceManager:TestVFSInstanceManager()
                                     directoryAccessProvider:m_AccessProvider
                                         contextMenuProvider:std::move(context_menu_provider)
                                             nativeFSManager:m_NativeFSManager
                                                  nativeHost:*m_NativeHost];
        m_Host = std::make_shared<ControllableNavigationHost>();
        m_SeedListing = UniformListing(m_Host, "/seed/", "seed.txt");
        [m_Controller loadListing:m_SeedListing];
    }

    ~PanelControllerNavigationFixture()
    {
        [m_Controller CancelBackgroundOperations];
        const bool drained = RunMainLoopUntil([&] { return !m_Controller || !m_Controller.isDoingBackgroundLoading; });
        if( !drained ) {
            std::fputs("PanelControllerNavigationFixture: background queues did not drain\n", stderr);
            std::terminate();
        }
        m_View.delegate = nil;
        m_View.nextResponder = nil;
        __weak PanelController *weak_controller = m_Controller;
        m_Controller = nil;
        // Queue-state UI notifications use a bounded delayed main-queue callback. Keep every
        // injected dependency alive until that callback releases the real controller.
        const bool controller_released = RunMainLoopUntil([&] { return weak_controller == nil; });
        if( !controller_released ) {
            std::fputs("PanelControllerNavigationFixture: controller was retained past teardown\n", stderr);
            std::terminate();
        }
        m_View = nil;
    }

    std::shared_ptr<DirectoryChangeRequest> Request(std::string _path,
                                                    const bool _asynchronous,
                                                    const std::shared_ptr<CallbackRecorder> &_callback = {},
                                                    const bool _initiated_by_user = true)
    {
        auto request = std::make_shared<DirectoryChangeRequest>();
        request->RequestedDirectory = std::move(_path);
        request->VFS = m_Host;
        request->PerformAsynchronous = _asynchronous;
        request->InitiatedByUser = _initiated_by_user;
        if( _callback ) {
            request->LoadingResultCallback = [_callback](const std::expected<void, nc::Error> &_result,
                                                         const DirectoryChangeResultSource _source,
                                                         const std::function<bool()> &_is_current) {
                _callback->Record(_result, _source, _is_current);
            };
        }
        return request;
    }

    PanelController *const &Controller() const { return m_Controller; }
    PanelControllerNavigationTestView *const &View() const { return m_View; }
    const std::shared_ptr<ControllableNavigationHost> &Host() const { return m_Host; }
    TestDirectoryAccessProvider &AccessProvider() { return m_AccessProvider; }
    const std::shared_ptr<nc::vfs::NativeHost> &NativeHost() const { return m_NativeHost; }
    TestNativeFSManager &NativeFSManager() { return m_NativeFSManager; }
    const VFSListingPtr &SeedListing() const { return m_SeedListing; }
    const std::shared_ptr<nc::panel::PanelViewLayoutsStorage> &Layouts() const { return m_Layouts; }
    bool WaitForPersistedLayoutName(const int _slot, const std::string_view _name)
    {
        return RunMainLoopUntil([&] {
            const auto layouts = m_Config.Get("tests.panel_controller_navigation.layouts");
            if( !layouts.IsArray() || _slot < 0 || static_cast<rapidjson::SizeType>(_slot) >= layouts.Size() )
                return false;
            const auto &layout = layouts[static_cast<rapidjson::SizeType>(_slot)];
            return layout.IsObject() && layout.HasMember("title") && layout["title"].IsString() &&
                   layout["title"].GetString() == _name;
        });
    }
    bool WaitForPersistedFilenameWidth(const int _slot, const int _width)
    {
        return RunMainLoopUntil([&] {
            const auto layouts = m_Config.Get("tests.panel_controller_navigation.layouts");
            if( !layouts.IsArray() || _slot < 0 || static_cast<rapidjson::SizeType>(_slot) >= layouts.Size() )
                return false;
            const auto &layout = layouts[static_cast<rapidjson::SizeType>(_slot)];
            if( !layout.IsObject() || !layout.HasMember("list") || !layout["list"].IsObject() )
                return false;
            const auto &list = layout["list"];
            if( !list.HasMember("columns") || !list["columns"].IsArray() || list["columns"].Empty() )
                return false;
            const auto &filename = list["columns"][0];
            return filename.IsObject() && filename.HasMember("width") && filename["width"].IsInt() &&
                   filename["width"].GetInt() == _width;
        });
    }
    void ReleaseControllerWithoutCancelling()
    {
        m_View.delegate = nil;
        m_View.nextResponder = nil;
        m_Controller = nil;
    }

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
            "tests": {
                "panel_controller_navigation": {
                    "layouts": [
                        {
                            "title": "Icons",
                            "brief": {
                                "mode": 2,
                                "dynamic_width_min": 140,
                                "dynamic_width_max": 300,
                                "dynamic_width_equal": false,
                                "icon_scale": 1
                            }
                        },
                        {
                            "title": "Details",
                            "list": {
                                "columns": [
                                    {"kind": 1, "width": 320, "min_width": 160, "max_width": 800},
                                    {"kind": 5, "width": 180, "min_width": 80, "max_width": 400}
                                ],
                                "icon_scale": 1
                            }
                        }
                    ]
                }
            }
        })json",
        std::make_shared<nc::config::NonPersistentOverwritesStorage>("")};
    TestNativeFSManager m_NativeFSManager;
    TestFSEventsFileUpdate m_FSEvents;
    TestActionsShortcutsManager m_Shortcuts;
    TestDirectoryAccessProvider m_AccessProvider;
    std::shared_ptr<nc::vfs::NativeHost> m_NativeHost;
    std::shared_ptr<nc::panel::PanelViewLayoutsStorage> m_Layouts;
    PanelControllerNavigationTestView *m_View = nil;
    PanelController *m_Controller = nil;
    std::shared_ptr<ControllableNavigationHost> m_Host;
    VFSListingPtr m_SeedListing;
};

void BindSeedListingToNativeVolume(PanelControllerNavigationFixture &_fixture,
                                   const nc::utility::NativeFSManager::Info &_volume)
{
    _fixture.NativeFSManager().SetPathVolume(_volume);
    _fixture.NativeFSManager().SetVolumes({_volume});
    _fixture.Host()->SetNativeFilesystem(true);
    [_fixture.Controller() loadListing:_fixture.SeedListing()];
}

nc::panel::PanelViewLayout MakeDetailsLayout(std::string _name, const short _filename_width)
{
    using nc::panel::PanelListViewColumns;
    using nc::panel::PanelListViewColumnsLayout;

    PanelListViewColumnsLayout list;
    list.columns.emplace_back(PanelListViewColumnsLayout::Column{
        .kind = PanelListViewColumns::Filename,
        .width = _filename_width,
        .max_width = 800,
        .min_width = 160,
    });
    list.columns.emplace_back(PanelListViewColumnsLayout::Column{
        .kind = PanelListViewColumns::DateModified,
        .width = 180,
        .max_width = 400,
        .min_width = 80,
    });
    list.icon_scale = 1;

    nc::panel::PanelViewLayout layout;
    layout.name = std::move(_name);
    layout.layout = std::move(list);
    return layout;
}

nc::panel::PanelViewLayout MakeBriefLayout(std::string _name)
{
    nc::panel::PanelBriefViewColumnsLayout brief;
    brief.mode = nc::panel::PanelBriefViewColumnsLayout::Mode::DynamicWidth;
    brief.dynamic_width_min = 140;
    brief.dynamic_width_max = 300;
    brief.icon_scale = 1;

    nc::panel::PanelViewLayout layout;
    layout.name = std::move(_name);
    layout.layout = brief;
    return layout;
}

class ScopedContextNotificationCounter
{
public:
    explicit ScopedContextNotificationCounter(PanelView *_view)
    {
        m_Token = [NSNotificationCenter.defaultCenter addObserverForName:NCPanelViewContextDidChangeNotification
                                                                  object:_view
                                                                   queue:nil
                                                              usingBlock:^(NSNotification *) {
                                                                ++m_Count;
                                                              }];
    }

    ~ScopedContextNotificationCounter()
    {
        if( m_Token )
            [NSNotificationCenter.defaultCenter removeObserver:m_Token];
    }

    ScopedContextNotificationCounter(const ScopedContextNotificationCounter &) = delete;
    ScopedContextNotificationCounter &operator=(const ScopedContextNotificationCounter &) = delete;

    int Count() const noexcept { return m_Count; }

private:
    __strong id m_Token = nil;
    int m_Count = 0;
};

} // namespace

#define PREFIX "PanelController production navigation "

TEST_CASE(PREFIX "pane-local presentation layout remains isolated and same-slot selection clears it")
{
    PanelControllerNavigationFixture fixture{true};
    PanelController *const controller = fixture.Controller();
    REQUIRE(controller.layoutIndex == 1);
    const auto configured = fixture.Layouts()->GetLayout(1);
    REQUIRE(configured);
    REQUIRE(configured->list());

    ScopedContextNotificationCounter notifications(fixture.View());
    const int initial_set_count = fixture.View().presentationLayoutSetCount;
    nc::panel::PanelViewLayout local = MakeDetailsLayout("stale persisted title", 420);
    std::get<nc::panel::PanelListViewColumnsLayout>(local.layout).icon_scale = 0;
    REQUIRE([controller applyPaneLocalPresentationLayout:local atConfiguredSlot:1]);
    CHECK(controller.layoutIndex == 1);
    CHECK([fixture.View() presentationLayout] == local.layout);
    CHECK(*fixture.Layouts()->GetLayout(1) == *configured);
    CHECK(fixture.View().presentationLayoutSetCount == initial_set_count + 1);
    CHECK(notifications.Count() == 1);

    REQUIRE([controller applyPaneLocalPresentationLayout:local atConfiguredSlot:1]);
    CHECK(fixture.View().presentationLayoutSetCount == initial_set_count + 1);
    CHECK(notifications.Count() == 1);

    auto resized = std::get<nc::panel::PanelListViewColumnsLayout>(local.layout);
    resized.columns.front().width = 460;
    [fixture.View() simulatePresentationLayoutChange:resized];
    CHECK([fixture.View() presentationLayout] == nc::panel::PanelViewLayout::LayoutVariant{resized});
    CHECK(*fixture.Layouts()->GetLayout(1) == *configured);
    CHECK(notifications.Count() == 2);

    const nc::panel::PanelViewLayout refreshed_configured = MakeDetailsLayout("Renamed Details", 350);
    fixture.Layouts()->ReplaceLayoutWithMandatoryNotification(refreshed_configured, 1);
    REQUIRE(fixture.WaitForPersistedLayoutName(1, refreshed_configured.name));
    CHECK([fixture.View() presentationLayout] == nc::panel::PanelViewLayout::LayoutVariant{resized});
    CHECK(*fixture.Layouts()->GetLayout(1) == refreshed_configured);
    CHECK(fixture.View().presentationLayoutSetCount == initial_set_count + 1);

    const int before_reset_notifications = notifications.Count();
    controller.layoutIndex = 1;
    CHECK([fixture.View() presentationLayout] == refreshed_configured.layout);
    CHECK(fixture.View().presentationLayoutSetCount == initial_set_count + 2);
    CHECK(notifications.Count() == before_reset_notifications + 1);

    controller.layoutIndex = 1;
    CHECK(fixture.View().presentationLayoutSetCount == initial_set_count + 2);
    CHECK(notifications.Count() == before_reset_notifications + 1);
}

TEST_CASE(PREFIX "current configured layout becomes a location-local baseline before first resize")
{
    PanelControllerNavigationFixture fixture{true};
    PanelController *const controller = fixture.Controller();
    REQUIRE(controller.layoutIndex == 1);
    const auto configured = fixture.Layouts()->GetLayout(1);
    REQUIRE(configured);
    REQUIRE(configured->list());

    ScopedContextNotificationCounter notifications(fixture.View());
    REQUIRE([controller applyPaneLocalPresentationLayout:*configured atConfiguredSlot:1]);
    CHECK(notifications.Count() == 1);

    auto resized = *configured->list();
    resized.columns.front().width += 40;
    [fixture.View() simulatePresentationLayoutChange:resized];
    CHECK([fixture.View() presentationLayout] == nc::panel::PanelViewLayout::LayoutVariant{resized});
    CHECK(*fixture.Layouts()->GetLayout(1) == *configured);

    controller.layoutIndex = 1;
    CHECK([fixture.View() presentationLayout] == configured->layout);
    CHECK(*fixture.Layouts()->GetLayout(1) == *configured);
}

TEST_CASE(PREFIX "pane-local Details accepts a reordered unique Filename column")
{
    PanelControllerNavigationFixture fixture{true};
    PanelController *const controller = fixture.Controller();
    const auto configured = fixture.Layouts()->GetLayout(1);
    REQUIRE(configured);

    nc::panel::PanelViewLayout reordered = MakeDetailsLayout("reordered", 420);
    auto &columns = std::get<nc::panel::PanelListViewColumnsLayout>(reordered.layout).columns;
    REQUIRE(columns.size() == 2);
    std::rotate(columns.begin(), columns.begin() + 1, columns.end());
    REQUIRE(columns.back().kind == nc::panel::PanelListViewColumns::Filename);

    REQUIRE([controller applyPaneLocalPresentationLayout:reordered atConfiguredSlot:1]);
    CHECK([fixture.View() presentationLayout] == reordered.layout);
    CHECK(*fixture.Layouts()->GetLayout(1) == *configured);
}

TEST_CASE(PREFIX "pane-local presentation layout rejects invalid or incompatible input atomically")
{
    PanelControllerNavigationFixture fixture{true};
    PanelController *const controller = fixture.Controller();
    REQUIRE(controller.layoutIndex == 1);
    const auto initial_layout = fixture.Layouts()->GetLayout(1);
    REQUIRE(initial_layout);
    const auto initial_presentation = [fixture.View() presentationLayout];
    const int initial_set_count = fixture.View().presentationLayoutSetCount;
    ScopedContextNotificationCounter notifications(fixture.View());

    const nc::panel::PanelViewLayout details = MakeDetailsLayout("Details", 420);
    CHECK_FALSE([controller applyPaneLocalPresentationLayout:details atConfiguredSlot:-1]);
    CHECK_FALSE([controller applyPaneLocalPresentationLayout:details atConfiguredSlot:20]);

    nc::panel::PanelViewLayout disabled;
    disabled.name = "Disabled";
    CHECK_FALSE([controller applyPaneLocalPresentationLayout:disabled atConfiguredSlot:1]);

    const nc::panel::PanelViewLayout brief = MakeBriefLayout("Icons");
    CHECK_FALSE([controller applyPaneLocalPresentationLayout:brief atConfiguredSlot:1]);
    CHECK_FALSE([controller applyPaneLocalPresentationLayout:details atConfiguredSlot:0]);

    nc::panel::PanelViewLayout malformed = details;
    auto &malformed_list = std::get<nc::panel::PanelListViewColumnsLayout>(malformed.layout);
    malformed_list.columns.emplace_back(malformed_list.columns.front());
    CHECK_FALSE([controller applyPaneLocalPresentationLayout:malformed atConfiguredSlot:1]);

    CHECK(controller.layoutIndex == 1);
    CHECK([fixture.View() presentationLayout] == initial_presentation);
    CHECK(*fixture.Layouts()->GetLayout(1) == *initial_layout);
    CHECK(fixture.View().presentationLayoutSetCount == initial_set_count);
    CHECK(notifications.Count() == 0);
}

TEST_CASE(PREFIX "pane-local presentation rejects a malformed configured slot atomically")
{
    PanelControllerNavigationFixture fixture{true};
    PanelController *const controller = fixture.Controller();

    nc::panel::PanelViewLayout malformed_slot = MakeDetailsLayout("Malformed Details", 360);
    auto &columns = std::get<nc::panel::PanelListViewColumnsLayout>(malformed_slot.layout).columns;
    columns.emplace_back(columns.front());
    fixture.Layouts()->ReplaceLayoutWithMandatoryNotification(malformed_slot, 1);
    REQUIRE(fixture.WaitForPersistedLayoutName(1, malformed_slot.name));

    const int set_count = fixture.View().presentationLayoutSetCount;
    const auto presentation = [fixture.View() presentationLayout];
    ScopedContextNotificationCounter notifications(fixture.View());
    const nc::panel::PanelViewLayout valid_local = MakeDetailsLayout("Local", 440);

    CHECK_FALSE([controller applyPaneLocalPresentationLayout:valid_local atConfiguredSlot:1]);
    CHECK(controller.layoutIndex == 1);
    CHECK([fixture.View() presentationLayout] == presentation);
    CHECK(fixture.View().presentationLayoutSetCount == set_count);
    CHECK(notifications.Count() == 0);
}

TEST_CASE(PREFIX "disabled configured slot ends the local override with the last-resort layout")
{
    PanelControllerNavigationFixture fixture{true};
    PanelController *const controller = fixture.Controller();
    REQUIRE(controller.layoutIndex == 1);

    const nc::panel::PanelViewLayout local = MakeDetailsLayout("local", 440);
    REQUIRE([controller applyPaneLocalPresentationLayout:local atConfiguredSlot:1]);
    CHECK([fixture.View() presentationLayout] == local.layout);

    nc::panel::PanelViewLayout disabled;
    disabled.name = "Disabled Details";
    fixture.Layouts()->ReplaceLayoutWithMandatoryNotification(disabled, 1);
    REQUIRE(fixture.WaitForPersistedLayoutName(1, disabled.name));

    const auto stored = fixture.Layouts()->GetLayout(1);
    REQUIRE(stored);
    CHECK(stored->is_disabled());
    CHECK(controller.layoutIndex == 1);
    CHECK([fixture.View() presentationLayout] == nc::panel::PanelViewLayoutsStorage::LastResortLayout()->layout);
}

TEST_CASE(PREFIX "configured type replacement ends the local override and restores Commander layout mutation")
{
    PanelControllerNavigationFixture fixture{true};
    PanelController *const controller = fixture.Controller();
    REQUIRE(controller.layoutIndex == 1);

    const nc::panel::PanelViewLayout local = MakeDetailsLayout("local", 440);
    REQUIRE([controller applyPaneLocalPresentationLayout:local atConfiguredSlot:1]);
    CHECK([fixture.View() presentationLayout] == local.layout);

    const nc::panel::PanelViewLayout brief = MakeBriefLayout("Switched to Icons");
    fixture.Layouts()->ReplaceLayoutWithMandatoryNotification(brief, 1);
    REQUIRE(fixture.WaitForPersistedLayoutName(1, brief.name));
    CHECK([fixture.View() presentationLayout] == brief.layout);
    CHECK(*fixture.Layouts()->GetLayout(1) == brief);

    const nc::panel::PanelViewLayout details = MakeDetailsLayout("Restored Details", 360);
    fixture.Layouts()->ReplaceLayoutWithMandatoryNotification(details, 1);
    REQUIRE(fixture.WaitForPersistedLayoutName(1, details.name));
    CHECK([fixture.View() presentationLayout] == details.layout);

    auto resized = std::get<nc::panel::PanelListViewColumnsLayout>(details.layout);
    resized.columns.front().width = 470;
    [fixture.View() simulatePresentationLayoutChange:resized];
    REQUIRE(fixture.WaitForPersistedFilenameWidth(1, 470));

    const auto stored = fixture.Layouts()->GetLayout(1);
    REQUIRE(stored);
    CHECK(stored->name == details.name);
    CHECK(stored->layout == nc::panel::PanelViewLayout::LayoutVariant{resized});
}

TEST_CASE(PREFIX "security-scope containment requires an exact path boundary")
{
    using nc::sandbox::PathIsWithinScope;

    CHECK(PathIsWithinScope("/scope", "/scope"));
    CHECK(PathIsWithinScope("/scope/child", "/scope"));
    CHECK(PathIsWithinScope("/scope/child/grandchild", "/scope"));
    CHECK_FALSE(PathIsWithinScope("/scope-other", "/scope"));
    CHECK_FALSE(PathIsWithinScope("/scopex", "/scope"));
    CHECK_FALSE(PathIsWithinScope("/scope", "/scope/child"));
    CHECK(PathIsWithinScope("/private/tmp", "/"));
    CHECK_FALSE(PathIsWithinScope("relative/path", "/"));
    CHECK_FALSE(PathIsWithinScope("/scope", ""));
}

TEST_CASE(PREFIX "forwards only its matching Store snapshot to the Explorer footer")
{
    PanelControllerNavigationFixture fixture;
    auto footer = [[PanelControllerNavigationSnapshotFooter alloc]
        initWithFrame:NSZeroRect
                theme:std::make_unique<TestFooterTheme>()
    explorerAppearance:true];
    [fixture.View() setValue:footer forKey:@"m_FooterView"];

    nc::core::PaneSnapshot snapshot;
    snapshot.pane_id = fixture.Controller().paneId;
    snapshot.state.item_count = 3;
    snapshot.state.selected_count = 2;
    snapshot.state.selected_bytes = 1536;
    [fixture.View() applyExplorerPaneSnapshot:snapshot];

    CHECK(footer.appliedSnapshotCount == 1);
    CHECK(footer.itemCount == 3);
    CHECK(footer.selectedCount == 2);
    CHECK(footer.selectedBytes == 1536);

    snapshot.pane_id = nc::core::PaneId{fixture.Controller().paneId.value + 1};
    snapshot.state.item_count = 99;
    [fixture.View() applyExplorerPaneSnapshot:snapshot];
    CHECK(footer.appliedSnapshotCount == 1);
    CHECK(footer.itemCount == 3);
}

TEST_CASE(PREFIX "publishes Committed only after the async worker mutates the model")
{
    PanelControllerNavigationFixture fixture;
    const auto target = UniformListing(fixture.Host(), "/success/", "loaded.txt");
    const auto plan = fixture.Host()->ScriptSuccess("/success/", target, true);
    const auto callback = std::make_shared<CallbackRecorder>();
    std::vector<PaneLifecycleEvent> events;
    bool committed_model_visible = false;
    int view_updates_at_commit = -1;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
        if( const auto *committed = std::get_if<PaneLifecycleCommitted>(&_event.payload) ) {
            committed_model_visible = fixture.Controller().data.ListingPtr() == committed->listing &&
                                      fixture.Controller().dataGeneration == committed->controller_generation;
            view_updates_at_commit = fixture.View().dataUpdateCount;
        }
    }];
    const int initial_view_updates = fixture.View().dataUpdateCount;

    const auto submission = [fixture.Controller() GoToDirWithContext:fixture.Request(
                                                                          "/success/", true, callback)];
    CHECK(submission.has_value());
    REQUIRE(events.size() == 1);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));
    REQUIRE(ControllableNavigationHost::WaitEntered(plan));
    ControllableNavigationHost::Release(plan);
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 &&
               std::holds_alternative<PaneLifecycleCommitted>(events.back().payload);
    }));

    CHECK(committed_model_visible);
    CHECK(view_updates_at_commit == initial_view_updates);
    CHECK(fixture.View().dataUpdateCount == initial_view_updates + 1);
    CHECK(fixture.Controller().data.ListingPtr() == target);
    const auto calls = callback->Calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls.front().succeeded);
    CHECK(calls.front().source == DirectoryChangeResultSource::Fetch);
    CHECK(calls.front().current);
}

TEST_CASE(PREFIX "keeps the main heartbeat responsive through a 100k prepared commit")
{
    PanelControllerNavigationFixture fixture;
    fixture.View().forwardsDataUpdatedToProduction = true;

    auto sort = fixture.Controller().data.SortMode();
    sort.sort = nc::panel::data::SortMode::SortByNameRev;
    sort.collation = nc::panel::data::SortMode::Collation::Natural;
    [fixture.Controller() changeSortingModeTo:sort];
    auto hard_filter = fixture.Controller().data.HardFiltering();
    hard_filter.show_hidden = false;
    [fixture.Controller() changeHardFilteringTo:hard_filter];

    const auto target = LargeUniformListing(fixture.Host(), "/large/", 100'000);
    fixture.Host()->ScriptSuccess("/large/", target, false);
    auto request = fixture.Request("/large/", true);
    request->RequestSelectedEntries = {"item-050000.txt"};
    request->RequestFocusedEntry = "item-050000.txt";

    struct HeartbeatState {
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration max_gap{};
        int ticks = 0;
    };
    const auto heartbeat = std::make_shared<HeartbeatState>();
    NSTimer *const timer = [NSTimer timerWithTimeInterval:0.01
                                                 repeats:YES
                                                   block:^(NSTimer *) {
                                                     const auto now = std::chrono::steady_clock::now();
                                                     heartbeat->max_gap =
                                                         std::max(heartbeat->max_gap, now - heartbeat->last);
                                                     heartbeat->last = now;
                                                     ++heartbeat->ticks;
                                                   }];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];

    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];
    REQUIRE([fixture.Controller() GoToDirWithContext:request].has_value());
    REQUIRE(RunMainLoopUntil(
        [&] {
            return events.size() == 2 &&
                   std::holds_alternative<PaneLifecycleCommitted>(events.back().payload);
        },
        15s));

    const auto final_gap = std::chrono::steady_clock::now() - heartbeat->last;
    heartbeat->max_gap = std::max(heartbeat->max_gap, final_gap);
    [timer invalidate];

    CHECK(heartbeat->ticks >= 3);
    CHECK(heartbeat->max_gap < 250ms);
    CHECK(fixture.Controller().data.ListingPtr() == target);
    CHECK(fixture.Controller().data.RawEntriesCount() == 100'002);
    CHECK(fixture.Controller().data.SortedIndexForName(".hidden-large-test") == -1);
    REQUIRE(fixture.Controller().data.SelectedEntriesSorted().size() == 1);
    CHECK(fixture.Controller().data.SelectedEntriesSorted().front().Filename() == "item-050000.txt");
    CHECK(fixture.View().lastPanelChangeFocus == "item-050000.txt");
}

TEST_CASE(PREFIX "rejects a prepared result after a newer sort preference")
{
    PanelControllerNavigationFixture fixture;
    const auto initial_listing = fixture.Controller().data.ListingPtr();
    const auto initial_generation = fixture.Controller().dataGeneration;
    const auto target = UniformListing(fixture.Host(), "/stale-options/", "stale.txt");
    const auto plan = fixture.Host()->ScriptSuccess("/stale-options/", target, true);
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    REQUIRE([fixture.Controller() GoToDirWithContext:fixture.Request("/stale-options/", true)].has_value());
    REQUIRE(ControllableNavigationHost::WaitEntered(plan));
    auto newer_sort = fixture.Controller().data.SortMode();
    newer_sort.sort = nc::panel::data::SortMode::SortByNameRev;
    [fixture.Controller() changeSortingModeTo:newer_sort];
    ControllableNavigationHost::Release(plan);

    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleCancelled>(events.back().payload);
    }));
    CHECK(std::get<PaneLifecycleCancelled>(events.back().payload).reason ==
          PaneCancellationReason::InternalAbort);
    CHECK(fixture.Controller().data.ListingPtr() == initial_listing);
    CHECK(fixture.Controller().dataGeneration == initial_generation);
    CHECK(fixture.Controller().data.SortMode() == newer_sort);
}

TEST_CASE(PREFIX "cancels a detached 100k preparation without committing it")
{
    PanelControllerNavigationFixture fixture;
    const auto initial_listing = fixture.Controller().data.ListingPtr();
    const auto initial_generation = fixture.Controller().dataGeneration;
    const auto target = LargeUniformListing(fixture.Host(), "/cancel-preparation/", 100'000);
    const auto plan = fixture.Host()->ScriptSuccess("/cancel-preparation/", target, false);
    const auto callback = std::make_shared<CallbackRecorder>();
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    REQUIRE([fixture.Controller() GoToDirWithContext:fixture.Request(
                                                      "/cancel-preparation/", true, callback)]
                .has_value());
    REQUIRE(RunMainLoopUntil([&] { return ControllableNavigationHost::Completed(plan); }));
    [fixture.Controller() CancelBackgroundOperations];
    REQUIRE(RunMainLoopUntil(
        [&] {
            return events.size() == 2 &&
                   std::holds_alternative<PaneLifecycleCancelled>(events.back().payload) &&
                   !fixture.Controller().isDoingBackgroundLoading;
        },
        15s));

    CHECK(std::get<PaneLifecycleCancelled>(events.back().payload).reason ==
          PaneCancellationReason::InternalAbort);
    CHECK(fixture.Controller().data.ListingPtr() == initial_listing);
    CHECK(fixture.Controller().dataGeneration == initial_generation);
    CHECK(callback->Calls().empty());
}

TEST_CASE(PREFIX "loads and force-refreshes a real local NativeHost directory")
{
    PanelControllerNavigationFixture fixture;
    TempTestDir temporary;
    const std::filesystem::path directory = temporary.directory / "native-live";
    std::filesystem::create_directories(directory);
    const std::filesystem::path first_file = directory / "first.txt";
    {
        std::ofstream output(first_file);
        REQUIRE(output);
        output << "first";
    }
    const std::string path = std::filesystem::canonical(directory).string() + "/";
    const auto callback = std::make_shared<CallbackRecorder>();
    auto request = std::make_shared<DirectoryChangeRequest>();
    request->RequestedDirectory = path;
    request->VFS = fixture.NativeHost();
    request->PerformAsynchronous = true;
    request->InitiatedByUser = true;
    request->LoadingResultCallback =
        [_callback = callback](const std::expected<void, nc::Error> &_result,
                               const DirectoryChangeResultSource _source,
                               const std::function<bool()> &_is_current) {
            _callback->Record(_result, _source, _is_current);
        };
    const auto initial_generation = fixture.Controller().dataGeneration;
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    REQUIRE([fixture.Controller() GoToDirWithContext:request].has_value());
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload);
    }));

    REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));
    CHECK(events.front().descriptor.kind == PaneRequestKind::Navigation);
    CHECK(events.front().descriptor.initiated_by_user);
    REQUIRE(events.front().descriptor.target);
    CHECK(events.front().descriptor.target->host == fixture.NativeHost());
    CHECK(events.front().descriptor.target->path == path);
    const VFSListingPtr first_listing = fixture.Controller().data.ListingPtr();
    REQUIRE(first_listing);
    CHECK(first_listing->IsUniform());
    CHECK(first_listing->Host() == fixture.NativeHost());
    CHECK(first_listing->Directory() == path);
    CHECK(fixture.Controller().dataGeneration == initial_generation + 1);
    CHECK(std::ranges::any_of(*first_listing, [](const VFSListingItem &_item) {
        return _item.Filename() == "first.txt";
    }));
    const auto calls = callback->Calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls.front().succeeded);
    CHECK(calls.front().source == DirectoryChangeResultSource::Fetch);
    CHECK(calls.front().current);

    const std::filesystem::path second_file = directory / "second.txt";
    {
        std::ofstream output(second_file);
        REQUIRE(output);
        output << "second";
    }
    REQUIRE([fixture.Controller() submitUserRefresh]);
    REQUIRE(RunMainLoopUntil([&] {
        const VFSListingPtr refreshed_listing = fixture.Controller().data.ListingPtr();
        return events.size() == 4 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload) &&
               refreshed_listing && refreshed_listing != first_listing &&
               std::ranges::any_of(*refreshed_listing, [](const VFSListingItem &_item) {
                   return _item.Filename() == "second.txt";
               });
    }));

    REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events[2].payload));
    CHECK(events[2].descriptor.kind == PaneRequestKind::Refresh);
    CHECK(events[2].descriptor.initiated_by_user);
    const VFSListingPtr refreshed_listing = fixture.Controller().data.ListingPtr();
    REQUIRE(refreshed_listing);
    CHECK(refreshed_listing->IsUniform());
    CHECK(refreshed_listing->Host() == fixture.NativeHost());
    CHECK(refreshed_listing->Directory() == path);
    CHECK(fixture.Controller().dataGeneration == initial_generation + 1);
}

TEST_CASE(PREFIX "publishes typed permission failure from a real local NativeHost directory")
{
    PanelControllerNavigationFixture fixture;
    TempTestDir temporary;
    const std::filesystem::path directory = temporary.directory / "native-permission-denied";
    std::filesystem::create_directories(directory);
    const std::string path = std::filesystem::canonical(directory).string() + "/";
    struct stat initial_status {};
    REQUIRE(::stat(directory.c_str(), &initial_status) == 0);
    struct RestoreDirectoryMode {
        std::filesystem::path directory;
        mode_t mode;

        ~RestoreDirectoryMode() { (void)::chmod(directory.c_str(), mode); }
    } restore_directory_mode{directory, static_cast<mode_t>(initial_status.st_mode & 07777)};
    REQUIRE(::chmod(directory.c_str(), 0) == 0);
    if( ::geteuid() == 0 || ::access(directory.c_str(), R_OK | X_OK) == 0 )
        SKIP("The current test user bypasses directory permissions.");

    const VFSListingPtr initial_listing = fixture.Controller().data.ListingPtr();
    const auto initial_generation = fixture.Controller().dataGeneration;
    const int initial_view_updates = fixture.View().dataUpdateCount;
    const auto callback = std::make_shared<CallbackRecorder>();
    auto request = std::make_shared<DirectoryChangeRequest>();
    request->RequestedDirectory = path;
    request->VFS = fixture.NativeHost();
    request->PerformAsynchronous = true;
    request->InitiatedByUser = true;
    request->LoadingResultCallback =
        [_callback = callback](const std::expected<void, nc::Error> &_result,
                               const DirectoryChangeResultSource _source,
                               const std::function<bool()> &_is_current) {
            _callback->Record(_result, _source, _is_current);
        };
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    REQUIRE([fixture.Controller() GoToDirWithContext:request].has_value());
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleFailed>(events.back().payload) &&
               callback->Calls().size() == 1;
    }));

    REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));
    REQUIRE(std::holds_alternative<PaneLifecycleFailed>(events.back().payload));
    const auto &failure = std::get<PaneLifecycleFailed>(events.back().payload).error;
    CHECK(failure.original_error == nc::Error{nc::Error::POSIX, EACCES});
    CHECK(failure.category == nc::core::FileManagerErrorCategory::PermissionError);
    CHECK(failure.user_message_key == "errors.permission");
    CHECK(failure.affected_items == std::vector<std::string>{path});
    REQUIRE(failure.provider_id);
    REQUIRE(fixture.NativeHost()->Tag());
    CHECK(*failure.provider_id == fixture.NativeHost()->Tag());
    CHECK(fixture.Controller().data.ListingPtr() == initial_listing);
    CHECK(fixture.Controller().dataGeneration == initial_generation);
    CHECK(fixture.View().dataUpdateCount == initial_view_updates);
    const auto calls = callback->Calls();
    REQUIRE(calls.size() == 1);
    CHECK_FALSE(calls.front().succeeded);
    REQUIRE(calls.front().error);
    CHECK(*calls.front().error == nc::Error{nc::Error::POSIX, EACCES});
    CHECK(calls.front().source == DirectoryChangeResultSource::Fetch);
    CHECK(calls.front().current);
}

TEST_CASE(PREFIX "fails closed when a user denies requested directory access before fetch")
{
    PanelControllerNavigationFixture fixture;
    fixture.AccessProvider().has_access = false;
    fixture.AccessProvider().grants_access = false;
    const auto callback = std::make_shared<CallbackRecorder>();
    const auto initial_listing = fixture.Controller().data.ListingPtr();
    const auto initial_generation = fixture.Controller().dataGeneration;
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    REQUIRE([fixture.Controller() GoToDirWithContext:fixture.Request("/access-denied/", true, callback)].has_value());
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleFailed>(events.back().payload) &&
               callback->Calls().size() == 1;
    }));

    REQUIRE(std::holds_alternative<PaneLifecycleFailed>(events.back().payload));
    const auto &failure = std::get<PaneLifecycleFailed>(events.back().payload).error;
    CHECK(failure.original_error == nc::Error{nc::Error::POSIX, EPERM});
    CHECK(failure.category == nc::core::FileManagerErrorCategory::PermissionError);
    CHECK(failure.user_message_key == "errors.permission");
    CHECK(fixture.AccessProvider().has_access_calls == 1);
    CHECK(fixture.AccessProvider().request_access_calls == 1);
    CHECK(fixture.AccessProvider().checked_directories == std::vector<std::string>{"/access-denied/"});
    CHECK(fixture.AccessProvider().requested_directories == std::vector<std::string>{"/access-denied/"});
    CHECK(fixture.Host()->FetchCount() == 0);
    CHECK(fixture.Controller().data.ListingPtr() == initial_listing);
    CHECK(fixture.Controller().dataGeneration == initial_generation);
    const auto calls = callback->Calls();
    REQUIRE(calls.size() == 1);
    REQUIRE(calls.front().error);
    CHECK(*calls.front().error == nc::Error{nc::Error::POSIX, EPERM});
    CHECK(calls.front().source == DirectoryChangeResultSource::Admission);
    CHECK(calls.front().current);
}

TEST_CASE(PREFIX "does not prompt for an automatic navigation without directory access")
{
    PanelControllerNavigationFixture fixture;
    fixture.AccessProvider().has_access = false;
    fixture.AccessProvider().grants_access = true;
    const auto callback = std::make_shared<CallbackRecorder>();
    const auto initial_listing = fixture.Controller().data.ListingPtr();
    const auto initial_generation = fixture.Controller().dataGeneration;
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    REQUIRE([fixture.Controller() GoToDirWithContext:fixture.Request("/automatic-denied/", true, callback, false)]
                .has_value());
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleFailed>(events.back().payload) &&
               callback->Calls().size() == 1;
    }));

    REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));
    CHECK_FALSE(events.front().descriptor.initiated_by_user);
    REQUIRE(std::holds_alternative<PaneLifecycleFailed>(events.back().payload));
    const auto &failure = std::get<PaneLifecycleFailed>(events.back().payload).error;
    CHECK(failure.original_error == nc::Error{nc::Error::POSIX, EPERM});
    CHECK(failure.category == nc::core::FileManagerErrorCategory::PermissionError);
    CHECK(fixture.AccessProvider().has_access_calls == 1);
    CHECK(fixture.AccessProvider().request_access_calls == 0);
    CHECK(fixture.Host()->FetchCount() == 0);
    CHECK(fixture.Controller().data.ListingPtr() == initial_listing);
    CHECK(fixture.Controller().dataGeneration == initial_generation);
}

TEST_CASE(PREFIX "continues user navigation after one granted directory-access prompt")
{
    PanelControllerNavigationFixture fixture;
    fixture.AccessProvider().has_access = false;
    fixture.AccessProvider().grants_access = true;
    const auto target = UniformListing(fixture.Host(), "/access-granted/", "granted.txt");
    const auto plan = fixture.Host()->ScriptSuccess("/access-granted/", target, true);
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    REQUIRE([fixture.Controller() GoToDirWithContext:fixture.Request("/access-granted/", true)].has_value());
    REQUIRE(events.size() == 1);
    REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));
    REQUIRE(ControllableNavigationHost::WaitEntered(plan));
    CHECK(fixture.AccessProvider().has_access_calls == 1);
    CHECK(fixture.AccessProvider().request_access_calls == 1);
    CHECK(fixture.Host()->FetchCount() == 1);

    ControllableNavigationHost::Release(plan);
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload);
    }));
    CHECK(fixture.Controller().data.ListingPtr() == target);
}

TEST_CASE(PREFIX "explicit Up submits one uniform parent request and preserves the departed focus")
{
    PanelControllerNavigationFixture fixture;
    const auto parent_listing = UniformListing(fixture.Host(), "/", "parent.txt");
    const auto plan = fixture.Host()->ScriptSuccess("/", parent_listing, true);
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    REQUIRE(nc::panel::actions::SubmitExplicitGoToEnclosingFolder(fixture.Controller()));
    REQUIRE(events.size() == 1);
    REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));
    CHECK(events.front().descriptor.kind == PaneRequestKind::Navigation);
    CHECK(events.front().descriptor.initiated_by_user);
    REQUIRE(events.front().descriptor.target);
    CHECK(events.front().descriptor.target->host == fixture.Host());
    CHECK(events.front().descriptor.target->path == "/");
    REQUIRE(ControllableNavigationHost::WaitEntered(plan));

    ControllableNavigationHost::Release(plan);
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload);
    }));
    CHECK(fixture.Controller().data.ListingPtr() == parent_listing);
    CHECK(fixture.View().lastPanelChangeFocus == "seed");
    CHECK(fixture.View().lastPanelChangeLoadedPreviousState);
}

TEST_CASE(PREFIX "explicit Up rejects a non-uniform pane without moving history")
{
    PanelControllerNavigationFixture fixture;
    [fixture.Controller() loadListing:NonUniformListing(fixture.Host())];
    REQUIRE_FALSE(fixture.Controller().isUniform);
    const auto before = fixture.Controller().history.GetNavigationState();

    CHECK_FALSE(nc::panel::actions::SubmitExplicitGoToEnclosingFolder(fixture.Controller()));
    CHECK(fixture.Controller().history.GetNavigationState() == before);
    CHECK_FALSE(fixture.Controller().isDoingBackgroundLoading);
}

TEST_CASE(PREFIX "dot-dot entry submits only the enclosing-folder request")
{
    PanelControllerNavigationFixture fixture;
    const int dotdot_position = fixture.Controller().data.SortedIndexForName("..");
    REQUIRE(dotdot_position >= 0);
    fixture.View().curpos = dotdot_position;
    REQUIRE(fixture.View().item);
    REQUIRE(fixture.View().item.IsDotDot());
    const auto parent_listing = UniformListing(fixture.Host(), "/", "parent.txt");
    const auto plan = fixture.Host()->ScriptSuccess("/", parent_listing, true);
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    nc::panel::actions::GoIntoFolder{false}.Perform(fixture.Controller(), nil);

    REQUIRE(events.size() == 1);
    REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));
    REQUIRE(ControllableNavigationHost::WaitEntered(plan));
    CHECK(fixture.Host()->FetchCount() == 1);
    ControllableNavigationHost::Release(plan);
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload);
    }));
    CHECK(fixture.Controller().data.ListingPtr() == parent_listing);
    CHECK(fixture.View().lastPanelChangeFocus == "seed");
    CHECK(fixture.View().lastPanelChangeLoadedPreviousState);
}

TEST_CASE(PREFIX "live command availability distinguishes navigation, refresh and external workers")
{
    PanelControllerNavigationFixture fixture;
    auto availability = [fixture.Controller() paneNavigationAvailability];
    REQUIRE(availability);
    CHECK(availability->up == nc::core::NavigationUpAvailability::Available);
    CHECK(availability->refresh == nc::core::NavigationRefreshAvailability::Available);

    const auto navigation_listing = UniformListing(fixture.Host(), "/active/", "active.txt");
    const auto navigation_plan = fixture.Host()->ScriptSuccess("/active/", navigation_listing, true);
    REQUIRE([fixture.Controller() GoToDirWithContext:fixture.Request("/active/", true)].has_value());
    REQUIRE(ControllableNavigationHost::WaitEntered(navigation_plan));
    availability = [fixture.Controller() paneNavigationAvailability];
    REQUIRE(availability);
    CHECK(availability->up == nc::core::NavigationUpAvailability::Busy);
    CHECK(availability->refresh == nc::core::NavigationRefreshAvailability::Busy);
    ControllableNavigationHost::Release(navigation_plan);
    REQUIRE(RunMainLoopUntil([&] {
        const auto current = [fixture.Controller() paneNavigationAvailability];
        return current && current->refresh == nc::core::NavigationRefreshAvailability::Available;
    }));

    const auto refreshed_listing = UniformListing(fixture.Host(), "/active/", "refreshed.txt");
    const auto refresh_plan = fixture.Host()->ScriptSuccess("/active/", refreshed_listing, true);
    REQUIRE([fixture.Controller() submitUserRefresh]);
    REQUIRE(ControllableNavigationHost::WaitEntered(refresh_plan));
    availability = [fixture.Controller() paneNavigationAvailability];
    REQUIRE(availability);
    CHECK(availability->up == nc::core::NavigationUpAvailability::Available);
    CHECK(availability->refresh == nc::core::NavigationRefreshAvailability::Available);
    ControllableNavigationHost::Release(refresh_plan);
    REQUIRE(RunMainLoopUntil([&] { return !fixture.Controller().isDoingBackgroundLoading; }));

    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool released = false;
    };
    const auto gate = std::make_shared<Gate>();
    [fixture.Controller() commitCancelableLoadingTask:[gate](const nc::panel::CancelableLoadingTaskContext &) {
        std::unique_lock lock{gate->mutex};
        gate->entered = true;
        gate->changed.notify_all();
        gate->changed.wait(lock, [&] { return gate->released; });
    }];
    {
        std::unique_lock lock{gate->mutex};
        REQUIRE(gate->changed.wait_for(lock, 1s, [&] { return gate->entered; }));
    }
    availability = [fixture.Controller() paneNavigationAvailability];
    REQUIRE(availability);
    CHECK(availability->up == nc::core::NavigationUpAvailability::Busy);
    CHECK(availability->refresh == nc::core::NavigationRefreshAvailability::Busy);
    CHECK_FALSE([fixture.Controller() submitUserRefresh]);
    {
        const std::lock_guard lock{gate->mutex};
        gate->released = true;
    }
    gate->changed.notify_all();
    REQUIRE(RunMainLoopUntil([&] { return !fixture.Controller().isDoingBackgroundLoading; }));
}

TEST_CASE(PREFIX "rejects a deferred navigation as Busy when a Started observer adds external loading work")
{
    PanelControllerNavigationFixture fixture;
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool released = false;
    };

    const auto gate = std::make_shared<Gate>();
    const auto deferred_callback = std::make_shared<CallbackRecorder>();
    const VFSListingPtr initial_listing = fixture.Controller().data.ListingPtr();
    const unsigned long initial_generation = fixture.Controller().dataGeneration;
    std::vector<PaneLifecycleEvent> events;
    bool submitted_deferred_navigation = false;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
        if( submitted_deferred_navigation || !std::holds_alternative<PaneLifecycleStarted>(_event.payload) ||
            !_event.descriptor.target || _event.descriptor.target->path != "/first/")
            return;

        [fixture.Controller() commitCancelableLoadingTask:[gate](const nc::panel::CancelableLoadingTaskContext &) {
            std::unique_lock lock{gate->mutex};
            gate->entered = true;
            gate->changed.notify_all();
            gate->changed.wait(lock, [&] { return gate->released; });
        }];
        submitted_deferred_navigation = true;
        CHECK([fixture.Controller() GoToDirWithContext:fixture.Request("/second/", true, deferred_callback)].has_value());
    }];

    CHECK([fixture.Controller() GoToDirWithContext:fixture.Request("/first/", true)].has_value());
    REQUIRE(submitted_deferred_navigation);
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 3 && std::holds_alternative<PaneLifecycleCancelled>(events[1].payload) &&
               std::holds_alternative<PaneLifecycleRejected>(events[2].payload);
    }));

    REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));
    CHECK(events[0].descriptor.target->path == "/first/");
    REQUIRE(std::holds_alternative<PaneLifecycleCancelled>(events[1].payload));
    CHECK(std::get<PaneLifecycleCancelled>(events[1].payload).reason == PaneCancellationReason::InternalAbort);
    REQUIRE(std::holds_alternative<PaneLifecycleRejected>(events[2].payload));
    CHECK(std::get<PaneLifecycleRejected>(events[2].payload).reason == PaneRejectionReason::Busy);
    CHECK(events[2].descriptor.target->path == "/second/");
    CHECK(fixture.Host()->FetchCount() == 0);
    CHECK(fixture.Controller().data.ListingPtr() == initial_listing);
    CHECK(fixture.Controller().dataGeneration == initial_generation);

    const auto deferred_calls = deferred_callback->Calls();
    REQUIRE(deferred_calls.size() == 1);
    CHECK_FALSE(deferred_calls.front().succeeded);
    REQUIRE(deferred_calls.front().error);
    CHECK(*deferred_calls.front().error == nc::Error{nc::Error::POSIX, EBUSY});
    CHECK(deferred_calls.front().source == DirectoryChangeResultSource::Admission);
    CHECK(deferred_calls.front().current);

    {
        std::unique_lock lock{gate->mutex};
        REQUIRE(gate->changed.wait_for(lock, 1s, [&] { return gate->entered; }));
        gate->released = true;
    }
    gate->changed.notify_all();
    REQUIRE(RunMainLoopUntil([&] { return !fixture.Controller().isDoingBackgroundLoading; }));
}

TEST_CASE(PREFIX "retains the committed model when the async fetch fails")
{
    PanelControllerNavigationFixture fixture;
    const auto callback = std::make_shared<CallbackRecorder>();
    fixture.Host()->ScriptError("/missing/", nc::Error{nc::Error::POSIX, ENOENT});
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];
    const auto initial_generation = fixture.Controller().dataGeneration;
    const auto initial_listing = fixture.Controller().data.ListingPtr();

    CHECK([fixture.Controller() GoToDirWithContext:fixture.Request("/missing/", true, callback)].has_value());
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleFailed>(events.back().payload);
    }));

    CHECK(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));
    const auto &failed = std::get<PaneLifecycleFailed>(events.back().payload);
    CHECK(failed.error.original_error == nc::Error{nc::Error::POSIX, ENOENT});
    CHECK(fixture.Controller().dataGeneration == initial_generation);
    CHECK(fixture.Controller().data.ListingPtr() == initial_listing);
    const auto calls = callback->Calls();
    REQUIRE(calls.size() == 1);
    CHECK_FALSE(calls.front().succeeded);
    REQUIRE(calls.front().error);
    CHECK(*calls.front().error == nc::Error{nc::Error::POSIX, ENOENT});
    CHECK(calls.front().source == DirectoryChangeResultSource::Fetch);
    CHECK(calls.front().current);
}

TEST_CASE(PREFIX "synchronous replacement supersedes a gated worker and suppresses its stale callback")
{
    PanelControllerNavigationFixture fixture;
    const auto first_listing = UniformListing(fixture.Host(), "/first/", "first.txt");
    const auto second_listing = UniformListing(fixture.Host(), "/second/", "second.txt");
    const auto first_plan = fixture.Host()->ScriptSuccess("/first/", first_listing, true);
    fixture.Host()->ScriptSuccess("/second/", second_listing, false);
    const auto first_callback = std::make_shared<CallbackRecorder>();
    const auto second_callback = std::make_shared<CallbackRecorder>();
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    CHECK([fixture.Controller() GoToDirWithContext:fixture.Request("/first/", true, first_callback)].has_value());
    REQUIRE(ControllableNavigationHost::WaitEntered(first_plan));
    const auto replacement =
        [fixture.Controller() GoToDirWithContext:fixture.Request("/second/", false, second_callback)];
    REQUIRE(replacement.has_value());
    REQUIRE(RunMainLoopUntil([&] { return ControllableNavigationHost::Completed(first_plan); }));

    REQUIRE(events.size() == 4);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));
    REQUIRE(std::holds_alternative<PaneLifecycleSuperseded>(events[1].payload));
    CHECK(events[1].request_id == events[0].request_id);
    CHECK(std::get<PaneLifecycleSuperseded>(events[1].payload).replacement == events[2].request_id);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[2].payload));
    CHECK(std::holds_alternative<PaneLifecycleCommitted>(events[3].payload));
    CHECK(events[3].request_id == events[2].request_id);
    CHECK(fixture.Controller().data.ListingPtr() == second_listing);
    CHECK(first_callback->Calls().empty());
    const auto replacement_calls = second_callback->Calls();
    REQUIRE(replacement_calls.size() == 1);
    CHECK(replacement_calls.front().succeeded);
    CHECK(replacement_calls.front().source == DirectoryChangeResultSource::Fetch);
    CHECK(replacement_calls.front().current);
}

TEST_CASE(PREFIX "cancels a gated worker without callback or model commit")
{
    PanelControllerNavigationFixture fixture;
    const auto target = UniformListing(fixture.Host(), "/cancelled/", "cancelled.txt");
    const auto plan = fixture.Host()->ScriptSuccess("/cancelled/", target, true);
    const auto callback = std::make_shared<CallbackRecorder>();
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];
    const auto initial_generation = fixture.Controller().dataGeneration;
    const auto initial_listing = fixture.Controller().data.ListingPtr();

    CHECK([fixture.Controller() GoToDirWithContext:fixture.Request("/cancelled/", true, callback)].has_value());
    REQUIRE(ControllableNavigationHost::WaitEntered(plan));
    REQUIRE(fixture.Controller().isDoingBackgroundLoading);
    [fixture.Controller() CancelBackgroundOperations];
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleCancelled>(events.back().payload) &&
               ControllableNavigationHost::Completed(plan) && !fixture.Controller().isDoingBackgroundLoading;
    }));

    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));
    REQUIRE(std::holds_alternative<PaneLifecycleCancelled>(events.back().payload));
    CHECK(std::get<PaneLifecycleCancelled>(events.back().payload).reason ==
          PaneCancellationReason::InternalAbort);
    CHECK(fixture.Controller().dataGeneration == initial_generation);
    CHECK(fixture.Controller().data.ListingPtr() == initial_listing);
    CHECK(callback->Calls().empty());
}

#undef PREFIX

#define PREFIX "PanelController production refresh "

TEST_CASE(PREFIX "commits a uniform refresh after the model swap without changing location generation")
{
    PanelControllerNavigationFixture fixture;
    const auto refreshed_listing = UniformListing(fixture.Host(), "/seed/", "fresh.txt");
    const auto plan = fixture.Host()->ScriptSuccess("/seed/", refreshed_listing, true);
    const auto initial_generation = fixture.Controller().dataGeneration;
    const int initial_data_updates = fixture.View().dataUpdateCount;
    const int initial_panel_changes = fixture.View().panelChangeCount;
    std::vector<PaneLifecycleEvent> events;
    bool committed_model_visible = false;
    int data_updates_at_commit = -1;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
        if( const auto *committed = std::get_if<PaneLifecycleCommitted>(&_event.payload) ) {
            committed_model_visible = fixture.Controller().data.ListingPtr() == committed->listing &&
                                      fixture.Controller().dataGeneration == committed->controller_generation;
            data_updates_at_commit = fixture.View().dataUpdateCount;
        }
    }];

    REQUIRE([fixture.Controller() submitUserRefresh]);
    REQUIRE(events.size() == 1);
    REQUIRE(fixture.Controller().isDoingBackgroundLoading);
    REQUIRE(ControllableNavigationHost::WaitEntered(plan));
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));
    CHECK(events.front().descriptor.kind == PaneRequestKind::Refresh);
    CHECK(events.front().descriptor.initiated_by_user);
    CHECK(fixture.Controller().data.ListingPtr() == fixture.SeedListing());
    CHECK((ControllableNavigationHost::FetchFlags(plan) & VFSFlags::F_ForceRefresh) != 0);
    ControllableNavigationHost::Release(plan);
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload);
    }));

    const auto &committed = std::get<PaneLifecycleCommitted>(events.back().payload);
    CHECK(committed.controller_generation == initial_generation);
    CHECK(committed.listing == refreshed_listing);
    CHECK(committed_model_visible);
    CHECK(data_updates_at_commit == initial_data_updates);
    CHECK(fixture.Controller().dataGeneration == initial_generation);
    CHECK(fixture.Controller().data.ListingPtr() == refreshed_listing);
    CHECK(fixture.View().dataUpdateCount == initial_data_updates + 1);
    CHECK(fixture.View().panelChangeCount == initial_panel_changes);
}

TEST_CASE(PREFIX "automatic soft refresh preserves provider caches and non-user descriptor")
{
    PanelControllerNavigationFixture fixture;
    const auto refreshed_listing = UniformListing(fixture.Host(), "/seed/", "soft.txt");
    const auto plan = fixture.Host()->ScriptSuccess("/seed/", refreshed_listing, true);
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    [fixture.Controller() refreshPanel];
    REQUIRE(events.size() == 1);
    REQUIRE(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));
    CHECK(events.front().descriptor.kind == PaneRequestKind::Refresh);
    CHECK_FALSE(events.front().descriptor.initiated_by_user);
    REQUIRE(ControllableNavigationHost::WaitEntered(plan));
    CHECK((ControllableNavigationHost::FetchFlags(plan) & VFSFlags::F_ForceRefresh) == 0);

    ControllableNavigationHost::Release(plan);
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload);
    }));
    CHECK(fixture.Controller().data.ListingPtr() == refreshed_listing);
}

TEST_CASE(PREFIX "retains committed content and publishes Failed when the provider refresh fails")
{
    PanelControllerNavigationFixture fixture;
    fixture.Host()->ScriptError("/seed/", nc::Error{nc::Error::POSIX, EIO});
    const auto initial_generation = fixture.Controller().dataGeneration;
    const auto initial_listing = fixture.Controller().data.ListingPtr();
    const int initial_data_updates = fixture.View().dataUpdateCount;
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleFailed>(events.back().payload);
    }));
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);

    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events.front().payload));
    const auto &failed = std::get<PaneLifecycleFailed>(events.back().payload);
    CHECK(failed.error.original_error == nc::Error{nc::Error::POSIX, EIO});
    REQUIRE(failed.error.affected_items.size() == 1);
    CHECK(failed.error.affected_items.front() == "/seed/");
    REQUIRE(failed.error.provider_id);
    CHECK(*failed.error.provider_id == fixture.Host()->Tag());
    CHECK(fixture.Controller().dataGeneration == initial_generation);
    CHECK(fixture.Controller().data.ListingPtr() == initial_listing);
    CHECK(fixture.View().dataUpdateCount == initial_data_updates);
}

TEST_CASE(PREFIX "presents a provider timeout without losing the committed refresh state")
{
    PanelControllerNavigationFixture fixture;
    fixture.Host()->ScriptError("/seed/", nc::Error{"panel_controller_navigation_timeout", 1});
    const auto initial_generation = fixture.Controller().dataGeneration;
    const auto initial_listing = fixture.Controller().data.ListingPtr();
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleFailed>(events.back().payload);
    }));

    const auto &failed = std::get<PaneLifecycleFailed>(events.back().payload);
    CHECK(failed.error.original_error == nc::Error{"panel_controller_navigation_timeout", 1});
    CHECK(failed.error.category == nc::core::FileManagerErrorCategory::TimeoutError);
    CHECK(failed.error.user_message_key == "errors.timeout");
    CHECK(failed.error.affected_items == std::vector<std::string>{"/seed/"});
    REQUIRE(failed.error.provider_id);
    CHECK(*failed.error.provider_id == fixture.Host()->Tag());
    CHECK(fixture.Controller().dataGeneration == initial_generation);
    CHECK(fixture.Controller().data.ListingPtr() == initial_listing);
}

TEST_CASE(PREFIX "publishes an invalid-location failure before lifecycle navigation recovers the pane")
{
    PanelControllerNavigationFixture fixture;
    const auto recovered_listing = UniformListing(fixture.Host(), "/", "recovered.txt");
    fixture.Host()->SetAccessibleDirectory("/");
    fixture.Host()->ScriptError("/seed/", nc::Error{nc::Error::POSIX, ENOENT});
    const auto recovery_plan = fixture.Host()->ScriptSuccess("/", recovered_listing, false);
    const auto initial_listing = fixture.Controller().data.ListingPtr();
    bool failed_content_was_retained = false;
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
        if( std::holds_alternative<PaneLifecycleFailed>(_event.payload) )
            failed_content_was_retained = fixture.Controller().data.ListingPtr() == initial_listing;
    }];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 4 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload) &&
               ControllableNavigationHost::Completed(recovery_plan) &&
               !fixture.Controller().isDoingBackgroundLoading;
    }));

    REQUIRE(events.size() == 4);
    CHECK(events[0].descriptor.kind == PaneRequestKind::Refresh);
    REQUIRE(std::holds_alternative<PaneLifecycleFailed>(events[1].payload));
    CHECK(std::get<PaneLifecycleFailed>(events[1].payload).error.original_error ==
          nc::Error{nc::Error::POSIX, ENOENT});
    CHECK(events[2].descriptor.kind == PaneRequestKind::Navigation);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[2].payload));
    REQUIRE(std::holds_alternative<PaneLifecycleCommitted>(events[3].payload));
    CHECK(failed_content_was_retained);
    CHECK(std::get<PaneLifecycleCommitted>(events[3].payload).listing == recovered_listing);
    CHECK(fixture.Controller().data.ListingPtr() == recovered_listing);
    CHECK(fixture.Controller().dataGeneration ==
          std::get<PaneLifecycleCommitted>(events[3].payload).controller_generation);
}

TEST_CASE(PREFIX "classifies an absent exact native volume and retains the disconnected location")
{
    PanelControllerNavigationFixture fixture;
    const auto volume = TestNativeVolume("/Volumes/Original");
    BindSeedListingToNativeVolume(fixture, volume);
    fixture.NativeFSManager().SetVolumes({});
    fixture.Host()->ScriptError("/seed/", nc::Error{nc::Error::POSIX, ENOENT});
    const auto committed_listing = fixture.Controller().data.ListingPtr();
    const auto committed_generation = fixture.Controller().dataGeneration;
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleFailed>(events.back().payload) &&
               !fixture.Controller().isDoingBackgroundLoading;
    }));
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);

    REQUIRE(events.size() == 2);
    const auto &failed = std::get<PaneLifecycleFailed>(events.back().payload);
    CHECK(failed.error.category == nc::core::FileManagerErrorCategory::VolumeUnavailableError);
    CHECK(failed.error.user_message_key == "errors.volumeUnavailable");
    CHECK(failed.error.original_error == nc::Error{nc::Error::POSIX, ENOENT});
    CHECK(fixture.Controller().data.ListingPtr() == committed_listing);
    CHECK(fixture.Controller().dataGeneration == committed_generation);
    CHECK(fixture.Host()->FetchCount() == 1);
}

TEST_CASE(PREFIX "keeps PathNotFound when the exact committed native volume is still present")
{
    PanelControllerNavigationFixture fixture;
    const auto volume = TestNativeVolume("/Volumes/Present");
    BindSeedListingToNativeVolume(fixture, volume);
    const auto recovered_listing = UniformListing(fixture.Host(), "/", "recovered.txt");
    fixture.Host()->SetAccessibleDirectory("/");
    fixture.Host()->ScriptError("/seed/", nc::Error{nc::Error::POSIX, ENOENT});
    fixture.Host()->ScriptSuccess("/", recovered_listing, false);
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 4 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload) &&
               !fixture.Controller().isDoingBackgroundLoading;
    }));

    REQUIRE(std::holds_alternative<PaneLifecycleFailed>(events[1].payload));
    CHECK(std::get<PaneLifecycleFailed>(events[1].payload).error.category ==
          nc::core::FileManagerErrorCategory::PathNotFoundError);
    CHECK(fixture.Controller().data.ListingPtr() == recovered_listing);
    CHECK(fixture.Host()->FetchCount() == 2);
}

TEST_CASE(PREFIX "keeps PathNotFound when exact native volume membership is unknown")
{
    PanelControllerNavigationFixture fixture;
    const auto volume = TestNativeVolume("/Volumes/Unknown");
    BindSeedListingToNativeVolume(fixture, volume);
    fixture.NativeFSManager().SetVolumesThrow(true);
    const auto recovered_listing = UniformListing(fixture.Host(), "/", "recovered.txt");
    fixture.Host()->SetAccessibleDirectory("/");
    fixture.Host()->ScriptError("/seed/", nc::Error{nc::Error::POSIX, ENOENT});
    fixture.Host()->ScriptSuccess("/", recovered_listing, false);
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 4 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload) &&
               !fixture.Controller().isDoingBackgroundLoading;
    }));

    REQUIRE(std::holds_alternative<PaneLifecycleFailed>(events[1].payload));
    CHECK(std::get<PaneLifecycleFailed>(events[1].payload).error.category ==
          nc::core::FileManagerErrorCategory::PathNotFoundError);
    CHECK(fixture.Controller().data.ListingPtr() == recovered_listing);
}

TEST_CASE(PREFIX "treats a same-path remount as absence of the exact committed volume")
{
    PanelControllerNavigationFixture fixture;
    const auto original = TestNativeVolume("/Volumes/Reused");
    BindSeedListingToNativeVolume(fixture, original);
    const auto replacement = TestNativeVolume("/Volumes/Reused");
    fixture.NativeFSManager().SetPathVolume(replacement);
    fixture.NativeFSManager().SetVolumes({replacement});
    fixture.Host()->ScriptError("/seed/", nc::Error{nc::Error::POSIX, ENOENT});
    const auto committed_listing = fixture.Controller().data.ListingPtr();
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleFailed>(events.back().payload) &&
               !fixture.Controller().isDoingBackgroundLoading;
    }));
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);

    REQUIRE(events.size() == 2);
    CHECK(std::get<PaneLifecycleFailed>(events.back().payload).error.category ==
          nc::core::FileManagerErrorCategory::VolumeUnavailableError);
    CHECK(fixture.Controller().data.ListingPtr() == committed_listing);
    CHECK(fixture.Host()->FetchCount() == 1);
}

TEST_CASE(PREFIX "classifies a thrown refresh error against the exact committed native volume")
{
    PanelControllerNavigationFixture fixture;
    const auto volume = TestNativeVolume("/Volumes/Thrown");
    BindSeedListingToNativeVolume(fixture, volume);
    fixture.NativeFSManager().SetVolumes({});
    fixture.Host()->ScriptThrownError("/seed/", nc::Error{nc::Error::POSIX, EIO});
    const auto committed_listing = fixture.Controller().data.ListingPtr();
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleFailed>(events.back().payload) &&
               !fixture.Controller().isDoingBackgroundLoading;
    }));

    const auto &failed = std::get<PaneLifecycleFailed>(events.back().payload);
    CHECK(failed.error.category == nc::core::FileManagerErrorCategory::VolumeUnavailableError);
    CHECK(failed.error.original_error == nc::Error{nc::Error::POSIX, EIO});
    CHECK(fixture.Controller().data.ListingPtr() == committed_listing);
}

TEST_CASE(PREFIX "latest refresh supersedes the previous worker and commits only the replacement")
{
    PanelControllerNavigationFixture fixture;
    const auto stale_listing = UniformListing(fixture.Host(), "/seed/", "stale.txt");
    const auto latest_listing = UniformListing(fixture.Host(), "/seed/", "latest.txt");
    const auto stale_plan = fixture.Host()->ScriptSuccess("/seed/", stale_listing, true);
    const auto latest_plan = fixture.Host()->ScriptSuccess("/seed/", latest_listing, false);
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(ControllableNavigationHost::WaitEntered(stale_plan));
    [fixture.Controller() forceRefreshPanel];
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 4 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload);
    }));

    REQUIRE(events.size() == 4);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));
    REQUIRE(std::holds_alternative<PaneLifecycleSuperseded>(events[1].payload));
    CHECK(std::get<PaneLifecycleSuperseded>(events[1].payload).replacement == events[2].request_id);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[2].payload));
    CHECK(std::holds_alternative<PaneLifecycleCommitted>(events[3].payload));
    CHECK(ControllableNavigationHost::Completed(stale_plan));
    CHECK(ControllableNavigationHost::Completed(latest_plan));
    CHECK(fixture.Controller().data.ListingPtr() == latest_listing);
}

TEST_CASE(PREFIX "navigation supersedes a gated refresh and prevents its stale model commit")
{
    PanelControllerNavigationFixture fixture;
    const auto stale_listing = UniformListing(fixture.Host(), "/seed/", "stale.txt");
    const auto navigation_listing = UniformListing(fixture.Host(), "/next/", "next.txt");
    const auto refresh_plan = fixture.Host()->ScriptSuccess("/seed/", stale_listing, true);
    fixture.Host()->ScriptSuccess("/next/", navigation_listing, false);
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(ControllableNavigationHost::WaitEntered(refresh_plan));
    const auto navigation =
        [fixture.Controller() GoToDirWithContext:fixture.Request("/next/", false)];
    REQUIRE(navigation.has_value());
    REQUIRE(RunMainLoopUntil([&] { return ControllableNavigationHost::Completed(refresh_plan); }));

    REQUIRE(events.size() == 4);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[0].payload));
    CHECK(events[0].descriptor.kind == PaneRequestKind::Refresh);
    CHECK(std::holds_alternative<PaneLifecycleSuperseded>(events[1].payload));
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events[2].payload));
    CHECK(events[2].descriptor.kind == PaneRequestKind::Navigation);
    CHECK(std::holds_alternative<PaneLifecycleCommitted>(events[3].payload));
    CHECK(fixture.Controller().data.ListingPtr() == navigation_listing);
}

TEST_CASE(PREFIX "rejects refresh as Busy behind navigation without invalidating the navigation worker")
{
    PanelControllerNavigationFixture fixture;
    const auto navigation_listing = UniformListing(fixture.Host(), "/busy/", "busy.txt");
    const auto navigation_plan = fixture.Host()->ScriptSuccess("/busy/", navigation_listing, true);
    const auto callback = std::make_shared<CallbackRecorder>();
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    CHECK([fixture.Controller() GoToDirWithContext:fixture.Request("/busy/", true, callback)].has_value());
    REQUIRE(ControllableNavigationHost::WaitEntered(navigation_plan));
    CHECK_FALSE([fixture.Controller() submitUserRefresh]);
    REQUIRE(events.size() == 2);
    REQUIRE(std::holds_alternative<PaneLifecycleRejected>(events.back().payload));
    CHECK(std::get<PaneLifecycleRejected>(events.back().payload).reason == PaneRejectionReason::Busy);
    ControllableNavigationHost::Release(navigation_plan);
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 3 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload);
    }));

    CHECK(fixture.Controller().data.ListingPtr() == navigation_listing);
    const auto calls = callback->Calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls.front().succeeded);
    CHECK(calls.front().current);
}

TEST_CASE(PREFIX "Escape cancels an active refresh before delayed queue activity becomes visible")
{
    PanelControllerNavigationFixture fixture;
    const auto refreshed_listing = UniformListing(fixture.Host(), "/seed/", "cancelled.txt");
    const auto plan = fixture.Host()->ScriptSuccess("/seed/", refreshed_listing, true);
    const auto initial_listing = fixture.Controller().data.ListingPtr();
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];
    NSEvent *const escape = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                             location:NSZeroPoint
                                        modifierFlags:0
                                            timestamp:0
                                         windowNumber:0
                                              context:nil
                                           characters:@"\x1b"
                          charactersIgnoringModifiers:@"\x1b"
                                            isARepeat:NO
                                              keyCode:53];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(events.size() == 1);
    REQUIRE(fixture.Controller().isDoingBackgroundLoading);
    REQUIRE(ControllableNavigationHost::WaitEntered(plan));
    CHECK([fixture.Controller() bidForHandlingKeyDown:escape forPanelView:fixture.View()] ==
          nc::panel::view::BiddingPriority::Default);
    [fixture.Controller() handleKeyDown:escape forPanelView:fixture.View()];
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleCancelled>(events.back().payload) &&
               ControllableNavigationHost::Completed(plan) && !fixture.Controller().isDoingBackgroundLoading;
    }));

    CHECK(std::get<PaneLifecycleCancelled>(events.back().payload).reason == PaneCancellationReason::User);
    CHECK(fixture.Controller().data.ListingPtr() == initial_listing);
}

TEST_CASE(PREFIX "normalizes a thrown provider cancellation to a Cancelled terminal")
{
    PanelControllerNavigationFixture fixture;
    const auto plan =
        fixture.Host()->ScriptThrownError("/seed/", nc::Error{nc::Error::POSIX, ECANCELED});
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 2 && std::holds_alternative<PaneLifecycleCancelled>(events.back().payload) &&
               ControllableNavigationHost::Completed(plan) &&
               !fixture.Controller().isDoingBackgroundLoading;
    }));

    CHECK(std::get<PaneLifecycleCancelled>(events.back().payload).reason ==
          PaneCancellationReason::QueueStopped);
    CHECK(fixture.Controller().data.ListingPtr() == fixture.SeedListing());
}

TEST_CASE(PREFIX "queues a successor behind a cancelled non-cooperative refresh without poisoning the queue")
{
    PanelControllerNavigationFixture fixture;
    const auto stale_listing = UniformListing(fixture.Host(), "/seed/", "stale.txt");
    const auto navigation_listing = UniformListing(fixture.Host(), "/next/", "next.txt");
    const auto successor_listing = UniformListing(fixture.Host(), "/next/", "fresh-next.txt");
    const auto stale_plan = fixture.Host()->ScriptSuccess("/seed/", stale_listing, true, false);
    fixture.Host()->ScriptSuccess("/next/", navigation_listing, false);
    const auto successor_plan = fixture.Host()->ScriptSuccess("/next/", successor_listing, false);
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(ControllableNavigationHost::WaitEntered(stale_plan));
    REQUIRE([fixture.Controller() GoToDirWithContext:fixture.Request("/next/", false)].has_value());
    CHECK(fixture.Controller().data.ListingPtr() == navigation_listing);
    [fixture.Controller() forceRefreshPanel];
    REQUIRE(events.size() == 5);
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events.back().payload));
    CHECK(events.back().descriptor.kind == PaneRequestKind::Refresh);

    ControllableNavigationHost::Release(stale_plan);
    REQUIRE(RunMainLoopUntil([&] {
        return events.size() == 6 && std::holds_alternative<PaneLifecycleCommitted>(events.back().payload);
    }));

    CHECK(ControllableNavigationHost::Completed(stale_plan));
    CHECK(ControllableNavigationHost::Completed(successor_plan));
    CHECK(fixture.Controller().data.ListingPtr() == successor_listing);
    CHECK(std::get<PaneLifecycleCommitted>(events.back().payload).listing == successor_listing);
}

TEST_CASE(PREFIX "coalesces a refresh burst to one running worker and one latest pending intent")
{
    PanelControllerNavigationFixture fixture;
    const auto stale_listing = UniformListing(fixture.Host(), "/seed/", "stale.txt");
    const auto latest_listing = UniformListing(fixture.Host(), "/seed/", "latest.txt");
    const auto stale_plan = fixture.Host()->ScriptSuccess("/seed/", stale_listing, true, false);
    const auto latest_plan = fixture.Host()->ScriptSuccess("/seed/", latest_listing, false);
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(ControllableNavigationHost::WaitEntered(stale_plan));
    constexpr int replacements = 12;
    for( int index = 0; index < replacements; ++index )
        [fixture.Controller() forceRefreshPanel];
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);

    CHECK(fixture.Host()->FetchCount() == 1);
    REQUIRE(events.size() == static_cast<size_t>(1 + replacements * 2));
    CHECK(std::holds_alternative<PaneLifecycleStarted>(events.back().payload));
    ControllableNavigationHost::Release(stale_plan);
    REQUIRE(RunMainLoopUntil([&] {
        return std::holds_alternative<PaneLifecycleCommitted>(events.back().payload) &&
               ControllableNavigationHost::Completed(latest_plan) &&
               !fixture.Controller().isDoingBackgroundLoading;
    }));

    CHECK(fixture.Host()->FetchCount() == 2);
    CHECK(fixture.Controller().data.ListingPtr() == latest_listing);
    CHECK(std::get<PaneLifecycleCommitted>(events.back().payload).listing == latest_listing);
}

TEST_CASE(PREFIX "controller teardown cancels a cooperative refresh without a worker retain cycle")
{
    PanelControllerNavigationFixture fixture;
    const auto refreshed_listing = UniformListing(fixture.Host(), "/seed/", "unused.txt");
    const auto plan = fixture.Host()->ScriptSuccess("/seed/", refreshed_listing, true);

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(ControllableNavigationHost::WaitEntered(plan));
    __weak PanelController *weak_controller = fixture.Controller();
    fixture.ReleaseControllerWithoutCancelling();

    CHECK(weak_controller == nil);
    CHECK(RunMainLoopUntil([&] { return ControllableNavigationHost::Completed(plan); }));
}

TEST_CASE(PREFIX "controller teardown does not wait for a non-cooperative refresh provider")
{
    PanelControllerNavigationFixture fixture;
    const auto refreshed_listing = UniformListing(fixture.Host(), "/seed/", "unused.txt");
    const auto plan = fixture.Host()->ScriptSuccess("/seed/", refreshed_listing, true, false);

    [fixture.Controller() forceRefreshPanel];
    REQUIRE(ControllableNavigationHost::WaitEntered(plan));
    __weak PanelController *weak_controller = fixture.Controller();
    const auto release_started = std::chrono::steady_clock::now();
    fixture.ReleaseControllerWithoutCancelling();
    const auto release_elapsed = std::chrono::steady_clock::now() - release_started;

    CHECK(weak_controller == nil);
    CHECK(release_elapsed < 100ms);
    CHECK_FALSE(ControllableNavigationHost::Completed(plan));
    ControllableNavigationHost::Release(plan);
    REQUIRE(RunMainLoopUntil([&] { return ControllableNavigationHost::Completed(plan); }));
}

TEST_CASE(PREFIX "controller teardown does not wait for a non-cooperative explicit Up provider")
{
    PanelControllerNavigationFixture fixture;
    const auto target_listing = UniformListing(fixture.Host(), "/", "seed");
    const auto plan = fixture.Host()->ScriptSuccess("/", target_listing, true, false);
    std::vector<PaneLifecycleEvent> events;
    auto subscription = [fixture.Controller() subscribeToPaneLifecycle:[&](const PaneLifecycleEvent &_event) {
        events.emplace_back(_event);
    }];

    REQUIRE(nc::panel::actions::SubmitExplicitGoToEnclosingFolder(fixture.Controller()));
    REQUIRE(ControllableNavigationHost::WaitEntered(plan));
    REQUIRE(events.size() == 1);
    __weak PanelController *weak_controller = fixture.Controller();
    const auto release_started = std::chrono::steady_clock::now();
    fixture.ReleaseControllerWithoutCancelling();
    const auto release_elapsed = std::chrono::steady_clock::now() - release_started;

    CHECK(weak_controller == nil);
    CHECK(release_elapsed < 100ms);
    CHECK_FALSE(ControllableNavigationHost::Completed(plan));
    REQUIRE(events.size() == 2);
    REQUIRE(std::holds_alternative<PaneLifecycleCancelled>(events.back().payload));
    CHECK(std::get<PaneLifecycleCancelled>(events.back().payload).reason ==
          PaneCancellationReason::ProducerShutdown);
    ControllableNavigationHost::Release(plan);
    REQUIRE(RunMainLoopUntil([&] { return ControllableNavigationHost::Completed(plan); }));
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);

    CHECK(weak_controller == nil);
    CHECK(events.size() == 2);
}

TEST_CASE(PREFIX "detached recovery does not retain controller teardown and suppresses its delayed commit")
{
    PanelControllerNavigationFixture fixture;
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool released = false;
        bool completed = false;
    };
    const auto gate = std::make_shared<Gate>();
    const auto committed = std::make_shared<std::atomic_bool>(false);

    [fixture.Controller()
        commitDetachedCancelableLoadingTask:[gate, committed](const nc::panel::CancelableLoadingTaskContext &_context) {
            {
                std::unique_lock lock{gate->mutex};
                gate->entered = true;
                gate->changed.notify_all();
                gate->changed.wait(lock, [&] { return gate->released; });
            }
            _context.commit_on_main(
                [committed] { committed->store(true, std::memory_order_release); });
            {
                const std::lock_guard lock{gate->mutex};
                gate->completed = true;
            }
            gate->changed.notify_all();
        }];
    {
        std::unique_lock lock{gate->mutex};
        REQUIRE(gate->changed.wait_for(lock, 1s, [&] { return gate->entered; }));
    }

    __weak PanelController *weak_controller = fixture.Controller();
    const auto release_started = std::chrono::steady_clock::now();
    fixture.ReleaseControllerWithoutCancelling();
    const auto release_elapsed = std::chrono::steady_clock::now() - release_started;

    CHECK(weak_controller == nil);
    CHECK(release_elapsed < 100ms);
    {
        const std::lock_guard lock{gate->mutex};
        gate->released = true;
    }
    gate->changed.notify_all();
    {
        std::unique_lock lock{gate->mutex};
        REQUIRE(gate->changed.wait_for(lock, 1s, [&] { return gate->completed; }));
    }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);

    CHECK_FALSE(committed->load(std::memory_order_acquire));
}

TEST_CASE(PREFIX "detached recovery commit is fenced by a newer content intent")
{
    PanelControllerNavigationFixture fixture;
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool released = false;
        bool completed = false;
    };
    const auto gate = std::make_shared<Gate>();
    const auto committed = std::make_shared<std::atomic_bool>(false);

    [fixture.Controller()
        commitDetachedCancelableLoadingTask:[gate, committed](const nc::panel::CancelableLoadingTaskContext &_context) {
            {
                std::unique_lock lock{gate->mutex};
                gate->entered = true;
                gate->changed.notify_all();
                gate->changed.wait(lock, [&] { return gate->released; });
            }
            _context.commit_on_main(
                [committed] { committed->store(true, std::memory_order_release); });
            {
                const std::lock_guard lock{gate->mutex};
                gate->completed = true;
            }
            gate->changed.notify_all();
        }];
    {
        std::unique_lock lock{gate->mutex};
        REQUIRE(gate->changed.wait_for(lock, 1s, [&] { return gate->entered; }));
    }

    [fixture.Controller() loadListing:UniformListing(fixture.Host(), "/latest/", "latest.txt")];
    {
        const std::lock_guard lock{gate->mutex};
        gate->released = true;
    }
    gate->changed.notify_all();
    {
        std::unique_lock lock{gate->mutex};
        REQUIRE(gate->changed.wait_for(lock, 1s, [&] { return gate->completed; }));
    }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);

    CHECK_FALSE(committed->load(std::memory_order_acquire));
}

TEST_CASE(PREFIX "rename editor clears failed presentation attachment before retry")
{
    REQUIRE([NSThread isMainThread]);
    PanelControllerNavigationFixture fixture;
    const int seed_position = fixture.Controller().data.SortedIndexForName("seed.txt");
    REQUIRE(seed_position >= 0);
    fixture.View().curpos = seed_position;
    REQUIRE(fixture.View().item);
    REQUIRE(fixture.View().item.Filename() == "seed.txt");

    auto presentation = [PanelControllerRenamePresentationProbe new];
    presentation.attachEditors = false;
    const ScopedItemsViewOverride items_view_override{fixture.View(), presentation};

    CHECK_FALSE([fixture.View() startFieldEditorRenaming]);
    REQUIRE(presentation.setupCallCount == 1);
    NCPanelViewFieldEditor *const first_editor = [presentation editorAtCall:0];
    CHECK(first_editor.superview == nil);

    CHECK_FALSE([fixture.View() startFieldEditorRenaming]);
    REQUIRE(presentation.setupCallCount == 2);
    NCPanelViewFieldEditor *const second_editor = [presentation editorAtCall:1];
    CHECK(second_editor != first_editor);
    CHECK(second_editor.superview == nil);
}

TEST_CASE(PREFIX "rename editor removes rejected first responder attachment before fresh retry")
{
    REQUIRE([NSThread isMainThread]);
    PanelControllerNavigationFixture fixture;
    const int seed_position = fixture.Controller().data.SortedIndexForName("seed.txt");
    REQUIRE(seed_position >= 0);
    fixture.View().curpos = seed_position;
    REQUIRE(fixture.View().item);
    REQUIRE(fixture.View().item.Filename() == "seed.txt");

    auto presentation = [PanelControllerRenamePresentationProbe new];
    presentation.attachEditors = true;
    auto rejecting_window = [PanelControllerRenameRejectingWindow new];
    __weak PanelControllerNavigationTestView *weak_view = fixture.View();
    presentation.onEditorAttached = ^{
      weak_view.renameWindowOverride = rejecting_window;
    };
    const ScopedItemsViewOverride items_view_override{fixture.View(), presentation};

    CHECK_FALSE([fixture.View() startFieldEditorRenaming]);
    REQUIRE(presentation.setupCallCount == 1);
    NCPanelViewFieldEditor *const first_editor = [presentation editorAtCall:0];
    CHECK(first_editor.superview == nil);

    fixture.View().renameWindowOverride = nil;
    CHECK_FALSE([fixture.View() startFieldEditorRenaming]);
    REQUIRE(presentation.setupCallCount == 2);
    NCPanelViewFieldEditor *const second_editor = [presentation editorAtCall:1];
    CHECK(second_editor != first_editor);
    CHECK(second_editor.superview == nil);
    CHECK(rejecting_window.makeFirstResponderCallCount == 4);
}

TEST_CASE(PREFIX "rename validation keeps the editor active until cancellation")
{
    REQUIRE([NSThread isMainThread]);
    PanelControllerNavigationFixture fixture;
    const int seed_position = fixture.Controller().data.SortedIndexForName("seed.txt");
    REQUIRE(seed_position >= 0);
    fixture.View().curpos = seed_position;

    @autoreleasepool {
        NSWindow *const window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 240, 160)
                                                           styleMask:NSWindowStyleMaskBorderless
                                                             backing:NSBackingStoreBuffered
                                                               defer:NO];
        const ScopedWindowContentViewAttachment window_attachment{window, fixture.View()};
        auto presentation = [PanelControllerRenamePresentationProbe new];
        presentation.attachEditors = true;
        [fixture.View() addSubview:presentation];
        const ScopedItemsViewOverride items_view_override{fixture.View(), presentation};

        REQUIRE([fixture.View() startFieldEditorRenaming]);
        REQUIRE(presentation.setupCallCount == 1);
        NCPanelViewFieldEditor *const editor = [presentation editorAtCall:0];
        editor.editor.string = @"renamed.txt";
        CHECK([editor textView:editor.editor doCommandBySelector:@selector(insertNewline:)]);
        CHECK(editor.superview == presentation);
        CHECK(editor.toolTip.length > 0);
        CHECK((window.firstResponder == editor || window.firstResponder == editor.editor));

        CHECK([editor textView:editor.editor doCommandBySelector:@selector(cancelOperation:)]);
        CHECK(editor.superview == nil);
        [presentation removeFromSuperview];
    }
}

TEST_CASE(PREFIX "replacement listing discards the exact-item rename editor")
{
    REQUIRE([NSThread isMainThread]);
    PanelControllerNavigationFixture fixture;
    const int seed_position = fixture.Controller().data.SortedIndexForName("seed.txt");
    REQUIRE(seed_position >= 0);
    fixture.View().curpos = seed_position;

    @autoreleasepool {
        NSWindow *const window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 240, 160)
                                                           styleMask:NSWindowStyleMaskBorderless
                                                             backing:NSBackingStoreBuffered
                                                               defer:NO];
        const ScopedWindowContentViewAttachment window_attachment{window, fixture.View()};
        auto presentation = [PanelControllerRenamePresentationProbe new];
        presentation.attachEditors = true;
        [fixture.View() addSubview:presentation];
        const ScopedItemsViewOverride items_view_override{fixture.View(), presentation};

        REQUIRE([fixture.View() startFieldEditorRenaming]);
        REQUIRE(presentation.setupCallCount == 1);
        NCPanelViewFieldEditor *const first_editor = [presentation editorAtCall:0];
        REQUIRE(first_editor.superview == presentation);

        fixture.View().forwardsDataUpdatedToProduction = true;
        [fixture.Controller() loadListing:UniformListing(fixture.Host(), "/seed/", "seed.txt")];
        CHECK(first_editor.superview == nil);

        fixture.View().curpos = fixture.Controller().data.SortedIndexForName("seed.txt");
        REQUIRE([fixture.View() startFieldEditorRenaming]);
        REQUIRE(presentation.setupCallCount == 2);
        NCPanelViewFieldEditor *const second_editor = [presentation editorAtCall:1];
        CHECK(second_editor != first_editor);
        [fixture.View() discardFieldEditor];
        CHECK(second_editor.superview == nil);
        [presentation removeFromSuperview];
    }
}

#undef PREFIX
#define PREFIX "PanelController production drag source exact seal "

TEST_CASE(PREFIX "matches the current exact listing generation and items")
{
    REQUIRE([NSThread isMainThread]);
    PanelControllerNavigationFixture fixture;
    PanelController *const controller = fixture.Controller();
    const int seed_position = controller.data.SortedIndexForName("seed.txt");
    REQUIRE(seed_position >= 0);
    const VFSListingItem seed = controller.data.EntryAtSortPosition(seed_position);
    REQUIRE(seed);

    FilesDraggingSource *const source =
        [[FilesDraggingSource alloc] initWithSourceController:controller nativeHost:*fixture.NativeHost()];
    PanelDraggingItem *const dragged_item = [[PanelDraggingItem alloc] initWithItem:seed];
    [source addItem:dragged_item];

    CHECK(source.sourceListing == fixture.SeedListing());
    CHECK(source.sourceGeneration == controller.dataGeneration);
    CHECK([source matchesCurrentSourceContext]);
}

TEST_CASE(PREFIX "rejects an equal-value replacement listing and generation")
{
    REQUIRE([NSThread isMainThread]);
    PanelControllerNavigationFixture fixture;
    PanelController *const controller = fixture.Controller();
    const int seed_position = controller.data.SortedIndexForName("seed.txt");
    REQUIRE(seed_position >= 0);
    const VFSListingItem seed = controller.data.EntryAtSortPosition(seed_position);
    REQUIRE(seed);

    FilesDraggingSource *const source =
        [[FilesDraggingSource alloc] initWithSourceController:controller nativeHost:*fixture.NativeHost()];
    PanelDraggingItem *const dragged_item = [[PanelDraggingItem alloc] initWithItem:seed];
    [source addItem:dragged_item];
    REQUIRE([source matchesCurrentSourceContext]);

    const VFSListingPtr replacement = UniformListing(fixture.Host(), "/seed/", "seed.txt");
    REQUIRE(replacement != source.sourceListing);
    [controller loadListing:replacement];

    CHECK(controller.data.ListingPtr() == replacement);
    CHECK(controller.dataGeneration != source.sourceGeneration);
    CHECK_FALSE([source matchesCurrentSourceContext]);
}

#undef PREFIX
#define PREFIX "PanelController navigation Registry "

TEST_CASE(PREFIX "Back and Forward selector surfaces route through Registry with current history state")
{
    REQUIRE([NSThread isMainThread]);
    PanelControllerNavigationFixture fixture;
    nc::core::CommandRegistry registry;
    nc::panel::PanelActionsMap actions;
    TestActionsShortcutsManager shortcuts;

    struct RoutedContext {
        nc::core::CommandInvocationSource source;
        const void *sender;
        void *target;
        std::optional<bool> can_go_back;
        std::optional<bool> can_go_forward;
        bool back;
    };
    std::vector<RoutedContext> evaluated;
    std::vector<RoutedContext> executed;
    const auto capture = [](const nc::core::CommandContext &_context, const bool _back) {
        return RoutedContext{
            .source = _context.source,
            .sender = _context.native_sender,
            .target = _context.native_target,
            .can_go_back = _context.can_go_back,
            .can_go_forward = _context.can_go_forward,
            .back = _back,
        };
    };
    const auto register_route = [&](const std::string_view _id, const bool _back) {
        nc::core::CommandRegistry::Registration registration;
        registration.descriptor.id = nc::core::CommandId{_id};
        registration.state_provider = [&, _back](const nc::core::CommandContext &_context) {
            evaluated.emplace_back(capture(_context, _back));
            nc::core::CommandState state;
            state.enabled = _context.native_target != nullptr &&
                            (_back ? _context.can_go_back.value_or(false)
                                   : _context.can_go_forward.value_or(false));
            return state;
        };
        registration.handler = [&, _back](const nc::core::CommandContext &_context) {
            executed.emplace_back(capture(_context, _back));
        };
        REQUIRE(registry.Register(std::move(registration)) ==
                nc::core::CommandRegistry::RegisterResult::Registered);
    };
    register_route(nc::core::command_ids::NavigationBack, true);
    register_route(nc::core::command_ids::NavigationForward, false);

    NCPanelControllerActionsDispatcher *const dispatcher =
        [[NCPanelControllerActionsDispatcher alloc] initWithController:fixture.Controller()
                                                            actionsMap:actions
                                               actionsShortcutsManager:shortcuts
                                                       commandRegistry:registry];
    NSMenuItem *const back_menu =
        [[NSMenuItem alloc] initWithTitle:@"Back" action:@selector(OnGoBack:) keyEquivalent:@"["];
    NSMenuItem *const forward_menu =
        [[NSMenuItem alloc] initWithTitle:@"Forward" action:@selector(OnGoForward:) keyEquivalent:@"]"];
    NSButton *const back_button = [NSButton buttonWithTitle:@"Back" target:dispatcher action:@selector(OnGoBack:)];
    NSButton *const forward_button =
        [NSButton buttonWithTitle:@"Forward" target:dispatcher action:@selector(OnGoForward:)];

    auto &history = fixture.Controller().history;
    const auto first = UniformListing(fixture.Host(), "/registry-history-first/", "first.txt");
    const auto second = UniformListing(fixture.Host(), "/registry-history-second/", "second.txt");
    const auto third = UniformListing(fixture.Host(), "/registry-history-third/", "third.txt");
    history.Put(*first);
    history.Put(*second);
    history.Put(*third);
    REQUIRE(history.CanMoveBack());
    REQUIRE_FALSE(history.CanMoveForth());

    CHECK([dispatcher validateMenuItem:back_menu]);
    REQUIRE_FALSE([dispatcher validateMenuItem:forward_menu]);
    REQUIRE(evaluated.size() >= 2);
    const RoutedContext &menu_state = evaluated[evaluated.size() - 2];
    CHECK(menu_state.source == nc::core::CommandInvocationSource::Menu);
    CHECK(menu_state.target == (__bridge void *)fixture.Controller());
    CHECK(menu_state.can_go_back == true);
    CHECK(menu_state.can_go_forward == false);

    [dispatcher OnGoBack:back_button];
    REQUIRE(executed.size() == 1);
    CHECK(executed.back().back);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Toolbar);
    CHECK(executed.back().sender == (__bridge const void *)back_button);

    [dispatcher executeBySelectorIfValidOrBeep:@selector(OnGoBack:) withSender:back_menu];
    REQUIRE(executed.size() == 2);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Menu);
    CHECK(executed.back().sender == (__bridge const void *)back_menu);

    [dispatcher executeNavigationBackCommandFromSource:nc::core::CommandInvocationSource::Shortcut
                                                sender:back_menu];
    REQUIRE(executed.size() == 3);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Shortcut);

    [dispatcher OnGoForward:forward_button];
    CHECK(executed.size() == 3);

    history.MoveBack();
    REQUIRE(history.CanMoveBack());
    REQUIRE(history.CanMoveForth());
    [dispatcher OnGoForward:forward_button];
    REQUIRE(executed.size() == 4);
    CHECK_FALSE(executed.back().back);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Toolbar);
    CHECK(executed.back().can_go_back == true);
    CHECK(executed.back().can_go_forward == true);
}

TEST_CASE(PREFIX "Up and Refresh selector surfaces route through Registry with live pane availability")
{
    REQUIRE([NSThread isMainThread]);
    PanelControllerNavigationFixture fixture;
    nc::core::CommandRegistry registry;
    nc::panel::PanelActionsMap actions;
    PaneNavigationShortcutManager shortcuts;

    struct RoutedContext {
        nc::core::CommandInvocationSource source;
        const void *sender;
        void *target;
        std::optional<nc::core::NavigationUpAvailability> up;
        std::optional<nc::core::NavigationRefreshAvailability> refresh;
        bool is_up;
    };
    std::vector<RoutedContext> evaluated;
    std::vector<RoutedContext> executed;
    const auto capture = [](const nc::core::CommandContext &_context, const bool _is_up) {
        return RoutedContext{
            .source = _context.source,
            .sender = _context.native_sender,
            .target = _context.native_target,
            .up = _context.navigation_up_availability,
            .refresh = _context.navigation_refresh_availability,
            .is_up = _is_up,
        };
    };
    const auto register_route = [&](const std::string_view _id, const bool _is_up) {
        nc::core::CommandRegistry::Registration registration;
        registration.descriptor.id = nc::core::CommandId{_id};
        registration.descriptor.legacy = nc::core::LegacyCommandMetadata{
            .selector_name = _is_up ? std::optional<std::string>{"OnGoToUpperDirectory:"}
                                    : std::optional<std::string>{"OnRefreshPanel:"},
            .shortcut_action_names = _is_up
                                         ? std::vector<std::string>{"menu.go.enclosing_folder",
                                                                    "panel.go_into_enclosing_folder"}
                                         : std::vector<std::string>{"menu.view.refresh"},
            .shortcut_tag = _is_up ? std::optional<int>{14'020} : std::optional<int>{13'040},
        };
        registration.state_provider = [&, _is_up](const nc::core::CommandContext &_context) {
            evaluated.emplace_back(capture(_context, _is_up));
            nc::core::CommandState state;
            state.enabled = _context.native_target != nullptr &&
                            (_is_up ? _context.navigation_up_availability ==
                                          nc::core::NavigationUpAvailability::Available
                                    : _context.navigation_refresh_availability ==
                                          nc::core::NavigationRefreshAvailability::Available);
            return state;
        };
        registration.handler = [&, _is_up](const nc::core::CommandContext &_context) {
            executed.emplace_back(capture(_context, _is_up));
        };
        REQUIRE(registry.Register(std::move(registration)) ==
                nc::core::CommandRegistry::RegisterResult::Registered);
    };
    register_route(nc::core::command_ids::NavigationUp, true);
    register_route(nc::core::command_ids::NavigationRefresh, false);

    NCPanelControllerActionsDispatcher *const dispatcher =
        [[NCPanelControllerActionsDispatcher alloc] initWithController:fixture.Controller()
                                                            actionsMap:actions
                                               actionsShortcutsManager:shortcuts
                                                       commandRegistry:registry];
    NSMenuItem *const up_menu =
        [[NSMenuItem alloc] initWithTitle:@"Up" action:@selector(OnGoToUpperDirectory:) keyEquivalent:@""];
    NSMenuItem *const refresh_menu =
        [[NSMenuItem alloc] initWithTitle:@"Refresh" action:@selector(OnRefreshPanel:) keyEquivalent:@"r"];
    NSButton *const up_button =
        [NSButton buttonWithTitle:@"Up" target:dispatcher action:@selector(OnGoToUpperDirectory:)];

    NSEvent *const secondary_up = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                                   location:NSZeroPoint
                                              modifierFlags:NSEventModifierFlagControl
                                                  timestamp:0
                                               windowNumber:0
                                                    context:nil
                                                 characters:@"u"
                                charactersIgnoringModifiers:@"u"
                                                  isARepeat:false
                                                    keyCode:32];
    NSEvent *const secondary_refresh = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                                        location:NSZeroPoint
                                                   modifierFlags:NSEventModifierFlagControl
                                                       timestamp:0
                                                    windowNumber:0
                                                         context:nil
                                                      characters:@"r"
                                     charactersIgnoringModifiers:@"r"
                                                       isARepeat:false
                                                         keyCode:15];
    CHECK([dispatcher commandInvocationSourceForSender:up_menu
                                             commandId:nc::core::command_ids::NavigationUp
                                          currentEvent:secondary_up] ==
          nc::core::CommandInvocationSource::Shortcut);
    CHECK([dispatcher commandInvocationSourceForSender:refresh_menu
                                             commandId:nc::core::command_ids::NavigationRefresh
                                          currentEvent:secondary_refresh] ==
          nc::core::CommandInvocationSource::Shortcut);
    CHECK([dispatcher validateMenuItem:up_menu]);
    CHECK([dispatcher validateMenuItem:refresh_menu]);
    REQUIRE(evaluated.size() >= 2);
    CHECK(evaluated[evaluated.size() - 2].source == nc::core::CommandInvocationSource::Menu);
    CHECK(evaluated[evaluated.size() - 2].target == (__bridge void *)fixture.Controller());
    CHECK(evaluated[evaluated.size() - 2].up == nc::core::NavigationUpAvailability::Available);
    CHECK(evaluated.back().refresh == nc::core::NavigationRefreshAvailability::Available);

    [dispatcher OnGoToUpperDirectory:up_button];
    REQUIRE(executed.size() == 1);
    CHECK(executed.back().is_up);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Toolbar);
    CHECK(executed.back().sender == (__bridge const void *)up_button);

    [dispatcher executeBySelectorIfValidOrBeep:@selector(OnRefreshPanel:) withSender:refresh_menu];
    REQUIRE(executed.size() == 2);
    CHECK_FALSE(executed.back().is_up);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Menu);
    CHECK(executed.back().sender == (__bridge const void *)refresh_menu);

    [dispatcher executeNavigationUpCommandFromSource:nc::core::CommandInvocationSource::Shortcut
                                              sender:up_menu];
    REQUIRE(executed.size() == 3);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Shortcut);

    [fixture.Controller() loadListing:NonUniformListing(fixture.Host())];
    CHECK_FALSE([dispatcher validateMenuItem:up_menu]);
    CHECK([dispatcher validateMenuItem:refresh_menu]);
    const auto non_uniform_up = [dispatcher navigationUpCommandState];
    CHECK_FALSE(non_uniform_up.enabled);
    REQUIRE_FALSE(evaluated.empty());
    CHECK(evaluated.back().up == nc::core::NavigationUpAvailability::HierarchyUnavailable);
}

TEST_CASE(PREFIX "file.open routes menu Enter context menu and Shift-Return through one Registry command")
{
    REQUIRE([NSThread isMainThread]);
    PanelControllerNavigationFixture fixture;
    nc::core::CommandRegistry registry;
    nc::panel::PanelActionsMap actions;
    FileOpenShortcutManager shortcuts;

    const auto open_listing = UniformListing(fixture.Host(), "/open/", "seed");
    [fixture.Controller() loadListing:open_listing];
    const int seed_position = fixture.Controller().data.SortedIndexForName("seed");
    REQUIRE(seed_position >= 0);
    fixture.View().curpos = seed_position;
    REQUIRE(fixture.View().item);
    REQUIRE(fixture.View().item.Filename() == "seed");
    const VFSListingItem focused_item = fixture.View().item;

    struct RoutedContext {
        nc::core::CommandInvocationSource source;
        const void *sender;
        void *target;
        std::size_t item_count;
        std::string filename;
        const nc::vfs::Host *host;
        VFSListingItem item;
    };
    const auto capture = [](const nc::core::CommandContext &_context) {
        return RoutedContext{
            .source = _context.source,
            .sender = _context.native_sender,
            .target = _context.native_target,
            .item_count = _context.items.size(),
            .filename = _context.items.empty() ? std::string{} : _context.items.front().Filename(),
            .host = _context.items.empty() ? nullptr : _context.items.front().Host().get(),
            .item = _context.items.empty() ? VFSListingItem{} : _context.items.front(),
        };
    };
    std::vector<RoutedContext> evaluated;
    std::vector<RoutedContext> executed;

    nc::core::CommandRegistry::Registration registration;
    registration.descriptor.id = nc::core::CommandId{nc::core::command_ids::FileOpen};
    registration.descriptor.legacy = nc::core::LegacyCommandMetadata{
        .selector_name = "OnOpenNatively:",
        .shortcut_action_names = {"menu.file.open"},
        .shortcut_tag = 11'020,
    };
    registration.state_provider = [&](const nc::core::CommandContext &_context) {
        evaluated.emplace_back(capture(_context));
        nc::core::CommandState state;
        state.enabled = _context.native_target != nullptr && _context.items.size() == 1 &&
                        _context.items.front().Filename() == "seed";
        return state;
    };
    registration.handler = [&](const nc::core::CommandContext &_context) {
        executed.emplace_back(capture(_context));
    };
    REQUIRE(registry.Register(std::move(registration)) ==
            nc::core::CommandRegistry::RegisterResult::Registered);
    CHECK(actions.find(@selector(OnOpenNatively:)) == actions.end());
    CHECK(nc::panel::actions::Enter::ResolveRoute(fixture.Controller()) ==
          nc::panel::actions::Enter::Route::OpenWithDefaultHandler);

    NCPanelControllerActionsDispatcher *const dispatcher =
        [[NCPanelControllerActionsDispatcher alloc] initWithController:fixture.Controller()
                                                            actionsMap:actions
                                               actionsShortcutsManager:shortcuts
                                                       commandRegistry:registry];
    NSMenuItem *const menu =
        [[NSMenuItem alloc] initWithTitle:@"Open" action:@selector(OnOpenNatively:) keyEquivalent:@""];

    CHECK([dispatcher validateMenuItem:menu]);
    CHECK([menu.title containsString:@"seed"]);
    REQUIRE_FALSE(evaluated.empty());
    CHECK(evaluated.back().source == nc::core::CommandInvocationSource::Menu);
    CHECK(evaluated.back().target == (__bridge void *)fixture.Controller());
    CHECK(evaluated.back().item_count == 1);
    CHECK(evaluated.back().filename == "seed");
    CHECK(evaluated.back().host == fixture.Host().get());
    CHECK(evaluated.back().item == focused_item);

    const int dotdot_position = fixture.Controller().data.SortedIndexForName("..");
    REQUIRE(dotdot_position >= 0);
    fixture.View().curpos = dotdot_position;
    [dispatcher OnOpenNatively:menu];
    CHECK(executed.empty());
    fixture.View().curpos = seed_position;

    [dispatcher OnOpenNatively:menu];
    REQUIRE(executed.size() == 1);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Menu);
    CHECK(executed.back().sender == (__bridge const void *)menu);

    [dispatcher OnOpen:nil];
    REQUIRE(executed.size() == 2);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Programmatic);
    CHECK(executed.back().sender == nullptr);

    const std::vector<VFSListingItem> items = fixture.Controller().selectedEntriesOrFocusedEntryWithDotDot;
    REQUIRE(items.size() == 1);
    fixture.View().curpos = dotdot_position;
    NSMenuItem *const context_menu =
        [[NSMenuItem alloc] initWithTitle:@"Open" action:nil keyEquivalent:@""];
    [dispatcher executeFileOpenCommandWithItems:items
                                         source:nc::core::CommandInvocationSource::ContextMenu
                                         sender:context_menu];
    REQUIRE(executed.size() == 3);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::ContextMenu);
    CHECK(executed.back().sender == (__bridge const void *)context_menu);
    CHECK(executed.back().item == focused_item);
    fixture.View().curpos = seed_position;

    [dispatcher executeFileOpenCommandFromSource:nc::core::CommandInvocationSource::Shortcut sender:menu];
    REQUIRE(executed.size() == 4);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Shortcut);
    CHECK(executed.back().sender == (__bridge const void *)menu);

    NSEvent *const shift_return = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                                   location:NSZeroPoint
                                              modifierFlags:NSEventModifierFlagShift
                                                  timestamp:0
                                               windowNumber:0
                                                    context:nil
                                                 characters:@"\r"
                                charactersIgnoringModifiers:@"\r"
                                                  isARepeat:false
                                                    keyCode:36];
    CHECK([dispatcher bidForHandlingKeyDown:shift_return forPanelView:fixture.View()] ==
          nc::panel::view::BiddingPriority::High);
    [dispatcher handleKeyDown:shift_return forPanelView:fixture.View()];
    REQUIRE(executed.size() == 5);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Shortcut);
    CHECK(executed.back().sender == (__bridge const void *)shift_return);

    for( const RoutedContext &context : executed ) {
        CHECK(context.target == (__bridge void *)fixture.Controller());
        CHECK(context.item_count == 1);
        CHECK(context.filename == "seed");
        CHECK(context.host == fixture.Host().get());
        CHECK(context.item == focused_item);
    }

    fixture.View().curpos = dotdot_position;
    CHECK(nc::panel::actions::Enter::ResolveRoute(fixture.Controller()) ==
          nc::panel::actions::Enter::Route::EnterFolder);
    [dispatcher OnOpen:nil];
    CHECK(executed.size() == 5);

    const auto executable_listing = UniformListing(
        fixture.NativeHost(), "/bin/", "tool", S_IFREG | S_IRUSR | S_IXUSR, DT_REG);
    [fixture.Controller() loadListing:executable_listing];
    const int executable_position = fixture.Controller().data.SortedIndexForName("tool");
    REQUIRE(executable_position >= 0);
    fixture.View().curpos = executable_position;
    CHECK(nc::panel::actions::Enter::ResolveRoute(fixture.Controller()) ==
          nc::panel::actions::Enter::Route::ExecuteInTerminal);
    [dispatcher OnOpen:nil];
    CHECK(executed.size() == 5);
}

TEST_CASE(PREFIX "inspector commands preserve admitted targets exact preview items and shortcut source")
{
    REQUIRE([NSThread isMainThread]);
    PanelControllerNavigationFixture fixture;
    nc::core::CommandRegistry registry;
    nc::panel::PanelActionsMap actions;
    FileOpenShortcutManager shortcuts{5};

    const int seed_position = fixture.Controller().data.SortedIndexForName("seed.txt");
    REQUIRE(seed_position >= 0);
    fixture.View().curpos = seed_position;
    const VFSListingItem seed = fixture.View().item;
    REQUIRE(seed);

    void *get_info_target = nullptr;
    int get_info_calls = 0;
    std::optional<nc::core::FileGetInfoPresentation> get_info_presentation;
    REQUIRE(registry.Register(nc::core::MakeFileGetInfoCommand(
                [&](void *_target, nc::core::FileGetInfoPresentation _presentation) {
                    ++get_info_calls;
                    get_info_target = _target;
                    get_info_presentation = std::move(_presentation);
                    return true;
                })) == nc::core::CommandRegistry::RegisterResult::Registered);

    struct PreviewCall {
        void *target;
        VFSListingItem item;
        nc::core::CommandInvocationSource source;
        const void *sender;
    };
    std::vector<PreviewCall> preview_calls;
    REQUIRE(registry.Register(nc::core::MakeFilePreviewCommand(
                [&](void *_target,
                    const VFSListingItem &_item,
                    const nc::core::FilePreviewIntent _intent) {
                    preview_calls.emplace_back(
                        PreviewCall{_target, _item, _intent.source, _intent.native_sender});
                    return true;
                })) == nc::core::CommandRegistry::RegisterResult::Registered);

    struct ToggleCall {
        void *target;
        bool expected;
        bool desired;
    };
    std::vector<ToggleCall> toggle_calls;
    REQUIRE(registry.Register(nc::core::MakeViewTogglePreviewPaneCommand(
                [&](void *_target, const bool _expected, const bool _desired) {
                    toggle_calls.emplace_back(ToggleCall{_target, _expected, _desired});
                    return true;
                })) == nc::core::CommandRegistry::RegisterResult::Registered);

    NCPanelControllerActionsDispatcher *const dispatcher =
        [[NCPanelControllerActionsDispatcher alloc] initWithController:fixture.Controller()
                                                            actionsMap:actions
                                               actionsShortcutsManager:shortcuts
                                                       commandRegistry:registry];

    CHECK_FALSE(dispatcher.fileGetInfoCommandState.enabled);
    CHECK(dispatcher.filePreviewCommandState.enabled);
    CHECK_FALSE(dispatcher.viewTogglePreviewPaneCommandState.enabled);

    PanelControllerInspectorPresenterTestState *const presenter =
        [[PanelControllerInspectorPresenterTestState alloc] init];
    fixture.Controller().state = presenter;

    NSMenuItem *const get_info_menu =
        [[NSMenuItem alloc] initWithTitle:@"Get Info" action:@selector(OnFileGetInfo:) keyEquivalent:@""];
    NSMenuItem *const preview_menu =
        [[NSMenuItem alloc] initWithTitle:@"Quick Look" action:@selector(OnFileViewCommand:) keyEquivalent:@""];
    NSMenuItem *const toggle_menu = [[NSMenuItem alloc] initWithTitle:@"Preview Pane"
                                                              action:@selector(OnTogglePreviewPane:)
                                                       keyEquivalent:@""];

    CHECK([dispatcher validateMenuItem:get_info_menu]);
    CHECK([dispatcher validateMenuItem:preview_menu]);
    CHECK([dispatcher validateMenuItem:toggle_menu]);
    CHECK(toggle_menu.state == NSControlStateValueOn);

    [dispatcher OnFileGetInfo:get_info_menu];
    CHECK(get_info_calls == 1);
    REQUIRE(get_info_presentation);
    CHECK(get_info_target == (__bridge void *)fixture.Controller());
    CHECK(get_info_presentation->source == nc::core::CommandInvocationSource::Menu);
    REQUIRE(get_info_presentation->items.size() == 1);
    CHECK(get_info_presentation->items.front().filename == "seed.txt");

    [dispatcher OnFileViewCommand:preview_menu];
    REQUIRE(preview_calls.size() == 1);
    CHECK(preview_calls.back().target == (__bridge void *)fixture.Controller());
    CHECK(preview_calls.back().source == nc::core::CommandInvocationSource::Menu);
    CHECK(preview_calls.back().sender == (__bridge const void *)preview_menu);
    CHECK(preview_calls.back().item == seed);

    [dispatcher OnTogglePreviewPane:toggle_menu];
    REQUIRE(toggle_calls.size() == 1);
    CHECK(toggle_calls.back().target == (__bridge void *)fixture.Controller());
    CHECK(toggle_calls.back().expected);
    CHECK_FALSE(toggle_calls.back().desired);

    const int dotdot_position = fixture.Controller().data.SortedIndexForName("..");
    REQUIRE(dotdot_position >= 0);
    fixture.View().curpos = dotdot_position;
    const std::array exact_preview{seed};
    [dispatcher executeFileGetInfoCommandWithItems:exact_preview
                                             source:nc::core::CommandInvocationSource::ContextMenu
                                             sender:get_info_menu];
    CHECK(get_info_calls == 2);
    REQUIRE(get_info_presentation);
    CHECK(get_info_presentation->source == nc::core::CommandInvocationSource::ContextMenu);
    REQUIRE(get_info_presentation->items.size() == 1);
    CHECK(get_info_presentation->items.front().filename == "seed.txt");
    [dispatcher executeFilePreviewCommandWithItems:exact_preview
                                             source:nc::core::CommandInvocationSource::ContextMenu
                                             sender:preview_menu];
    REQUIRE(preview_calls.size() == 2);
    CHECK(preview_calls.back().source == nc::core::CommandInvocationSource::ContextMenu);
    CHECK(preview_calls.back().sender == (__bridge const void *)preview_menu);
    CHECK(preview_calls.back().item == seed);

    const std::array parent_item{fixture.View().item};
    REQUIRE(parent_item.front().IsDotDot());
    [dispatcher executeFileGetInfoCommandWithItems:parent_item
                                             source:nc::core::CommandInvocationSource::ContextMenu
                                             sender:get_info_menu];
    [dispatcher executeFilePreviewCommandWithItems:parent_item
                                             source:nc::core::CommandInvocationSource::ContextMenu
                                             sender:preview_menu];
    CHECK(preview_calls.size() == 2);
    CHECK(get_info_calls == 2);

    fixture.View().curpos = seed_position;
    NSEvent *const space = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                          location:NSZeroPoint
                                     modifierFlags:0
                                         timestamp:0
                                      windowNumber:0
                                           context:nil
                                        characters:@" "
                       charactersIgnoringModifiers:@" "
                                         isARepeat:false
                                           keyCode:49];
    CHECK([dispatcher bidForHandlingKeyDown:space forPanelView:fixture.View()] ==
          nc::panel::view::BiddingPriority::High);
    [dispatcher handleKeyDown:space forPanelView:fixture.View()];
    REQUIRE(preview_calls.size() == 3);
    CHECK(preview_calls.back().source == nc::core::CommandInvocationSource::Shortcut);
    CHECK(preview_calls.back().sender == (__bridge const void *)space);
    CHECK(preview_calls.back().item == seed);

    fixture.Controller().state = nil;
}

TEST_CASE(PREFIX "Paste Trash and Permanent Delete preserve menu toolbar shortcut and exact context")
{
    REQUIRE([NSThread isMainThread]);
    PanelControllerNavigationFixture fixture;
    nc::core::CommandRegistry registry;
    nc::panel::PanelActionsMap actions;
    TestActionsShortcutsManager shortcuts;

    const int seed_position = fixture.Controller().data.SortedIndexForName("seed.txt");
    REQUIRE(seed_position >= 0);
    fixture.View().curpos = seed_position;
    const VFSListingItem seed = fixture.View().item;
    REQUIRE(seed);

    struct RoutedContext {
        std::string id;
        nc::core::CommandInvocationSource source;
        const void *sender;
        void *target;
        std::vector<VFSListingItem> items;
    };
    std::vector<RoutedContext> evaluated;
    std::vector<RoutedContext> executed;
    const auto capture = [](const std::string_view _id, const nc::core::CommandContext &_context) {
        return RoutedContext{
            .id = std::string{_id},
            .source = _context.source,
            .sender = _context.native_sender,
            .target = _context.native_target,
            .items = {_context.items.begin(), _context.items.end()},
        };
    };
    const auto register_route = [&](const std::string_view _id,
                                    const std::string_view _selector,
                                    const std::string_view _shortcut,
                                    const int _tag) {
        nc::core::CommandRegistry::Registration registration;
        registration.descriptor.id = nc::core::CommandId{_id};
        registration.descriptor.legacy = nc::core::LegacyCommandMetadata{
            .selector_name = std::string{_selector},
            .shortcut_action_names = {std::string{_shortcut}},
            .shortcut_tag = _tag,
        };
        registration.state_provider = [&, id = std::string{_id}](const nc::core::CommandContext &_context) {
            evaluated.emplace_back(capture(id, _context));
            nc::core::CommandState state;
            state.enabled = _context.native_target != nullptr &&
                            (id == nc::core::command_ids::FilePaste || _context.items.size() == 1);
            return state;
        };
        registration.handler = [&, id = std::string{_id}](const nc::core::CommandContext &_context) {
            executed.emplace_back(capture(id, _context));
        };
        REQUIRE(registry.Register(std::move(registration)) ==
                nc::core::CommandRegistry::RegisterResult::Registered);
    };
    register_route(nc::core::command_ids::FilePaste, "paste:", "menu.edit.paste", 12'010);
    register_route(
        nc::core::command_ids::FileTrash, "OnMoveToTrash:", "menu.command.move_to_trash", 15'160);
    register_route(nc::core::command_ids::FileDelete,
                   "OnDeletePermanentlyCommand:",
                   "menu.command.delete_permanently",
                   15'180);

    NCPanelControllerActionsDispatcher *const dispatcher =
        [[NCPanelControllerActionsDispatcher alloc] initWithController:fixture.Controller()
                                                            actionsMap:actions
                                               actionsShortcutsManager:shortcuts
                                                       commandRegistry:registry];
    NSMenuItem *const paste_menu =
        [[NSMenuItem alloc] initWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    NSMenuItem *const trash_menu =
        [[NSMenuItem alloc] initWithTitle:@"Trash" action:@selector(OnMoveToTrash:) keyEquivalent:@""];
    NSMenuItem *const delete_menu = [[NSMenuItem alloc] initWithTitle:@"Delete Permanently"
                                                               action:@selector(OnDeletePermanentlyCommand:)
                                                        keyEquivalent:@""];

    CHECK([dispatcher validateMenuItem:paste_menu]);
    CHECK([dispatcher validateMenuItem:trash_menu]);
    CHECK([dispatcher validateMenuItem:delete_menu]);
    REQUIRE(evaluated.size() == 3);
    CHECK(evaluated[0].id == nc::core::command_ids::FilePaste);
    CHECK(evaluated[0].source == nc::core::CommandInvocationSource::Menu);
    CHECK(evaluated[0].items.empty());
    CHECK(evaluated[1].id == nc::core::command_ids::FileTrash);
    CHECK(evaluated[1].source == nc::core::CommandInvocationSource::Menu);
    CHECK(evaluated[1].items == std::vector<VFSListingItem>{seed});
    CHECK(evaluated[2].id == nc::core::command_ids::FileDelete);
    CHECK(evaluated[2].source == nc::core::CommandInvocationSource::Menu);
    CHECK(evaluated[2].items == std::vector<VFSListingItem>{seed});

    [dispatcher paste:paste_menu];
    REQUIRE(executed.size() == 1);
    CHECK(executed.back().id == nc::core::command_ids::FilePaste);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Menu);
    CHECK(executed.back().sender == (__bridge const void *)paste_menu);
    CHECK(executed.back().items.empty());

    NSButton *const trash_button =
        [NSButton buttonWithTitle:@"Trash" target:dispatcher action:@selector(OnMoveToTrash:)];
    [dispatcher OnMoveToTrash:trash_button];
    REQUIRE(executed.size() == 2);
    CHECK(executed.back().id == nc::core::command_ids::FileTrash);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Toolbar);
    CHECK(executed.back().items == std::vector<VFSListingItem>{seed});

    const std::vector<VFSListingItem> exact_items{seed};
    [dispatcher executeFileTrashCommandWithItems:exact_items
                                          source:nc::core::CommandInvocationSource::ContextMenu
                                          sender:trash_menu];
    REQUIRE(executed.size() == 3);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::ContextMenu);
    CHECK(executed.back().items == exact_items);

    [dispatcher executeFileDeleteCommandFromSource:nc::core::CommandInvocationSource::Shortcut
                                            sender:delete_menu];
    REQUIRE(executed.size() == 4);
    CHECK(executed.back().id == nc::core::command_ids::FileDelete);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Shortcut);
    CHECK(executed.back().items == exact_items);
    for( const RoutedContext &context : executed )
        CHECK(context.target == (__bridge void *)fixture.Controller());
}

TEST_CASE(PREFIX "New File admits only provider implementations with proven exclusive creation")
{
    class ProviderProbe final : public nc::vfs::Host
    {
    public:
        explicit ProviderProbe(const char *_tag) : Host("/", nullptr, _tag)
        {
            AddFeatures(nc::vfs::HostFeatures::CreateFile);
        }

        bool IsWritable() const override { return true; }
    };

    ProviderProbe native{VFSNativeHost::UniqueTag};
    ProviderProbe sftp{nc::vfs::SFTPHost::UniqueTag};
    ProviderProbe ftp{nc::vfs::FTPHost::UniqueTag};
    ProviderProbe webdav{nc::vfs::WebDAVHost::UniqueTag};
    ProviderProbe unproven{"exclusive_new_file_unproven"};

    for( ProviderProbe *const provider : {&native, &sftp, &ftp, &webdav, &unproven} )
        REQUIRE(nc::vfs::ProviderCapabilitiesResolver::Resolve(*provider, "/").can_create_file);

    CHECK(nc::panel::actions::SupportsExclusiveQuickNewFile(native));
    CHECK(nc::panel::actions::SupportsExclusiveQuickNewFile(sftp));
    CHECK_FALSE(nc::panel::actions::SupportsExclusiveQuickNewFile(ftp));
    CHECK_FALSE(nc::panel::actions::SupportsExclusiveQuickNewFile(webdav));
    CHECK_FALSE(nc::panel::actions::SupportsExclusiveQuickNewFile(unproven));
}

TEST_CASE(PREFIX "creation and visible-selection commands route every mounted selector through Registry")
{
    REQUIRE([NSThread isMainThread]);
    PanelControllerNavigationFixture fixture;
    nc::core::CommandRegistry registry;
    nc::panel::PanelActionsMap actions;
    TestActionsShortcutsManager shortcuts;

    struct RoutedContext {
        std::string id;
        nc::core::CommandInvocationSource source;
        const void *sender;
        void *target;
    };
    std::vector<RoutedContext> evaluated;
    std::vector<RoutedContext> executed;
    const auto register_route = [&](const std::string_view _id,
                                    const std::string_view _selector,
                                    const std::string_view _shortcut,
                                    const int _tag) {
        nc::core::CommandRegistry::Registration registration;
        registration.descriptor.id = nc::core::CommandId{_id};
        registration.descriptor.legacy = nc::core::LegacyCommandMetadata{
            .selector_name = std::string{_selector},
            .shortcut_action_names = {std::string{_shortcut}},
            .shortcut_tag = _tag,
        };
        registration.state_provider = [&, id = std::string{_id}](const nc::core::CommandContext &_context) {
            evaluated.emplace_back(RoutedContext{id, _context.source, _context.native_sender, _context.native_target});
            nc::core::CommandState state;
            state.enabled = _context.native_target != nullptr;
            return state;
        };
        registration.handler = [&, id = std::string{_id}](const nc::core::CommandContext &_context) {
            executed.emplace_back(RoutedContext{id, _context.source, _context.native_sender, _context.native_target});
        };
        REQUIRE(registry.Register(std::move(registration)) ==
                nc::core::CommandRegistry::RegisterResult::Registered);
    };
    register_route(nc::core::command_ids::FileNewFolder,
                   "OnQuickNewFolder:",
                   "menu.file.new_folder",
                   11'090);
    register_route(nc::core::command_ids::FileNewFile,
                   "OnQuickNewFile:",
                   "menu.file.new_file",
                   11'120);
    register_route(nc::core::command_ids::PaneSelectAll, "selectAll:", "menu.edit.select_all", 12'020);
    register_route(nc::core::command_ids::PaneInvertSelection,
                   "OnMenuInvertSelection:",
                   "menu.edit.invert_selection",
                   12'040);

    NCPanelControllerActionsDispatcher *const dispatcher =
        [[NCPanelControllerActionsDispatcher alloc] initWithController:fixture.Controller()
                                                            actionsMap:actions
                                               actionsShortcutsManager:shortcuts
                                                       commandRegistry:registry];
    NSMenuItem *const new_folder_menu =
        [[NSMenuItem alloc] initWithTitle:@"New Folder" action:@selector(OnQuickNewFolder:) keyEquivalent:@""];
    NSMenuItem *const new_file_menu =
        [[NSMenuItem alloc] initWithTitle:@"New File" action:@selector(OnQuickNewFile:) keyEquivalent:@""];
    NSMenuItem *const select_all_menu =
        [[NSMenuItem alloc] initWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    NSMenuItem *const invert_menu = [[NSMenuItem alloc] initWithTitle:@"Invert Selection"
                                                               action:@selector(OnMenuInvertSelection:)
                                                        keyEquivalent:@""];

    CHECK([dispatcher validateMenuItem:new_folder_menu]);
    CHECK([dispatcher validateMenuItem:new_file_menu]);
    CHECK([dispatcher validateMenuItem:select_all_menu]);
    CHECK([dispatcher validateMenuItem:invert_menu]);
    REQUIRE(evaluated.size() == 4);
    for( const RoutedContext &context : evaluated ) {
        CHECK(context.source == nc::core::CommandInvocationSource::Menu);
        CHECK(context.target == (__bridge void *)fixture.Controller());
    }

    [dispatcher OnQuickNewFolder:new_folder_menu];
    [dispatcher OnQuickNewFile:new_file_menu];
    [dispatcher selectAll:select_all_menu];
    REQUIRE(executed.size() == 3);
    CHECK(executed[0].id == nc::core::command_ids::FileNewFolder);
    CHECK(executed[0].source == nc::core::CommandInvocationSource::Menu);
    CHECK(executed[1].id == nc::core::command_ids::FileNewFile);
    CHECK(executed[1].source == nc::core::CommandInvocationSource::Menu);
    CHECK(executed[2].id == nc::core::command_ids::PaneSelectAll);
    CHECK(executed[2].source == nc::core::CommandInvocationSource::Menu);

    NSButton *const new_folder_button =
        [NSButton buttonWithTitle:@"New Folder" target:dispatcher action:@selector(OnQuickNewFolder:)];
    [dispatcher OnQuickNewFolder:new_folder_button];
    REQUIRE(executed.size() == 4);
    CHECK(executed.back().id == nc::core::command_ids::FileNewFolder);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Toolbar);

    [dispatcher executePaneInvertSelectionCommandFromSource:nc::core::CommandInvocationSource::Shortcut
                                                     sender:invert_menu];
    REQUIRE(executed.size() == 5);
    CHECK(executed.back().id == nc::core::command_ids::PaneInvertSelection);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Shortcut);
    CHECK(executed.back().target == (__bridge void *)fixture.Controller());
}

TEST_CASE(PREFIX "payload commands route menu shortcut and exact context through Registry")
{
    REQUIRE([NSThread isMainThread]);
    PanelControllerNavigationFixture fixture;
    nc::core::CommandRegistry registry;
    nc::panel::PanelActionsMap actions;
    TestActionsShortcutsManager shortcuts;

    const int seed_position = fixture.Controller().data.SortedIndexForName("seed.txt");
    REQUIRE(seed_position >= 0);
    fixture.View().curpos = seed_position;
    const VFSListingItem seed = fixture.View().item;
    REQUIRE(seed);

    struct RoutedContext {
        std::string id;
        nc::core::CommandInvocationSource source;
        const void *sender;
        void *target;
        std::vector<VFSListingItem> items;
    };
    std::vector<RoutedContext> evaluated;
    std::vector<RoutedContext> executed;
    const auto capture = [](const std::string_view _id, const nc::core::CommandContext &_context) {
        return RoutedContext{
            .id = std::string{_id},
            .source = _context.source,
            .sender = _context.native_sender,
            .target = _context.native_target,
            .items = {_context.items.begin(), _context.items.end()},
        };
    };
    const auto register_route = [&](const std::string_view _id,
                                    const std::string_view _selector,
                                    const std::string_view _shortcut,
                                    const int _tag) {
        nc::core::CommandRegistry::Registration registration;
        registration.descriptor.id = nc::core::CommandId{_id};
        registration.descriptor.legacy = nc::core::LegacyCommandMetadata{
            .selector_name = std::string{_selector},
            .shortcut_action_names = {std::string{_shortcut}},
            .shortcut_tag = _tag,
        };
        registration.state_provider = [&, id = std::string{_id}](const nc::core::CommandContext &_context) {
            evaluated.emplace_back(capture(id, _context));
            nc::core::CommandState state;
            state.enabled = _context.native_target != nullptr && _context.items.size() == 1;
            return state;
        };
        registration.handler = [&, id = std::string{_id}](const nc::core::CommandContext &_context) {
            executed.emplace_back(capture(id, _context));
        };
        REQUIRE(registry.Register(std::move(registration)) ==
                nc::core::CommandRegistry::RegisterResult::Registered);
    };
    register_route(nc::core::command_ids::ArchiveCreate,
                   "onCompressItemsHere:",
                   "menu.command.compress_here",
                   15'100);
    register_route(nc::core::command_ids::FileDuplicate, "OnDuplicate:", "menu.file.duplicate", 11'150);
    register_route(nc::core::command_ids::FileCopyPath,
                   "OnCopyCurrentFilePath:",
                   "menu.command.copy_file_path",
                   15'040);
    register_route(nc::core::command_ids::FileCalculateSizes,
                   "OnCalculateSizes:",
                   "menu.file.calculate_sizes",
                   11'030);
    register_route(nc::core::command_ids::FileBatchRename,
                   "OnBatchRename:",
                   "menu.command.batch_rename",
                   15'220);

    NCPanelControllerActionsDispatcher *const dispatcher =
        [[NCPanelControllerActionsDispatcher alloc] initWithController:fixture.Controller()
                                                            actionsMap:actions
                                               actionsShortcutsManager:shortcuts
                                                       commandRegistry:registry];
    NSMenuItem *const archive_menu =
        [[NSMenuItem alloc] initWithTitle:@"Compress" action:@selector(onCompressItemsHere:) keyEquivalent:@""];
    NSMenuItem *const duplicate_menu =
        [[NSMenuItem alloc] initWithTitle:@"Duplicate" action:@selector(OnDuplicate:) keyEquivalent:@""];
    NSMenuItem *const copy_path_menu = [[NSMenuItem alloc] initWithTitle:@"Copy Path"
                                                                 action:@selector(OnCopyCurrentFilePath:)
                                                          keyEquivalent:@""];
    NSMenuItem *const calculate_sizes_menu = [[NSMenuItem alloc] initWithTitle:@"Calculate Sizes"
                                                                       action:@selector(OnCalculateSizes:)
                                                                keyEquivalent:@""];
    NSMenuItem *const batch_rename_menu = [[NSMenuItem alloc] initWithTitle:@"Batch Rename"
                                                                    action:@selector(OnBatchRename:)
                                                             keyEquivalent:@""];

    CHECK([dispatcher validateMenuItem:archive_menu]);
    CHECK([dispatcher validateMenuItem:duplicate_menu]);
    CHECK([dispatcher validateMenuItem:copy_path_menu]);
    CHECK([dispatcher validateMenuItem:calculate_sizes_menu]);
    CHECK([dispatcher validateMenuItem:batch_rename_menu]);
    REQUIRE(evaluated.size() == 5);
    for( const RoutedContext &context : evaluated ) {
        CHECK(context.source == nc::core::CommandInvocationSource::Menu);
        CHECK(context.target == (__bridge void *)fixture.Controller());
        CHECK(context.items == std::vector<VFSListingItem>{seed});
    }

    [dispatcher onCompressItemsHere:archive_menu];
    [dispatcher OnDuplicate:duplicate_menu];
    [dispatcher OnCopyCurrentFilePath:copy_path_menu];
    [dispatcher OnCalculateSizes:calculate_sizes_menu];
    [dispatcher OnBatchRename:batch_rename_menu];
    REQUIRE(executed.size() == 5);
    CHECK(executed[0].id == nc::core::command_ids::ArchiveCreate);
    CHECK(executed[1].id == nc::core::command_ids::FileDuplicate);
    CHECK(executed[2].id == nc::core::command_ids::FileCopyPath);
    CHECK(executed[3].id == nc::core::command_ids::FileCalculateSizes);
    CHECK(executed[4].id == nc::core::command_ids::FileBatchRename);
    for( const RoutedContext &context : executed ) {
        CHECK(context.source == nc::core::CommandInvocationSource::Menu);
        CHECK(context.items == std::vector<VFSListingItem>{seed});
    }

    const std::vector<VFSListingItem> exact_items{seed};
    [dispatcher executeArchiveCreateCommandWithItems:exact_items
                                               source:nc::core::CommandInvocationSource::ContextMenu
                                               sender:archive_menu];
    [dispatcher executeFileDuplicateCommandWithItems:exact_items
                                               source:nc::core::CommandInvocationSource::ContextMenu
                                               sender:duplicate_menu];
    [dispatcher executeFileCopyPathCommandWithItems:exact_items
                                              source:nc::core::CommandInvocationSource::ContextMenu
                                              sender:copy_path_menu];
    [dispatcher executeFileCalculateSizesCommandWithItems:exact_items
                                                     source:nc::core::CommandInvocationSource::ContextMenu
                                                     sender:calculate_sizes_menu];
    [dispatcher executeFileBatchRenameCommandWithItems:exact_items
                                                  source:nc::core::CommandInvocationSource::ContextMenu
                                                  sender:batch_rename_menu];
    REQUIRE(executed.size() == 10);
    for( std::size_t index = 5; index < executed.size(); ++index ) {
        CHECK(executed[index].source == nc::core::CommandInvocationSource::ContextMenu);
        CHECK(executed[index].items == exact_items);
    }

    [dispatcher executeArchiveCreateCommandFromSource:nc::core::CommandInvocationSource::Shortcut
                                                sender:archive_menu];
    [dispatcher executeFileCalculateSizesCommandFromSource:nc::core::CommandInvocationSource::Shortcut
                                                    sender:calculate_sizes_menu];
    [dispatcher executeFileBatchRenameCommandFromSource:nc::core::CommandInvocationSource::Shortcut
                                                 sender:batch_rename_menu];
    REQUIRE(executed.size() == 13);
    CHECK(executed.back().source == nc::core::CommandInvocationSource::Shortcut);
    CHECK(executed[10].sender == (__bridge const void *)archive_menu);
    CHECK(executed[11].id == nc::core::command_ids::FileCalculateSizes);
    CHECK(executed[12].id == nc::core::command_ids::FileBatchRename);
}

TEST_CASE(PREFIX "Select All and Invert Selection use only visible non-parent entries")
{
    PanelControllerNavigationFixture fixture;
    PanelController *const panel = fixture.Controller();

    REQUIRE(nc::panel::actions::ApplySelectAll(panel) ==
            nc::panel::actions::PaneSelectionActionResult::Available);
    CHECK(panel.data.Stats().selected_entries_amount == 1);
    const int dotdot = panel.data.SortedIndexForName("..");
    REQUIRE(dotdot >= 0);
    CHECK_FALSE(panel.data.VolatileDataAtSortPosition(dotdot).is_selected());

    [panel loadListing:NonUniformListing(fixture.Host())];
    REQUIRE(nc::panel::actions::ApplySelectAll(panel) ==
            nc::panel::actions::PaneSelectionActionResult::Available);
    CHECK(panel.data.Stats().selected_entries_amount == 2);

    nc::panel::data::TextualFilter filter;
    filter.text = @"first";
    [panel changeDataOptions:[&](nc::panel::data::Model &_data) { _data.SetSoftFiltering(filter); }];
    REQUIRE(nc::panel::actions::ApplySelectAll(panel) ==
            nc::panel::actions::PaneSelectionActionResult::Available);
    const int first = panel.data.SortedIndexForName("first.txt");
    const int second = panel.data.SortedIndexForName("second.txt");
    REQUIRE(first >= 0);
    REQUIRE(second >= 0);
    CHECK(panel.data.VolatileDataAtSortPosition(first).is_selected());
    CHECK_FALSE(panel.data.VolatileDataAtSortPosition(second).is_selected());

    REQUIRE(nc::panel::actions::ApplyInvertSelection(panel) ==
            nc::panel::actions::PaneSelectionActionResult::Available);
    CHECK_FALSE(panel.data.VolatileDataAtSortPosition(first).is_selected());
    CHECK_FALSE(panel.data.VolatileDataAtSortPosition(second).is_selected());

    filter.text = @"absent";
    [panel changeDataOptions:[&](nc::panel::data::Model &_data) { _data.SetSoftFiltering(filter); }];
    CHECK(nc::panel::actions::EvaluatePaneSelectionAction(panel) ==
          nc::panel::actions::PaneSelectionActionResult::Empty);
}

TEST_CASE(PREFIX "Paste releases a claimed Cut when its pane target is unavailable")
{
    PanelControllerNavigationFixture fixture;
    NSPasteboard *const pasteboard = [NSPasteboard pasteboardWithUniqueName];
    if( !pasteboard )
        SKIP("This host does not provide a pasteboard server.");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [pasteboard declareTypes:@[NSFilenamesPboardType] owner:nil];
    REQUIRE([pasteboard setPropertyList:@[@"/tmp/unsubmitted-cut"] forType:NSFilenamesPboardType]);
#pragma clang diagnostic pop
    REQUIRE(nc::panel::PasteboardSupport::MarkCurrentFileListForMove(pasteboard));
    const auto token = nc::panel::PasteboardSupport::CurrentCutToken(pasteboard);
    REQUIRE(token);

    const nc::panel::actions::PasteFromPasteboard action{*fixture.NativeHost()};
    CHECK(action.Execute(nil, pasteboard) == nc::panel::actions::PasteSubmissionResult::PaneUnavailable);
    CHECK_FALSE(nc::panel::PasteboardSupport::IsCutInFlight(pasteboard));
    CHECK(nc::panel::PasteboardSupport::CurrentCutToken(pasteboard) == token);
}

#undef PREFIX
