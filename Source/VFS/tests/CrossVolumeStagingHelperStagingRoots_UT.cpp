// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "../../RoutedIO/source/CrossVolumeStagingHelperStagingRoots.h"
#include "../../RoutedIO/source/CrossVolumeStagingHelperPublicationBarrier.h"
#include "../../RoutedIO/source/CrossVolumeStagingHelperPublicationLifecycle.h"
#include "../../RoutedIO/source/CrossVolumeStagingHelperStagingSessionRunner.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <variant>

namespace nc::routedio::cross_volume_staging::helper {

class StagingRootAuthorityTestAccess final
{
public:
    [[nodiscard]] static std::expected<StagingRootAuthority::LockedSession, StagingRootAuthority::Error>
    Acquire(LeaseStore::TerminalLease _terminal_lease,
            const int _borrowed_source_root_fd,
            const int _borrowed_destination_root_fd) noexcept
    {
        return StagingRootAuthority::Acquire(
            std::move(_terminal_lease), _borrowed_source_root_fd, _borrowed_destination_root_fd);
    }

    [[nodiscard]] static bool IsCreatedByCurrentProcess(const StagingRootAuthority::LockedSession &_session) noexcept
    {
        return _session.IsCreatedByCurrentProcess();
    }
};

class StagingSessionRunnerTestAccess final
{
public:
    [[nodiscard]] static std::expected<DestinationStageWriter::SealedDestinationStage, StagingSessionRunner::Error>
    Run(StagingRootAuthority::LockedSession _session,
        const SourceSnapshotWriter::Cancellation _source_cancellation,
        const DestinationStageWriter::Cancellation _destination_cancellation) noexcept
    {
        return StagingSessionRunner::Run(std::move(_session), _source_cancellation, _destination_cancellation);
    }
};

class StagingPublicationBarrierTestAccess final
{
public:
    [[nodiscard]] static std::expected<StagingPublicationBarrier::PublicationPermit, StagingPublicationBarrier::Error>
    Prepare(DestinationStageWriter::SealedDestinationStage _stage,
            const StagingPublicationBarrier::Cancellation _cancellation) noexcept
    {
        return StagingPublicationBarrier::Prepare(std::move(_stage), _cancellation);
    }
};

class StagingPublicationLifecycleTestAccess final
{
public:
    [[nodiscard]] static std::expected<void, StagingPublicationLifecycle::Error>
    RecordStaged(ProtectedRootLedger &_source_root,
                 ProtectedRootLedger &_destination_root,
                 const DestinationStageWriter::SealedDestinationStage &_stage) noexcept
    {
        return StagingPublicationLifecycle::RecordStaged(_source_root, _destination_root, _stage);
    }

    [[nodiscard]] static std::expected<StagingPublicationLifecycle::Inspection, StagingPublicationLifecycle::Error>
    Inspect(const int _borrowed_source_root_fd, const int _borrowed_destination_root_fd, const Header &_header) noexcept
    {
        return StagingPublicationLifecycle::Inspect(_borrowed_source_root_fd, _borrowed_destination_root_fd, _header);
    }
};

} // namespace nc::routedio::cross_volume_staging::helper

#define PREFIX "RoutedIO cross-volume staging helper roots "

namespace CrossVolumeStagingHelperStagingRootsTests {

namespace protocol = nc::routedio::cross_volume_staging;
namespace codec = protocol::xpc_codec;
namespace helper = protocol::helper;

constexpr int kProcessTimeoutMilliseconds = 5'000;

class ScopedDictionary final
{
public:
    explicit ScopedDictionary(xpc_object_t _dictionary) noexcept : m_Dictionary{_dictionary} {}
    ScopedDictionary(const ScopedDictionary &) = delete;
    ScopedDictionary &operator=(const ScopedDictionary &) = delete;
    ~ScopedDictionary()
    {
        if( m_Dictionary != nullptr )
            xpc_release(m_Dictionary);
    }

