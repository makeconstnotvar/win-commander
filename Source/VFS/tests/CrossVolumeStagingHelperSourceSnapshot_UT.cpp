// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "../../RoutedIO/source/CrossVolumeStagingHelperDestinationStage.h"
#include "../../RoutedIO/source/CrossVolumeStagingHelperSourceSnapshot.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#define PREFIX "RoutedIO cross-volume staging helper source snapshot "

namespace CrossVolumeStagingHelperSourceSnapshotTests {

namespace protocol = nc::routedio::cross_volume_staging;
namespace codec = protocol::xpc_codec;
namespace helper = protocol::helper;

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
        .birth_time = TimestampFrom(status.st_birthtimespec),
        .modification_time = TimestampFrom(status.st_mtimespec),
        .status_change_time = TimestampFrom(status.st_ctimespec),
    };
}

static bool WriteAll(const int _fd, const void *_bytes, const size_t _size) noexcept
{
    const auto *buffer = static_cast<const char *>(_bytes);
    size_t offset = 0;
    while( offset < _size ) {
        ssize_t written = -1;
        do {
            written = write(_fd, buffer + offset, _size - offset);
        } while( written < 0 && errno == EINTR );
        if( written <= 0 )
            return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

constexpr int kChildResultTimeoutMilliseconds = 5'000;

static bool ReadExactWithin(const int _fd, void *_buffer, size_t _size) noexcept
{
    auto *cursor = static_cast<char *>(_buffer);
    while( _size != 0 ) {
        struct pollfd descriptor{.fd = _fd, .events = POLLIN, .revents = 0};
        int ready = -1;
        do {
            ready = ::poll(&descriptor, 1, kChildResultTimeoutMilliseconds);
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

static bool ReadChildResultAndWait(const pid_t _child, const int _result_fd, uint8_t &_result, int &_status) noexcept
{
    const bool received = ReadExactWithin(_result_fd, &_result, sizeof(_result));
    if( !received )
        (void)::kill(_child, SIGKILL);
    for( ;; ) {
        const pid_t waited = ::waitpid(_child, &_status, 0);
        if( waited == _child )
            return received;
        if( waited < 0 && errno != EINTR )
            return false;
    }
}

static int OpenProtectedRoot(const std::filesystem::path &_directory)
{
    REQUIRE(chmod(_directory.c_str(), 0700) == 0);
    const int fd = open(_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(fd >= 0);
    return fd;
}

class SourcePair final
{
public:
    SourcePair(const std::filesystem::path &_directory,
               const std::string_view _name,
               const std::vector<uint8_t> &_payload)
        : m_Path{_directory / std::string{_name}}, m_ParentPath{_directory}
    {
        const int created = open(m_Path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        REQUIRE(created >= 0);
        REQUIRE(WriteAll(created, _payload.data(), _payload.size()));
        REQUIRE(close(created) == 0);
        m_SourceFD = open(m_Path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        m_ParentFD = open(m_ParentPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        REQUIRE(m_SourceFD >= 0);
        REQUIRE(m_ParentFD >= 0);
    }

    SourcePair(const SourcePair &) = delete;
    SourcePair &operator=(const SourcePair &) = delete;
    ~SourcePair()
    {
        if( m_SourceFD >= 0 )
            close(m_SourceFD);
        if( m_ParentFD >= 0 )
            close(m_ParentFD);
    }

    [[nodiscard]] int SourceFD() const noexcept { return m_SourceFD; }
    [[nodiscard]] int ParentFD() const noexcept { return m_ParentFD; }
    [[nodiscard]] const std::filesystem::path &Path() const noexcept { return m_Path; }

private:
    int m_SourceFD{-1};
    int m_ParentFD{-1};
    std::filesystem::path m_Path;
    std::filesystem::path m_ParentPath;
};

static protocol::BeginRequest Request(const protocol::Header &_header, const SourcePair &_source)
{
    constexpr std::array<uint8_t, 8> destination_name{'d', 'e', 's', 't', '.', 't', 'x', 't'};
    const auto component = protocol::DestinationComponent::Create(destination_name);
    REQUIRE(component);
    return {
        .header = _header,
        .source = SealFromFD(_source.SourceFD()),
        .destination_parent = SealFromFD(_source.ParentFD()),
        .destination_name = *component,
    };
}

enum class TerminalRequest : uint8_t {
    Commit,
    Abort,
};

static helper::LeaseStore::TerminalLease TakeTerminal(const protocol::BeginRequest &_request,
                                                      const int _source_fd,
                                                      const int _destination_parent_fd,
                                                      const TerminalRequest _terminal_request = TerminalRequest::Commit)
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
    const auto validation_error = validated ? 0U : static_cast<unsigned>(validated.error());
    CAPTURE(validation_error);
    REQUIRE(validated);
    const auto granted = store.Grant(1, std::move(*validated));
    REQUIRE(granted);
    std::expected<helper::LeaseStore::TerminalLease, helper::LeaseStore::Error> terminal = [&] {
        if( _terminal_request == TerminalRequest::Commit ) {
            return store.Take(1,
                              protocol::CommitRequest{
                                  .header = granted->header,
                                  .lease = *granted,
                              });
        }
        return store.Take(1,
                          protocol::AbortRequest{
                              .header = granted->header,
                              .lease = *granted,
                          });
    }();
    REQUIRE(terminal);
    return std::move(*terminal);
}

static helper::LeaseStore::TerminalLease TakeTerminal(const protocol::BeginRequest &_request,
                                                      const SourcePair &_source,
                                                      const TerminalRequest _terminal_request = TerminalRequest::Commit)
{
    return TakeTerminal(_request, _source.SourceFD(), _source.ParentFD(), _terminal_request);
}

static std::vector<uint8_t> ReadFile(const std::filesystem::path &_path)
{
    const int fd = open(_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(fd >= 0);
    struct stat status{};
    REQUIRE(fstat(fd, &status) == 0);
    REQUIRE(status.st_size >= 0);
    std::vector<uint8_t> contents(static_cast<size_t>(status.st_size));
    size_t offset = 0;
    while( offset < contents.size() ) {
        const ssize_t read = ::read(fd, contents.data() + offset, contents.size() - offset);
        REQUIRE(read > 0);
        offset += static_cast<size_t>(read);
    }
    REQUIRE(close(fd) == 0);
    return contents;
}

static std::filesystem::path SingleArtifact(const std::filesystem::path &_root)
{
    std::filesystem::path artifact;
    for( const auto &entry : std::filesystem::directory_iterator{_root} ) {
        const auto name = entry.path().filename().string();
        if( name.starts_with(".wc-cross-volume-artifact-") ) {
            REQUIRE(artifact.empty());
            artifact = entry.path();
        }
    }
    REQUIRE_FALSE(artifact.empty());
    return artifact;
}

static size_t CountEntries(const std::filesystem::path &_root, const std::string_view _prefix)
{
    size_t count = 0;
    for( const auto &entry : std::filesystem::directory_iterator{_root} ) {
        if( entry.path().filename().string().starts_with(_prefix) )
            ++count;
    }
    return count;
}

/**
 * A supplied cross-device root is intentionally retained.  The test has no cleanup authority over a same-UID
 * actor's namespace; callers must point it at a disposable root and inspect/remove retained state explicitly.
 */
class RetainedDestinationFixture final
{
public:
    explicit RetainedDestinationFixture(const std::filesystem::path &_base) : m_Base{std::filesystem::canonical(_base)}
    {
        std::string pattern = (m_Base / ".wc-stage-ut.XXXXXX").string();
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

struct ReservationTamperContext final {
    int root_fd{-1};
    bool called{false};
    bool succeeded{false};
};

static bool RemovePrimaryReservationAtBeforeArtifactCreate(const helper::SourceSnapshotWriter::CancellationPoint _point,
                                                           void *_context) noexcept
{
    if( _point != helper::SourceSnapshotWriter::CancellationPoint::BeforeArtifactCreate )
        return false;
    auto &context = *static_cast<ReservationTamperContext *>(_context);
    context.called = true;
    const int scan_fd = fcntl(context.root_fd, F_DUPFD_CLOEXEC, 0);
    if( scan_fd < 0 )
        return false;
    DIR *directory = fdopendir(scan_fd);
    if( directory == nullptr ) {
        close(scan_fd);
        return false;
    }
    while( const dirent *entry = readdir(directory) ) {
        if( std::string_view{entry->d_name}.starts_with(".wc-cross-volume-record-") ) {
            context.succeeded = unlinkat(context.root_fd, entry->d_name, 0) == 0;
            break;
        }
    }
    closedir(directory);
    return false;
}

struct PathReplacementContext final {
    const char *source_path{nullptr};
    const char *displaced_path{nullptr};
    const uint8_t *replacement{nullptr};
    size_t replacement_size{0};
    bool called{false};
    bool succeeded{false};
};

static bool ReplacePathAtBeforeArtifactCreate(const helper::SourceSnapshotWriter::CancellationPoint _point,
                                              void *_context) noexcept
{
    if( _point != helper::SourceSnapshotWriter::CancellationPoint::BeforeArtifactCreate )
        return false;
    auto &context = *static_cast<PathReplacementContext *>(_context);
    context.called = true;
    if( rename(context.source_path, context.displaced_path) != 0 )
        return false;
    const int replacement = open(context.source_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if( replacement < 0 )
        return false;
    context.succeeded = WriteAll(replacement, context.replacement, context.replacement_size) &&
                        fchmod(replacement, 0600) == 0 && close(replacement) == 0;
    return false;
}

struct SourceMutationContext final {
    helper::SourceSnapshotWriter::CancellationPoint mutation_point;
    int source_fd{-1};
    off_t write_offset{0};
    bool called{false};
    bool succeeded{false};
};

static bool MutateHeldSourceAt(const helper::SourceSnapshotWriter::CancellationPoint _point, void *_context) noexcept
{
    auto &context = *static_cast<SourceMutationContext *>(_context);
    if( _point != context.mutation_point )
        return false;
    context.called = true;
    constexpr char marker = '!';
    context.succeeded = pwrite(context.source_fd, &marker, sizeof(marker), context.write_offset) == sizeof(marker);
    return false;
}

struct CancelContext final {
    helper::SourceSnapshotWriter::CancellationPoint cancellation_point;
};

static bool CancelAt(const helper::SourceSnapshotWriter::CancellationPoint _point, void *_context) noexcept
{
    return _point == static_cast<CancelContext *>(_context)->cancellation_point;
}

struct LedgerReentryContext final {
    helper::ProtectedRootLedger *ledger{nullptr};
    bool called{false};
    bool observed_active_reservation{false};
    bool observed_reconcile_busy{false};
};

static bool ObserveLedgerAtBeforeArtifactCreate(const helper::SourceSnapshotWriter::CancellationPoint _point,
                                                void *_context) noexcept
{
    if( _point != helper::SourceSnapshotWriter::CancellationPoint::BeforeArtifactCreate )
        return false;
    auto &context = *static_cast<LedgerReentryContext *>(_context);
    context.called = true;
    context.observed_active_reservation = context.ledger->ActiveReservationCount() == 1;
    const auto reconciliation = context.ledger->Reconcile();
    context.observed_reconcile_busy =
        !reconciliation && reconciliation.error() == helper::ProtectedRootLedger::Error::Busy;
    return false;
}

struct DestinationStageReentryContext final {
    helper::ProtectedRootLedger *ledger{nullptr};
    bool called{false};
    bool observed_active_reservation{false};
    bool observed_reconcile_busy{false};
};

static bool CancelAndObserveDestinationStage(const helper::DestinationStageWriter::CancellationPoint _point,
                                             void *_context) noexcept
{
    if( _point != helper::DestinationStageWriter::CancellationPoint::BeforeArtifactCreate )
        return false;
    auto &context = *static_cast<DestinationStageReentryContext *>(_context);
    context.called = true;
    context.observed_active_reservation = context.ledger->ActiveReservationCount() == 1;
    const auto reconciliation = context.ledger->Reconcile();
    context.observed_reconcile_busy =
        !reconciliation && reconciliation.error() == helper::ProtectedRootLedger::Error::Busy;
    return true;
}

struct DestinationParentMutationContext final {
    int descriptor{-1};
    bool called{false};
    bool succeeded{false};
};

static bool
MutateDestinationParentAtBeforeArtifactCreate(const helper::DestinationStageWriter::CancellationPoint _point,
                                              void *_context) noexcept
{
    if( _point != helper::DestinationStageWriter::CancellationPoint::BeforeArtifactCreate )
        return false;
    auto &context = *static_cast<DestinationParentMutationContext *>(_context);
    context.called = true;
    struct stat status{};
    if( fstat(context.descriptor, &status) != 0 )
        return false;
    const auto changed_mode = static_cast<mode_t>((status.st_mode & 07777) ^ S_IXUSR);
    context.succeeded = fchmod(context.descriptor, changed_mode) == 0;
    return false;
}

TEST_CASE(PREFIX
          "copies exact descriptor bytes into a sealed private artifact and reconciles it read-only after restart")
{
    TestDir workspace;
    const auto source_directory = workspace.directory / "source";
    const auto root_directory = workspace.directory / "root";
    REQUIRE(std::filesystem::create_directory(source_directory));
    REQUIRE(std::filesystem::create_directory(root_directory));
    std::vector<uint8_t> payload(1024 * 1024 + 31);
    for( size_t index = 0; index != payload.size(); ++index )
        payload[index] = static_cast<uint8_t>((index * 31) & 0xFF);
    payload[17] = 0;
    payload[8193] = 0xFF;
    SourcePair source{source_directory, "source", payload};
    const auto request = Request(Header(1), source);
    REQUIRE(lseek(source.SourceFD(), 17, SEEK_SET) == 17);
    const int root_fd = OpenProtectedRoot(root_directory);

    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        auto snapshot = helper::SourceSnapshotWriter::Create(ledger, TakeTerminal(request, source), {});
        REQUIRE(snapshot);
        CHECK(snapshot->Correlation() == request.header);
        CHECK(snapshot->SourceSeal() == request.source);
        const auto artifact = SingleArtifact(root_directory);
        const int artifact_fd = open(artifact.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        REQUIRE(artifact_fd >= 0);
        const auto artifact_seal = SealFromFD(artifact_fd);
        struct stat status{};
        REQUIRE(fstat(artifact_fd, &status) == 0);
        CHECK((status.st_mode & 07777) == 0600);
        CHECK(status.st_nlink == 1);
        CHECK(close(artifact_fd) == 0);
        CHECK(snapshot->ArtifactSeal() == artifact_seal);
        CHECK(ReadFile(artifact) == payload);
        CHECK(CountEntries(root_directory, ".wc-cross-volume-record-") == 1);
        CHECK(CountEntries(root_directory, ".wc-cross-volume-seal-") == 1);
    }

    auto reopened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(reopened);
    auto restarted = std::move(*reopened);
    const auto reconcile = restarted.Reconcile();
    REQUIRE(reconcile);
    CHECK(reconcile->removed_reservations == 0);
    CHECK(reconcile->retained_records == 1);
    CHECK(reconcile->inspected_sealed_artifacts == 1);
    CHECK(reconcile->exact_sealed_artifacts == 1);
    CHECK(reconcile->retained_incomplete_artifacts == 0);
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "runs cancellation probes outside the ledger mutex")
{
    TestDir workspace;
    const auto source_directory = workspace.directory / "source";
    const auto root_directory = workspace.directory / "root";
    REQUIRE(std::filesystem::create_directory(source_directory));
    REQUIRE(std::filesystem::create_directory(root_directory));
    SourcePair source{source_directory, "source", std::vector<uint8_t>{'r', 'e', 'e', 'n', 't', 'r', 'y'}};
    const int root_fd = OpenProtectedRoot(root_directory);
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        LedgerReentryContext context{.ledger = &ledger};
        const helper::SourceSnapshotWriter::Cancellation cancellation{
            .probe = ObserveLedgerAtBeforeArtifactCreate,
            .context = &context,
        };
        auto snapshot = helper::SourceSnapshotWriter::Create(
            ledger, TakeTerminal(Request(Header(21), source), source), cancellation);
        REQUIRE(snapshot);
        CHECK(context.called);
        CHECK(context.observed_active_reservation);
        CHECK(context.observed_reconcile_busy);
        CHECK(ledger.ActiveReservationCount() == 1);
    }
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "fails closed when a cancellation probe removes the primary reservation")
{
    TestDir workspace;
    const auto source_directory = workspace.directory / "source";
    const auto root_directory = workspace.directory / "root";
    REQUIRE(std::filesystem::create_directory(source_directory));
    REQUIRE(std::filesystem::create_directory(root_directory));
    SourcePair source{source_directory, "source", std::vector<uint8_t>{'s', 'e', 'a', 'l'}};
    const int root_fd = OpenProtectedRoot(root_directory);
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        ReservationTamperContext context{.root_fd = root_fd};
        const helper::SourceSnapshotWriter::Cancellation cancellation{
            .probe = RemovePrimaryReservationAtBeforeArtifactCreate,
            .context = &context,
        };
        const auto snapshot = helper::SourceSnapshotWriter::Create(
            ledger, TakeTerminal(Request(Header(22), source), source), cancellation);
        REQUIRE_FALSE(snapshot);
        CHECK(snapshot.error() == helper::SourceSnapshotWriter::Error::ReservationFailed);
        CHECK(context.called);
        CHECK(context.succeeded);
        CHECK(CountEntries(root_directory, ".wc-cross-volume-record-") == 0);
        CHECK(CountEntries(root_directory, ".wc-cross-volume-artifact-") == 0);
        CHECK(CountEntries(root_directory, ".wc-cross-volume-seal-") == 0);
    }
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "rejects source pathname replacement as exact source-seal drift")
{
    TestDir workspace;
    const auto source_directory = workspace.directory / "source";
    const auto root_directory = workspace.directory / "root";
    REQUIRE(std::filesystem::create_directory(source_directory));
    REQUIRE(std::filesystem::create_directory(root_directory));
    const std::vector<uint8_t> original{'o', 'l', 'd', 0, 'b', 'y', 't', 'e', 's'};
    constexpr std::array<uint8_t, 7> replacement{'n', 'e', 'w', 0, 'f', 'i', 'l'};
    SourcePair source{source_directory, "source", original};
    const auto request = Request(Header(2), source);
    const auto displaced = source_directory / "held-source";
    PathReplacementContext replacement_context{
        .source_path = source.Path().c_str(),
        .displaced_path = displaced.c_str(),
        .replacement = replacement.data(),
        .replacement_size = replacement.size(),
    };
    const int root_fd = OpenProtectedRoot(root_directory);
    auto opened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(opened);
    auto ledger = std::move(*opened);
    const helper::SourceSnapshotWriter::Cancellation cancellation{
        .probe = ReplacePathAtBeforeArtifactCreate,
        .context = &replacement_context,
    };
    const auto snapshot = helper::SourceSnapshotWriter::Create(ledger, TakeTerminal(request, source), cancellation);
    REQUIRE_FALSE(snapshot);
    CHECK(snapshot.error() == helper::SourceSnapshotWriter::Error::SourceStale);
    CHECK(replacement_context.called);
    CHECK(replacement_context.succeeded);
    CHECK(SealFromFD(source.SourceFD()) != request.source);
    CHECK(ReadFile(source.Path()) == std::vector<uint8_t>{replacement.begin(), replacement.end()});
    CHECK(CountEntries(root_directory, ".wc-cross-volume-record-") == 1);
    CHECK(CountEntries(root_directory, ".wc-cross-volume-artifact-") == 0);
    CHECK(CountEntries(root_directory, ".wc-cross-volume-seal-") == 0);
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "rejects source drift at every copy-to-seal checkpoint without a sealed companion")
{
    constexpr std::array checkpoints{
        helper::SourceSnapshotWriter::CancellationPoint::BeforeArtifactCreate,
        helper::SourceSnapshotWriter::CancellationPoint::AfterCopy,
        helper::SourceSnapshotWriter::CancellationPoint::BeforeSealManifest,
    };
    for( const auto checkpoint : checkpoints ) {
        TestDir workspace;
        const auto source_directory = workspace.directory / "source";
        const auto root_directory = workspace.directory / "root";
        REQUIRE(std::filesystem::create_directory(source_directory));
        REQUIRE(std::filesystem::create_directory(root_directory));
        const std::vector<uint8_t> payload(32 * 1024 + 9, 0x5A);
        SourcePair source{source_directory, "source", payload};
        const auto request = Request(Header(static_cast<uint8_t>(10 + static_cast<uint8_t>(checkpoint))), source);
        const int mutator = open(source.Path().c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        REQUIRE(mutator >= 0);
        const auto close_mutator = at_scope_end([mutator] { close(mutator); });
        SourceMutationContext mutation{
            .mutation_point = checkpoint,
            .source_fd = mutator,
            .write_offset = static_cast<off_t>(payload.size()),
        };
        const int root_fd = OpenProtectedRoot(root_directory);
        {
            auto opened = helper::ProtectedRootLedger::Open(root_fd);
            REQUIRE(opened);
            auto ledger = std::move(*opened);
            const helper::SourceSnapshotWriter::Cancellation cancellation{
                .probe = MutateHeldSourceAt,
                .context = &mutation,
            };
            const auto snapshot =
                helper::SourceSnapshotWriter::Create(ledger, TakeTerminal(request, source), cancellation);
            REQUIRE_FALSE(snapshot);
            CHECK(snapshot.error() == helper::SourceSnapshotWriter::Error::SourceStale);
            CHECK(mutation.called);
            CHECK(mutation.succeeded);
            CHECK(CountEntries(root_directory, ".wc-cross-volume-seal-") == 0);
            CHECK(ledger.ActiveReservationCount() == 1);
            if( checkpoint == helper::SourceSnapshotWriter::CancellationPoint::BeforeArtifactCreate )
                CHECK(CountEntries(root_directory, ".wc-cross-volume-artifact-") == 0);
            else
                CHECK(CountEntries(root_directory, ".wc-cross-volume-artifact-") == 1);
        }
        auto reopened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(reopened);
        auto restarted = std::move(*reopened);
        const auto reconcile = restarted.Reconcile();
        REQUIRE(reconcile);
        if( checkpoint == helper::SourceSnapshotWriter::CancellationPoint::BeforeArtifactCreate ) {
            CHECK(reconcile->removed_reservations == 1);
            CHECK(reconcile->retained_records == 0);
        }
        else {
            CHECK(reconcile->removed_reservations == 0);
            CHECK(reconcile->retained_records == 1);
            CHECK(reconcile->retained_incomplete_artifacts == 1);
        }
        CHECK(reconcile->exact_sealed_artifacts == 0);
        CHECK(close(root_fd) == 0);
    }
}

TEST_CASE(PREFIX "cancellation consumes terminal authority and never seals an incomplete artifact")
{
    TestDir workspace;
    const auto source_directory = workspace.directory / "source";
    const auto root_directory = workspace.directory / "root";
    REQUIRE(std::filesystem::create_directory(source_directory));
    REQUIRE(std::filesystem::create_directory(root_directory));
    const std::vector<uint8_t> payload(32 * 1024 + 5, 0x6B);
    SourcePair source{source_directory, "source", payload};
    const int root_fd = OpenProtectedRoot(root_directory);
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);

        CancelContext before_reservation{
            .cancellation_point = helper::SourceSnapshotWriter::CancellationPoint::BeforeReservation,
        };
        const helper::SourceSnapshotWriter::Cancellation cancel_before_reservation{
            .probe = CancelAt,
            .context = &before_reservation,
        };
        const auto aborted = helper::SourceSnapshotWriter::Create(
            ledger,
            TakeTerminal(Request(Header(30), source), source, TerminalRequest::Abort),
            cancel_before_reservation);
        REQUIRE_FALSE(aborted);
        CHECK(aborted.error() == helper::SourceSnapshotWriter::Error::InvalidTerminalLease);
        CHECK(ledger.ActiveReservationCount() == 0);
        CHECK(std::filesystem::is_empty(root_directory));

        auto cancelled_terminal = TakeTerminal(Request(Header(31), source), source);
        const auto cancelled =
            helper::SourceSnapshotWriter::Create(ledger, std::move(cancelled_terminal), cancel_before_reservation);
        REQUIRE_FALSE(cancelled);
        CHECK(cancelled.error() == helper::SourceSnapshotWriter::Error::Cancelled);
        CHECK(ledger.ActiveReservationCount() == 0);
        CHECK(std::filesystem::is_empty(root_directory));

        const auto reused = helper::SourceSnapshotWriter::Create(ledger, std::move(cancelled_terminal), {});
        REQUIRE_FALSE(reused);
        CHECK(reused.error() == helper::SourceSnapshotWriter::Error::InvalidTerminalLease);
        CHECK(ledger.ActiveReservationCount() == 0);

        CancelContext before_seal{
            .cancellation_point = helper::SourceSnapshotWriter::CancellationPoint::BeforeSealManifest,
        };
        const helper::SourceSnapshotWriter::Cancellation cancel_before_seal{
            .probe = CancelAt,
            .context = &before_seal,
        };
        const auto incomplete = helper::SourceSnapshotWriter::Create(
            ledger, TakeTerminal(Request(Header(32), source), source), cancel_before_seal);
        REQUIRE_FALSE(incomplete);
        CHECK(incomplete.error() == helper::SourceSnapshotWriter::Error::Cancelled);
        CHECK(ledger.ActiveReservationCount() == 1);
        CHECK(CountEntries(root_directory, ".wc-cross-volume-record-") == 1);
        CHECK(CountEntries(root_directory, ".wc-cross-volume-artifact-") == 1);
        CHECK(CountEntries(root_directory, ".wc-cross-volume-seal-") == 0);
    }
    auto reopened = helper::ProtectedRootLedger::Open(root_fd);
    REQUIRE(reopened);
    auto restarted = std::move(*reopened);
    const auto reconcile = restarted.Reconcile();
    REQUIRE(reconcile);
    CHECK(reconcile->removed_reservations == 0);
    CHECK(reconcile->retained_records == 1);
    CHECK(reconcile->retained_incomplete_artifacts == 1);
    CHECK(reconcile->exact_sealed_artifacts == 0);
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "rejects an inherited committed terminal lease before source reservation")
{
    TestDir workspace;
    const auto source_directory = workspace.directory / "source";
    const auto root_directory = workspace.directory / "root";
    REQUIRE(std::filesystem::create_directory(source_directory));
    REQUIRE(std::filesystem::create_directory(root_directory));
    SourcePair source{source_directory, "source", std::vector<uint8_t>{'p', 'i', 'd'}};
    const int root_fd = OpenProtectedRoot(root_directory);
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        auto terminal = TakeTerminal(Request(Header(35), source), source);

        int result_pipe[2]{};
        REQUIRE(pipe(result_pipe) == 0);
        const pid_t child = fork();
        REQUIRE(child >= 0);
        if( child == 0 ) {
            (void)::close(result_pipe[0]);
            const auto snapshot = helper::SourceSnapshotWriter::Create(ledger, std::move(terminal), {});
            const uint8_t result =
                !snapshot && snapshot.error() == helper::SourceSnapshotWriter::Error::InvalidTerminalLease ? 1 : 0;
            const bool wrote = WriteAll(result_pipe[1], &result, sizeof(result));
            (void)::close(result_pipe[1]);
            _exit(wrote && result == 1 ? 0 : 20);
        }

        REQUIRE(close(result_pipe[1]) == 0);
        uint8_t result = 0;
        int status = 0;
        const bool completed = ReadChildResultAndWait(child, result_pipe[0], result, status);
        REQUIRE(close(result_pipe[0]) == 0);
        REQUIRE(completed);
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
        CHECK(result == 1);
        CHECK(ledger.ActiveReservationCount() == 0);
        CHECK(CountEntries(root_directory, ".wc-cross-volume-record-") == 0);
        CHECK(CountEntries(root_directory, ".wc-cross-volume-artifact-") == 0);
        CHECK(CountEntries(root_directory, ".wc-cross-volume-seal-") == 0);
    }
    CHECK(close(root_fd) == 0);
}

TEST_CASE(PREFIX "rejects an inherited source snapshot before destination binding or reservation")
{
    TestDir workspace;
    const auto source_directory = workspace.directory / "source";
    const auto source_root_directory = workspace.directory / "source-root";
    const auto destination_root_directory = workspace.directory / "destination-root";
    REQUIRE(std::filesystem::create_directory(source_directory));
    REQUIRE(std::filesystem::create_directory(source_root_directory));
    REQUIRE(std::filesystem::create_directory(destination_root_directory));
    SourcePair source{source_directory, "source", std::vector<uint8_t>{'s', 'n', 'a', 'p'}};
    const auto request = Request(Header(36), source);
    const int source_root_fd = OpenProtectedRoot(source_root_directory);
    const int destination_root_fd = OpenProtectedRoot(destination_root_directory);
    {
        auto opened_source = helper::ProtectedRootLedger::Open(source_root_fd);
        auto opened_destination = helper::ProtectedRootLedger::Open(destination_root_fd);
        REQUIRE(opened_source);
        REQUIRE(opened_destination);
        auto source_ledger = std::move(*opened_source);
        auto destination_ledger = std::move(*opened_destination);
        auto snapshot = helper::SourceSnapshotWriter::Create(source_ledger, TakeTerminal(request, source), {});
        REQUIRE(snapshot);

        int result_pipe[2]{};
        REQUIRE(pipe(result_pipe) == 0);
        const pid_t child = fork();
        REQUIRE(child >= 0);
        if( child == 0 ) {
            (void)::close(result_pipe[0]);
            const auto stage = helper::DestinationStageWriter::Create(destination_ledger, std::move(*snapshot), {});
            const uint8_t result =
                !stage && stage.error() == helper::DestinationStageWriter::Error::InvalidSourceSnapshot ? 1 : 0;
            const bool wrote = WriteAll(result_pipe[1], &result, sizeof(result));
            (void)::close(result_pipe[1]);
            _exit(wrote && result == 1 ? 0 : 21);
        }

        REQUIRE(close(result_pipe[1]) == 0);
        uint8_t result = 0;
        int status = 0;
        const bool completed = ReadChildResultAndWait(child, result_pipe[0], result, status);
        REQUIRE(close(result_pipe[0]) == 0);
        REQUIRE(completed);
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
        CHECK(result == 1);
        CHECK(destination_ledger.ActiveReservationCount() == 0);
        CHECK(CountEntries(destination_root_directory, ".wc-cross-volume-record-") == 0);
        CHECK(CountEntries(destination_root_directory, ".wc-cross-volume-artifact-") == 0);
        CHECK(CountEntries(destination_root_directory, ".wc-cross-volume-seal-") == 0);
    }
    CHECK(close(source_root_fd) == 0);
    CHECK(close(destination_root_fd) == 0);
}

TEST_CASE(PREFIX "does not create a destination stage when reviewed source and destination remain on one device")
{
    TestDir workspace;
    const auto source_directory = workspace.directory / "source";
    const auto source_root_directory = workspace.directory / "source-root";
    const auto destination_root_directory = workspace.directory / "destination-root";
    REQUIRE(std::filesystem::create_directory(source_directory));
    REQUIRE(std::filesystem::create_directory(source_root_directory));
    REQUIRE(std::filesystem::create_directory(destination_root_directory));
    SourcePair source{source_directory, "source", std::vector<uint8_t>{'s', 't', 'a', 'g', 'e'}};
    const auto request = Request(Header(40), source);
    const int source_root_fd = OpenProtectedRoot(source_root_directory);
    const int destination_root_fd = OpenProtectedRoot(destination_root_directory);
    {
        auto opened_source = helper::ProtectedRootLedger::Open(source_root_fd);
        auto opened_destination = helper::ProtectedRootLedger::Open(destination_root_fd);
        REQUIRE(opened_source);
        REQUIRE(opened_destination);
        auto source_ledger = std::move(*opened_source);
        auto destination_ledger = std::move(*opened_destination);
        auto snapshot = helper::SourceSnapshotWriter::Create(source_ledger, TakeTerminal(request, source), {});
        REQUIRE(snapshot);

        const auto stage = helper::DestinationStageWriter::Create(destination_ledger, std::move(*snapshot), {});
        REQUIRE_FALSE(stage);
        CHECK(stage.error() == helper::DestinationStageWriter::Error::CrossVolumeBindingFailed);
        CHECK(destination_ledger.ActiveReservationCount() == 0);
        CHECK(std::filesystem::is_empty(destination_root_directory));
        CHECK_FALSE(std::filesystem::exists(source_directory / "dest.txt"));
    }
    CHECK(close(source_root_fd) == 0);
    CHECK(close(destination_root_fd) == 0);
}

TEST_CASE(PREFIX "materializes and seals a private stage across an explicitly supplied device-bound fixture",
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
    RetainedDestinationFixture destination_fixture{destination_base};
    const auto destination_parent_directory = destination_fixture.Directory() / "destination-parent";
    const auto destination_root_directory = destination_fixture.Directory() / "destination-root";
    const auto cancellation_destination_root_directory = destination_fixture.Directory() / "cancellation-root";
    const auto wrong_source_root_directory = destination_fixture.Directory() / "wrong-source-root";
    const auto stale_destination_parent_directory = destination_fixture.Directory() / "stale-destination-parent";
    const auto stale_destination_root_directory = destination_fixture.Directory() / "stale-destination-root";
    REQUIRE(std::filesystem::create_directory(destination_parent_directory));
    REQUIRE(std::filesystem::create_directory(destination_root_directory));
    REQUIRE(std::filesystem::create_directory(cancellation_destination_root_directory));
    REQUIRE(std::filesystem::create_directory(wrong_source_root_directory));
    REQUIRE(std::filesystem::create_directory(stale_destination_parent_directory));
    REQUIRE(std::filesystem::create_directory(stale_destination_root_directory));
    if( !SupportsFullSync(destination_root_directory) )
        SKIP("destination fixture does not support F_FULLFSYNC");

    TestDir source_workspace;
    const auto source_directory = source_workspace.directory / "source";
    const auto source_root_directory = source_workspace.directory / "source-root";
    const auto cancellation_source_root_directory = source_workspace.directory / "cancellation-source-root";
    const auto wrong_destination_root_directory = source_workspace.directory / "wrong-destination-root";
    const auto stale_source_root_directory = source_workspace.directory / "stale-source-root";
    REQUIRE(std::filesystem::create_directory(source_directory));
    REQUIRE(std::filesystem::create_directory(source_root_directory));
    REQUIRE(std::filesystem::create_directory(cancellation_source_root_directory));
    REQUIRE(std::filesystem::create_directory(wrong_destination_root_directory));
    REQUIRE(std::filesystem::create_directory(stale_source_root_directory));
    std::vector<uint8_t> payload(32 * 1024 + 37);
    for( size_t index = 0; index != payload.size(); ++index )
        payload[index] = static_cast<uint8_t>((index * 13) & 0xFF);
    payload[17] = 0;
    payload[16384] = 0xFF;
    SourcePair source{source_directory, "source", payload};
    const int destination_parent_fd =
        open(destination_parent_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(destination_parent_fd >= 0);
    const int source_root_fd = OpenProtectedRoot(source_root_directory);
    const int destination_root_fd = OpenProtectedRoot(destination_root_directory);
    const int wrong_source_root_fd = OpenProtectedRoot(wrong_source_root_directory);
    const int wrong_destination_root_fd = OpenProtectedRoot(wrong_destination_root_directory);
    auto request = Request(Header(41), source);
    request.destination_parent = SealFromFD(destination_parent_fd);
    if( request.source.device == request.destination_parent.device )
        SKIP("destination fixture must expose a different st_dev from the source workspace");

    [&] {
        auto opened_wrong_source = helper::ProtectedRootLedger::Open(wrong_source_root_fd);
        REQUIRE(opened_wrong_source);
        auto wrong_source_ledger = std::move(*opened_wrong_source);
        const auto snapshot = helper::SourceSnapshotWriter::Create(
            wrong_source_ledger, TakeTerminal(request, source.SourceFD(), destination_parent_fd), {});
        REQUIRE_FALSE(snapshot);
        CHECK(snapshot.error() == helper::SourceSnapshotWriter::Error::SourceRootBindingFailed);
        CHECK(wrong_source_ledger.ActiveReservationCount() == 0);
        CHECK(std::filesystem::is_empty(wrong_source_root_directory));
    }();

    [&] {
        auto opened_source = helper::ProtectedRootLedger::Open(source_root_fd);
        auto opened_destination = helper::ProtectedRootLedger::Open(destination_root_fd);
        REQUIRE(opened_source);
        REQUIRE(opened_destination);
        auto source_ledger = std::move(*opened_source);
        auto destination_ledger = std::move(*opened_destination);
        auto snapshot = helper::SourceSnapshotWriter::Create(
            source_ledger, TakeTerminal(request, source.SourceFD(), destination_parent_fd), {});
        REQUIRE(snapshot);
        const auto stage = helper::DestinationStageWriter::Create(destination_ledger, std::move(*snapshot), {});
        REQUIRE(stage);
        CHECK(stage->Correlation() == request.header);
        CHECK(stage->SourceSeal() == request.source);
        CHECK(stage->SourceSnapshotSeal().byte_size == payload.size());
        CHECK(stage->StageSeal().byte_size == payload.size());
        CHECK(stage->StageSeal().device == request.destination_parent.device);
        const auto artifact = SingleArtifact(destination_root_directory);
        const int artifact_fd = open(artifact.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        REQUIRE(artifact_fd >= 0);
        struct stat artifact_status{};
        REQUIRE(fstat(artifact_fd, &artifact_status) == 0);
        CHECK((artifact_status.st_mode & 07777) == 0600);
        CHECK(artifact_status.st_nlink == 1);
        CHECK(close(artifact_fd) == 0);
        CHECK(ReadFile(artifact) == payload);
        CHECK(CountEntries(destination_root_directory, ".wc-cross-volume-record-") == 1);
        CHECK(CountEntries(destination_root_directory, ".wc-cross-volume-seal-") == 1);
        CHECK_FALSE(std::filesystem::exists(destination_parent_directory / "dest.txt"));
    }();
    [&] {
        auto reopened_destination = helper::ProtectedRootLedger::Open(destination_root_fd);
        REQUIRE(reopened_destination);
        auto restarted_destination = std::move(*reopened_destination);
        const auto reconciliation = restarted_destination.Reconcile();
        REQUIRE(reconciliation);
        CHECK(reconciliation->retained_records == 1);
        CHECK(reconciliation->exact_sealed_artifacts == 1);
        CHECK(reconciliation->retained_incomplete_artifacts == 0);
    }();

    auto wrong_destination_request = Request(Header(43), source);
    wrong_destination_request.destination_parent = SealFromFD(destination_parent_fd);
    [&] {
        auto opened_source = helper::ProtectedRootLedger::Open(source_root_fd);
        auto opened_destination = helper::ProtectedRootLedger::Open(wrong_destination_root_fd);
        REQUIRE(opened_source);
        REQUIRE(opened_destination);
        auto source_ledger = std::move(*opened_source);
        auto wrong_destination_ledger = std::move(*opened_destination);
        auto snapshot = helper::SourceSnapshotWriter::Create(
            source_ledger, TakeTerminal(wrong_destination_request, source.SourceFD(), destination_parent_fd), {});
        REQUIRE(snapshot);
        const auto stage = helper::DestinationStageWriter::Create(wrong_destination_ledger, std::move(*snapshot), {});
        REQUIRE_FALSE(stage);
        CHECK(stage.error() == helper::DestinationStageWriter::Error::CrossVolumeBindingFailed);
        CHECK(wrong_destination_ledger.ActiveReservationCount() == 0);
        CHECK(std::filesystem::is_empty(wrong_destination_root_directory));
    }();

    const int cancellation_destination_root_fd = OpenProtectedRoot(cancellation_destination_root_directory);
    const int cancellation_source_root_fd = OpenProtectedRoot(cancellation_source_root_directory);
    auto cancellation_request = Request(Header(42), source);
    cancellation_request.destination_parent = SealFromFD(destination_parent_fd);
    [&] {
        auto opened_source = helper::ProtectedRootLedger::Open(cancellation_source_root_fd);
        auto opened_destination = helper::ProtectedRootLedger::Open(cancellation_destination_root_fd);
        REQUIRE(opened_source);
        REQUIRE(opened_destination);
        auto source_ledger = std::move(*opened_source);
        auto destination_ledger = std::move(*opened_destination);
        auto snapshot = helper::SourceSnapshotWriter::Create(
            source_ledger, TakeTerminal(cancellation_request, source.SourceFD(), destination_parent_fd), {});
        REQUIRE(snapshot);
        DestinationStageReentryContext context{.ledger = &destination_ledger};
        const helper::DestinationStageWriter::Cancellation cancellation{
            .probe = CancelAndObserveDestinationStage,
            .context = &context,
        };
        const auto stage =
            helper::DestinationStageWriter::Create(destination_ledger, std::move(*snapshot), cancellation);
        REQUIRE_FALSE(stage);
        CHECK(stage.error() == helper::DestinationStageWriter::Error::Cancelled);
        CHECK(context.called);
        CHECK(context.observed_active_reservation);
        CHECK(context.observed_reconcile_busy);
        CHECK(destination_ledger.ActiveReservationCount() == 1);
        CHECK(CountEntries(cancellation_destination_root_directory, ".wc-cross-volume-record-") == 1);
        CHECK(CountEntries(cancellation_destination_root_directory, ".wc-cross-volume-artifact-") == 0);
        CHECK(CountEntries(cancellation_destination_root_directory, ".wc-cross-volume-seal-") == 0);
    }();
    [&] {
        auto reopened_destination = helper::ProtectedRootLedger::Open(cancellation_destination_root_fd);
        REQUIRE(reopened_destination);
        auto restarted_destination = std::move(*reopened_destination);
        const auto reconciliation = restarted_destination.Reconcile();
        REQUIRE(reconciliation);
        CHECK(reconciliation->removed_reservations == 1);
        CHECK(reconciliation->retained_records == 0);
    }();

    const int stale_destination_parent_fd =
        open(stale_destination_parent_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(stale_destination_parent_fd >= 0);
    const int stale_destination_root_fd = OpenProtectedRoot(stale_destination_root_directory);
    const int stale_source_root_fd = OpenProtectedRoot(stale_source_root_directory);
    auto stale_request = Request(Header(44), source);
    stale_request.destination_parent = SealFromFD(stale_destination_parent_fd);
    [&] {
        auto opened_source = helper::ProtectedRootLedger::Open(stale_source_root_fd);
        auto opened_destination = helper::ProtectedRootLedger::Open(stale_destination_root_fd);
        REQUIRE(opened_source);
        REQUIRE(opened_destination);
        auto source_ledger = std::move(*opened_source);
        auto destination_ledger = std::move(*opened_destination);
        auto snapshot = helper::SourceSnapshotWriter::Create(
            source_ledger, TakeTerminal(stale_request, source.SourceFD(), stale_destination_parent_fd), {});
        REQUIRE(snapshot);
        DestinationParentMutationContext context{.descriptor = stale_destination_parent_fd};
        const helper::DestinationStageWriter::Cancellation cancellation{
            .probe = MutateDestinationParentAtBeforeArtifactCreate,
            .context = &context,
        };
        const auto stage =
            helper::DestinationStageWriter::Create(destination_ledger, std::move(*snapshot), cancellation);
        REQUIRE_FALSE(stage);
        CHECK(stage.error() == helper::DestinationStageWriter::Error::DestinationParentStale);
        CHECK(context.called);
        CHECK(context.succeeded);
        CHECK(destination_ledger.ActiveReservationCount() == 1);
        CHECK(CountEntries(stale_destination_root_directory, ".wc-cross-volume-record-") == 1);
        CHECK(CountEntries(stale_destination_root_directory, ".wc-cross-volume-artifact-") == 0);
        CHECK(CountEntries(stale_destination_root_directory, ".wc-cross-volume-seal-") == 0);
    }();
    [&] {
        auto reopened_destination = helper::ProtectedRootLedger::Open(stale_destination_root_fd);
        REQUIRE(reopened_destination);
        auto restarted_destination = std::move(*reopened_destination);
        const auto reconciliation = restarted_destination.Reconcile();
        REQUIRE(reconciliation);
        CHECK(reconciliation->removed_reservations == 1);
        CHECK(reconciliation->retained_records == 0);
    }();

    CHECK(close(stale_source_root_fd) == 0);
    CHECK(close(stale_destination_root_fd) == 0);
    CHECK(close(stale_destination_parent_fd) == 0);
    CHECK(close(cancellation_source_root_fd) == 0);
    CHECK(close(cancellation_destination_root_fd) == 0);
    CHECK(close(wrong_destination_root_fd) == 0);
    CHECK(close(wrong_source_root_fd) == 0);
    CHECK(close(source_root_fd) == 0);
    CHECK(close(destination_root_fd) == 0);
    CHECK(close(destination_parent_fd) == 0);
}

} // namespace CrossVolumeStagingHelperSourceSnapshotTests

#undef PREFIX
