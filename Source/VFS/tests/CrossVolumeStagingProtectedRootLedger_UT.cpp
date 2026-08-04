// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "../../RoutedIO/source/CrossVolumeStagingProtectedRootLedger.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#define PREFIX "RoutedIO cross-volume staging protected-root ledger "

namespace CrossVolumeStagingProtectedRootLedgerTests {

namespace protocol = nc::routedio::cross_volume_staging;
namespace helper = protocol::helper;

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

static void WriteFileAt(const int _root_fd, const std::string_view _name, const std::string_view _contents, const mode_t _mode)
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
           "\nstate=sealed\nartifact_device=" + number(_seal.device) + "\nartifact_inode=" +
           number(_seal.inode) + "\nartifact_uid=" + number(_seal.uid) + "\nartifact_gid=" + number(_seal.gid) +
           "\nartifact_mode=" + number(_seal.mode) + "\nartifact_flags=" + number(_seal.flags) +
           "\nartifact_nlink=" + number(_seal.link_count) + "\nartifact_size=" + number(_seal.byte_size) +
           "\nartifact_birth_seconds=" + number(_seal.birth_time.seconds) + "\nartifact_birth_nanoseconds=" +
           number(_seal.birth_time.nanoseconds) + "\nartifact_mtime_seconds=" + number(_seal.modification_time.seconds) +
           "\nartifact_mtime_nanoseconds=" + number(_seal.modification_time.nanoseconds) +
           "\nartifact_ctime_seconds=" + number(_seal.status_change_time.seconds) + "\nartifact_ctime_nanoseconds=" +
           number(_seal.status_change_time.nanoseconds) + "\n";
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
        WriteSealedManifest(
            root_fd, header, helper::ArtifactRole::DestinationStage, id, seal);
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
        auto reservation = ledger.Reserve(header, helper::ArtifactRole::SourceSnapshot);
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

TEST_CASE(PREFIX "keeps the durable sealed quota bounded across helper restart")
{
    TestDir directory;
    const int root_fd = OpenProtectedRoot(directory);
    {
        auto opened = helper::ProtectedRootLedger::Open(root_fd);
        REQUIRE(opened);
        auto ledger = std::move(*opened);
        for( uint8_t value = 1; value <= helper::ProtectedRootLedger::kMaximumReservations; ++value ) {
            auto reservation = ledger.Reserve(Header(value), helper::ArtifactRole::SourceSnapshot);
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
    CHECK(release.error() == helper::ProtectedRootLedger::Error::RecordRemoveFailed);
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