    [[nodiscard]] xpc_object_t Get() const noexcept { return m_Dictionary; }

private:
    xpc_object_t m_Dictionary;
};

static protocol::Header Header(const uint8_t _correlation_byte)
{
    protocol::Header header;
    header.correlation[0] = _correlation_byte;
    return header;
}

static protocol::Timestamp TimestampFrom(const timespec &_value)
{
    return {.seconds = _value.tv_sec, .nanoseconds = static_cast<uint32_t>(_value.tv_nsec)};
}

static protocol::ObjectSeal SealFromStatus(const struct stat &_status) noexcept
{
    return {
        .device = static_cast<uint64_t>(_status.st_dev),
        .inode = static_cast<uint64_t>(_status.st_ino),
        .uid = static_cast<uint32_t>(_status.st_uid),
        .gid = static_cast<uint32_t>(_status.st_gid),
        .mode = static_cast<uint32_t>(_status.st_mode),
        .flags = static_cast<uint32_t>(_status.st_flags),
        .link_count = static_cast<uint64_t>(_status.st_nlink),
        .byte_size = static_cast<uint64_t>(_status.st_size),
        .birth_time = TimestampFrom(_status.st_birthtimespec),
        .modification_time = TimestampFrom(_status.st_mtimespec),
        .status_change_time = TimestampFrom(_status.st_ctimespec),
    };
}

static protocol::ObjectSeal SealFromFD(const int _fd)
{
    struct stat status{};
    REQUIRE(fstat(_fd, &status) == 0);
    return SealFromStatus(status);
}

static protocol::BeginRequest Request(const protocol::Header &_header, const int _source_fd, const int _parent_fd)
{
    constexpr std::array<uint8_t, 8> destination_name{'d', 'e', 's', 't', '.', 't', 'x', 't'};
    const auto component = protocol::DestinationComponent::Create(destination_name);
    REQUIRE(component);
    return {
        .header = _header,
        .source = SealFromFD(_source_fd),
        .destination_parent = SealFromFD(_parent_fd),
        .destination_name = *component,
    };
}

static helper::LeaseStore::TerminalLease
TakeCommittedTerminal(const protocol::BeginRequest &_request, const int _source_fd, const int _destination_parent_fd)
{
    helper::LeaseStore store;
    const auto encoded = codec::EncodeBegin(_request,
                                            {
                                                .source_fd = _source_fd,
                                                .destination_parent_fd = _destination_parent_fd,
                                            });
    REQUIRE(encoded);
    ScopedDictionary message{*encoded};
    auto decoded = codec::DecodeBegin(message.Get());
    REQUIRE(decoded);
    auto validated = helper::ValidateBeginDescriptors(std::move(*decoded));
    REQUIRE(validated);
    const auto granted = store.Grant(1, std::move(*validated));
    REQUIRE(granted);
    auto terminal = store.Take(1,
                               protocol::CommitRequest{
                                   .header = granted->header,
                                   .lease = *granted,
                               });
    REQUIRE(terminal);
    return std::move(*terminal);
}

static helper::LeaseStore::TerminalLease
TakeAbortedTerminal(const protocol::BeginRequest &_request, const int _source_fd, const int _destination_parent_fd)
{
    helper::LeaseStore store;
    const auto encoded = codec::EncodeBegin(_request,
                                            {
                                                .source_fd = _source_fd,
                                                .destination_parent_fd = _destination_parent_fd,
                                            });
    REQUIRE(encoded);
    ScopedDictionary message{*encoded};
    auto decoded = codec::DecodeBegin(message.Get());
    REQUIRE(decoded);
    auto validated = helper::ValidateBeginDescriptors(std::move(*decoded));
    REQUIRE(validated);
    const auto granted = store.Grant(1, std::move(*validated));
    REQUIRE(granted);
    auto terminal = store.Take(1,
                               protocol::AbortRequest{
                                   .header = granted->header,
                                   .lease = *granted,
                               });
    REQUIRE(terminal);
    return std::move(*terminal);
}

static int OpenProtectedRoot(const std::filesystem::path &_directory)
{
    REQUIRE(chmod(_directory.c_str(), 0700) == 0);
    const int fd = open(_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(fd >= 0);
    return fd;
}

static bool WriteExact(const int _fd, const void *_buffer, size_t _size) noexcept
{
    const auto *cursor = static_cast<const char *>(_buffer);
    while( _size != 0 ) {
        ssize_t written = -1;
        do {
            written = ::write(_fd, cursor, _size);
        } while( written < 0 && errno == EINTR );
        if( written <= 0 )
            return false;
        cursor += written;
        _size -= static_cast<size_t>(written);
    }
    return true;
}

static bool ReadExactWithin(const int _fd, void *_buffer, size_t _size) noexcept
{
    auto *cursor = static_cast<char *>(_buffer);
    while( _size != 0 ) {
        struct pollfd descriptor{.fd = _fd, .events = POLLIN, .revents = 0};
        int ready = -1;
        do {
            ready = ::poll(&descriptor, 1, kProcessTimeoutMilliseconds);
        } while( ready < 0 && errno == EINTR );
        if( ready <= 0 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0 )
            return false;
        ssize_t read = -1;
        do {
            read = ::read(_fd, cursor, _size);
        } while( read < 0 && errno == EINTR );
        if( read <= 0 )
            return false;
        cursor += read;
        _size -= static_cast<size_t>(read);
    }
    return true;
}

static bool WaitForChild(const pid_t _child, int &_status) noexcept
{
    for( ;; ) {
        const pid_t waited = ::waitpid(_child, &_status, 0);
        if( waited == _child )
            return true;
        if( waited < 0 && errno != EINTR )
            return false;
    }
}

class LocalFixture final
{
public:
    LocalFixture()
    {
        m_SourceParent = m_Workspace.directory / "source-parent";
        m_SourceRoot = m_Workspace.directory / "source-root";
        m_DestinationRoot = m_Workspace.directory / "destination-root";
        REQUIRE(std::filesystem::create_directory(m_SourceParent));
        REQUIRE(std::filesystem::create_directory(m_SourceRoot));
        REQUIRE(std::filesystem::create_directory(m_DestinationRoot));

        const auto source_path = m_SourceParent / "source";
        const int created = open(source_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        REQUIRE(created >= 0);
        constexpr std::string_view contents{"staging-root authority"};
        REQUIRE(WriteExact(created, contents.data(), contents.size()));
        REQUIRE(close(created) == 0);
        m_SourceFD = open(source_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        m_DestinationParentFD = open(m_Workspace.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        m_SourceRootFD = OpenProtectedRoot(m_SourceRoot);
        m_DestinationRootFD = OpenProtectedRoot(m_DestinationRoot);
        REQUIRE(m_SourceFD >= 0);
        REQUIRE(m_DestinationParentFD >= 0);
    }

    LocalFixture(const LocalFixture &) = delete;
    LocalFixture &operator=(const LocalFixture &) = delete;
    ~LocalFixture()
    {
        if( m_SourceFD >= 0 )
            (void)close(m_SourceFD);
        if( m_DestinationParentFD >= 0 )
            (void)close(m_DestinationParentFD);
        if( m_SourceRootFD >= 0 )
            (void)close(m_SourceRootFD);
        if( m_DestinationRootFD >= 0 )
            (void)close(m_DestinationRootFD);
    }

    [[nodiscard]] protocol::BeginRequest RequestFor(const uint8_t _correlation) const
    {
        return Request(Header(_correlation), m_SourceFD, m_DestinationParentFD);
    }
    [[nodiscard]] helper::LeaseStore::TerminalLease TakeCommitted(const uint8_t _correlation) const
    {
        return TakeCommittedTerminal(RequestFor(_correlation), m_SourceFD, m_DestinationParentFD);
    }
    [[nodiscard]] helper::LeaseStore::TerminalLease TakeAborted(const uint8_t _correlation) const
    {
        return TakeAbortedTerminal(RequestFor(_correlation), m_SourceFD, m_DestinationParentFD);
    }

    TestDir m_Workspace;
    std::filesystem::path m_SourceParent;
    std::filesystem::path m_SourceRoot;
    std::filesystem::path m_DestinationRoot;
    int m_SourceFD{-1};
    int m_DestinationParentFD{-1};
    int m_SourceRootFD{-1};
    int m_DestinationRootFD{-1};
};

class RetainedDestinationFixture final
{
public:
    explicit RetainedDestinationFixture(const std::filesystem::path &_base) : m_Base{std::filesystem::canonical(_base)}
    {
        std::string pattern = (m_Base / ".wc-staging-roots-ut.XXXXXX").string();
        std::vector<char> mutable_pattern{pattern.begin(), pattern.end()};
        mutable_pattern.push_back('\0');
        char *const created = ::mkdtemp(mutable_pattern.data());
        REQUIRE(created != nullptr);
        m_Directory = created;
    }

    RetainedDestinationFixture(const RetainedDestinationFixture &) = delete;
    RetainedDestinationFixture &operator=(const RetainedDestinationFixture &) = delete;

    [[nodiscard]] const std::filesystem::path &Directory() const noexcept { return m_Directory; }

private:
    std::filesystem::path m_Base;
    std::filesystem::path m_Directory;
};

enum class DeferredOpenResult : uint8_t {
    Opened = 1,
    RootBusy = 2,
    ForkedProcess = 3,
    OtherFailure = 4,
};

class DeferredRootOpen final
{
public:
    explicit DeferredRootOpen(const std::filesystem::path &_root)
    {
        int start_pipe[2]{};
        int result_pipe[2]{};
        REQUIRE(pipe(start_pipe) == 0);
        REQUIRE(pipe(result_pipe) == 0);
        m_Child = fork();
        if( m_Child == 0 ) {
            (void)close(start_pipe[1]);
            (void)close(result_pipe[0]);
            char start = '\0';
            ssize_t received = -1;
            do {
                received = ::read(start_pipe[0], &start, sizeof(start));
            } while( received < 0 && errno == EINTR );
            if( received != sizeof(start) || start != 'G' )
                _exit(20);
            (void)close(start_pipe[0]);

            const int root_fd = open(_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            DeferredOpenResult result = DeferredOpenResult::OtherFailure;
            if( root_fd >= 0 ) {
                const auto opened = helper::ProtectedRootLedger::Open(root_fd);
                (void)close(root_fd);
                if( opened )
                    result = DeferredOpenResult::Opened;
                else if( opened.error() == helper::ProtectedRootLedger::Error::RootBusy )
                    result = DeferredOpenResult::RootBusy;
                else if( opened.error() == helper::ProtectedRootLedger::Error::ForkedProcess )
                    result = DeferredOpenResult::ForkedProcess;
            }
            const auto byte = static_cast<uint8_t>(result);
            if( !WriteExact(result_pipe[1], &byte, sizeof(byte)) )
                _exit(21);
            (void)close(result_pipe[1]);
            _exit(0);
        }
        REQUIRE(m_Child > 0);
        REQUIRE(close(start_pipe[0]) == 0);
        REQUIRE(close(result_pipe[1]) == 0);
        m_StartFD = start_pipe[1];
        m_ResultFD = result_pipe[0];
    }

    DeferredRootOpen(const DeferredRootOpen &) = delete;
    DeferredRootOpen &operator=(const DeferredRootOpen &) = delete;
    ~DeferredRootOpen() { Cancel(); }

    [[nodiscard]] DeferredOpenResult Complete() noexcept
    {
        if( m_Child <= 0 )
            return DeferredOpenResult::OtherFailure;
        constexpr char go = 'G';
        const bool sent = WriteExact(m_StartFD, &go, sizeof(go));
        (void)close(m_StartFD);
        m_StartFD = -1;
        uint8_t byte = 0;
        const bool received = sent && ReadExactWithin(m_ResultFD, &byte, sizeof(byte));
        (void)close(m_ResultFD);
        m_ResultFD = -1;
        int status = 0;
        if( !received )
            (void)kill(m_Child, SIGKILL);
        const bool waited = WaitForChild(m_Child, status);
        m_Child = -1;
        if( !received || !waited || !WIFEXITED(status) || WEXITSTATUS(status) != 0 )
            return DeferredOpenResult::OtherFailure;
        return static_cast<DeferredOpenResult>(byte);
    }

private:
    void Cancel() noexcept
    {
        if( m_StartFD >= 0 )
            (void)close(m_StartFD);
        if( m_ResultFD >= 0 )
            (void)close(m_ResultFD);
        if( m_Child > 0 ) {
            (void)kill(m_Child, SIGKILL);
            int status = 0;
            (void)WaitForChild(m_Child, status);
        }
        m_StartFD = -1;
        m_ResultFD = -1;
        m_Child = -1;
    }

    pid_t m_Child{-1};
    int m_StartFD{-1};
    int m_ResultFD{-1};
};

struct CrossDeviceContext final {
    const protocol::BeginRequest &request;
    int source_fd{-1};
    int destination_parent_fd{-1};
    int source_root_fd{-1};
    int destination_root_fd{-1};
    const std::filesystem::path &source_root;
    const std::filesystem::path &destination_root;
};

static bool SupportsFullSync(const std::filesystem::path &_directory)
{
    const auto probe_path = _directory / "durability-probe";
    const int fd = open(probe_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if( fd < 0 )
        return false;
    const bool prepared = fchmod(fd, 0600) == 0 && fsync(fd) == 0 && fcntl(fd, F_FULLFSYNC) == 0;
    const bool closed = close(fd) == 0;
    const bool removed = unlink(probe_path.c_str()) == 0;
    return prepared && closed && removed;
}

static void RequireRootLocksReleased(const CrossDeviceContext &_context)
{
    auto reopened_source = helper::ProtectedRootLedger::Open(_context.source_root_fd);
    REQUIRE(reopened_source);
    auto source_ledger = std::move(*reopened_source);
    auto reopened_destination = helper::ProtectedRootLedger::Open(_context.destination_root_fd);
    REQUIRE(reopened_destination);
    auto destination_ledger = std::move(*reopened_destination);
}

static std::vector<std::string> DirectoryEntryNames(const std::filesystem::path &_directory)
{
    std::vector<std::string> names;
    for( const auto &entry : std::filesystem::directory_iterator{_directory} )
        names.emplace_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

struct DirectoryEntrySnapshot final {
    std::string name;
    protocol::ObjectSeal seal;

    bool operator==(const DirectoryEntrySnapshot &) const noexcept = default;
};

static std::vector<DirectoryEntrySnapshot> SnapshotDirectoryEntries(const std::filesystem::path &_directory)
{
    const int directory_fd = open(_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(directory_fd >= 0);
    std::vector<DirectoryEntrySnapshot> entries;
    for( const auto &entry : std::filesystem::directory_iterator{_directory} ) {
        const std::string name = entry.path().filename().string();
        struct stat status{};
        REQUIRE(fstatat(directory_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0);
        entries.push_back({.name = name, .seal = SealFromStatus(status)});
    }
    REQUIRE(close(directory_fd) == 0);
    std::sort(
        entries.begin(), entries.end(), [](const auto &_left, const auto &_right) { return _left.name < _right.name; });
    return entries;
}

struct RootLockProbeContext final {
    DeferredRootOpen *source_contender{nullptr};
    DeferredRootOpen *destination_contender{nullptr};
    bool called{false};
    bool observed_source_root_busy{false};
    bool observed_destination_root_busy{false};
};

static bool ObserveSourceRootLockAtBeforeReservation(const helper::SourceSnapshotWriter::CancellationPoint _point,
                                                     void *_context) noexcept
{
    if( _point != helper::SourceSnapshotWriter::CancellationPoint::BeforeReservation )
        return false;
    auto &context = *static_cast<RootLockProbeContext *>(_context);
    context.called = true;
    context.observed_source_root_busy =
        context.source_contender != nullptr && context.source_contender->Complete() == DeferredOpenResult::RootBusy;
    context.observed_destination_root_busy = context.destination_contender != nullptr &&
                                             context.destination_contender->Complete() == DeferredOpenResult::RootBusy;
    return false;
}

static bool
ObserveDestinationRootLockAtBeforeReservation(const helper::DestinationStageWriter::CancellationPoint _point,
                                              void *_context) noexcept
{
    if( _point != helper::DestinationStageWriter::CancellationPoint::BeforeReservation )
        return false;
    auto &context = *static_cast<RootLockProbeContext *>(_context);
    context.called = true;
    context.observed_source_root_busy =
        context.source_contender != nullptr && context.source_contender->Complete() == DeferredOpenResult::RootBusy;
    context.observed_destination_root_busy = context.destination_contender != nullptr &&
                                             context.destination_contender->Complete() == DeferredOpenResult::RootBusy;
    return false;
}

struct SourceCancellationContext final {
    bool called{false};
};

static bool CancelSourceAtBeforeReservation(const helper::SourceSnapshotWriter::CancellationPoint _point,
                                            void *_context) noexcept
{
    if( _point != helper::SourceSnapshotWriter::CancellationPoint::BeforeReservation )
        return false;
    static_cast<SourceCancellationContext *>(_context)->called = true;
    return true;
}

struct DestinationCancellationContext final {
    bool called{false};
};

static bool CancelDestinationAtBeforeReservation(const helper::DestinationStageWriter::CancellationPoint _point,
                                                 void *_context) noexcept
{
    if( _point != helper::DestinationStageWriter::CancellationPoint::BeforeReservation )
        return false;
    static_cast<DestinationCancellationContext *>(_context)->called = true;
    return true;
}

struct CallbackProbeContext final {
    bool called{false};
};

static bool ObserveSourceCallback(const helper::SourceSnapshotWriter::CancellationPoint, void *_context) noexcept
{
    static_cast<CallbackProbeContext *>(_context)->called = true;
    return false;
}

static bool ObserveDestinationCallback(const helper::DestinationStageWriter::CancellationPoint, void *_context) noexcept
{
    static_cast<CallbackProbeContext *>(_context)->called = true;
    return false;
}

static void VerifyRunnerRejectsInheritedSession(const CrossDeviceContext &_context)
{
    {
        auto session = helper::StagingRootAuthorityTestAccess::Acquire(
            TakeCommittedTerminal(_context.request, _context.source_fd, _context.destination_parent_fd),
            _context.source_root_fd,
            _context.destination_root_fd);
        REQUIRE(session);

        int result_pipe[2]{};
        REQUIRE(pipe(result_pipe) == 0);
        const pid_t child = fork();
        if( child == 0 ) {
            (void)close(result_pipe[0]);
            CallbackProbeContext source_probe;
            CallbackProbeContext destination_probe;
            const auto result = helper::StagingSessionRunnerTestAccess::Run(
                std::move(*session),
                {.probe = ObserveSourceCallback, .context = &source_probe},
                {.probe = ObserveDestinationCallback, .context = &destination_probe});
            const bool rejected_before_callbacks =
                !result && result.error().phase == helper::StagingSessionRunner::Phase::Session &&
                std::holds_alternative<helper::StagingSessionRunner::SessionError>(result.error().cause) &&
                std::get<helper::StagingSessionRunner::SessionError>(result.error().cause) ==
                    helper::StagingSessionRunner::SessionError::ForkedProcess &&
                !source_probe.called && !destination_probe.called;
            const uint8_t value = rejected_before_callbacks ? 1 : 0;
            (void)WriteExact(result_pipe[1], &value, sizeof(value));
            (void)close(result_pipe[1]);
            _exit(rejected_before_callbacks ? 0 : 1);
        }
        REQUIRE(child > 0);
        REQUIRE(close(result_pipe[1]) == 0);
        uint8_t result = 0;
        REQUIRE(ReadExactWithin(result_pipe[0], &result, sizeof(result)));
        REQUIRE(close(result_pipe[0]) == 0);
        int status = 0;
        REQUIRE(WaitForChild(child, status));
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
        CHECK(result == 1);
    }
    RequireRootLocksReleased(_context);
}

static void VerifyRunnerSuccessAndSameProcessReuse(const CrossDeviceContext &_context,
                                                   const std::filesystem::path &_destination_parent)
{
    DeferredRootOpen source_phase_source_contender{_context.source_root};
    DeferredRootOpen source_phase_destination_contender{_context.destination_root};
    DeferredRootOpen destination_phase_source_contender{_context.source_root};
    DeferredRootOpen destination_phase_destination_contender{_context.destination_root};
    auto session = helper::StagingRootAuthorityTestAccess::Acquire(
        TakeCommittedTerminal(_context.request, _context.source_fd, _context.destination_parent_fd),
        _context.source_root_fd,
        _context.destination_root_fd);
    REQUIRE(session);

    RootLockProbeContext source_lock_probe{
        .source_contender = &source_phase_source_contender,
        .destination_contender = &source_phase_destination_contender,
    };
    RootLockProbeContext destination_lock_probe{
        .source_contender = &destination_phase_source_contender,
        .destination_contender = &destination_phase_destination_contender,
    };
    const auto stage = helper::StagingSessionRunnerTestAccess::Run(
        std::move(*session),
        {.probe = ObserveSourceRootLockAtBeforeReservation, .context = &source_lock_probe},
        {.probe = ObserveDestinationRootLockAtBeforeReservation, .context = &destination_lock_probe});
    REQUIRE(stage);
    CHECK(source_lock_probe.called);
    CHECK(source_lock_probe.observed_source_root_busy);
    CHECK(source_lock_probe.observed_destination_root_busy);
    CHECK(destination_lock_probe.called);
    CHECK(destination_lock_probe.observed_source_root_busy);
    CHECK(destination_lock_probe.observed_destination_root_busy);
    CHECK(stage->Correlation() == _context.request.header);
    CHECK(stage->SourceSeal() == _context.request.source);
    CHECK(stage->SourceSnapshotSeal().byte_size == _context.request.source.byte_size);
    CHECK(stage->StageSeal().byte_size == _context.request.source.byte_size);
    CHECK(stage->StageSeal().device == _context.request.destination_parent.device);
    CHECK_FALSE(std::filesystem::is_empty(_context.source_root));
    CHECK_FALSE(std::filesystem::is_empty(_context.destination_root));
    CHECK_FALSE(std::filesystem::exists(_destination_parent / "dest.txt"));

    const auto source_root_before_inspection = SnapshotDirectoryEntries(_context.source_root);
    const auto destination_root_before_inspection = SnapshotDirectoryEntries(_context.destination_root);
    const auto destination_parent_before_inspection = DirectoryEntryNames(_destination_parent);
    const auto inspection = helper::StagingPublicationLifecycleTestAccess::Inspect(
        _context.source_root_fd, _context.destination_root_fd, _context.request.header);
    REQUIRE(inspection);
    CHECK(inspection->state == helper::StagingPublicationLifecycle::State::ExactPending);
    CHECK(SnapshotDirectoryEntries(_context.source_root) == source_root_before_inspection);
    CHECK(SnapshotDirectoryEntries(_context.destination_root) == destination_root_before_inspection);
    CHECK(DirectoryEntryNames(_destination_parent) == destination_parent_before_inspection);
    CHECK_FALSE(std::filesystem::exists(_destination_parent / "dest.txt"));

    int lifecycle_fork_pipe[2]{};
    REQUIRE(pipe(lifecycle_fork_pipe) == 0);
    const pid_t lifecycle_child = fork();
    if( lifecycle_child == 0 ) {
        (void)close(lifecycle_fork_pipe[0]);
        bool rejected = false;
        auto source_root = helper::ProtectedRootLedger::Open(_context.source_root_fd);
        auto destination_root = helper::ProtectedRootLedger::Open(_context.destination_root_fd);
        if( source_root && destination_root ) {
            const auto recorded =
                helper::StagingPublicationLifecycleTestAccess::RecordStaged(*source_root, *destination_root, *stage);
            rejected = !recorded && recorded.error() == helper::StagingPublicationLifecycle::Error::ForkedProcess;
        }
        const uint8_t value = rejected ? 1 : 0;
        (void)WriteExact(lifecycle_fork_pipe[1], &value, sizeof(value));
        (void)close(lifecycle_fork_pipe[1]);
        _exit(rejected ? 0 : 1);
    }
    REQUIRE(lifecycle_child > 0);
    REQUIRE(close(lifecycle_fork_pipe[1]) == 0);
    uint8_t lifecycle_fork_result = 0;
    REQUIRE(ReadExactWithin(lifecycle_fork_pipe[0], &lifecycle_fork_result, sizeof(lifecycle_fork_result)));
    REQUIRE(close(lifecycle_fork_pipe[0]) == 0);
    int lifecycle_fork_status = 0;
    REQUIRE(WaitForChild(lifecycle_child, lifecycle_fork_status));
    CHECK(WIFEXITED(lifecycle_fork_status));
    CHECK(WEXITSTATUS(lifecycle_fork_status) == 0);
    CHECK(lifecycle_fork_result == 1);
    CHECK(SnapshotDirectoryEntries(_context.source_root) == source_root_before_inspection);
    CHECK(SnapshotDirectoryEntries(_context.destination_root) == destination_root_before_inspection);
    CHECK(DirectoryEntryNames(_destination_parent) == destination_parent_before_inspection);

    CallbackProbeContext reuse_source_probe;
    CallbackProbeContext reuse_destination_probe;
    const auto reused = helper::StagingSessionRunnerTestAccess::Run(
        std::move(*session),
        {.probe = ObserveSourceCallback, .context = &reuse_source_probe},
        {.probe = ObserveDestinationCallback, .context = &reuse_destination_probe});
    REQUIRE_FALSE(reused);
    CHECK(reused.error().phase == helper::StagingSessionRunner::Phase::Session);
    REQUIRE(std::holds_alternative<helper::StagingSessionRunner::SessionError>(reused.error().cause));
    CHECK(std::get<helper::StagingSessionRunner::SessionError>(reused.error().cause) ==
          helper::StagingSessionRunner::SessionError::InvalidSession);
    CHECK_FALSE(reuse_source_probe.called);
    CHECK_FALSE(reuse_destination_probe.called);

    RequireRootLocksReleased(_context);
}

static void VerifyRunnerSourceCancellation(const CrossDeviceContext &_context)
{
    DeferredRootOpen source_contender{_context.source_root};
    DeferredRootOpen destination_contender{_context.destination_root};
    auto session = helper::StagingRootAuthorityTestAccess::Acquire(
        TakeCommittedTerminal(_context.request, _context.source_fd, _context.destination_parent_fd),
        _context.source_root_fd,
        _context.destination_root_fd);
    REQUIRE(session);
    RootLockProbeContext lock_probe{
        .source_contender = &source_contender,
        .destination_contender = &destination_contender,
    };
    SourceCancellationContext cancellation_context;
    // The callback completes the two contenders before requesting cancellation, proving both root locks remain held
    // even at the earliest source-writer checkpoint.
    const auto source_cancellation = helper::SourceSnapshotWriter::Cancellation{
        .probe =
            [](const helper::SourceSnapshotWriter::CancellationPoint point, void *context) noexcept {
                auto &pair = *static_cast<std::pair<RootLockProbeContext *, SourceCancellationContext *> *>(context);
                (void)ObserveSourceRootLockAtBeforeReservation(point, pair.first);
                return CancelSourceAtBeforeReservation(point, pair.second);
            },
        .context = nullptr,
    };
    std::pair<RootLockProbeContext *, SourceCancellationContext *> cancellation_context_pair{&lock_probe,
                                                                                             &cancellation_context};
    const auto failed = helper::StagingSessionRunnerTestAccess::Run(
        std::move(*session), {.probe = source_cancellation.probe, .context = &cancellation_context_pair}, {});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().phase == helper::StagingSessionRunner::Phase::SourceSnapshot);
    REQUIRE(std::holds_alternative<helper::SourceSnapshotWriter::Error>(failed.error().cause));
    CHECK(std::get<helper::SourceSnapshotWriter::Error>(failed.error().cause) ==
          helper::SourceSnapshotWriter::Error::Cancelled);
    CHECK(cancellation_context.called);
    CHECK(lock_probe.called);
    CHECK(lock_probe.observed_source_root_busy);
    CHECK(lock_probe.observed_destination_root_busy);
    CHECK(std::filesystem::is_empty(_context.source_root));
    CHECK(std::filesystem::is_empty(_context.destination_root));
    RequireRootLocksReleased(_context);
}

static void VerifyRunnerDestinationCancellation(const CrossDeviceContext &_context)
{
    DeferredRootOpen source_contender{_context.source_root};
    DeferredRootOpen destination_contender{_context.destination_root};
    auto session = helper::StagingRootAuthorityTestAccess::Acquire(
        TakeCommittedTerminal(_context.request, _context.source_fd, _context.destination_parent_fd),
        _context.source_root_fd,
        _context.destination_root_fd);
    REQUIRE(session);
    RootLockProbeContext lock_probe{
        .source_contender = &source_contender,
        .destination_contender = &destination_contender,
    };
    DestinationCancellationContext cancellation_context;
    const auto destination_cancellation = helper::DestinationStageWriter::Cancellation{
        .probe =
            [](const helper::DestinationStageWriter::CancellationPoint point, void *context) noexcept {
                auto &pair =
                    *static_cast<std::pair<RootLockProbeContext *, DestinationCancellationContext *> *>(context);
                (void)ObserveDestinationRootLockAtBeforeReservation(point, pair.first);
                return CancelDestinationAtBeforeReservation(point, pair.second);
            },
        .context = nullptr,
    };
    std::pair<RootLockProbeContext *, DestinationCancellationContext *> cancellation_context_pair{
        &lock_probe, &cancellation_context};
    const auto failed = helper::StagingSessionRunnerTestAccess::Run(
        std::move(*session), {}, {.probe = destination_cancellation.probe, .context = &cancellation_context_pair});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().phase == helper::StagingSessionRunner::Phase::DestinationStage);
    REQUIRE(std::holds_alternative<helper::DestinationStageWriter::Error>(failed.error().cause));
    CHECK(std::get<helper::DestinationStageWriter::Error>(failed.error().cause) ==
          helper::DestinationStageWriter::Error::Cancelled);
    CHECK(cancellation_context.called);
    CHECK(lock_probe.called);
    CHECK(lock_probe.observed_source_root_busy);
    CHECK(lock_probe.observed_destination_root_busy);
    CHECK_FALSE(std::filesystem::is_empty(_context.source_root));
    CHECK(std::filesystem::is_empty(_context.destination_root));
    RequireRootLocksReleased(_context);
}

static void RunStagingSessionRunnerFixture(const std::filesystem::path &_destination_base)
{
    RetainedDestinationFixture destination_fixture{_destination_base};
    const auto destination_parent = destination_fixture.Directory() / "destination-parent";
    const auto success_destination_root = destination_fixture.Directory() / "success-destination-root";
    const auto source_cancellation_destination_root =
        destination_fixture.Directory() / "source-cancellation-destination-root";
    const auto destination_cancellation_destination_root =
        destination_fixture.Directory() / "destination-cancellation-destination-root";
    REQUIRE(std::filesystem::create_directory(destination_parent));
    REQUIRE(std::filesystem::create_directory(success_destination_root));
    REQUIRE(std::filesystem::create_directory(source_cancellation_destination_root));
    REQUIRE(std::filesystem::create_directory(destination_cancellation_destination_root));

    TestDir source_workspace;
    const auto source_parent = source_workspace.directory / "source-parent";
    const auto success_source_root = source_workspace.directory / "success-source-root";
    const auto source_cancellation_source_root = source_workspace.directory / "source-cancellation-source-root";
    const auto destination_cancellation_source_root =
        source_workspace.directory / "destination-cancellation-source-root";
    REQUIRE(std::filesystem::create_directory(source_parent));
    REQUIRE(std::filesystem::create_directory(success_source_root));
    REQUIRE(std::filesystem::create_directory(source_cancellation_source_root));
    REQUIRE(std::filesystem::create_directory(destination_cancellation_source_root));

    const auto source_path = source_parent / "source";
    const int created = open(source_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    REQUIRE(created >= 0);
    std::vector<uint8_t> payload(32 * 1024 + 37);
    for( size_t index = 0; index != payload.size(); ++index )
        payload[index] = static_cast<uint8_t>((index * 13) & 0xFF);
    payload[17] = 0;
    payload[16384] = 0xFF;
    REQUIRE(WriteExact(created, payload.data(), payload.size()));
    REQUIRE(close(created) == 0);

    const int source_fd = open(source_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    const int destination_parent_fd = open(destination_parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(source_fd >= 0);
    REQUIRE(destination_parent_fd >= 0);
    const auto success_request = Request(Header(50), source_fd, destination_parent_fd);
    if( success_request.source.device == success_request.destination_parent.device )
        SKIP("supplied destination root must be on a distinct device");
    if( !SupportsFullSync(success_source_root) || !SupportsFullSync(success_destination_root) ||
        !SupportsFullSync(destination_cancellation_source_root) )
        SKIP("source or destination fixture does not support F_FULLFSYNC");

    const int success_source_root_fd = OpenProtectedRoot(success_source_root);
    const int success_destination_root_fd = OpenProtectedRoot(success_destination_root);
    const int source_cancellation_source_root_fd = OpenProtectedRoot(source_cancellation_source_root);
    const int source_cancellation_destination_root_fd = OpenProtectedRoot(source_cancellation_destination_root);
    const int destination_cancellation_source_root_fd = OpenProtectedRoot(destination_cancellation_source_root);
    const int destination_cancellation_destination_root_fd =
        OpenProtectedRoot(destination_cancellation_destination_root);

    const CrossDeviceContext success_context{
        .request = success_request,
        .source_fd = source_fd,
        .destination_parent_fd = destination_parent_fd,
        .source_root_fd = success_source_root_fd,
        .destination_root_fd = success_destination_root_fd,
        .source_root = success_source_root,
        .destination_root = success_destination_root,
    };
    VerifyRunnerRejectsInheritedSession(success_context);
    VerifyRunnerSuccessAndSameProcessReuse(success_context, destination_parent);

    const auto source_cancellation_request = Request(Header(51), source_fd, destination_parent_fd);
    const CrossDeviceContext source_cancellation_context{
        .request = source_cancellation_request,
        .source_fd = source_fd,
        .destination_parent_fd = destination_parent_fd,
        .source_root_fd = source_cancellation_source_root_fd,
        .destination_root_fd = source_cancellation_destination_root_fd,
        .source_root = source_cancellation_source_root,
        .destination_root = source_cancellation_destination_root,
    };
    VerifyRunnerSourceCancellation(source_cancellation_context);

    const auto destination_cancellation_request = Request(Header(52), source_fd, destination_parent_fd);
    const CrossDeviceContext destination_cancellation_context{
        .request = destination_cancellation_request,
        .source_fd = source_fd,
        .destination_parent_fd = destination_parent_fd,
        .source_root_fd = destination_cancellation_source_root_fd,
        .destination_root_fd = destination_cancellation_destination_root_fd,
        .source_root = destination_cancellation_source_root,
        .destination_root = destination_cancellation_destination_root,
    };
    VerifyRunnerDestinationCancellation(destination_cancellation_context);

    CHECK_FALSE(std::filesystem::exists(destination_parent / "dest.txt"));
    CHECK(close(destination_cancellation_destination_root_fd) == 0);
    CHECK(close(destination_cancellation_source_root_fd) == 0);
    CHECK(close(source_cancellation_destination_root_fd) == 0);
    CHECK(close(source_cancellation_source_root_fd) == 0);
    CHECK(close(success_destination_root_fd) == 0);
    CHECK(close(success_source_root_fd) == 0);
    CHECK(close(destination_parent_fd) == 0);
    CHECK(close(source_fd) == 0);
}

class PublicationBarrierCase final
{
public:
    PublicationBarrierCase(TestDir &_source_workspace,
                           RetainedDestinationFixture &_destination_fixture,
                           const std::string_view _name,
                           const uint8_t _correlation)
    {
        const std::string prefix = "publication-barrier-" + std::string{_name};
        m_SourceParent = _source_workspace.directory / (prefix + "-source-parent");
        m_SourceRoot = _source_workspace.directory / (prefix + "-source-root");
        m_DestinationParent = _destination_fixture.Directory() / (prefix + "-destination-parent");
        m_DestinationRoot = _destination_fixture.Directory() / (prefix + "-destination-root");
        REQUIRE(std::filesystem::create_directory(m_SourceParent));
        REQUIRE(std::filesystem::create_directory(m_SourceRoot));
        REQUIRE(std::filesystem::create_directory(m_DestinationParent));
        REQUIRE(std::filesystem::create_directory(m_DestinationRoot));

        m_SourcePath = m_SourceParent / "source";
        const int created = open(m_SourcePath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        REQUIRE(created >= 0);
        std::array<uint8_t, 37> payload{};
        for( size_t index = 0; index != payload.size(); ++index )
            payload[index] = static_cast<uint8_t>((index * 29) & 0xFF);
        payload[7] = 0;
        payload[23] = 0xFF;
        REQUIRE(WriteExact(created, payload.data(), payload.size()));
        REQUIRE(close(created) == 0);

        m_SourceFD = open(m_SourcePath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        m_DestinationParentFD = open(m_DestinationParent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        REQUIRE(m_SourceFD >= 0);
        REQUIRE(m_DestinationParentFD >= 0);
        m_Request = Request(Header(_correlation), m_SourceFD, m_DestinationParentFD);
        m_SourceRootFD = OpenProtectedRoot(m_SourceRoot);
        m_DestinationRootFD = OpenProtectedRoot(m_DestinationRoot);
    }

    PublicationBarrierCase(const PublicationBarrierCase &) = delete;
    PublicationBarrierCase &operator=(const PublicationBarrierCase &) = delete;
    ~PublicationBarrierCase()
    {
        if( m_DestinationRootFD >= 0 )
            (void)close(m_DestinationRootFD);
        if( m_SourceRootFD >= 0 )
            (void)close(m_SourceRootFD);
        if( m_DestinationParentFD >= 0 )
            (void)close(m_DestinationParentFD);
        if( m_SourceFD >= 0 )
            (void)close(m_SourceFD);
    }

    [[nodiscard]] CrossDeviceContext Context() const noexcept
    {
        return {
            .request = m_Request,
            .source_fd = m_SourceFD,
            .destination_parent_fd = m_DestinationParentFD,
            .source_root_fd = m_SourceRootFD,
            .destination_root_fd = m_DestinationRootFD,
            .source_root = m_SourceRoot,
            .destination_root = m_DestinationRoot,
        };
    }

    [[nodiscard]] const std::filesystem::path &SourcePath() const noexcept { return m_SourcePath; }
    [[nodiscard]] const std::filesystem::path &DestinationParent() const noexcept { return m_DestinationParent; }
    [[nodiscard]] const std::filesystem::path &DestinationRoot() const noexcept { return m_DestinationRoot; }
    [[nodiscard]] int DestinationParentFD() const noexcept { return m_DestinationParentFD; }
    [[nodiscard]] int DestinationRootFD() const noexcept { return m_DestinationRootFD; }

private:
    protocol::BeginRequest m_Request;
    std::filesystem::path m_SourceParent;
    std::filesystem::path m_SourceRoot;
    std::filesystem::path m_DestinationParent;
    std::filesystem::path m_DestinationRoot;
    std::filesystem::path m_SourcePath;
    int m_SourceFD{-1};
    int m_DestinationParentFD{-1};
    int m_SourceRootFD{-1};
    int m_DestinationRootFD{-1};
};

static helper::DestinationStageWriter::SealedDestinationStage MakeStagedDestination(const CrossDeviceContext &_context)
{
    auto session = helper::StagingRootAuthorityTestAccess::Acquire(
        TakeCommittedTerminal(_context.request, _context.source_fd, _context.destination_parent_fd),
        _context.source_root_fd,
        _context.destination_root_fd);
    REQUIRE(session);
    auto stage = helper::StagingSessionRunnerTestAccess::Run(std::move(*session), {}, {});
    REQUIRE(stage);
    return std::move(*stage);
}

static void CheckDestinationParentEmpty(const PublicationBarrierCase &_case)
{
    CHECK(DirectoryEntryNames(_case.DestinationParent()).empty());
}

static std::filesystem::path SingleRootEntry(const std::filesystem::path &_root, const std::string_view _prefix)
{
    std::filesystem::path result;
    for( const auto &entry : std::filesystem::directory_iterator{_root} ) {
        if( entry.path().filename().string().starts_with(_prefix) ) {
            REQUIRE(result.empty());
            result = entry.path();
        }
    }
    REQUIRE_FALSE(result.empty());
    return result;
}

using BarrierAction = bool (*)(void *) noexcept;

struct BarrierCallbackContext final {
    BarrierAction action{nullptr};
    void *action_context{nullptr};
    bool cancel{false};
    bool called{false};
    bool action_succeeded{false};
};

static bool InvokeBarrierCallback(const helper::StagingPublicationBarrier::CancellationPoint _point,
                                  void *_context) noexcept
{
    if( _point != helper::StagingPublicationBarrier::CancellationPoint::BeforePublication )
        return false;
    auto &context = *static_cast<BarrierCallbackContext *>(_context);
    context.called = true;
    context.action_succeeded = context.action == nullptr || context.action(context.action_context);
    return context.cancel;
}

struct RootContenderActionContext final {
    DeferredRootOpen *contender{nullptr};
    bool completed{false};
    bool observed_root_busy{false};
};

static bool CompleteRootContender(void *_context) noexcept
{
    auto &context = *static_cast<RootContenderActionContext *>(_context);
    context.completed = context.contender != nullptr;
    context.observed_root_busy = context.completed && context.contender->Complete() == DeferredOpenResult::RootBusy;
    return context.observed_root_busy;
}

static helper::StagingPublicationBarrier::Cancellation BarrierCancellation(BarrierCallbackContext &_context) noexcept
{
    return {.probe = InvokeBarrierCallback, .context = &_context};
}

struct SourceMutationActionContext final {
    const std::filesystem::path *path{nullptr};
    bool called{false};
    bool succeeded{false};
};

static bool MutateSourceAfterBarrierValidation(void *_context) noexcept
{
    auto &context = *static_cast<SourceMutationActionContext *>(_context);
    context.called = true;
    if( context.path == nullptr )
        return false;
    const int fd = open(context.path->c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if( fd < 0 )
        return false;
    constexpr char marker = '!';
    const bool written = pwrite(fd, &marker, sizeof(marker), 0) == sizeof(marker);
    const bool closed = close(fd) == 0;
    context.succeeded = written && closed;
    return context.succeeded;
}

struct ParentModeMutationActionContext final {
    int fd{-1};
    mode_t original_mode{0};
    bool called{false};
    bool succeeded{false};
};

static bool MutateDestinationParentModeAfterBarrierValidation(void *_context) noexcept
{
    auto &context = *static_cast<ParentModeMutationActionContext *>(_context);
    context.called = true;
    struct stat status{};
    if( context.fd < 0 || fstat(context.fd, &status) != 0 )
        return false;
    context.original_mode = status.st_mode & 07777;
    context.succeeded = fchmod(context.fd, context.original_mode ^ S_IXUSR) == 0;
    return context.succeeded;
}

struct FileMutationActionContext final {
    const std::filesystem::path *path{nullptr};
    bool called{false};
    bool succeeded{false};
};

static bool MutatePrivateFileAfterBarrierValidation(void *_context) noexcept
{
    auto &context = *static_cast<FileMutationActionContext *>(_context);
    context.called = true;
    if( context.path == nullptr )
        return false;
    const int fd = open(context.path->c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if( fd < 0 )
        return false;
    constexpr char marker = '#';
    const bool written = pwrite(fd, &marker, sizeof(marker), 0) == sizeof(marker);
    const bool closed = close(fd) == 0;
    context.succeeded = written && closed;
    return context.succeeded;
}

struct RootModeMutationActionContext final {
    int fd{-1};
    mode_t original_mode{0};
    bool called{false};
    bool succeeded{false};
};

static bool MutateRootModeAfterBarrierValidation(void *_context) noexcept
{
    auto &context = *static_cast<RootModeMutationActionContext *>(_context);
    context.called = true;
    struct stat status{};
    if( context.fd < 0 || fstat(context.fd, &status) != 0 )
        return false;
    context.original_mode = status.st_mode & 07777;
    context.succeeded = fchmod(context.fd, 0750) == 0;
    return context.succeeded;
}

struct UnlinkActionContext final {
    const std::filesystem::path *path{nullptr};
    bool called{false};
    bool succeeded{false};
};

static bool UnlinkPrivateFileAfterBarrierValidation(void *_context) noexcept
{
    auto &context = *static_cast<UnlinkActionContext *>(_context);
    context.called = true;
    context.succeeded = context.path != nullptr && unlink(context.path->c_str()) == 0;
    return context.succeeded;
}

struct DestinationSentinelActionContext final {
    int parent_fd{-1};
    bool called{false};
    bool succeeded{false};
};

static bool CreateDestinationSentinelAfterBarrierValidation(void *_context) noexcept
{
    auto &context = *static_cast<DestinationSentinelActionContext *>(_context);
    context.called = true;
    constexpr std::string_view name{"dest.txt"};
    constexpr std::string_view contents{"barrier-sentinel"};
    const int fd = openat(context.parent_fd, name.data(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if( fd < 0 )
        return false;
    const bool written = WriteExact(fd, contents.data(), contents.size());
    const bool closed = close(fd) == 0;
    context.succeeded = written && closed;
    return context.succeeded;
}

static bool HasExactDestinationSentinel(const std::filesystem::path &_parent)
{
    const auto path = _parent / "dest.txt";
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if( fd < 0 )
        return false;
    constexpr std::string_view contents{"barrier-sentinel"};
    std::array<char, contents.size()> read{};
    const ssize_t received = ::read(fd, read.data(), read.size());
    const bool exact =
        received == static_cast<ssize_t>(read.size()) && std::string_view{read.data(), read.size()} == contents;
    return close(fd) == 0 && exact;
}

static void VerifyPublicationBarrierPermit(const PublicationBarrierCase &_case)
{
    const auto context = _case.Context();
    auto stage = MakeStagedDestination(context);
    const auto root_entries_before = SnapshotDirectoryEntries(_case.DestinationRoot());
    DeferredRootOpen callback_contender{context.destination_root};
    DeferredRootOpen permit_contender{context.destination_root};
    RootContenderActionContext contender_action{.contender = &callback_contender};
    BarrierCallbackContext callback{
        .action = CompleteRootContender,
        .action_context = &contender_action,
    };

    {
        const auto permit =
            helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(callback));
        REQUIRE(permit);
        CHECK(callback.called);
        CHECK(callback.action_succeeded);
        CHECK(contender_action.completed);
        CHECK(contender_action.observed_root_busy);
        CHECK(permit_contender.Complete() == DeferredOpenResult::RootBusy);
        CHECK(SnapshotDirectoryEntries(_case.DestinationRoot()) == root_entries_before);
        CheckDestinationParentEmpty(_case);
    }
    RequireRootLocksReleased(context);
}

static void VerifyPublicationBarrierCancellation(const PublicationBarrierCase &_case)
{
    const auto context = _case.Context();
    auto stage = MakeStagedDestination(context);
    DeferredRootOpen callback_contender{context.destination_root};
    RootContenderActionContext contender_action{.contender = &callback_contender};
    BarrierCallbackContext callback{
        .action = CompleteRootContender,
        .action_context = &contender_action,
        .cancel = true,
    };
    const auto cancelled =
        helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(callback));
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error() == helper::StagingPublicationBarrier::Error::Cancelled);
    CHECK(callback.called);
    CHECK(callback.action_succeeded);
    CHECK(contender_action.completed);
    CHECK(contender_action.observed_root_busy);
    CheckDestinationParentEmpty(_case);
    RequireRootLocksReleased(context);
}

static void VerifyPublicationBarrierSourceRevalidation(const PublicationBarrierCase &_case)
{
    const auto context = _case.Context();
    auto stage = MakeStagedDestination(context);
    SourceMutationActionContext mutation{.path = &_case.SourcePath()};
    BarrierCallbackContext callback{
        .action = MutateSourceAfterBarrierValidation,
        .action_context = &mutation,
    };
    const auto failed =
        helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(callback));
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == helper::StagingPublicationBarrier::Error::SourceStale);
    CHECK(callback.called);
    CHECK(callback.action_succeeded);
    CHECK(mutation.called);
    CHECK(mutation.succeeded);
    CheckDestinationParentEmpty(_case);
    RequireRootLocksReleased(context);
}

static void VerifyPublicationBarrierParentRevalidation(const PublicationBarrierCase &_case)
{
    const auto context = _case.Context();
    auto stage = MakeStagedDestination(context);
    ParentModeMutationActionContext mutation{.fd = _case.DestinationParentFD()};
    BarrierCallbackContext callback{
        .action = MutateDestinationParentModeAfterBarrierValidation,
        .action_context = &mutation,
    };
    const auto failed =
        helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(callback));
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == helper::StagingPublicationBarrier::Error::DestinationParentStale);
    CHECK(callback.called);
    CHECK(callback.action_succeeded);
    CHECK(mutation.called);
    CHECK(mutation.succeeded);
    REQUIRE(fchmod(_case.DestinationParentFD(), mutation.original_mode) == 0);
    CheckDestinationParentEmpty(_case);
    RequireRootLocksReleased(context);
}

static void VerifyPublicationBarrierStageRevalidation(const PublicationBarrierCase &_case)
{
    const auto context = _case.Context();
    auto stage = MakeStagedDestination(context);
    const auto artifact = SingleRootEntry(_case.DestinationRoot(), ".wc-cross-volume-artifact-");
    FileMutationActionContext mutation{.path = &artifact};
    BarrierCallbackContext callback{
        .action = MutatePrivateFileAfterBarrierValidation,
        .action_context = &mutation,
    };
    const auto failed =
        helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(callback));
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == helper::StagingPublicationBarrier::Error::StageStale);
    CHECK(callback.called);
    CHECK(callback.action_succeeded);
    CHECK(mutation.called);
    CHECK(mutation.succeeded);
    CheckDestinationParentEmpty(_case);
    RequireRootLocksReleased(context);
}

static void VerifyPublicationBarrierRootRevalidation(const PublicationBarrierCase &_case)
{
    const auto context = _case.Context();
    auto stage = MakeStagedDestination(context);
    RootModeMutationActionContext mutation{.fd = _case.DestinationRootFD()};
    BarrierCallbackContext callback{
        .action = MutateRootModeAfterBarrierValidation,
        .action_context = &mutation,
    };
    const auto failed =
        helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(callback));
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == helper::StagingPublicationBarrier::Error::DestinationRootBindingFailed);
    CHECK(callback.called);
    CHECK(callback.action_succeeded);
    CHECK(mutation.called);
    CHECK(mutation.succeeded);
    REQUIRE(fchmod(_case.DestinationRootFD(), mutation.original_mode) == 0);
    CheckDestinationParentEmpty(_case);
    RequireRootLocksReleased(context);
}

static void VerifyPublicationBarrierPrimaryManifestRevalidation(const PublicationBarrierCase &_case)
{
    const auto context = _case.Context();
    auto stage = MakeStagedDestination(context);
    const auto record = SingleRootEntry(_case.DestinationRoot(), ".wc-cross-volume-record-");
    UnlinkActionContext mutation{.path = &record};
    BarrierCallbackContext callback{
        .action = UnlinkPrivateFileAfterBarrierValidation,
        .action_context = &mutation,
    };
    const auto failed =
        helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(callback));
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == helper::StagingPublicationBarrier::Error::StageManifestFailed);
    CHECK(callback.called);
    CHECK(callback.action_succeeded);
    CHECK(mutation.called);
    CHECK(mutation.succeeded);
    CheckDestinationParentEmpty(_case);
    RequireRootLocksReleased(context);
}

static void VerifyPublicationBarrierSealedManifestRevalidation(const PublicationBarrierCase &_case)
{
    const auto context = _case.Context();
    auto stage = MakeStagedDestination(context);
    const auto seal = SingleRootEntry(_case.DestinationRoot(), ".wc-cross-volume-seal-");
    UnlinkActionContext mutation{.path = &seal};
    BarrierCallbackContext callback{
        .action = UnlinkPrivateFileAfterBarrierValidation,
        .action_context = &mutation,
    };
    const auto failed =
        helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(callback));
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == helper::StagingPublicationBarrier::Error::StageManifestFailed);
    CHECK(callback.called);
    CHECK(callback.action_succeeded);
    CHECK(mutation.called);
    CHECK(mutation.succeeded);
    CheckDestinationParentEmpty(_case);
    RequireRootLocksReleased(context);
}

static void VerifyPublicationBarrierDestinationRace(const PublicationBarrierCase &_case)
{
    const auto context = _case.Context();
    auto stage = MakeStagedDestination(context);
    DestinationSentinelActionContext sentinel{.parent_fd = _case.DestinationParentFD()};
    BarrierCallbackContext callback{
        .action = CreateDestinationSentinelAfterBarrierValidation,
        .action_context = &sentinel,
    };
    const auto failed =
        helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(callback));
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == helper::StagingPublicationBarrier::Error::DestinationExists);
    CHECK(callback.called);
    CHECK(callback.action_succeeded);
    CHECK(sentinel.called);
    CHECK(sentinel.succeeded);
    CHECK(HasExactDestinationSentinel(_case.DestinationParent()));
    CHECK(DirectoryEntryNames(_case.DestinationParent()) == std::vector<std::string>{"dest.txt"});
    RequireRootLocksReleased(context);
}

static void VerifyPublicationBarrierRejectsBusyRootBeforeCallback(const PublicationBarrierCase &_case)
{
    const auto context = _case.Context();
    auto stage = MakeStagedDestination(context);
    auto held_root = helper::ProtectedRootLedger::Open(context.destination_root_fd);
    REQUIRE(held_root);
    auto held_ledger = std::move(*held_root);
    BarrierCallbackContext callback;
    const auto failed =
        helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(callback));
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == helper::StagingPublicationBarrier::Error::DestinationRootBusy);
    CHECK_FALSE(callback.called);
    CheckDestinationParentEmpty(_case);
}

