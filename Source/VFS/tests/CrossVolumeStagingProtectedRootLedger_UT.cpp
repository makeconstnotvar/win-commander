// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "../../RoutedIO/source/CrossVolumeStagingProtectedRootLedger.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PREFIX "RoutedIO cross-volume staging protected-root ledger "

namespace CrossVolumeStagingProtectedRootLedgerTests {

namespace protocol = nc::routedio::cross_volume_staging;
namespace helper = protocol::helper;

constexpr int kProcessTimeoutMilliseconds = 5'000;

static protocol::Header Header(const uint8_t _correlation_byte)
{
    protocol::Header header;
    header.correlation[0] = _correlation_byte;
    return header;
}

static int OpenProtectedRoot(TestDir &_directory)
{
    REQUIRE(chmod(_directory.directory.c_str(), 0700) == 0);
    const int fd = open(_directory.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
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

static bool ReadExact(const int _fd, void *_buffer, size_t _size) noexcept
{
    auto *cursor = static_cast<char *>(_buffer);
    while( _size != 0 ) {
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

static bool WaitForChildWithin(const pid_t _child, int &_status) noexcept
{
    constexpr int kPollIntervalMilliseconds = 10;
    constexpr long kPollIntervalNanoseconds = kPollIntervalMilliseconds * 1'000'000L;
    for( int elapsed = 0; elapsed < kProcessTimeoutMilliseconds; elapsed += kPollIntervalMilliseconds ) {
        const pid_t result = ::waitpid(_child, &_status, WNOHANG);
        if( result == _child )
            return true;
        if( result < 0 ) {
            if( errno == EINTR )
                continue;
            return false;
        }
        struct timespec remaining{.tv_sec = 0, .tv_nsec = kPollIntervalNanoseconds};
        while( ::nanosleep(&remaining, &remaining) != 0 ) {
            if( errno != EINTR )
                return false;
        }
    }
    return ::waitpid(_child, &_status, WNOHANG) == _child;
}

enum class DeferredOpenResult : uint8_t {
    Opened = 1,
    RootBusy = 2,
    ForkedProcess = 3,
    OtherFailure = 4,
};

struct DeferredOpenAttempt final {
    pid_t child{-1};
    int start_fd{-1};
    int result_fd{-1};
};

class ProcessGroupCleanup final
{
public:
    explicit ProcessGroupCleanup(const pid_t _leader) noexcept : m_Leader(_leader) {}

    ~ProcessGroupCleanup()
    {
        if( m_Armed ) {
            (void)::kill(m_Leader, SIGKILL);
            (void)::kill(-m_Leader, SIGKILL);
        }
    }

    void Disarm() noexcept { m_Armed = false; }

private:
    pid_t m_Leader{-1};
    bool m_Armed{true};
};

static DeferredOpenResult OpenRootInChild(const std::filesystem::path &_root) noexcept
{
    const int root_fd = ::open(_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if( root_fd < 0 )
        return DeferredOpenResult::OtherFailure;
    const auto opened = helper::ProtectedRootLedger::Open(root_fd);
    (void)::close(root_fd);
    if( opened )
        return DeferredOpenResult::Opened;
    if( opened.error() == helper::ProtectedRootLedger::Error::RootBusy )
        return DeferredOpenResult::RootBusy;
    if( opened.error() == helper::ProtectedRootLedger::Error::ForkedProcess )
        return DeferredOpenResult::ForkedProcess;
    return DeferredOpenResult::OtherFailure;
}

static DeferredOpenAttempt SpawnDeferredOpenAttempt(const std::filesystem::path &_root)
{
    int start_pipe[2]{};
    int result_pipe[2]{};
    REQUIRE(pipe(start_pipe) == 0);
    REQUIRE(pipe(result_pipe) == 0);
    const pid_t child = fork();
    if( child == 0 ) {
        (void)::close(start_pipe[1]);
        (void)::close(result_pipe[0]);
        char start = '\0';
        if( !ReadExact(start_pipe[0], &start, sizeof(start)) || start != 'G' )
            _exit(20);
        (void)::close(start_pipe[0]);

        const auto result = OpenRootInChild(_root);
        const auto byte = static_cast<uint8_t>(result);
        if( !WriteExact(result_pipe[1], &byte, sizeof(byte)) )
            _exit(21);
        (void)::close(result_pipe[1]);
        _exit(0);
    }
    REQUIRE(child > 0);
    REQUIRE(close(start_pipe[0]) == 0);
    REQUIRE(close(result_pipe[1]) == 0);
    return {.child = child, .start_fd = start_pipe[1], .result_fd = result_pipe[0]};
}

static DeferredOpenResult CompleteDeferredOpenAttempt(DeferredOpenAttempt _attempt)
{
    constexpr char go = 'G';
    REQUIRE(WriteExact(_attempt.start_fd, &go, sizeof(go)));
    REQUIRE(close(_attempt.start_fd) == 0);
    uint8_t result = 0;
    REQUIRE(ReadExactWithin(_attempt.result_fd, &result, sizeof(result)));
    REQUIRE(close(_attempt.result_fd) == 0);
    int status = 0;
    REQUIRE(WaitForChildWithin(_attempt.child, status));
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
    return static_cast<DeferredOpenResult>(result);
}

static void CancelDeferredOpenAttempt(DeferredOpenAttempt _attempt)
{
    constexpr char cancel = 'C';
    REQUIRE(WriteExact(_attempt.start_fd, &cancel, sizeof(cancel)));
    REQUIRE(close(_attempt.start_fd) == 0);
    REQUIRE(close(_attempt.result_fd) == 0);
    int status = 0;
    REQUIRE(WaitForChildWithin(_attempt.child, status));
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 20);
}

static void
WriteFileAt(const int _root_fd, const std::string_view _name, const std::string_view _contents, const mode_t _mode)
{
    const int fd = openat(_root_fd, _name.data(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, _mode);
    REQUIRE(fd >= 0);
    REQUIRE(write(fd, _contents.data(), _contents.size()) == static_cast<ssize_t>(_contents.size()));
    REQUIRE(fchmod(fd, _mode) == 0);
    REQUIRE(close(fd) == 0);
}

static void AppendFileAt(const int _root_fd, const std::string_view _name, const std::string_view _contents)
{
    const int fd = openat(_root_fd, _name.data(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(fd >= 0);
    REQUIRE(write(fd, _contents.data(), _contents.size()) == static_cast<ssize_t>(_contents.size()));
    REQUIRE(close(fd) == 0);
}

template <size_t Count>
static std::string Hex(const std::array<uint8_t, Count> &_bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(_bytes.size() * 2);
    for( const uint8_t byte : _bytes ) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0F]);
    }
    return result;
}

static std::string RecordName(const helper::ArtifactID &_id)
{
    return ".wc-cross-volume-record-" + Hex(_id.bytes) + ".manifest";
}

static std::string SealManifestName(const helper::ArtifactID &_id)
{
    return ".wc-cross-volume-seal-" + Hex(_id.bytes) + ".manifest";
}

static std::string ArtifactName(const helper::ArtifactID &_id)
{
    return ".wc-cross-volume-artifact-" + Hex(_id.bytes) + ".data";
}

static protocol::ObjectSeal SealFromFD(const int _fd)
{
    struct stat status{};
    REQUIRE(fstat(_fd, &status) == 0);
    return {
        .device = static_cast<uint64_t>(status.st_dev),
        .inode = static_cast<uint64_t>(status.st_ino),
        .uid = static_cast<uint32_t>(status.st_uid),
        .gid = static_cast<uint32_t>(status.st_gid),
        .mode = static_cast<uint32_t>(status.st_mode),
        .flags = static_cast<uint32_t>(status.st_flags),
        .link_count = static_cast<uint64_t>(status.st_nlink),
        .byte_size = static_cast<uint64_t>(status.st_size),
        .birth_time = {.seconds = status.st_birthtimespec.tv_sec,
                       .nanoseconds = static_cast<uint32_t>(status.st_birthtimespec.tv_nsec)},
        .modification_time = {.seconds = status.st_mtimespec.tv_sec,
                              .nanoseconds = static_cast<uint32_t>(status.st_mtimespec.tv_nsec)},
        .status_change_time = {.seconds = status.st_ctimespec.tv_sec,
                               .nanoseconds = static_cast<uint32_t>(status.st_ctimespec.tv_nsec)},
    };
}

static protocol::ObjectSeal CreateArtifact(const int _root_fd, const helper::ArtifactID &_id)
{
    const auto name = ArtifactName(_id);
    const int fd = openat(_root_fd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    REQUIRE(fd >= 0);
    constexpr std::string_view contents{"sealed artifact"};
    REQUIRE(write(fd, contents.data(), contents.size()) == static_cast<ssize_t>(contents.size()));
    REQUIRE(fchmod(fd, 0600) == 0);
    const auto seal = SealFromFD(fd);
    REQUIRE(close(fd) == 0);
    return seal;
}

static std::string RoleName(const helper::ArtifactRole _role)
{
    return _role == helper::ArtifactRole::SourceSnapshot ? "source-snapshot" : "destination-stage";
}

static std::string SealedRecordContents(const int _root_fd,
                                        const protocol::Header &_header,
                                        const helper::ArtifactRole _role,
                                        const helper::ArtifactID &_id,
                                        const protocol::ObjectSeal &_seal)
{
    struct stat root{};
    REQUIRE(fstat(_root_fd, &root) == 0);
    const auto number = [](const auto _value) { return std::to_string(static_cast<uint64_t>(_value)); };
    return "schema=wincommander-cross-volume-ledger-v1\nroot_device=" + number(root.st_dev) +
           "\nroot_inode=" + number(root.st_ino) + "\ncorrelation=" + Hex(_header.correlation) +
           "\nrole=" + RoleName(_role) + "\nartifact=" + Hex(_id.bytes) +
           "\nstate=sealed\nartifact_device=" + number(_seal.device) + "\nartifact_inode=" + number(_seal.inode) +
           "\nartifact_uid=" + number(_seal.uid) + "\nartifact_gid=" + number(_seal.gid) +
           "\nartifact_mode=" + number(_seal.mode) + "\nartifact_flags=" + number(_seal.flags) +
           "\nartifact_nlink=" + number(_seal.link_count) + "\nartifact_size=" + number(_seal.byte_size) +
           "\nartifact_birth_seconds=" + number(_seal.birth_time.seconds) +
           "\nartifact_birth_nanoseconds=" + number(_seal.birth_time.nanoseconds) +
           "\nartifact_mtime_seconds=" + number(_seal.modification_time.seconds) +
           "\nartifact_mtime_nanoseconds=" + number(_seal.modification_time.nanoseconds) +
           "\nartifact_ctime_seconds=" + number(_seal.status_change_time.seconds) +
           "\nartifact_ctime_nanoseconds=" + number(_seal.status_change_time.nanoseconds) + "\n";
}

static void WriteSealedManifest(const int _root_fd,
                                const protocol::Header &_header,
                                const helper::ArtifactRole _role,
                                const helper::ArtifactID &_id,
                                const protocol::ObjectSeal &_seal)
{
    const auto name = SealManifestName(_id);
    const auto contents = SealedRecordContents(_root_fd, _header, _role, _id, _seal);
    WriteFileAt(_root_fd, name, contents, 0600);
}

static std::filesystem::path SingleLedgerRecord(const TestDir &_directory)
{
    std::filesystem::path record;
    for( const auto &entry : std::filesystem::directory_iterator{_directory.directory} ) {
        const auto name = entry.path().filename().string();
        if( name.starts_with(".wc-cross-volume-record-") ) {
            REQUIRE(record.empty());
            record = entry.path();
        }
    }
    REQUIRE_FALSE(record.empty());
    return record;
}

TEST_CASE(PREFIX "persists a reservation before exposing it and removes it only after a restart")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        const auto reservation = ledger.Reserve(Header(1), helper::ArtifactRole::DestinationStage);
        REQUIRE(reservation);
        CHECK(reservation->Correlation() == Header(1));
        CHECK(reservation->Role() == helper::ArtifactRole::DestinationStage);
        CHECK(reservation->ID().bytes != helper::ArtifactID{}.bytes);
        CHECK(ledger.ActiveReservationCount() == 1);
        CHECK_FALSE(ledger.Reconcile());
    }
    close(root_fd);

    const int restart_root_fd = OpenProtectedRoot(directory);
    auto reopened = helper::ProtectedRootLedger::Open(restart_root_fd);
    REQUIRE(reopened);
    auto restarted = std::move(*reopened);
    const auto result = restarted.Reconcile();
    REQUIRE(result);
    CHECK(result->removed_reservations == 1);
    CHECK(result->retained_records == 0);
    CHECK(result->ignored_entries == 0);
    CHECK(std::filesystem::is_empty(directory.directory));
    close(restart_root_fd);
}

TEST_CASE(PREFIX "rejects roots outside the helper-private 0700 contract")
{
    TestDir directory;
    REQUIRE(chmod(directory.directory.c_str(), 0750) == 0);
    const int unsafe_root_fd = open(directory.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(unsafe_root_fd >= 0);
    const auto unsafe = helper::ProtectedRootLedger::Open(unsafe_root_fd);
    REQUIRE_FALSE(unsafe);
    CHECK(unsafe.error() == helper::ProtectedRootLedger::Error::InvalidRoot);
    CHECK(close(unsafe_root_fd) == 0);

    const auto regular_path = directory.directory / "regular";
    const int regular_fd = open(regular_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    REQUIRE(regular_fd >= 0);
    REQUIRE(close(regular_fd) == 0);
    const int borrowed_regular_fd = open(regular_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(borrowed_regular_fd >= 0);
    const auto regular = helper::ProtectedRootLedger::Open(borrowed_regular_fd);
    REQUIRE_FALSE(regular);
    CHECK(regular.error() == helper::ProtectedRootLedger::Error::InvalidRoot);
    CHECK(close(borrowed_regular_fd) == 0);

    const int protected_root_fd = OpenProtectedRoot(directory);
    auto first = helper::ProtectedRootLedger::Open(protected_root_fd);
    REQUIRE(first);
    const auto second = helper::ProtectedRootLedger::Open(protected_root_fd);
    REQUIRE_FALSE(second);
    CHECK(second.error() == helper::ProtectedRootLedger::Error::RootBusy);
    CHECK(close(protected_root_fd) == 0);
}

TEST_CASE(PREFIX "serializes one protected root across helper processes")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    const auto contender = SpawnDeferredOpenAttempt(directory.directory);
    auto opened = helper::ProtectedRootLedger::Open(root_fd);
    if( !opened ) {
        CancelDeferredOpenAttempt(contender);
        FAIL("the parent must acquire the initially unlocked protected root");
    }
    {
        auto ledger = std::move(*opened);
        CHECK(CompleteDeferredOpenAttempt(contender) == DeferredOpenResult::RootBusy);
    }
    const auto reopened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(reopened);
    CHECK(std::filesystem::is_empty(directory.directory));
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "fails closed while the exact protected-root contract is stale")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        REQUIRE(chmod(directory.directory.c_str(), 0750) == 0);
        const auto reservation = ledger.Reserve(Header(1), helper::ArtifactRole::DestinationStage);
        REQUIRE_FALSE(reservation);
        CHECK(reservation.error() == helper::ProtectedRootLedger::Error::InvalidRoot);
        REQUIRE(chmod(directory.directory.c_str(), 0700) == 0);
    }
    const auto reopened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(reopened);
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "invalidates inherited ledger authority and releases a crashed holder while a descendant survives")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    int descendant_ready_pipe[2]{};
    int descendant_release_pipe[2]{};
    int descendant_done_pipe[2]{};
    int descendant_pid_pipe[2]{};
    REQUIRE(pipe(descendant_ready_pipe) == 0);
    REQUIRE(pipe(descendant_release_pipe) == 0);
    REQUIRE(pipe(descendant_done_pipe) == 0);
    REQUIRE(pipe(descendant_pid_pipe) == 0);

    const pid_t holder = fork();
    if( holder == 0 ) {
        if( ::setpgid(0, 0) != 0 )
            _exit(29);
        (void)::close(descendant_ready_pipe[0]);
        (void)::close(descendant_release_pipe[1]);
        (void)::close(descendant_done_pipe[0]);
        (void)::close(descendant_pid_pipe[0]);
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        if( !opened )
            _exit(30);
        auto ledger = std::move(*opened);
        const pid_t descendant = fork();
        if( descendant < 0 )
            _exit(31);
        if( descendant == 0 ) {
            (void)::close(descendant_pid_pipe[1]);
            const auto reservation = ledger.Reserve(Header(1), helper::ArtifactRole::DestinationStage);
            const auto inherited_open = helper::ProtectedRootLedger::Open(root_fd);
            const uint8_t inherited_authority_rejected =
                !reservation && reservation.error() == helper::ProtectedRootLedger::Error::InvalidRoot &&
                        !inherited_open && inherited_open.error() == helper::ProtectedRootLedger::Error::ForkedProcess
                    ? 1
                    : 0;
            if( !WriteExact(
                    descendant_ready_pipe[1], &inherited_authority_rejected, sizeof(inherited_authority_rejected)) )
                _exit(32);
            char release = '\0';
            if( !ReadExact(descendant_release_pipe[0], &release, sizeof(release)) || release != 'R' )
                _exit(33);
            if( !WriteExact(
                    descendant_done_pipe[1], &inherited_authority_rejected, sizeof(inherited_authority_rejected)) )
                _exit(34);
            (void)::close(descendant_done_pipe[1]);
            _exit(inherited_authority_rejected == 1 ? 0 : 35);
        }
        (void)::close(descendant_ready_pipe[1]);
        (void)::close(descendant_release_pipe[0]);
        (void)::close(descendant_done_pipe[1]);
        if( !WriteExact(descendant_pid_pipe[1], &descendant, sizeof(descendant)) )
            _exit(36);
        (void)::close(descendant_pid_pipe[1]);
        for( ;; )
            (void)::pause();
    }
    REQUIRE(holder > 0);
    ProcessGroupCleanup process_group_cleanup{holder};
    REQUIRE(close(descendant_ready_pipe[1]) == 0);
    REQUIRE(close(descendant_release_pipe[0]) == 0);
    REQUIRE(close(descendant_done_pipe[1]) == 0);
    REQUIRE(close(descendant_pid_pipe[1]) == 0);

    pid_t descendant = -1;
    REQUIRE(ReadExactWithin(descendant_pid_pipe[0], &descendant, sizeof(descendant)));
    REQUIRE(close(descendant_pid_pipe[0]) == 0);
    REQUIRE(descendant > 0);

    uint8_t inherited_authority_rejected = 0;
    REQUIRE(
        ReadExactWithin(descendant_ready_pipe[0], &inherited_authority_rejected, sizeof(inherited_authority_rejected)));
    REQUIRE(close(descendant_ready_pipe[0]) == 0);
    CHECK(inherited_authority_rejected == 1);

    const auto live_holder_contender = SpawnDeferredOpenAttempt(directory.directory);
    CHECK(CompleteDeferredOpenAttempt(live_holder_contender) == DeferredOpenResult::RootBusy);

    REQUIRE(kill(holder, SIGKILL) == 0);
    int holder_status = 0;
    REQUIRE(WaitForChildWithin(holder, holder_status));
    REQUIRE(WIFSIGNALED(holder_status));
    CHECK(WTERMSIG(holder_status) == SIGKILL);

    {
        const auto reopened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(reopened);
    }

    constexpr char release = 'R';
    REQUIRE(WriteExact(descendant_release_pipe[1], &release, sizeof(release)));
    REQUIRE(close(descendant_release_pipe[1]) == 0);
    uint8_t descendant_done = 0;
    REQUIRE(ReadExactWithin(descendant_done_pipe[0], &descendant_done, sizeof(descendant_done)));
    REQUIRE(close(descendant_done_pipe[0]) == 0);
    CHECK(descendant_done == 1);
    process_group_cleanup.Disarm();
    CHECK(std::filesystem::is_empty(directory.directory));
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "bounds live reservations by exact correlation")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    auto opened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(opened);
    auto ledger = std::move(*opened);

    auto first = ledger.Reserve(Header(1), helper::ArtifactRole::SourceSnapshot);
    REQUIRE(first);
    const auto same_correlation_other_role = ledger.Reserve(Header(1), helper::ArtifactRole::DestinationStage);
    REQUIRE(same_correlation_other_role);
    const auto duplicate = ledger.Reserve(Header(1), helper::ArtifactRole::SourceSnapshot);
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error() == helper::ProtectedRootLedger::Error::DuplicateCorrelation);

    for( uint8_t value = 2; value < helper::ProtectedRootLedger::kMaximumReservations; ++value )
        REQUIRE(ledger.Reserve(Header(value), helper::ArtifactRole::DestinationStage));
    CHECK(ledger.ActiveReservationCount() == helper::ProtectedRootLedger::kMaximumReservations);
    const auto overflow = ledger.Reserve(Header(helper::ProtectedRootLedger::kMaximumReservations + 1),
                                         helper::ArtifactRole::DestinationStage);
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error() == helper::ProtectedRootLedger::Error::CapacityExceeded);
    REQUIRE(ledger.Release(std::move(*first)));
    CHECK(ledger.ActiveReservationCount() == helper::ProtectedRootLedger::kMaximumReservations - 1);
    CHECK(ledger.Reserve(Header(helper::ProtectedRootLedger::kMaximumReservations + 1),
                         helper::ArtifactRole::DestinationStage));
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "rejects a durable correlation-role duplicate after restart")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        REQUIRE(ledger.Reserve(Header(1), helper::ArtifactRole::DestinationStage));
    }

    auto reopened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(reopened);
    auto restarted = std::move(*reopened);
    const auto duplicate = restarted.Reserve(Header(1), helper::ArtifactRole::DestinationStage);
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error() == helper::ProtectedRootLedger::Error::DuplicateCorrelation);
    CHECK(restarted.Reserve(Header(1), helper::ArtifactRole::SourceSnapshot));
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "retains malformed and unsafe records instead of treating them as cleanup authority")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    std::filesystem::path record;
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        auto reservation = ledger.Reserve(Header(1), helper::ArtifactRole::DestinationStage);
        REQUIRE(reservation);
        record = SingleLedgerRecord(directory);
        CHECK(chmod(record.c_str(), 0640) == 0);
        const auto release = ledger.Release(std::move(*reservation));
        REQUIRE_FALSE(release);
        CHECK(release.error() == helper::ProtectedRootLedger::Error::RecordRemoveFailed);
        CHECK(ledger.ActiveReservationCount() == 1);
    }

    WriteFileAt(root_fd,
                ".wc-cross-volume-record-0000000000000000000000000000000000000000000000000000000000000000.manifest",
                "corrupt\n",
                0600);
    WriteFileAt(root_fd, "unrelated", "payload", 0600);

    auto reopened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(reopened);
    auto restarted = std::move(*reopened);
    const auto result = restarted.Reconcile();
    REQUIRE(result);
    CHECK(result->removed_reservations == 0);
    CHECK(result->retained_records == 2);
    CHECK(result->ignored_entries == 1);
    CHECK(std::filesystem::exists(record));
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "retains a manifest with an embedded NUL instead of accepting its valid prefix")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    std::filesystem::path record;
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        const auto reservation = ledger.Reserve(Header(1), helper::ArtifactRole::DestinationStage);
        REQUIRE(reservation);
        record = SingleLedgerRecord(directory);
        AppendFileAt(root_fd, record.filename().string(), std::string_view{"\0junk", 5});
    }

    auto reopened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(reopened);
    auto restarted = std::move(*reopened);
    const auto result = restarted.Reconcile();
    REQUIRE(result);
    CHECK(result->removed_reservations == 0);
    CHECK(result->retained_records == 1);
    CHECK(result->retained_incomplete_artifacts == 0);
    CHECK(std::filesystem::exists(record));
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "classifies a fully sealed artifact read-only after restart")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    const auto header = Header(1);
    helper::ArtifactID id;
    std::filesystem::path artifact;
    std::filesystem::path record;
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        const auto reservation = ledger.Reserve(header, helper::ArtifactRole::DestinationStage);
        REQUIRE(reservation);
        id = reservation->ID();
        const auto seal = CreateArtifact(root_fd, id);
        WriteSealedManifest(root_fd, header, helper::ArtifactRole::DestinationStage, id, seal);
        artifact = directory.directory / ArtifactName(id);
        record = directory.directory / RecordName(id);
        CHECK_FALSE(ledger.Reconcile());
    }

    auto reopened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(reopened);
    auto restarted = std::move(*reopened);
    const auto result = restarted.Reconcile();
    REQUIRE(result);
    CHECK(result->removed_reservations == 0);
    CHECK(result->retained_records == 1);
    CHECK(result->ignored_entries == 1);
    CHECK(result->inspected_sealed_artifacts == 1);
    CHECK(result->exact_sealed_artifacts == 1);
    CHECK(result->retained_incomplete_artifacts == 0);
    CHECK(std::filesystem::exists(record));
    CHECK(std::filesystem::exists(artifact));
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "materializes an empty artifact through an append-only sealed companion manifest")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    const auto header = Header(1);
    helper::ArtifactID id;
    std::filesystem::path artifact;
    std::filesystem::path reservation_record;
    std::filesystem::path seal_manifest;
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        auto reservation = ledger.Reserve(header, helper::ArtifactRole::DestinationStage);
        REQUIRE(reservation);
        id = reservation->ID();
        REQUIRE(ledger.MaterializeEmptyAndSeal(std::move(*reservation)));
        CHECK(ledger.ActiveReservationCount() == 1);
        CHECK_FALSE(ledger.Reconcile());
        artifact = directory.directory / ArtifactName(id);
        reservation_record = directory.directory / RecordName(id);
        seal_manifest = directory.directory / SealManifestName(id);
    }

    struct stat artifact_status{};
    REQUIRE(stat(artifact.c_str(), &artifact_status) == 0);
    CHECK(S_ISREG(artifact_status.st_mode));
    CHECK((artifact_status.st_mode & 07777) == 0600);
    CHECK(artifact_status.st_nlink == 1);
    CHECK(artifact_status.st_size == 0);

    auto reopened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(reopened);
    auto restarted = std::move(*reopened);
    const auto result = restarted.Reconcile();
    REQUIRE(result);
    CHECK(result->removed_reservations == 0);
    CHECK(result->retained_records == 1);
    CHECK(result->ignored_entries == 1);
    CHECK(result->inspected_sealed_artifacts == 1);
    CHECK(result->exact_sealed_artifacts == 1);
    CHECK(result->retained_incomplete_artifacts == 0);
    CHECK(std::filesystem::exists(reservation_record));
    CHECK(std::filesystem::exists(seal_manifest));
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "consumes reservation capabilities once and rejects moved or reused tokens")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    auto opened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(opened);
    auto ledger = std::move(*opened);

    auto release_reservation = ledger.Reserve(Header(1), helper::ArtifactRole::DestinationStage);
    REQUIRE(release_reservation);
    auto moved_reservation = std::move(*release_reservation);
    const auto moved_from = ledger.Release(std::move(*release_reservation));
    REQUIRE_FALSE(moved_from);
    CHECK(moved_from.error() == helper::ProtectedRootLedger::Error::UnknownReservation);
    REQUIRE(ledger.Release(std::move(moved_reservation)));
    const auto reused_release = ledger.Release(std::move(moved_reservation));
    REQUIRE_FALSE(reused_release);
    CHECK(reused_release.error() == helper::ProtectedRootLedger::Error::UnknownReservation);

    auto materialize_reservation = ledger.Reserve(Header(2), helper::ArtifactRole::DestinationStage);
    REQUIRE(materialize_reservation);
    REQUIRE(ledger.MaterializeEmptyAndSeal(std::move(*materialize_reservation)));
    const auto reused_materialize = ledger.MaterializeEmptyAndSeal(std::move(*materialize_reservation));
    REQUIRE_FALSE(reused_materialize);
    CHECK(reused_materialize.error() == helper::ProtectedRootLedger::Error::UnknownReservation);

    auto source_snapshot = ledger.Reserve(Header(3), helper::ArtifactRole::SourceSnapshot);
    REQUIRE(source_snapshot);
    const auto rejected_source_snapshot = ledger.MaterializeEmptyAndSeal(std::move(*source_snapshot));
    REQUIRE_FALSE(rejected_source_snapshot);
    CHECK(rejected_source_snapshot.error() == helper::ProtectedRootLedger::Error::InvalidRole);
    const auto reused_source_snapshot = ledger.Release(std::move(*source_snapshot));
    REQUIRE_FALSE(reused_source_snapshot);
    CHECK(reused_source_snapshot.error() == helper::ProtectedRootLedger::Error::UnknownReservation);
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "keeps the durable sealed quota bounded across helper restart")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        for( uint8_t value = 1; value <= helper::ProtectedRootLedger::kMaximumReservations; ++value ) {
            auto reservation = ledger.Reserve(Header(value), helper::ArtifactRole::DestinationStage);
            REQUIRE(reservation);
            REQUIRE(ledger.MaterializeEmptyAndSeal(std::move(*reservation)));
        }
        CHECK(ledger.ActiveReservationCount() == helper::ProtectedRootLedger::kMaximumReservations);
    }

    auto reopened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(reopened);
    auto restarted = std::move(*reopened);
    const auto overflow = restarted.Reserve(Header(17), helper::ArtifactRole::DestinationStage);
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error() == helper::ProtectedRootLedger::Error::CapacityExceeded);
    const auto result = restarted.Reconcile();
    REQUIRE(result);
    CHECK(result->removed_reservations == 0);
    CHECK(result->retained_records == helper::ProtectedRootLedger::kMaximumReservations);
    CHECK(result->ignored_entries == helper::ProtectedRootLedger::kMaximumReservations);
    CHECK(result->inspected_sealed_artifacts == helper::ProtectedRootLedger::kMaximumReservations);
    CHECK(result->exact_sealed_artifacts == helper::ProtectedRootLedger::kMaximumReservations);
    CHECK(result->retained_incomplete_artifacts == 0);
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "retains a live reservation whenever its private artifact name is occupied")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    auto opened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(opened);
    auto ledger = std::move(*opened);
    auto reservation = ledger.Reserve(Header(1), helper::ArtifactRole::DestinationStage);
    REQUIRE(reservation);
    const auto record = directory.directory / RecordName(reservation->ID());
    const auto artifact = directory.directory / ArtifactName(reservation->ID());
    CreateArtifact(root_fd, reservation->ID());

    const auto materialized = ledger.MaterializeEmptyAndSeal(std::move(*reservation));
    REQUIRE_FALSE(materialized);
    CHECK(materialized.error() == helper::ProtectedRootLedger::Error::ArtifactCreateFailed);
    const auto release = ledger.Release(std::move(*reservation));
    REQUIRE_FALSE(release);
    CHECK(release.error() == helper::ProtectedRootLedger::Error::UnknownReservation);
    CHECK(ledger.ActiveReservationCount() == 1);
    CHECK(std::filesystem::exists(record));
    CHECK(std::filesystem::exists(artifact));
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "retains a sealed artifact whose canonical name resolves through a symlink")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    const auto header = Header(1);
    helper::ArtifactID id;
    std::filesystem::path record;
    std::filesystem::path artifact;
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        const auto reservation = ledger.Reserve(header, helper::ArtifactRole::SourceSnapshot);
        REQUIRE(reservation);
        id = reservation->ID();
        const auto seal = CreateArtifact(root_fd, id);
        WriteSealedManifest(root_fd, header, helper::ArtifactRole::SourceSnapshot, id, seal);
        const auto artifact_name = ArtifactName(id);
        REQUIRE(unlinkat(root_fd, artifact_name.c_str(), 0) == 0);
        WriteFileAt(root_fd, "artifact-symlink-target", "payload", 0600);
        REQUIRE(symlinkat("artifact-symlink-target", root_fd, artifact_name.c_str()) == 0);
        record = directory.directory / RecordName(id);
        artifact = directory.directory / artifact_name;
    }

    auto reopened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(reopened);
    auto restarted = std::move(*reopened);
    const auto result = restarted.Reconcile();
    REQUIRE(result);
    CHECK(result->removed_reservations == 0);
    CHECK(result->retained_records == 1);
    CHECK(result->ignored_entries == 2);
    CHECK(result->inspected_sealed_artifacts == 1);
    CHECK(result->exact_sealed_artifacts == 0);
    CHECK(result->retained_incomplete_artifacts == 1);
    CHECK(std::filesystem::exists(record));
    CHECK(std::filesystem::is_symlink(artifact));
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "retains any artifact without a fully exact sealed manifest")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    helper::ArtifactID incomplete_id;
    helper::ArtifactID missing_id;
    helper::ArtifactID mismatched_id;
    std::filesystem::path incomplete_artifact;
    std::filesystem::path missing_record;
    std::filesystem::path mismatched_artifact;
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        const auto incomplete = ledger.Reserve(Header(1), helper::ArtifactRole::SourceSnapshot);
        REQUIRE(incomplete);
        incomplete_id = incomplete->ID();
        CreateArtifact(root_fd, incomplete_id);
        incomplete_artifact = directory.directory / ArtifactName(incomplete_id);

        const auto missing = ledger.Reserve(Header(2), helper::ArtifactRole::DestinationStage);
        REQUIRE(missing);
        missing_id = missing->ID();
        const auto unavailable_artifact_seal = CreateArtifact(root_fd, missing_id);
        const auto missing_artifact_name = ArtifactName(missing_id);
        REQUIRE(unlinkat(root_fd, missing_artifact_name.c_str(), 0) == 0);
        WriteSealedManifest(
            root_fd, Header(2), helper::ArtifactRole::DestinationStage, missing_id, unavailable_artifact_seal);
        missing_record = directory.directory / RecordName(missing_id);

        const auto mismatched = ledger.Reserve(Header(3), helper::ArtifactRole::DestinationStage);
        REQUIRE(mismatched);
        mismatched_id = mismatched->ID();
        auto mismatched_artifact_seal = CreateArtifact(root_fd, mismatched_id);
        ++mismatched_artifact_seal.byte_size;
        WriteSealedManifest(
            root_fd, Header(3), helper::ArtifactRole::DestinationStage, mismatched_id, mismatched_artifact_seal);
        mismatched_artifact = directory.directory / ArtifactName(mismatched_id);
    }

    auto reopened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(reopened);
    auto restarted = std::move(*reopened);
    const auto result = restarted.Reconcile();
    REQUIRE(result);
    CHECK(result->removed_reservations == 0);
    CHECK(result->retained_records == 3);
    CHECK(result->ignored_entries == 2);
    CHECK(result->inspected_sealed_artifacts == 2);
    CHECK(result->exact_sealed_artifacts == 0);
    CHECK(result->retained_incomplete_artifacts == 3);
    CHECK(std::filesystem::exists(incomplete_artifact));
    CHECK(std::filesystem::exists(missing_record));
    CHECK(std::filesystem::exists(mismatched_artifact));
    CHECK(close(root_fd) == 0);
}

} // namespace CrossVolumeStagingProtectedRootLedgerTests

#undef PREFIX
