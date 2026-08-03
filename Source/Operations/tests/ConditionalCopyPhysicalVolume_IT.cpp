// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"

#include "../source/CopyOperationOrchestrator.h"
#include "../source/VFSOperationPlanningProbes.h"
#include "../../VFS/source/Native/ConditionalCopy.h"

#include <Operations/OperationJournal.h>
#include <Operations/Pool.h>
#include <Utility/NativeFSManager.h>
#include <VFS/Native.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <expected>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace nc::ops {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view g_MarkerName = ".wincommander-operations-it-root";
constexpr std::string_view g_MarkerContents = "wincommander-operations-it-root\n";
constexpr std::string_view g_InternalRootEnvironment = "WINCOMMANDER_OPERATIONS_IT_INTERNAL_ROOT";
constexpr std::string_view g_ExternalRootEnvironment = "WINCOMMANDER_OPERATIONS_IT_EXTERNAL_ROOT";
constexpr std::string_view g_RequireRootsEnvironment = "WINCOMMANDER_OPERATIONS_IT_REQUIRE_VOLUMES";

enum class PhysicalVolumeRole : uint8_t {
    Internal,
    External
};

bool PhysicalVolumeRootsRequired() noexcept
{
    const char *const value = std::getenv(g_RequireRootsEnvironment.data());
    return value != nullptr && std::strcmp(value, "1") == 0;
}

[[noreturn]] void SkipOrFailPhysicalFixture(const std::string &_reason)
{
    const auto message = "physical Conditional Copy fixture: " + _reason;
    if( PhysicalVolumeRootsRequired() )
        FAIL(message);
    SKIP(message);
}

