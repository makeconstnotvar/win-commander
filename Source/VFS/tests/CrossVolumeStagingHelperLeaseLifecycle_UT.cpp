// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "../../RoutedIO/source/CrossVolumeStagingHelperLeaseStore.h"

#include <array>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#define PREFIX "RoutedIO cross-volume staging helper lease lifecycle "

namespace CrossVolumeStagingHelperLeaseLifecycleTests {

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

struct PreparedBegin final {
    helper::ValidatedBegin begin;
    int source_fd;
    int destination_parent_fd;
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

static void Write(const std::filesystem::path &_path)
{
    const int fd = open(_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    REQUIRE(fd >= 0);
    constexpr std::string_view contents{"payload"};
    REQUIRE(write(fd, contents.data(), contents.size()) == static_cast<ssize_t>(contents.size()));
    REQUIRE(close(fd) == 0);
}

static PreparedBegin PrepareBegin(TestDir &_directory,
                                  const protocol::Header &_header,
                                  const std::string_view _suffix = {})
{
    const auto source_path = _directory.directory /
                             ("source-" + std::to_string(_header.correlation[0]) + std::string{_suffix});
    Write(source_path);
    const int source_fd = open(source_path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    const int destination_parent_fd =
        open(_directory.directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    REQUIRE(source_fd >= 0);
    REQUIRE(destination_parent_fd >= 0);
    const auto close_source = at_scope_end([source_fd] { close(source_fd); });
    const auto close_parent = at_scope_end([destination_parent_fd] { close(destination_parent_fd); });

    constexpr std::array<uint8_t, 9> destination_bytes{'d', 'e', 's', 't', '-', 'n', 'a', 'm', 'e'};
    const auto destination_name = protocol::DestinationComponent::Create(destination_bytes);
    REQUIRE(destination_name);
    const protocol::BeginRequest request{
        .header = _header,
        .source = SealFromFD(source_fd),
        .destination_parent = SealFromFD(destination_parent_fd),
        .destination_name = *destination_name,
    };
    const auto encoded = codec::EncodeBegin(
        request, {.source_fd = source_fd, .destination_parent_fd = destination_parent_fd});
    REQUIRE(encoded);
    ScopedDictionary message{*encoded};
    auto decoded = codec::DecodeBegin(message.Get());
    REQUIRE(decoded);
    const int helper_source_fd = decoded->descriptors.SourceFD();
    const int helper_destination_parent_fd = decoded->descriptors.DestinationParentFD();
    auto validated = helper::ValidateBeginDescriptors(std::move(*decoded));
    REQUIRE(validated);
    return {
        .begin = std::move(*validated),
        .source_fd = helper_source_fd,
        .destination_parent_fd = helper_destination_parent_fd,
    };
}

static protocol::CommitRequest Commit(const protocol::Lease &_lease)
{
    return {.header = _lease.header, .lease = _lease};
}

static protocol::AbortRequest Abort(const protocol::Lease &_lease)
{
    return {.header = _lease.header, .lease = _lease};
}

static size_t EntryCount(const TestDir &_directory)
{
    size_t entries = 0;
    for( const auto &_ : std::filesystem::directory_iterator{_directory.directory} )
        ++entries;
    return entries;
}

TEST_CASE(PREFIX "grants a namespace-mutation-free lease and abort closes its descriptor rights")
{
    TestDir directory;
    helper::LeaseStore store;
    helper::LeaseLifecycle lifecycle{store};
    auto prepared = PrepareBegin(directory, Header(1));
    const size_t entries_before_begin = EntryCount(directory);

    const auto begun = lifecycle.Begin(7, std::move(prepared.begin));
    REQUIRE(protocol::Validate(begun));
    CHECK(begun.disposition == protocol::BeginDisposition::Granted);
    CHECK(begun.failure == protocol::BeginFailure::None);
    CHECK(store.ActiveLeaseCount() == 1);
    CHECK(EntryCount(directory) == entries_before_begin);
    CHECK(fcntl(prepared.source_fd, F_GETFD) >= 0);
    CHECK(fcntl(prepared.destination_parent_fd, F_GETFD) >= 0);

    const auto aborted = lifecycle.Abort(7, Abort(begun.lease));
    REQUIRE(protocol::Validate(aborted));
    CHECK(aborted.publication == protocol::Publication::NotPublished);
    CHECK(aborted.failure == protocol::CompletionFailure::Aborted);
    CHECK(store.ActiveLeaseCount() == 0);
    errno = 0;
    CHECK(fcntl(prepared.source_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
    errno = 0;
    CHECK(fcntl(prepared.destination_parent_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
    errno = 0;
    CHECK(fcntl(prepared.destination_parent_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
}

TEST_CASE(PREFIX "Commit consumes the exact lease and confirms no publication before staging exists")
{
    TestDir directory;
    helper::LeaseStore store;
    helper::LeaseLifecycle lifecycle{store};
    auto prepared = PrepareBegin(directory, Header(2));
    const auto begun = lifecycle.Begin(7, std::move(prepared.begin));
    REQUIRE(begun.disposition == protocol::BeginDisposition::Granted);

    const auto committed = lifecycle.Commit(7, Commit(begun.lease));
    REQUIRE(protocol::Validate(committed));
    CHECK(committed.publication == protocol::Publication::NotPublished);
    CHECK(committed.failure == protocol::CompletionFailure::HelperFailure);
    CHECK(committed.system_error == EOPNOTSUPP);
    CHECK(store.ActiveLeaseCount() == 0);
    errno = 0;
    CHECK(fcntl(prepared.source_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);

    const auto duplicate_terminal = lifecycle.Abort(7, Abort(begun.lease));
    REQUIRE(protocol::Validate(duplicate_terminal));
    CHECK(duplicate_terminal.publication == protocol::Publication::Unknown);
    CHECK(duplicate_terminal.failure == protocol::CompletionFailure::HelperFailure);
}

TEST_CASE(PREFIX "wrong owner and token leave the original lease live until its owner aborts")
{
    TestDir directory;
    helper::LeaseStore store;
    helper::LeaseLifecycle lifecycle{store};
    auto prepared = PrepareBegin(directory, Header(3));
    const auto begun = lifecycle.Begin(7, std::move(prepared.begin));
    REQUIRE(begun.disposition == protocol::BeginDisposition::Granted);

    const auto wrong_owner = lifecycle.Abort(8, Abort(begun.lease));
    REQUIRE(protocol::Validate(wrong_owner));
    CHECK(wrong_owner.publication == protocol::Publication::Unknown);
    CHECK(store.ActiveLeaseCount() == 1);
    CHECK(fcntl(prepared.source_fd, F_GETFD) >= 0);

    auto wrong_token = begun.lease;
    wrong_token.token.bytes[1] = wrong_token.token.bytes[1] == 0xFF ? 0 : static_cast<uint8_t>(wrong_token.token.bytes[1] + 1);
    const auto wrong_token_result = lifecycle.Commit(7, Commit(wrong_token));
    REQUIRE(protocol::Validate(wrong_token_result));
    CHECK(wrong_token_result.publication == protocol::Publication::Unknown);
    CHECK(store.ActiveLeaseCount() == 1);

    const auto aborted = lifecycle.Abort(7, Abort(begun.lease));
    REQUIRE(protocol::Validate(aborted));
    CHECK(aborted.publication == protocol::Publication::NotPublished);
    CHECK(store.ActiveLeaseCount() == 0);
}

TEST_CASE(PREFIX "duplicate correlation and owner revocation retain no reusable descriptor claim")
{
    TestDir directory;
    helper::LeaseStore store;
    helper::LeaseLifecycle lifecycle{store};
    auto first = PrepareBegin(directory, Header(4));
    const auto begun = lifecycle.Begin(7, std::move(first.begin));
    REQUIRE(begun.disposition == protocol::BeginDisposition::Granted);

    auto duplicate = PrepareBegin(directory, Header(4), "-duplicate");
    const auto rejected = lifecycle.Begin(7, std::move(duplicate.begin));
    REQUIRE(protocol::Validate(rejected));
    CHECK(rejected.disposition == protocol::BeginDisposition::Rejected);
    CHECK(rejected.failure == protocol::BeginFailure::HelperFailure);
    CHECK(store.ActiveLeaseCount() == 1);
    errno = 0;
    CHECK(fcntl(duplicate.source_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
    errno = 0;
    CHECK(fcntl(duplicate.destination_parent_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);

    CHECK(lifecycle.RevokeOwner(7) == 1);
    CHECK(store.ActiveLeaseCount() == 0);
    errno = 0;
    CHECK(fcntl(first.source_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
    errno = 0;
    CHECK(fcntl(first.destination_parent_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
    const auto after_revoke = lifecycle.Commit(7, Commit(begun.lease));
    REQUIRE(protocol::Validate(after_revoke));
    CHECK(after_revoke.publication == protocol::Publication::Unknown);
}

} // namespace CrossVolumeStagingHelperLeaseLifecycleTests

#undef PREFIX