static void VerifyPublicationBarrierRejectsInvalidRootBeforeCallback(const PublicationBarrierCase &_case)
{
    const auto context = _case.Context();
    auto stage = MakeStagedDestination(context);
    REQUIRE(fchmod(_case.DestinationRootFD(), 0750) == 0);
    BarrierCallbackContext callback;
    const auto failed =
        helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(callback));
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == helper::StagingPublicationBarrier::Error::DestinationRootInvalid);
    CHECK_FALSE(callback.called);
    REQUIRE(fchmod(_case.DestinationRootFD(), 0700) == 0);
    CheckDestinationParentEmpty(_case);
    RequireRootLocksReleased(context);
}

static void VerifyPublicationBarrierRejectsInheritedStageBeforeCallback(const PublicationBarrierCase &_case)
{
    const auto context = _case.Context();
    auto stage = MakeStagedDestination(context);
    int result_pipe[2]{};
    REQUIRE(pipe(result_pipe) == 0);
    const pid_t child = fork();
    if( child == 0 ) {
        (void)close(result_pipe[0]);
        BarrierCallbackContext callback;
        const auto failed =
            helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(callback));
        const bool expected =
            !failed && failed.error() == helper::StagingPublicationBarrier::Error::ForkedProcess && !callback.called;
        const uint8_t value = expected ? 1 : 0;
        (void)WriteExact(result_pipe[1], &value, sizeof(value));
        (void)close(result_pipe[1]);
        _exit(expected ? 0 : 1);
    }
    REQUIRE(child > 0);
    REQUIRE(close(result_pipe[1]) == 0);
    uint8_t result = 0;
    REQUIRE(ReadExactWithin(result_pipe[0], &result, sizeof(result)));
    REQUIRE(close(result_pipe[0]) == 0);
    int status = 0;
    REQUIRE(WaitForChild(child, status));
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(result == 1);
    CheckDestinationParentEmpty(_case);
    RequireRootLocksReleased(context);
}