std::expected<int, std::string> OpenAbsoluteDirectoryWithoutSymlinks(const std::string_view _path)
{
    if( _path.size() < 2 || _path.front() != '/' || _path.back() == '/' || _path.find("//") != std::string_view::npos )
        return std::unexpected("path must be a canonical absolute directory without a trailing slash");

    int current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if( current < 0 )
        return std::unexpected("cannot open filesystem root: " + std::string{std::strerror(errno)});

    size_t offset = 1;
    while( offset < _path.size() ) {
        const size_t slash = _path.find('/', offset);
        const auto component =
            _path.substr(offset, slash == std::string_view::npos ? _path.size() - offset : slash - offset);
        if( component.empty() || component == "." || component == ".." ) {
            ::close(current);
            return std::unexpected("path contains a non-canonical component");
        }
        const std::string name{component};
        const int next = ::openat(current, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if( next < 0 ) {
            const auto error = errno;
            ::close(current);
            return std::unexpected("cannot open path component " + name + ": " + std::strerror(error));
        }
        ::close(current);
        current = next;
        if( slash == std::string_view::npos )
            break;
        offset = slash + 1;
    }
    return current;
}

std::expected<void, std::string> VerifyMarkerFile(const int _root_fd)
{
    struct stat marker_stat{};
    if( ::fstatat(_root_fd, g_MarkerName.data(), &marker_stat, AT_SYMLINK_NOFOLLOW) != 0 )
        return std::unexpected("marker is absent or inaccessible: " + std::string{std::strerror(errno)});
    if( !S_ISREG(marker_stat.st_mode) || marker_stat.st_uid != ::geteuid() || marker_stat.st_nlink != 1 )
        return std::unexpected("marker is not the required user-owned regular file");

    const int marker_fd = ::openat(_root_fd, g_MarkerName.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if( marker_fd < 0 )
        return std::unexpected("marker cannot be opened without following links: " + std::string{std::strerror(errno)});

    std::array<char, g_MarkerContents.size() + 1> contents{};
    const ssize_t read = ::read(marker_fd, contents.data(), contents.size());
    const auto close_result = ::close(marker_fd);
    if( read != static_cast<ssize_t>(g_MarkerContents.size()) || close_result != 0 ||
        std::string_view{contents.data(), g_MarkerContents.size()} != g_MarkerContents )
        return std::unexpected("marker contents are not exact");
    return {};
}

class PhysicalVolumeRoot final
{
public:
    PhysicalVolumeRoot() = delete;
    PhysicalVolumeRoot(const PhysicalVolumeRoot &) = delete;
    PhysicalVolumeRoot &operator=(const PhysicalVolumeRoot &) = delete;
    PhysicalVolumeRoot(PhysicalVolumeRoot &&_other) noexcept
        : m_Fd{std::exchange(_other.m_Fd, -1)}, m_Path{std::move(_other.m_Path)}, m_Device{_other.m_Device},
          m_Inode{_other.m_Inode}
    {
    }
    PhysicalVolumeRoot &operator=(PhysicalVolumeRoot &&) = delete;
    ~PhysicalVolumeRoot()
    {
        if( m_Fd >= 0 )
            ::close(m_Fd);
    }

    [[nodiscard]] static std::expected<PhysicalVolumeRoot, std::string> Open(const std::string_view _environment,
                                                                             const PhysicalVolumeRole _role)
    {
        const char *const configured = std::getenv(_environment.data());
        if( configured == nullptr || configured[0] == '\0' )
            return std::unexpected(std::string{_environment} + " is not set");
        const std::string raw{configured};
        if( raw.empty() || raw.front() != '/' )
            return std::unexpected(std::string{_environment} + " must be an absolute path");

        std::error_code canonical_error;
        const auto canonical = std::filesystem::canonical(raw, canonical_error);
        if( canonical_error || canonical.generic_string() != raw )
            return std::unexpected(std::string{_environment} + " must already be canonical and contain no aliases");

        auto opened = OpenAbsoluteDirectoryWithoutSymlinks(raw);
        if( !opened )
            return std::unexpected(std::string{_environment} + ": " + opened.error());
        const int root_fd = *opened;
        const auto close_on_error = [&] { ::close(root_fd); };

        struct stat root_stat{};
        if( ::fstat(root_fd, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode) ) {
            close_on_error();
            return std::unexpected(std::string{_environment} + " is not an accessible directory");
        }
        if( root_stat.st_uid != ::geteuid() || (root_stat.st_mode & 0022) != 0 ) {
            close_on_error();
            return std::unexpected(std::string{_environment} + " must be user-owned and not group/world writable");
        }
        if( const auto marker = VerifyMarkerFile(root_fd); !marker ) {
            close_on_error();
            return std::unexpected(std::string{_environment} + ": " + marker.error());
        }

        const auto volume = TestEnv().native_fs_man->VolumeFromFD(root_fd);
        if( !volume ) {
            close_on_error();
            return std::unexpected(std::string{_environment} + " has no native volume identity");
        }
        const auto mounted_at = std::filesystem::path{volume->mounted_at_path}.lexically_normal();
        if( canonical == mounted_at || canonical == std::filesystem::path{"/"} ) {
            close_on_error();
            return std::unexpected(std::string{_environment} +
                                   " must be a dedicated child of its volume, not a mount root");
        }

        const auto decision = nc::vfs::native::EvaluateConditionalCopyVolume(*volume);
        if( _role == PhysicalVolumeRole::Internal ) {
            if( !decision.IsSupported() ) {
                close_on_error();
                return std::unexpected(std::string{_environment} +
                                       " is not a supported internal APFS Conditional Copy root");
            }
        }
        else if( decision.disposition != nc::vfs::native::ConditionalCopyVolumeDisposition::UnsupportedExternalMedia ) {
            close_on_error();
            return std::unexpected(
                std::string{_environment} +
                " must be an external/removable/ejectable APFS volume rejected as UnsupportedExternalMedia");
        }

        return PhysicalVolumeRoot{root_fd, canonical, root_stat.st_dev, root_stat.st_ino};
    }

    [[nodiscard]] int Fd() const noexcept { return m_Fd; }
    [[nodiscard]] const std::filesystem::path &Path() const noexcept { return m_Path; }
    [[nodiscard]] bool Contains(const std::filesystem::path &_candidate) const noexcept
    {
        const auto root = m_Path.generic_string();
        const auto candidate = _candidate.generic_string();
        return candidate.size() > root.size() && candidate.starts_with(root) && candidate[root.size()] == '/';
    }
    [[nodiscard]] bool MarkerStillValid() const noexcept { return VerifyMarkerFile(m_Fd).has_value(); }

private:
    PhysicalVolumeRoot(const int _fd, std::filesystem::path _path, const dev_t _device, const ino_t _inode)
        : m_Fd{_fd}, m_Path{std::move(_path)}, m_Device{_device}, m_Inode{_inode}
    {
    }

    int m_Fd{-1};
    std::filesystem::path m_Path;
    dev_t m_Device{};
    ino_t m_Inode{};
};

std::string PhysicalWorkspaceName()
{
    std::array<unsigned char, 16> bytes{};
    arc4random_buf(bytes.data(), bytes.size());
    constexpr char digits[] = "0123456789abcdef";
    std::string name{"conditional-copy-it-"};
    name.reserve(name.size() + bytes.size() * 2);
    for( const auto byte : bytes ) {
        name.push_back(digits[byte >> 4]);
        name.push_back(digits[byte & 0x0f]);
    }
    return name;
}

bool RemoveChildrenAt(const int _directory_fd) noexcept
{
    DIR *const directory = ::fdopendir(::dup(_directory_fd));
    if( directory == nullptr )
        return false;
    bool success = true;
    while( const dirent *const entry = ::readdir(directory) ) {
        const std::string_view name{entry->d_name};
        if( name == "." || name == ".." )
            continue;
        struct stat status{};
        if( ::fstatat(_directory_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0 ) {
            success = false;
            continue;
        }
        if( S_ISDIR(status.st_mode) ) {
            const int child_fd =
                ::openat(_directory_fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            const bool removed_children = child_fd >= 0 && RemoveChildrenAt(child_fd);
            const bool closed = child_fd >= 0 && ::close(child_fd) == 0;
            if( !removed_children || !closed || ::unlinkat(_directory_fd, entry->d_name, AT_REMOVEDIR) != 0 ) {
                success = false;
            }
        }
        else if( ::unlinkat(_directory_fd, entry->d_name, 0) != 0 ) {
            success = false;
        }
    }
    return ::closedir(directory) == 0 && success;
}

class PhysicalWorkspace final
{
public:
    PhysicalWorkspace() = delete;
    PhysicalWorkspace(const PhysicalWorkspace &) = delete;
    PhysicalWorkspace &operator=(const PhysicalWorkspace &) = delete;
    PhysicalWorkspace(PhysicalWorkspace &&_other) noexcept
        : m_Root{_other.m_Root}, m_RootFd{std::exchange(_other.m_RootFd, -1)},
          m_WorkspaceFd{std::exchange(_other.m_WorkspaceFd, -1)}, m_Name{std::move(_other.m_Name)},
          m_Path{std::move(_other.m_Path)}, m_Device{_other.m_Device}, m_Inode{_other.m_Inode},
          m_Cleaned{std::exchange(_other.m_Cleaned, true)}
    {
    }
    PhysicalWorkspace &operator=(PhysicalWorkspace &&) = delete;
    ~PhysicalWorkspace()
    {
        if( !m_Cleaned )
            static_cast<void>(Cleanup());
        if( m_WorkspaceFd >= 0 )
            ::close(m_WorkspaceFd);
        if( m_RootFd >= 0 )
            ::close(m_RootFd);
    }

    [[nodiscard]] static std::expected<PhysicalWorkspace, std::string> Create(const PhysicalVolumeRoot &_root)
    {
        const int root_fd = ::dup(_root.Fd());
        if( root_fd < 0 )
            return std::unexpected("cannot duplicate fixture root descriptor: " + std::string{std::strerror(errno)});

        for( int attempt = 0; attempt < 8; ++attempt ) {
            const auto name = PhysicalWorkspaceName();
            if( ::mkdirat(root_fd, name.c_str(), 0700) != 0 ) {
                if( errno == EEXIST )
                    continue;
                const auto error = std::string{std::strerror(errno)};
                ::close(root_fd);
                return std::unexpected("cannot create fixture workspace: " + error);
            }
            const int workspace_fd = ::openat(root_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if( workspace_fd < 0 ) {
                const auto error = std::string{std::strerror(errno)};
                ::unlinkat(root_fd, name.c_str(), AT_REMOVEDIR);
                ::close(root_fd);
                return std::unexpected("cannot reopen fixture workspace: " + error);
            }
            struct stat created{};
            struct stat opened{};
            if( ::fstatat(root_fd, name.c_str(), &created, AT_SYMLINK_NOFOLLOW) != 0 ||
                ::fstat(workspace_fd, &opened) != 0 || !S_ISDIR(created.st_mode) || created.st_dev != opened.st_dev ||
                created.st_ino != opened.st_ino ) {
                ::close(workspace_fd);
                ::unlinkat(root_fd, name.c_str(), AT_REMOVEDIR);
                ::close(root_fd);
                return std::unexpected("fixture workspace identity cannot be anchored");
            }
            return PhysicalWorkspace{
                _root, root_fd, workspace_fd, name, _root.Path() / name, opened.st_dev, opened.st_ino};
        }
        ::close(root_fd);
        return std::unexpected("could not allocate a unique fixture workspace");
    }

    [[nodiscard]] int Fd() const noexcept { return m_WorkspaceFd; }
    [[nodiscard]] const std::filesystem::path &Path() const noexcept { return m_Path; }

    [[nodiscard]] std::expected<std::filesystem::path, std::string> WriteNewFile(const std::string_view _name,
                                                                                 const std::string_view _contents) const
    {
        if( !ValidDirectChildName(_name) )
            return std::unexpected("invalid fixture file name");
        const std::string name{_name};
        const int file =
            ::openat(m_WorkspaceFd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if( file < 0 )
            return std::unexpected("cannot create fixture file: " + std::string{std::strerror(errno)});
        size_t offset = 0;
        while( offset < _contents.size() ) {
            const ssize_t written = ::write(file, _contents.data() + offset, _contents.size() - offset);
            if( written > 0 ) {
                offset += static_cast<size_t>(written);
                continue;
            }
            if( written < 0 && errno == EINTR )
                continue;
            const auto error = std::string{std::strerror(errno)};
            ::close(file);
            return std::unexpected("cannot write fixture file: " + error);
        }
        if( ::fsync(file) != 0 || ::close(file) != 0 )
            return std::unexpected("cannot synchronize fixture file: " + std::string{std::strerror(errno)});
        return m_Path / name;
    }

    [[nodiscard]] std::expected<std::filesystem::path, std::string> CreateDirectory(const std::string_view _name) const
    {
        if( !ValidDirectChildName(_name) )
            return std::unexpected("invalid fixture directory name");
        const std::string name{_name};
        if( ::mkdirat(m_WorkspaceFd, name.c_str(), 0700) != 0 )
            return std::unexpected("cannot create fixture directory: " + std::string{std::strerror(errno)});
        return m_Path / name;
    }

    [[nodiscard]] bool Cleanup() noexcept
    {
        if( m_Cleaned )
            return true;
        if( !m_Root->MarkerStillValid() )
            return false;
        struct stat current{};
        if( ::fstatat(m_RootFd, m_Name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(current.st_mode) ||
            current.st_dev != m_Device || current.st_ino != m_Inode )
            return false;
        if( !RemoveChildrenAt(m_WorkspaceFd) || ::unlinkat(m_RootFd, m_Name.c_str(), AT_REMOVEDIR) != 0 )
            return false;
        m_Cleaned = true;
        return true;
    }

private:
    static bool ValidDirectChildName(const std::string_view _name) noexcept
    {
        return !_name.empty() && _name != "." && _name != ".." && _name.find('/') == std::string_view::npos;
    }

    PhysicalWorkspace(const PhysicalVolumeRoot &_root,
                      const int _root_fd,
                      const int _workspace_fd,
                      std::string _name,
                      std::filesystem::path _path,
                      const dev_t _device,
                      const ino_t _inode)
        : m_Root{&_root}, m_RootFd{_root_fd}, m_WorkspaceFd{_workspace_fd}, m_Name{std::move(_name)},
          m_Path{std::move(_path)}, m_Device{_device}, m_Inode{_inode}
    {
    }

    const PhysicalVolumeRoot *m_Root;
    int m_RootFd{-1};
    int m_WorkspaceFd{-1};
    std::string m_Name;
    std::filesystem::path m_Path;
    dev_t m_Device{};
    ino_t m_Inode{};
    bool m_Cleaned{false};
};

OperationPlan
PhysicalCopyPlan(const std::string _id, const std::filesystem::path &_source, const std::filesystem::path &_destination)
{
    auto plan = OperationPlan::Create({
        .plan_id = _id,
        .type = OperationPlanType::Copy,
        .sources = {OperationPlanSourceInput{"local", _source.string()}},
        .destination =
            OperationPlanDestinationInput{"local", _destination.string(), OperationPlanDestinationKind::Directory},
        .conflict_policy =
            OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem},
        .created_at = OperationPlan::TimePoint{1'700'000'000s},
    });
    REQUIRE(plan);
    return std::move(*plan);
}

ReviewedVFSOperationPreflight PhysicalCopyReview(const std::string _id,
                                                 const std::filesystem::path &_source,
                                                 const std::filesystem::path &_destination)
{
    auto bindings = VFSOperationPlanningBindings::Create({{"local", TestEnv().vfs_native}});
    REQUIRE(bindings);
    auto probes = VFSOperationPlanningProbes::Create(
        *bindings,
        [](const OperationPlanningPath &,
           OperationPlanningRequiredAccess,
           nc::vfs::Host &) -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    REQUIRE(probes);
    auto reviewed = ReviewedVFSOperationPreflight::Review(
        probes->Preflight(PhysicalCopyPlan(_id, _source, _destination)), VFSOperationPreflightReviewDecision::Approved);
    REQUIRE(reviewed);
    return std::move(*reviewed);
}

std::string ReadExactFile(const std::filesystem::path &_path)
{
    std::ifstream stream{_path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void RequireDistinctRoots(const PhysicalVolumeRoot &_internal, const PhysicalVolumeRoot &_external)
{
    REQUIRE(_internal.Path() != _external.Path());
    REQUIRE_FALSE(_internal.Contains(_external.Path()));
    REQUIRE_FALSE(_external.Contains(_internal.Path()));
}

} // namespace

TEST_CASE("CopyAs physical volumes: internal APFS publication reaches durable completion",
          "[reviewed-copy-as-physical][operations-it]")
{
    auto root_result = PhysicalVolumeRoot::Open(g_InternalRootEnvironment, PhysicalVolumeRole::Internal);
    if( !root_result )
        SkipOrFailPhysicalFixture(root_result.error());
    auto root = std::move(*root_result);

    auto workspace_result = PhysicalWorkspace::Create(root);
    REQUIRE(workspace_result);
    auto workspace = std::move(*workspace_result);
    auto source_result = workspace.WriteNewFile("source.txt", "physical-internal-conditional-copy\n");
    auto destination_result = workspace.CreateDirectory("destination");
    REQUIRE(source_result);
    REQUIRE(destination_result);
    const auto source = *source_result;
    const auto destination_directory = *destination_result;
    const auto destination = destination_directory / source.filename();

    REQUIRE(TestEnv().vfs_native->ConditionalCopyPathSupport(source.string(), destination_directory.string()) ==
            nc::vfs::ProviderConditionalCopyPathSupport::Supported);
    const auto journal_result = OperationJournal::Open(workspace.Path().string());
    REQUIRE(journal_result);
    auto journal = std::make_shared<OperationJournal>(std::move(*journal_result));
    auto pool = Pool::Make();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();

    std::mutex terminal_lock;
    std::optional<CopyOperationDurableTerminalOutcome> terminal_outcome;
    std::atomic_bool removal_after_terminal{false};
    auto removal_ticket = pool->Observe(Pool::NotifyAboutRemoval, [&] {
        const auto guard = std::lock_guard{terminal_lock};
        removal_after_terminal = terminal_outcome.has_value();
    });
    CopyOperationSubmissionHooks hooks{
        .durable_terminal_observer = [&](const CopyOperationDurableTerminalOutcome &_outcome) {
            const auto guard = std::lock_guard{terminal_lock};
            terminal_outcome = _outcome;
        }};

    CopyOperationOrchestrator orchestrator{journal, pool, custodian};
    auto submitted = orchestrator.Submit(
        PhysicalCopyReview("physical-internal-copy", source, destination_directory), {}, std::move(hooks));
    REQUIRE(submitted);
    REQUIRE((*submitted)->Wait(10s));
    REQUIRE(pool->Empty());
    pool->StopAndWaitForShutdown();

    CHECK(custodian->PendingCount() == 0);
    CHECK(ReadExactFile(destination) == ReadExactFile(source));
    struct stat source_stat{};
    struct stat destination_stat{};
    REQUIRE(::stat(source.c_str(), &source_stat) == 0);
    REQUIRE(::stat(destination.c_str(), &destination_stat) == 0);
    CHECK(source_stat.st_dev == destination_stat.st_dev);
    CHECK(source_stat.st_ino != destination_stat.st_ino);
    CHECK((source_stat.st_mode & 07777) == (destination_stat.st_mode & 07777));
    {
        const auto guard = std::lock_guard{terminal_lock};
        REQUIRE(terminal_outcome);
        CHECK(terminal_outcome->plan_id == "physical-internal-copy");
        CHECK(terminal_outcome->state == OperationJournalState::Completed);
        REQUIRE(terminal_outcome->item_result);
        CHECK(terminal_outcome->item_result->destination_publication == OperationJournalPublicationState::Published);
        CHECK(terminal_outcome->item_result->filesystem_sync_status == OperationJournalFilesystemSyncStatus::Confirmed);
    }
    CHECK(removal_after_terminal);
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Completed);
    REQUIRE(snapshot[0].item_results.size() == 1);
    CHECK(snapshot[0].item_results[0].status == OperationJournalItemStatus::Succeeded);
    CHECK(snapshot[0].item_results[0].destination_publication == OperationJournalPublicationState::Published);
    CHECK(snapshot[0].item_results[0].filesystem_sync_status == OperationJournalFilesystemSyncStatus::Confirmed);
    REQUIRE(workspace.Cleanup());
}

TEST_CASE("CopyAs physical volumes: external APFS scope fails before Pool admission",
          "[reviewed-copy-as-physical][operations-it]")
{
    auto internal_root_result = PhysicalVolumeRoot::Open(g_InternalRootEnvironment, PhysicalVolumeRole::Internal);
    if( !internal_root_result )
        SkipOrFailPhysicalFixture(internal_root_result.error());
    auto external_root_result = PhysicalVolumeRoot::Open(g_ExternalRootEnvironment, PhysicalVolumeRole::External);
    if( !external_root_result )
        SkipOrFailPhysicalFixture(external_root_result.error());
    auto internal_root = std::move(*internal_root_result);
    auto external_root = std::move(*external_root_result);
    RequireDistinctRoots(internal_root, external_root);

    auto journal_workspace_result = PhysicalWorkspace::Create(internal_root);
    auto external_workspace_result = PhysicalWorkspace::Create(external_root);
    REQUIRE(journal_workspace_result);
    REQUIRE(external_workspace_result);
    auto journal_workspace = std::move(*journal_workspace_result);
    auto external_workspace = std::move(*external_workspace_result);
    auto source_result = external_workspace.WriteNewFile("source.txt", "physical-external-rejection\n");
    auto destination_result = external_workspace.CreateDirectory("destination");
    REQUIRE(source_result);
    REQUIRE(destination_result);
    const auto source = *source_result;
    const auto destination_directory = *destination_result;
    const auto destination = destination_directory / source.filename();

    REQUIRE(TestEnv().vfs_native->ConditionalCopyPathSupport(source.string(), destination_directory.string()) ==
            nc::vfs::ProviderConditionalCopyPathSupport::Unsupported);
    const auto journal_result = OperationJournal::Open(journal_workspace.Path().string());
    REQUIRE(journal_result);
    auto journal = std::make_shared<OperationJournal>(std::move(*journal_result));
    auto pool = Pool::Make();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int additions{0};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });

    CopyOperationOrchestrator orchestrator{journal, pool, custodian};
    auto submitted =
        orchestrator.Submit(PhysicalCopyReview("physical-external-rejection", source, destination_directory));
    REQUIRE_FALSE(submitted);
    CHECK(submitted.error().code == CopyOperationOrchestratorErrorCode::ExecutionFactoryFailed);
    REQUIRE(submitted.error().reviewed_factory_error);
    CHECK(submitted.error().reviewed_factory_error->code ==
          ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable);
    CHECK(additions == 0);
    CHECK(pool->Empty());
    CHECK(custodian->PendingCount() == 0);
    CHECK_FALSE(std::filesystem::exists(destination));
    CHECK(ReadExactFile(source) == "physical-external-rejection\n");
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Failed);
    CHECK(snapshot[0].item_results.empty());
    pool->StopAndWaitForShutdown();
    REQUIRE(external_workspace.Cleanup());
    REQUIRE(journal_workspace.Cleanup());
}

} // namespace nc::ops
