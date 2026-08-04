// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "../../RoutedIO/source/CrossVolumeStagingHelperLeaseStore.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#define PREFIX "RoutedIO cross-volume staging helper lease store "

namespace CrossVolumeStagingHelperLeaseStoreTests {

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

static protocol::BeginRequest BeginRequest(const protocol::Header &_header, const int _source_fd, const int _parent_fd)
{
    constexpr std::array<uint8_t, 5> bytes{'a', 0xFF, '.', 't', 'x'};
    auto destination_name = protocol::DestinationComponent::Create(bytes);
    REQUIRE(destination_name);
    return protocol::BeginRequest{
        .header = _header,
        .source = SealFromFD(_source_fd),
        .destination_parent = SealFromFD(_parent_fd),
        .destination_name = *destination_name,
    };
}

static void Write(const std::filesystem::path &_path)
{
    const int fd = open(_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    REQUIRE(fd >= 0);
    const char contents[] = "payload";
    REQUIRE(write(fd, contents, sizeof(contents)) == sizeof(contents));
    REQUIRE(close(fd) == 0);
}

static codec::DecodedBegin DecodedBegin(TestDir &_directory,
                                       const protocol::Header &_header,
                                       const std::string_view _suffix = {})
{
    const auto source_path = _directory.directory /
                             ("source-" + std::to_string(_header.correlation[0]) + std::string{_suffix});
    Write(source_path);
    const int source_fd = open(source_path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    const int destination_parent_fd = open(_directory.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    REQUIRE(source_fd >= 0);
    REQUIRE(destination_parent_fd >= 0);
    const auto close_source = at_scope_end([source_fd] { close(source_fd); });
    const auto close_parent = at_scope_end([destination_parent_fd] { close(destination_parent_fd); });

    auto encoded = codec::EncodeBegin(BeginRequest(_header, source_fd, destination_parent_fd),
                                      {.source_fd = source_fd, .destination_parent_fd = destination_parent_fd});
    REQUIRE(encoded);
    ScopedDictionary message{*encoded};
    auto decoded = codec::DecodeBegin(message.Get());
    REQUIRE(decoded);
    return std::move(*decoded);
}

static helper::ValidatedBegin MakeValidatedBegin(TestDir &_directory,
                                                 const protocol::Header &_header,
                                                 const std::string_view _suffix = {})
{
    auto validated = helper::ValidateBeginDescriptors(DecodedBegin(_directory, _header, _suffix));
    REQUIRE(validated);
    return std::move(*validated);
}

static protocol::CommitRequest Commit(const protocol::Lease &_lease)
{
    return protocol::CommitRequest{
        .header = _lease.header,
        .lease = _lease,
    };
}

static protocol::AbortRequest Abort(const protocol::Lease &_lease)
{
    return protocol::AbortRequest{
        .header = _lease.header,
        .lease = _lease,
    };
}

TEST_CASE(PREFIX "retains decoded descriptor rights until an exact terminal take")
{
    TestDir directory;
    helper::LeaseStore store;
    auto decoded = DecodedBegin(directory, Header(1));
    const int stored_source_fd = decoded.descriptors.SourceFD();
    const int stored_parent_fd = decoded.descriptors.DestinationParentFD();

    const auto expected_request = decoded.request;
    auto validated = helper::ValidateBeginDescriptors(std::move(decoded));
    REQUIRE(validated);
    const auto granted = store.Grant(1, std::move(*validated));
    REQUIRE(granted);
    CHECK(granted->header == Header(1));
    CHECK(granted->token.bytes != protocol::LeaseToken{}.bytes);
    CHECK(store.ActiveLeaseCount() == 1);
    CHECK(fcntl(stored_source_fd, F_GETFD) >= 0);
    CHECK(fcntl(stored_parent_fd, F_GETFD) >= 0);

    {
        const auto terminal = store.Take(1, Commit(*granted));
        REQUIRE(terminal);
        CHECK(terminal->Request() == expected_request);
        CHECK(terminal->Authority() == *granted);
    }
    errno = 0;
    CHECK(fcntl(stored_source_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
    CHECK(store.ActiveLeaseCount() == 0);
    CHECK_FALSE(store.Take(1, Abort(*granted)));
}

TEST_CASE(PREFIX "wrong owner, correlation and token leave the exact lease available")
{
    TestDir directory;
    helper::LeaseStore store;
    const auto granted = store.Grant(1, MakeValidatedBegin(directory, Header(1)));
    REQUIRE(granted);

    const auto wrong_owner = store.Take(2, Commit(*granted));
    REQUIRE_FALSE(wrong_owner);
    CHECK(wrong_owner.error() == helper::LeaseStore::Error::OwnerMismatch);

    auto wrong_correlation = *granted;
    wrong_correlation.header.correlation[1] = 1;
    CHECK_FALSE(store.Take(1, Commit(wrong_correlation)));

    auto wrong_token = *granted;
    wrong_token.token.bytes[1] = 1;
    CHECK_FALSE(store.Take(1, Commit(wrong_token)));
    CHECK(store.ActiveLeaseCount() == 1);

    CHECK(store.Take(1, Commit(*granted)));
    CHECK(store.ActiveLeaseCount() == 0);
}

TEST_CASE(PREFIX "Commit and Abort compete for exactly one terminal claim")
{
    TestDir directory;
    helper::LeaseStore store;
    const auto committed = store.Grant(1, MakeValidatedBegin(directory, Header(1)));
    REQUIRE(committed);
    CHECK(store.Take(1, Commit(*committed)));
    CHECK_FALSE(store.Take(1, Abort(*committed)));

    const auto aborted = store.Grant(1, MakeValidatedBegin(directory, Header(2)));
    REQUIRE(aborted);
    CHECK(store.Take(1, Abort(*aborted)));
    CHECK_FALSE(store.Take(1, Commit(*aborted)));
    CHECK(store.ActiveLeaseCount() == 0);
}

TEST_CASE(PREFIX "duplicate correlation and capacity reject without disturbing live claims")
{
    TestDir directory;
    helper::LeaseStore store;
    const auto first = store.Grant(1, MakeValidatedBegin(directory, Header(1)));
    REQUIRE(first);
    auto duplicate_decoded = DecodedBegin(directory, Header(1), "-second");
    const int duplicate_source_fd = duplicate_decoded.descriptors.SourceFD();
    auto duplicate_begin = helper::ValidateBeginDescriptors(std::move(duplicate_decoded));
    REQUIRE(duplicate_begin);
    const auto duplicate_correlation = store.Grant(1, std::move(*duplicate_begin));
    REQUIRE_FALSE(duplicate_correlation);
    CHECK(duplicate_correlation.error() == helper::LeaseStore::Error::DuplicateCorrelation);
    errno = 0;
    CHECK(fcntl(duplicate_source_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
    CHECK(store.ActiveLeaseCount() == 1);

    for( uint8_t value = 3; value <= helper::LeaseStore::kMaximumLeases + 1; ++value ) {
        const auto granted = store.Grant(1, MakeValidatedBegin(directory, Header(value)));
        REQUIRE(granted);
    }
    CHECK(store.ActiveLeaseCount() == helper::LeaseStore::kMaximumLeases);
    auto overflow_decoded = DecodedBegin(directory, Header(18));
    const int overflow_source_fd = overflow_decoded.descriptors.SourceFD();
    auto overflow_begin = helper::ValidateBeginDescriptors(std::move(overflow_decoded));
    REQUIRE(overflow_begin);
    const auto overflow = store.Grant(1, std::move(*overflow_begin));
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error() == helper::LeaseStore::Error::CapacityExceeded);
    errno = 0;
    CHECK(fcntl(overflow_source_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
    CHECK(store.Take(1, Commit(*first)));
    CHECK(store.ActiveLeaseCount() == helper::LeaseStore::kMaximumLeases - 1);
}

TEST_CASE(PREFIX "owner revoke closes pending descriptor rights without affecting another peer")
{
    TestDir directory;
    helper::LeaseStore store;
    auto revoked_begin = DecodedBegin(directory, Header(1));
    const int revoked_source_fd = revoked_begin.descriptors.SourceFD();
    auto validated_revoked = helper::ValidateBeginDescriptors(std::move(revoked_begin));
    REQUIRE(validated_revoked);
    const auto revoked = store.Grant(1, std::move(*validated_revoked));
    REQUIRE(revoked);
    const auto retained = store.Grant(2, MakeValidatedBegin(directory, Header(2)));
    REQUIRE(retained);

    CHECK(store.RevokeOwner(1) == 1);
    CHECK(store.ActiveLeaseCount() == 1);
    errno = 0;
    CHECK(fcntl(revoked_source_fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
    CHECK(store.Take(2, Commit(*retained)));
    CHECK(store.ActiveLeaseCount() == 0);
}

} // namespace CrossVolumeStagingHelperLeaseStoreTests

#undef PREFIX