static void VerifyPublicationBarrierRejectsMovedStageBeforeCallback(const PublicationBarrierCase &_case)
{
    const auto context = _case.Context();
    auto stage = MakeStagedDestination(context);
    BarrierCallbackContext cancelled_callback{.cancel = true};
    const auto cancelled =
        helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(cancelled_callback));
    REQUIRE_FALSE(cancelled);
    REQUIRE(cancelled.error() == helper::StagingPublicationBarrier::Error::Cancelled);
    CHECK(cancelled_callback.called);
    BarrierCallbackContext reused_callback;
    const auto reused =
        helper::StagingPublicationBarrierTestAccess::Prepare(std::move(stage), BarrierCancellation(reused_callback));
    REQUIRE_FALSE(reused);
    CHECK(reused.error() == helper::StagingPublicationBarrier::Error::InvalidStage);
    CHECK_FALSE(reused_callback.called);
    CheckDestinationParentEmpty(_case);
    RequireRootLocksReleased(context);
}

using PublicationBarrierVerifier = void (*)(const PublicationBarrierCase &);

[[clang::noinline]] static void RunPublicationBarrierCase(TestDir &_source_workspace,
                                                          RetainedDestinationFixture &_destination_fixture,
                                                          const std::string_view _name,
                                                          const uint8_t _correlation,
                                                          const PublicationBarrierVerifier _verify)
{
    PublicationBarrierCase test_case{_source_workspace, _destination_fixture, _name, _correlation};
    _verify(test_case);
}

static void RunPublicationBarrierFixture(const std::filesystem::path &_destination_base)
{
    RetainedDestinationFixture destination_fixture{_destination_base};
    TestDir source_workspace;

    {
        PublicationBarrierCase probe_case{source_workspace, destination_fixture, "probe", 59};
        const auto probe_context = probe_case.Context();
        if( probe_context.request.source.device == probe_context.request.destination_parent.device )
            SKIP("supplied destination root must be on a distinct device");
        if( !SupportsFullSync(probe_context.source_root) || !SupportsFullSync(probe_context.destination_root) )
            SKIP("source or destination fixture does not support F_FULLFSYNC");
    }

    RunPublicationBarrierCase(source_workspace, destination_fixture, "permit", 60, VerifyPublicationBarrierPermit);
    RunPublicationBarrierCase(
        source_workspace, destination_fixture, "cancel", 61, VerifyPublicationBarrierCancellation);
    RunPublicationBarrierCase(
        source_workspace, destination_fixture, "source", 62, VerifyPublicationBarrierSourceRevalidation);
    RunPublicationBarrierCase(
        source_workspace, destination_fixture, "parent", 63, VerifyPublicationBarrierParentRevalidation);
    RunPublicationBarrierCase(
        source_workspace, destination_fixture, "stage", 64, VerifyPublicationBarrierStageRevalidation);
    RunPublicationBarrierCase(
        source_workspace, destination_fixture, "root", 65, VerifyPublicationBarrierRootRevalidation);
    RunPublicationBarrierCase(source_workspace,
                              destination_fixture,
                              "primary-manifest",
                              66,
                              VerifyPublicationBarrierPrimaryManifestRevalidation);
    RunPublicationBarrierCase(source_workspace,
                              destination_fixture,
                              "sealed-manifest",
                              67,
                              VerifyPublicationBarrierSealedManifestRevalidation);
    RunPublicationBarrierCase(
        source_workspace, destination_fixture, "destination-race", 68, VerifyPublicationBarrierDestinationRace);
    RunPublicationBarrierCase(
        source_workspace, destination_fixture, "busy", 69, VerifyPublicationBarrierRejectsBusyRootBeforeCallback);
    RunPublicationBarrierCase(source_workspace,
                              destination_fixture,
                              "invalid-root",
                              70,
                              VerifyPublicationBarrierRejectsInvalidRootBeforeCallback);
    RunPublicationBarrierCase(
        source_workspace, destination_fixture, "fork", 71, VerifyPublicationBarrierRejectsInheritedStageBeforeCallback);
    RunPublicationBarrierCase(
        source_workspace, destination_fixture, "moved", 72, VerifyPublicationBarrierRejectsMovedStageBeforeCallback);
}

class LocalLifecycleInspectionFixture final
{
public:
    LocalLifecycleInspectionFixture()
    {
        m_SourceParent = m_Workspace.directory / "lifecycle-source-parent";
        m_SourceRoot = m_Workspace.directory / "lifecycle-source-root";
        m_DestinationRoot = m_Workspace.directory / "lifecycle-destination-root";
        m_UserDestinationParent = m_Workspace.directory / "lifecycle-user-destination";
        REQUIRE(std::filesystem::create_directory(m_SourceParent));
        REQUIRE(std::filesystem::create_directory(m_SourceRoot));
        REQUIRE(std::filesystem::create_directory(m_DestinationRoot));
        REQUIRE(std::filesystem::create_directory(m_UserDestinationParent));

        m_SourcePath = m_SourceParent / "source";
        const int source_fd = open(m_SourcePath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        REQUIRE(source_fd >= 0);
        REQUIRE(close(source_fd) == 0);

        const auto sentinel = m_UserDestinationParent / "dest.txt";
        const int sentinel_fd = open(sentinel.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        REQUIRE(sentinel_fd >= 0);
        constexpr std::string_view contents{"lifecycle-user-sentinel"};
        REQUIRE(WriteExact(sentinel_fd, contents.data(), contents.size()));
        REQUIRE(close(sentinel_fd) == 0);

        m_SourceFD = open(m_SourcePath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        m_UserDestinationParentFD =
            open(m_UserDestinationParent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        REQUIRE(m_SourceFD >= 0);
        REQUIRE(m_UserDestinationParentFD >= 0);
        m_SourceRootFD = OpenProtectedRoot(m_SourceRoot);
        m_DestinationRootFD = OpenProtectedRoot(m_DestinationRoot);
    }

    LocalLifecycleInspectionFixture(const LocalLifecycleInspectionFixture &) = delete;
    LocalLifecycleInspectionFixture &operator=(const LocalLifecycleInspectionFixture &) = delete;

    ~LocalLifecycleInspectionFixture()
    {
        if( m_DestinationRootFD >= 0 )
            (void)close(m_DestinationRootFD);
        if( m_SourceRootFD >= 0 )
            (void)close(m_SourceRootFD);
        if( m_UserDestinationParentFD >= 0 )
            (void)close(m_UserDestinationParentFD);
        if( m_SourceFD >= 0 )
            (void)close(m_SourceFD);
    }

    [[nodiscard]] protocol::BeginRequest RequestFor(const uint8_t _correlation) const
    {
        return Request(Header(_correlation), m_SourceFD, m_UserDestinationParentFD);
    }
    [[nodiscard]] helper::LeaseStore::TerminalLease TakeCommitted(const uint8_t _correlation) const
    {
        return TakeCommittedTerminal(RequestFor(_correlation), m_SourceFD, m_UserDestinationParentFD);
    }

    [[nodiscard]] int SourceFD() const noexcept { return m_SourceFD; }
    [[nodiscard]] int UserDestinationParentFD() const noexcept { return m_UserDestinationParentFD; }
    [[nodiscard]] int SourceRootFD() const noexcept { return m_SourceRootFD; }
    [[nodiscard]] int DestinationRootFD() const noexcept { return m_DestinationRootFD; }
    [[nodiscard]] const std::filesystem::path &SourceRoot() const noexcept { return m_SourceRoot; }
    [[nodiscard]] const std::filesystem::path &DestinationRoot() const noexcept { return m_DestinationRoot; }
    [[nodiscard]] const std::filesystem::path &UserDestinationParent() const noexcept
    {
        return m_UserDestinationParent;
    }

private:
    TestDir m_Workspace;
    std::filesystem::path m_SourceParent;
    std::filesystem::path m_SourceRoot;
    std::filesystem::path m_DestinationRoot;
    std::filesystem::path m_UserDestinationParent;
    std::filesystem::path m_SourcePath;
    int m_SourceFD{-1};
    int m_UserDestinationParentFD{-1};
    int m_SourceRootFD{-1};
    int m_DestinationRootFD{-1};
};

struct LocalLifecyclePair final {
    protocol::Header header;
    std::string source_snapshot_id;
    std::string destination_stage_id;
    protocol::ObjectSeal source_snapshot_seal;
    protocol::ObjectSeal destination_stage_seal;
    protocol::ObjectSeal source_seal;
    protocol::ObjectSeal destination_parent_seal;
    uint64_t source_root_device{0};
    uint64_t source_root_inode{0};
    uint64_t destination_root_device{0};
    uint64_t destination_root_inode{0};
};

template <size_t Size>
static std::string HexString(const std::array<uint8_t, Size> &_bytes)
{
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string result(Size * 2, '\0');
    for( size_t index = 0; index != Size; ++index ) {
        result[index * 2] = digits[_bytes[index] >> 4];
        result[index * 2 + 1] = digits[_bytes[index] & 0x0F];
    }
    return result;
}

static std::string ArtifactIDFromArtifactPath(const std::filesystem::path &_path)
{
    constexpr std::string_view prefix{".wc-cross-volume-artifact-"};
    constexpr std::string_view suffix{".data"};
    const std::string name = _path.filename().string();
    REQUIRE(name.starts_with(prefix));
    REQUIRE(name.ends_with(suffix));
    REQUIRE(name.size() == prefix.size() + 64 + suffix.size());
    return name.substr(prefix.size(), 64);
}

static std::filesystem::path StandardArtifactPath(const std::filesystem::path &_root,
                                                  const std::string_view _artifact_id)
{
    return _root / (".wc-cross-volume-artifact-" + std::string{_artifact_id} + ".data");
}

static std::filesystem::path LifecyclePrimaryPath(const std::filesystem::path &_root,
                                                  const std::string_view _artifact_id)
{
    return _root / (".wc-cross-volume-lifecycle-" + std::string{_artifact_id} + ".manifest");
}

static std::filesystem::path LifecycleSealPath(const std::filesystem::path &_root, const std::string_view _artifact_id)
{
    return _root / (".wc-cross-volume-lifecycle-seal-" + std::string{_artifact_id} + ".manifest");
}

static void WritePrivateLifecycleRecord(const std::filesystem::path &_path, const std::string_view _contents)
{
    const int fd = open(_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    REQUIRE(fd >= 0);
    REQUIRE(WriteExact(fd, _contents.data(), _contents.size()));
    REQUIRE(fsync(fd) == 0);
    REQUIRE(close(fd) == 0);
}

static void AppendLifecycleNumber(std::string &_contents, const std::string_view _name, const uint64_t _value)
{
    _contents += _name;
    _contents += '=';
    _contents += std::to_string(_value);
    _contents += '\n';
}

static void
AppendLifecycleSeal(std::string &_contents, const std::string_view _prefix, const protocol::ObjectSeal &_seal)
{
    REQUIRE(_seal.birth_time.seconds >= 0);
    REQUIRE(_seal.modification_time.seconds >= 0);
    REQUIRE(_seal.status_change_time.seconds >= 0);
    AppendLifecycleNumber(_contents, std::string{_prefix} + "device", _seal.device);
    AppendLifecycleNumber(_contents, std::string{_prefix} + "inode", _seal.inode);
    AppendLifecycleNumber(_contents, std::string{_prefix} + "uid", _seal.uid);
    AppendLifecycleNumber(_contents, std::string{_prefix} + "gid", _seal.gid);
    AppendLifecycleNumber(_contents, std::string{_prefix} + "mode", _seal.mode);
    AppendLifecycleNumber(_contents, std::string{_prefix} + "flags", _seal.flags);
    AppendLifecycleNumber(_contents, std::string{_prefix} + "nlink", _seal.link_count);
    AppendLifecycleNumber(_contents, std::string{_prefix} + "size", _seal.byte_size);
    AppendLifecycleNumber(
        _contents, std::string{_prefix} + "birth_seconds", static_cast<uint64_t>(_seal.birth_time.seconds));
    AppendLifecycleNumber(_contents, std::string{_prefix} + "birth_nanoseconds", _seal.birth_time.nanoseconds);
    AppendLifecycleNumber(
        _contents, std::string{_prefix} + "mtime_seconds", static_cast<uint64_t>(_seal.modification_time.seconds));
    AppendLifecycleNumber(_contents, std::string{_prefix} + "mtime_nanoseconds", _seal.modification_time.nanoseconds);
    AppendLifecycleNumber(
        _contents, std::string{_prefix} + "ctime_seconds", static_cast<uint64_t>(_seal.status_change_time.seconds));
    AppendLifecycleNumber(_contents, std::string{_prefix} + "ctime_nanoseconds", _seal.status_change_time.nanoseconds);
}

static std::string BuildLocalLifecycleManifest(const LocalLifecyclePair &_pair,
                                               const std::string_view _source_snapshot_id,
                                               const std::string_view _destination_stage_id,
                                               const std::string_view _destination_name,
                                               const std::string_view _state)
{
    REQUIRE(_source_snapshot_id.size() == 64);
    REQUIRE(_destination_stage_id.size() == 64);
    REQUIRE(_destination_name.size() >= 2);
    REQUIRE((_destination_name.size() & 1) == 0);
    REQUIRE((_state == "reserved" || _state == "sealed"));

    std::string contents;
    contents.reserve(2048);
    contents += "schema=wincommander-cross-volume-lifecycle-v1\ncorrelation=";
    contents += HexString(_pair.header.correlation);
    contents += '\n';
    AppendLifecycleNumber(contents, "source_root_device", _pair.source_root_device);
    AppendLifecycleNumber(contents, "source_root_inode", _pair.source_root_inode);
    AppendLifecycleNumber(contents, "destination_root_device", _pair.destination_root_device);
    AppendLifecycleNumber(contents, "destination_root_inode", _pair.destination_root_inode);
    contents += "source_snapshot_artifact=";
    contents += _source_snapshot_id;
    contents += "\ndestination_stage_artifact=";
    contents += _destination_stage_id;
    contents += '\n';
    AppendLifecycleSeal(contents, "source_snapshot_", _pair.source_snapshot_seal);
    AppendLifecycleSeal(contents, "destination_stage_", _pair.destination_stage_seal);
    AppendLifecycleSeal(contents, "source_", _pair.source_seal);
    AppendLifecycleSeal(contents, "destination_parent_", _pair.destination_parent_seal);
    contents += "destination_name=";
    contents += _destination_name;
    contents += "\nstate=";
    contents += _state;
    contents += '\n';
    return contents;
}

static void WriteLocalLifecyclePair(const std::filesystem::path &_root,
                                    const std::string_view _local_artifact_id,
                                    const LocalLifecyclePair &_pair,
                                    const std::string_view _source_snapshot_id,
                                    const std::string_view _destination_stage_id,
                                    const std::string_view _destination_name = "646573742e747874")
{
    WritePrivateLifecycleRecord(
        LifecyclePrimaryPath(_root, _local_artifact_id),
        BuildLocalLifecycleManifest(_pair, _source_snapshot_id, _destination_stage_id, _destination_name, "reserved"));
    WritePrivateLifecycleRecord(
        LifecycleSealPath(_root, _local_artifact_id),
        BuildLocalLifecycleManifest(_pair, _source_snapshot_id, _destination_stage_id, _destination_name, "sealed"));
}

static LocalLifecyclePair PrepareLocalLifecyclePair(LocalLifecycleInspectionFixture &_fixture,
                                                    const uint8_t _correlation)
{
    if( !SupportsFullSync(_fixture.SourceRoot()) || !SupportsFullSync(_fixture.DestinationRoot()) )
        SKIP("local lifecycle fixture does not support F_FULLFSYNC");

    LocalLifecyclePair pair;
    const auto request = _fixture.RequestFor(_correlation);
    pair.header = request.header;
    pair.source_seal = request.source;
    pair.destination_parent_seal = request.destination_parent;

    struct stat source_root_status{};
    struct stat destination_root_status{};
    REQUIRE(fstat(_fixture.SourceRootFD(), &source_root_status) == 0);
    REQUIRE(fstat(_fixture.DestinationRootFD(), &destination_root_status) == 0);
    pair.source_root_device = static_cast<uint64_t>(source_root_status.st_dev);
    pair.source_root_inode = static_cast<uint64_t>(source_root_status.st_ino);
    pair.destination_root_device = static_cast<uint64_t>(destination_root_status.st_dev);
    pair.destination_root_inode = static_cast<uint64_t>(destination_root_status.st_ino);

    {
        auto source_root = helper::ProtectedRootLedger::Open(_fixture.SourceRootFD());
        REQUIRE(source_root);
        auto destination_root = helper::ProtectedRootLedger::Open(_fixture.DestinationRootFD());
        REQUIRE(destination_root);

        auto snapshot = helper::SourceSnapshotWriter::Create(*source_root, _fixture.TakeCommitted(_correlation), {});
        REQUIRE(snapshot);
        pair.source_snapshot_seal = snapshot->ArtifactSeal();
        pair.source_snapshot_id =
            ArtifactIDFromArtifactPath(SingleRootEntry(_fixture.SourceRoot(), ".wc-cross-volume-artifact-"));

        auto reservation = destination_root->Reserve(request.header, helper::ArtifactRole::DestinationStage);
        REQUIRE(reservation);
        pair.destination_stage_id = HexString(reservation->ID().bytes);
        REQUIRE(destination_root->MaterializeEmptyAndSeal(std::move(*reservation)));

        const auto stage_path = StandardArtifactPath(_fixture.DestinationRoot(), pair.destination_stage_id);
        const int stage_fd = open(stage_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        REQUIRE(stage_fd >= 0);
        pair.destination_stage_seal = SealFromFD(stage_fd);
        REQUIRE(close(stage_fd) == 0);
    }
    return pair;
}

static void
CheckLifecycleInspectionLeavesEveryNamespaceUntouched(const LocalLifecycleInspectionFixture &_fixture,
                                                      const protocol::Header &_header,
                                                      const helper::StagingPublicationLifecycle::State _expected_state =
                                                          helper::StagingPublicationLifecycle::State::Absent)
{
    const auto source_before = SnapshotDirectoryEntries(_fixture.SourceRoot());
    const auto destination_before = SnapshotDirectoryEntries(_fixture.DestinationRoot());
    const auto user_before = SnapshotDirectoryEntries(_fixture.UserDestinationParent());

    const auto inspection = helper::StagingPublicationLifecycleTestAccess::Inspect(
        _fixture.SourceRootFD(), _fixture.DestinationRootFD(), _header);
    REQUIRE(inspection);
    CHECK(inspection->state == _expected_state);
    CHECK(SnapshotDirectoryEntries(_fixture.SourceRoot()) == source_before);
    CHECK(SnapshotDirectoryEntries(_fixture.DestinationRoot()) == destination_before);
    CHECK(SnapshotDirectoryEntries(_fixture.UserDestinationParent()) == user_before);
}

TEST_CASE(PREFIX "rejects an Abort or a consumed terminal capability before root access")
{
    LocalFixture fixture;

    const auto aborted = helper::StagingRootAuthorityTestAccess::Acquire(fixture.TakeAborted(1), -1, -1);
    REQUIRE_FALSE(aborted);
    CHECK(aborted.error() == helper::StagingRootAuthority::Error::InvalidTerminalLease);

    auto terminal = fixture.TakeCommitted(2);
    const auto first = helper::StagingRootAuthorityTestAccess::Acquire(
        std::move(terminal), fixture.m_SourceFD, fixture.m_DestinationRootFD);
    REQUIRE_FALSE(first);
    CHECK(first.error() == helper::StagingRootAuthority::Error::SourceRootInvalid);
    const auto consumed = helper::StagingRootAuthorityTestAccess::Acquire(std::move(terminal), -1, -1);
    REQUIRE_FALSE(consumed);
    CHECK(consumed.error() == helper::StagingRootAuthority::Error::InvalidTerminalLease);
}

TEST_CASE(PREFIX "rejects an inherited terminal capability before root access")
{
    LocalFixture fixture;
    auto terminal = fixture.TakeCommitted(1);
    int result_pipe[2]{};
    REQUIRE(pipe(result_pipe) == 0);
    const pid_t child = fork();
    if( child == 0 ) {
        (void)close(result_pipe[0]);
        const auto result = helper::StagingRootAuthorityTestAccess::Acquire(std::move(terminal), -1, -1);
        const uint8_t value = !result && result.error() == helper::StagingRootAuthority::Error::ForkedProcess ? 1 : 0;
        (void)WriteExact(result_pipe[1], &value, sizeof(value));
        (void)close(result_pipe[1]);
        _exit(value == 1 ? 0 : 1);
    }
    REQUIRE(child > 0);
    REQUIRE(close(result_pipe[1]) == 0);
    uint8_t result = 0;
    REQUIRE(ReadExactWithin(result_pipe[0], &result, sizeof(result)));
    REQUIRE(close(result_pipe[0]) == 0);
    int status = 0;
    REQUIRE(WaitForChild(child, status));
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(result == 1);
}

TEST_CASE(PREFIX "validates root shape and releases the terminal capability on every failed admission")
{
    LocalFixture fixture;

    const auto regular_source = helper::StagingRootAuthorityTestAccess::Acquire(
        fixture.TakeCommitted(1), fixture.m_SourceFD, fixture.m_DestinationRootFD);
    REQUIRE_FALSE(regular_source);
    CHECK(regular_source.error() == helper::StagingRootAuthority::Error::SourceRootInvalid);

    REQUIRE(fchmod(fixture.m_DestinationRootFD, 0750) == 0);
    const auto non_private_destination = helper::StagingRootAuthorityTestAccess::Acquire(
        fixture.TakeCommitted(2), fixture.m_SourceRootFD, fixture.m_DestinationRootFD);
    REQUIRE_FALSE(non_private_destination);
    CHECK(non_private_destination.error() == helper::StagingRootAuthority::Error::DestinationRootInvalid);
    REQUIRE(fchmod(fixture.m_DestinationRootFD, 0700) == 0);

    const auto same_device = helper::StagingRootAuthorityTestAccess::Acquire(
        fixture.TakeCommitted(3), fixture.m_SourceRootFD, fixture.m_DestinationRootFD);
    REQUIRE_FALSE(same_device);
    CHECK(same_device.error() == helper::StagingRootAuthority::Error::RootsOnSameDevice);
    CHECK(fcntl(fixture.m_SourceRootFD, F_GETFD) >= 0);
    CHECK(fcntl(fixture.m_DestinationRootFD, F_GETFD) >= 0);

    auto reopened_source = helper::ProtectedRootLedger::Open(fixture.m_SourceRootFD);
    REQUIRE(reopened_source);
    auto source_ledger = std::move(*reopened_source);
    auto reopened_destination = helper::ProtectedRootLedger::Open(fixture.m_DestinationRootFD);
    REQUIRE(reopened_destination);
    auto destination_ledger = std::move(*reopened_destination);
}

TEST_CASE(PREFIX "inspects an absent same-device lifecycle pair without namespace mutation (non-physical)")
{
    LocalLifecycleInspectionFixture fixture;
    CheckLifecycleInspectionLeavesEveryNamespaceUntouched(fixture, Header(73));
}

TEST_CASE(PREFIX "never classifies a full same-device lifecycle candidate as pending (non-physical)")
{
    LocalLifecycleInspectionFixture fixture;
    const auto pair = PrepareLocalLifecyclePair(fixture, 76);
    WriteLocalLifecyclePair(
        fixture.SourceRoot(), pair.source_snapshot_id, pair, pair.source_snapshot_id, pair.destination_stage_id);
    WriteLocalLifecyclePair(
        fixture.DestinationRoot(), pair.destination_stage_id, pair, pair.source_snapshot_id, pair.destination_stage_id);
    CheckLifecycleInspectionLeavesEveryNamespaceUntouched(
        fixture, pair.header, helper::StagingPublicationLifecycle::State::Mismatched);
}

TEST_CASE(PREFIX "retains a source-only lifecycle pair without namespace mutation (non-physical)")
{
    LocalLifecycleInspectionFixture fixture;
    const auto pair = PrepareLocalLifecyclePair(fixture, 77);
    WriteLocalLifecyclePair(
        fixture.SourceRoot(), pair.source_snapshot_id, pair, pair.source_snapshot_id, pair.destination_stage_id);
    CheckLifecycleInspectionLeavesEveryNamespaceUntouched(
        fixture, pair.header, helper::StagingPublicationLifecycle::State::Incomplete);
}

TEST_CASE(PREFIX "retains mismatched lifecycle pairs without namespace mutation (non-physical)")
{
    LocalLifecycleInspectionFixture fixture;
    const auto pair = PrepareLocalLifecyclePair(fixture, 78);
    WriteLocalLifecyclePair(
        fixture.SourceRoot(), pair.source_snapshot_id, pair, pair.source_snapshot_id, pair.destination_stage_id);
    WriteLocalLifecyclePair(fixture.DestinationRoot(),
                            pair.destination_stage_id,
                            pair,
                            pair.source_snapshot_id,
                            pair.destination_stage_id,
                            "6f746865722e747874");
    CheckLifecycleInspectionLeavesEveryNamespaceUntouched(
        fixture, pair.header, helper::StagingPublicationLifecycle::State::Mismatched);
}

TEST_CASE(PREFIX "classifies a malformed lifecycle record without namespace mutation (non-physical)")
{
    LocalLifecycleInspectionFixture fixture;
    WritePrivateLifecycleRecord(LifecyclePrimaryPath(fixture.SourceRoot(), std::string(64, 'a')), "malformed\n");
    CheckLifecycleInspectionLeavesEveryNamespaceUntouched(
        fixture, Header(79), helper::StagingPublicationLifecycle::State::Malformed);
}

TEST_CASE(PREFIX "rejects a stale full same-device candidate without namespace mutation (non-physical)")
{
    LocalLifecycleInspectionFixture fixture;
    const auto pair = PrepareLocalLifecyclePair(fixture, 80);
    WriteLocalLifecyclePair(
        fixture.SourceRoot(), pair.source_snapshot_id, pair, pair.source_snapshot_id, pair.destination_stage_id);
    WriteLocalLifecyclePair(
        fixture.DestinationRoot(), pair.destination_stage_id, pair, pair.source_snapshot_id, pair.destination_stage_id);
    const auto stage_path = StandardArtifactPath(fixture.DestinationRoot(), pair.destination_stage_id);
    REQUIRE(chmod(stage_path.c_str(), 0640) == 0);
    CheckLifecycleInspectionLeavesEveryNamespaceUntouched(
        fixture, pair.header, helper::StagingPublicationLifecycle::State::Mismatched);
    REQUIRE(chmod(stage_path.c_str(), 0600) == 0);
}

TEST_CASE(PREFIX "retains a full lifecycle pair plus an extra same-correlation primary without mutation (non-physical)")
{
    LocalLifecycleInspectionFixture fixture;
    const auto pair = PrepareLocalLifecyclePair(fixture, 81);
    WriteLocalLifecyclePair(
        fixture.SourceRoot(), pair.source_snapshot_id, pair, pair.source_snapshot_id, pair.destination_stage_id);
    WriteLocalLifecyclePair(
        fixture.DestinationRoot(), pair.destination_stage_id, pair, pair.source_snapshot_id, pair.destination_stage_id);
    std::string extra_source_id = pair.source_snapshot_id;
    std::string extra_destination_id = pair.destination_stage_id;
    extra_source_id[0] = extra_source_id[0] == 'a' ? 'b' : 'a';
    extra_destination_id[0] = extra_destination_id[0] == 'b' ? 'c' : 'b';
    WritePrivateLifecycleRecord(
        LifecyclePrimaryPath(fixture.SourceRoot(), extra_source_id),
        BuildLocalLifecycleManifest(pair, extra_source_id, extra_destination_id, "646573742e747874", "reserved"));
    CheckLifecycleInspectionLeavesEveryNamespaceUntouched(
        fixture, pair.header, helper::StagingPublicationLifecycle::State::Mismatched);
}

TEST_CASE(PREFIX "retains a full same-device candidate after fork without namespace mutation (non-physical)")
{
    LocalLifecycleInspectionFixture fixture;
    const auto pair = PrepareLocalLifecyclePair(fixture, 82);
    WriteLocalLifecyclePair(
        fixture.SourceRoot(), pair.source_snapshot_id, pair, pair.source_snapshot_id, pair.destination_stage_id);
    WriteLocalLifecyclePair(
        fixture.DestinationRoot(), pair.destination_stage_id, pair, pair.source_snapshot_id, pair.destination_stage_id);
    const auto source_before = SnapshotDirectoryEntries(fixture.SourceRoot());
    const auto destination_before = SnapshotDirectoryEntries(fixture.DestinationRoot());
    const auto user_before = SnapshotDirectoryEntries(fixture.UserDestinationParent());

    int result_pipe[2]{};
    REQUIRE(pipe(result_pipe) == 0);
    const pid_t child = fork();
    if( child == 0 ) {
        (void)close(result_pipe[0]);
        const auto inspection = helper::StagingPublicationLifecycleTestAccess::Inspect(
            fixture.SourceRootFD(), fixture.DestinationRootFD(), pair.header);
        const uint8_t value =
            inspection && inspection->state == helper::StagingPublicationLifecycle::State::Mismatched ? 1 : 0;
        (void)WriteExact(result_pipe[1], &value, sizeof(value));
        (void)close(result_pipe[1]);
        _exit(value == 1 ? 0 : 1);
    }
    REQUIRE(child > 0);
    REQUIRE(close(result_pipe[1]) == 0);
    uint8_t result = 0;
    REQUIRE(ReadExactWithin(result_pipe[0], &result, sizeof(result)));
    REQUIRE(close(result_pipe[0]) == 0);
    int status = 0;
    REQUIRE(WaitForChild(child, status));
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(result == 1);
    CHECK(SnapshotDirectoryEntries(fixture.SourceRoot()) == source_before);
    CHECK(SnapshotDirectoryEntries(fixture.DestinationRoot()) == destination_before);
    CHECK(SnapshotDirectoryEntries(fixture.UserDestinationParent()) == user_before);
}

TEST_CASE(PREFIX "rejects an invalid lifecycle header without namespace mutation (non-physical)")
{
    LocalLifecycleInspectionFixture fixture;
    const auto source_before = SnapshotDirectoryEntries(fixture.SourceRoot());
    const auto destination_before = SnapshotDirectoryEntries(fixture.DestinationRoot());
    const auto user_before = SnapshotDirectoryEntries(fixture.UserDestinationParent());
    const protocol::Header invalid_header;
    const auto inspection = helper::StagingPublicationLifecycleTestAccess::Inspect(
        fixture.SourceRootFD(), fixture.DestinationRootFD(), invalid_header);
    REQUIRE_FALSE(inspection);
    CHECK(inspection.error() == helper::StagingPublicationLifecycle::Error::InvalidHeader);
    CHECK(SnapshotDirectoryEntries(fixture.SourceRoot()) == source_before);
    CHECK(SnapshotDirectoryEntries(fixture.DestinationRoot()) == destination_before);
    CHECK(SnapshotDirectoryEntries(fixture.UserDestinationParent()) == user_before);
}

TEST_CASE(PREFIX "rejects a busy lifecycle source root without namespace mutation (non-physical)")
{
    LocalLifecycleInspectionFixture fixture;
    const auto source_before = SnapshotDirectoryEntries(fixture.SourceRoot());
    const auto destination_before = SnapshotDirectoryEntries(fixture.DestinationRoot());
    const auto user_before = SnapshotDirectoryEntries(fixture.UserDestinationParent());

    auto held_source = helper::ProtectedRootLedger::Open(fixture.SourceRootFD());
    REQUIRE(held_source);
    const auto inspection = helper::StagingPublicationLifecycleTestAccess::Inspect(
        fixture.SourceRootFD(), fixture.DestinationRootFD(), Header(74));
    REQUIRE_FALSE(inspection);
    CHECK(inspection.error() == helper::StagingPublicationLifecycle::Error::SourceRootBusy);
    CHECK(SnapshotDirectoryEntries(fixture.SourceRoot()) == source_before);
    CHECK(SnapshotDirectoryEntries(fixture.DestinationRoot()) == destination_before);
    CHECK(SnapshotDirectoryEntries(fixture.UserDestinationParent()) == user_before);
}

TEST_CASE(PREFIX "rejects a busy lifecycle destination root without namespace mutation (non-physical)")
{
    LocalLifecycleInspectionFixture fixture;
    const auto source_before = SnapshotDirectoryEntries(fixture.SourceRoot());
    const auto destination_before = SnapshotDirectoryEntries(fixture.DestinationRoot());
    const auto user_before = SnapshotDirectoryEntries(fixture.UserDestinationParent());

    auto held_destination = helper::ProtectedRootLedger::Open(fixture.DestinationRootFD());
    REQUIRE(held_destination);
    const auto inspection = helper::StagingPublicationLifecycleTestAccess::Inspect(
        fixture.SourceRootFD(), fixture.DestinationRootFD(), Header(83));
    REQUIRE_FALSE(inspection);
    CHECK(inspection.error() == helper::StagingPublicationLifecycle::Error::DestinationRootBusy);
    CHECK(SnapshotDirectoryEntries(fixture.SourceRoot()) == source_before);
    CHECK(SnapshotDirectoryEntries(fixture.DestinationRoot()) == destination_before);
    CHECK(SnapshotDirectoryEntries(fixture.UserDestinationParent()) == user_before);
}

TEST_CASE(PREFIX "rejects an invalid lifecycle root without namespace mutation (non-physical)")
{
    LocalLifecycleInspectionFixture fixture;
    const auto source_before = SnapshotDirectoryEntries(fixture.SourceRoot());
    const auto destination_before = SnapshotDirectoryEntries(fixture.DestinationRoot());
    const auto user_before = SnapshotDirectoryEntries(fixture.UserDestinationParent());

    REQUIRE(chmod(fixture.SourceRoot().c_str(), 0750) == 0);
    const auto inspection = helper::StagingPublicationLifecycleTestAccess::Inspect(
        fixture.SourceRootFD(), fixture.DestinationRootFD(), Header(75));
    REQUIRE_FALSE(inspection);
    CHECK(inspection.error() == helper::StagingPublicationLifecycle::Error::SourceRootInvalid);
    REQUIRE(chmod(fixture.SourceRoot().c_str(), 0700) == 0);
    CHECK(SnapshotDirectoryEntries(fixture.SourceRoot()) == source_before);
    CHECK(SnapshotDirectoryEntries(fixture.DestinationRoot()) == destination_before);
    CHECK(SnapshotDirectoryEntries(fixture.UserDestinationParent()) == user_before);
}

TEST_CASE(PREFIX "characterizes same-euid named-stage substitution before RENAME_EXCL (test-only)")
{
#if !defined(__APPLE__)
    SKIP("renameatx_np is a Darwin-only characterization primitive");
#else
    TestDir workspace;
    const auto stage_root = workspace.directory / "private-stage-root";
    REQUIRE(std::filesystem::create_directory(stage_root));
    REQUIRE(chmod(stage_root.c_str(), 0700) == 0);

    const int parent_fd = open(stage_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(parent_fd >= 0);
    struct stat root_status{};
    REQUIRE(fstat(parent_fd, &root_status) == 0);
    REQUIRE(S_ISDIR(root_status.st_mode));
    CHECK((root_status.st_mode & 07777) == 0700);
    CHECK(root_status.st_uid == geteuid());

    constexpr std::string_view stage_name{"stage"};
    constexpr std::string_view held_stage_name{"validated-stage"};
    constexpr std::string_view destination_name{"destination"};
    constexpr std::string_view validated_contents{"validated-stage-payload"};
    constexpr std::string_view replacement_contents{"replacement-stage-payload"};

    const int created_stage =
        openat(parent_fd, stage_name.data(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    REQUIRE(created_stage >= 0);
    REQUIRE(WriteExact(created_stage, validated_contents.data(), validated_contents.size()));
    REQUIRE(close(created_stage) == 0);

    const int validated_stage_fd = openat(parent_fd, stage_name.data(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(validated_stage_fd >= 0);
    struct stat validated_stage{};
    REQUIRE(fstat(validated_stage_fd, &validated_stage) == 0);
    CHECK(validated_stage.st_uid == geteuid());
    struct stat named_stage{};
    REQUIRE(fstatat(parent_fd, stage_name.data(), &named_stage, AT_SYMLINK_NOFOLLOW) == 0);
    CHECK(named_stage.st_dev == validated_stage.st_dev);
    CHECK(named_stage.st_ino == validated_stage.st_ino);

    // A same-euid actor may replace the stage name after this validation. This test deliberately uses only
    // test-owned paths and does not model or grant helper publication authority.
    REQUIRE(renameat(parent_fd, stage_name.data(), parent_fd, held_stage_name.data()) == 0);
    const int replacement_stage =
        openat(parent_fd, stage_name.data(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    REQUIRE(replacement_stage >= 0);
    REQUIRE(WriteExact(replacement_stage, replacement_contents.data(), replacement_contents.size()));
    REQUIRE(close(replacement_stage) == 0);
    struct stat replacement_stage_status{};
    REQUIRE(fstatat(parent_fd, stage_name.data(), &replacement_stage_status, AT_SYMLINK_NOFOLLOW) == 0);
    CHECK(replacement_stage_status.st_dev == validated_stage.st_dev);
    CHECK(replacement_stage_status.st_ino != validated_stage.st_ino);

    errno = 0;
    const int publish_result =
        renameatx_np(parent_fd, stage_name.data(), parent_fd, destination_name.data(), RENAME_EXCL);
    const int publish_error = errno;
    if( publish_result != 0 && publish_error == ENOTSUP ) {
        (void)close(validated_stage_fd);
        (void)close(parent_fd);
        SKIP("the test filesystem does not implement RENAME_EXCL");
    }
    REQUIRE(publish_result == 0);

    struct stat published_stage{};
    REQUIRE(fstatat(parent_fd, destination_name.data(), &published_stage, AT_SYMLINK_NOFOLLOW) == 0);
    CHECK(published_stage.st_dev == replacement_stage_status.st_dev);
    CHECK(published_stage.st_ino == replacement_stage_status.st_ino);
    CHECK(published_stage.st_ino != validated_stage.st_ino);

    std::array<char, validated_contents.size()> retained_contents{};
    REQUIRE(ReadExactWithin(validated_stage_fd, retained_contents.data(), retained_contents.size()));
    CHECK(std::string_view{retained_contents.data(), retained_contents.size()} == validated_contents);
    struct stat retained_stage{};
    REQUIRE(fstat(validated_stage_fd, &retained_stage) == 0);
    CHECK(retained_stage.st_dev == validated_stage.st_dev);
    CHECK(retained_stage.st_ino == validated_stage.st_ino);

    const int held_stage_fd =
        openat(parent_fd, held_stage_name.data(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(held_stage_fd >= 0);
    struct stat held_stage{};
    REQUIRE(fstat(held_stage_fd, &held_stage) == 0);
    CHECK(held_stage.st_dev == validated_stage.st_dev);
    CHECK(held_stage.st_ino == validated_stage.st_ino);
    std::array<char, validated_contents.size()> held_contents{};
    REQUIRE(ReadExactWithin(held_stage_fd, held_contents.data(), held_contents.size()));
    CHECK(std::string_view{held_contents.data(), held_contents.size()} == validated_contents);
    REQUIRE(close(held_stage_fd) == 0);

    const int published_stage_fd =
        openat(parent_fd, destination_name.data(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(published_stage_fd >= 0);
    std::array<char, replacement_contents.size()> published_contents{};
    REQUIRE(ReadExactWithin(published_stage_fd, published_contents.data(), published_contents.size()));
    CHECK(std::string_view{published_contents.data(), published_contents.size()} == replacement_contents);
    REQUIRE(close(published_stage_fd) == 0);

    struct stat absent_stage{};
    const int absent_result = fstatat(parent_fd, stage_name.data(), &absent_stage, AT_SYMLINK_NOFOLLOW);
    const int absent_error = errno;
    REQUIRE(absent_result != 0);
    CHECK(absent_error == ENOENT);
    REQUIRE(close(validated_stage_fd) == 0);
    REQUIRE(close(parent_fd) == 0);
#endif
}

static void VerifyCrossDeviceBindingsAndBusy(const CrossDeviceContext &_context)
{
    const auto wrong_source_root = helper::StagingRootAuthorityTestAccess::Acquire(
        TakeCommittedTerminal(_context.request, _context.source_fd, _context.destination_parent_fd),
        _context.destination_root_fd,
        _context.destination_root_fd);
    REQUIRE_FALSE(wrong_source_root);
    CHECK(wrong_source_root.error() == helper::StagingRootAuthority::Error::SourceRootDeviceMismatch);
    const auto wrong_destination_root = helper::StagingRootAuthorityTestAccess::Acquire(
        TakeCommittedTerminal(_context.request, _context.source_fd, _context.destination_parent_fd),
        _context.source_root_fd,
        _context.source_root_fd);
    REQUIRE_FALSE(wrong_destination_root);
    CHECK(wrong_destination_root.error() == helper::StagingRootAuthority::Error::DestinationRootDeviceMismatch);

    {
        auto source_locked = helper::ProtectedRootLedger::Open(_context.source_root_fd);
        REQUIRE(source_locked);
        const auto source_busy = helper::StagingRootAuthorityTestAccess::Acquire(
            TakeCommittedTerminal(_context.request, _context.source_fd, _context.destination_parent_fd),
            _context.source_root_fd,
            _context.destination_root_fd);
        REQUIRE_FALSE(source_busy);
        CHECK(source_busy.error() == helper::StagingRootAuthority::Error::SourceRootBusy);
        auto destination_after_source_busy = helper::ProtectedRootLedger::Open(_context.destination_root_fd);
        REQUIRE(destination_after_source_busy);
    }
    {
        auto destination_locked = helper::ProtectedRootLedger::Open(_context.destination_root_fd);
        REQUIRE(destination_locked);
        const auto destination_busy = helper::StagingRootAuthorityTestAccess::Acquire(
            TakeCommittedTerminal(_context.request, _context.source_fd, _context.destination_parent_fd),
            _context.source_root_fd,
            _context.destination_root_fd);
        REQUIRE_FALSE(destination_busy);
        CHECK(destination_busy.error() == helper::StagingRootAuthority::Error::DestinationRootBusy);
        auto source_after_destination_busy = helper::ProtectedRootLedger::Open(_context.source_root_fd);
        REQUIRE(source_after_destination_busy);
    }
}

static void VerifyLockedSession(const CrossDeviceContext &_context)
{
    DeferredRootOpen source_contender{_context.source_root};
    DeferredRootOpen destination_contender{_context.destination_root};
    {
        auto session = helper::StagingRootAuthorityTestAccess::Acquire(
            TakeCommittedTerminal(_context.request, _context.source_fd, _context.destination_parent_fd),
            _context.source_root_fd,
            _context.destination_root_fd);
        REQUIRE(session);
        CHECK(helper::StagingRootAuthorityTestAccess::IsCreatedByCurrentProcess(*session));
        CHECK(fcntl(_context.source_root_fd, F_GETFD) >= 0);
        CHECK(fcntl(_context.destination_root_fd, F_GETFD) >= 0);
        CHECK(source_contender.Complete() == DeferredOpenResult::RootBusy);
        CHECK(destination_contender.Complete() == DeferredOpenResult::RootBusy);

        auto moved_session = std::move(*session);
        CHECK(helper::StagingRootAuthorityTestAccess::IsCreatedByCurrentProcess(moved_session));
        int result_pipe[2]{};
        REQUIRE(pipe(result_pipe) == 0);
        const pid_t child = fork();
        if( child == 0 ) {
            (void)close(result_pipe[0]);
            auto inherited_move = std::move(moved_session);
            const uint8_t value =
                helper::StagingRootAuthorityTestAccess::IsCreatedByCurrentProcess(inherited_move) ? 0 : 1;
            (void)WriteExact(result_pipe[1], &value, sizeof(value));
            (void)close(result_pipe[1]);
            _exit(value == 1 ? 0 : 1);
        }
        REQUIRE(child > 0);
        REQUIRE(close(result_pipe[1]) == 0);
        uint8_t result = 0;
        REQUIRE(ReadExactWithin(result_pipe[0], &result, sizeof(result)));
        REQUIRE(close(result_pipe[0]) == 0);
        int status = 0;
        REQUIRE(WaitForChild(child, status));
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
        CHECK(result == 1);
        CHECK(helper::StagingRootAuthorityTestAccess::IsCreatedByCurrentProcess(moved_session));
    }

    auto reopened_source = helper::ProtectedRootLedger::Open(_context.source_root_fd);
    REQUIRE(reopened_source);
    auto reopened_destination = helper::ProtectedRootLedger::Open(_context.destination_root_fd);
    REQUIRE(reopened_destination);
}

TEST_CASE(PREFIX "locks exact cross-device roots in a private move-only session", "[.cross-volume-stage-device]")
{
    constexpr std::string_view destination_root_environment = "WINCOMMANDER_VFS_DESTINATION_STAGE_ROOT";
    const char *const configured_destination_root = std::getenv(destination_root_environment.data());
    if( configured_destination_root == nullptr || configured_destination_root[0] == '\0' )
        SKIP(std::string{destination_root_environment} + " must name a disposable destination root");

    std::error_code filesystem_error;
    const std::filesystem::path destination_base{configured_destination_root};
    if( !std::filesystem::is_directory(destination_base, filesystem_error) || filesystem_error )
        SKIP(std::string{destination_root_environment} + " is not an accessible directory");
    RetainedDestinationFixture destination_fixture{destination_base};
    const auto destination_parent = destination_fixture.Directory() / "destination-parent";
    const auto destination_root = destination_fixture.Directory() / "destination-root";
    REQUIRE(std::filesystem::create_directory(destination_parent));
    REQUIRE(std::filesystem::create_directory(destination_root));

    TestDir source_workspace;
    const auto source_parent = source_workspace.directory / "source-parent";
    const auto source_root = source_workspace.directory / "source-root";
    REQUIRE(std::filesystem::create_directory(source_parent));
    REQUIRE(std::filesystem::create_directory(source_root));
    const auto source_path = source_parent / "source";
    const int created = open(source_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    REQUIRE(created >= 0);
    constexpr std::string_view contents{"cross-device staging roots"};
    REQUIRE(WriteExact(created, contents.data(), contents.size()));
    REQUIRE(close(created) == 0);
    const int source_fd = open(source_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    const int destination_parent_fd = open(destination_parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(source_fd >= 0);
    REQUIRE(destination_parent_fd >= 0);
    const auto request = Request(Header(1), source_fd, destination_parent_fd);
    if( request.source.device == request.destination_parent.device )
        SKIP("supplied destination root must be on a distinct device");
    const int source_root_fd = OpenProtectedRoot(source_root);
    const int destination_root_fd = OpenProtectedRoot(destination_root);
    const CrossDeviceContext context{
        .request = request,
        .source_fd = source_fd,
        .destination_parent_fd = destination_parent_fd,
        .source_root_fd = source_root_fd,
        .destination_root_fd = destination_root_fd,
        .source_root = source_root,
        .destination_root = destination_root,
    };
    VerifyCrossDeviceBindingsAndBusy(context);
    VerifyLockedSession(context);

    CHECK(std::filesystem::is_empty(source_root));
    CHECK(std::filesystem::is_empty(destination_root));
    CHECK(close(source_root_fd) == 0);
    CHECK(close(destination_root_fd) == 0);
    CHECK(close(source_fd) == 0);
    CHECK(close(destination_parent_fd) == 0);
}

TEST_CASE(PREFIX "runs a locked session through stage and lifecycle inspection without publication",
          "[.cross-volume-stage-device]")
{
    constexpr std::string_view destination_root_environment = "WINCOMMANDER_VFS_DESTINATION_STAGE_ROOT";
    const char *const configured_destination_root = std::getenv(destination_root_environment.data());
    if( configured_destination_root == nullptr || configured_destination_root[0] == '\0' )
        SKIP(std::string{destination_root_environment} + " must name a disposable destination root");

    std::error_code filesystem_error;
    const std::filesystem::path destination_base{configured_destination_root};
    if( !std::filesystem::is_directory(destination_base, filesystem_error) || filesystem_error )
        SKIP(std::string{destination_root_environment} + " is not an accessible directory");
    RunStagingSessionRunnerFixture(destination_base);
}

TEST_CASE(PREFIX "revalidates a sealed stage under a private publication barrier without publishing",
          "[.cross-volume-stage-device]")
{
    constexpr std::string_view destination_root_environment = "WINCOMMANDER_VFS_DESTINATION_STAGE_ROOT";
    const char *const configured_destination_root = std::getenv(destination_root_environment.data());
    if( configured_destination_root == nullptr || configured_destination_root[0] == '\0' )
        SKIP(std::string{destination_root_environment} + " must name a disposable destination root");

    std::error_code filesystem_error;
    const std::filesystem::path destination_base{configured_destination_root};
    if( !std::filesystem::is_directory(destination_base, filesystem_error) || filesystem_error )
        SKIP(std::string{destination_root_environment} + " is not an accessible directory");
    RunPublicationBarrierFixture(destination_base);
}

} // namespace CrossVolumeStagingHelperStagingRootsTests

#undef PREFIX
