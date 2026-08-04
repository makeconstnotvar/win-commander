// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "../../RoutedIO/source/CrossVolumeStagingHelperDescriptorSealValidator.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#define PREFIX "RoutedIO cross-volume staging helper descriptor-seal validator "

namespace CrossVolumeStagingHelperDescriptorSealValidatorTests {

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

class DescriptorPair final
{
public:
    explicit DescriptorPair(TestDir &_directory)
    {
        const auto source_path = _directory.directory / "source";
        const int created = open(source_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        REQUIRE(created >= 0);
        constexpr std::string_view contents{"payload"};
        REQUIRE(write(created, contents.data(), contents.size()) == static_cast<ssize_t>(contents.size()));
        REQUIRE(close(created) == 0);

        m_SourceFD = open(source_path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        m_DestinationParentFD =
            open(_directory.directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        REQUIRE(m_SourceFD >= 0);
        REQUIRE(m_DestinationParentFD >= 0);
        m_SourcePath = source_path;
    }

    DescriptorPair(const DescriptorPair &) = delete;
    DescriptorPair &operator=(const DescriptorPair &) = delete;
    ~DescriptorPair()
    {
        if( m_SourceFD >= 0 )
            close(m_SourceFD);
        if( m_DestinationParentFD >= 0 )
            close(m_DestinationParentFD);
    }

    [[nodiscard]] int SourceFD() const noexcept { return m_SourceFD; }
    [[nodiscard]] int DestinationParentFD() const noexcept { return m_DestinationParentFD; }
    [[nodiscard]] const std::filesystem::path &SourcePath() const noexcept { return m_SourcePath; }
    [[nodiscard]] codec::BorrowedBeginDescriptors Borrow() const noexcept
    {
        return {.source_fd = m_SourceFD, .destination_parent_fd = m_DestinationParentFD};
    }

private:
    int m_SourceFD{-1};
    int m_DestinationParentFD{-1};
    std::filesystem::path m_SourcePath;
};

static protocol::Timestamp TimestampFrom(const timespec &_value)
{
    return {
        .seconds = _value.tv_sec,
        .nanoseconds = static_cast<uint32_t>(_value.tv_nsec),
    };
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

static protocol::BeginRequest BeginRequest(const DescriptorPair &_pair)
{
    constexpr std::array<uint8_t, 8> destination_name{'d', 'e', 's', 't', '.', 't', 'x', 't'};
    const auto component = protocol::DestinationComponent::Create(destination_name);
    REQUIRE(component);

    protocol::Header header;
    header.correlation[0] = 1;
    return {
        .header = header,
        .source = SealFromFD(_pair.SourceFD()),
        .destination_parent = SealFromFD(_pair.DestinationParentFD()),
        .destination_name = *component,
    };
}

static codec::DecodedBegin DecodeBegin(const protocol::BeginRequest &_request,
                                       const codec::BorrowedBeginDescriptors _descriptors)
{
    const auto encoded = codec::EncodeBegin(_request, _descriptors);
    REQUIRE(encoded);
    ScopedDictionary message{*encoded};
    auto decoded = codec::DecodeBegin(message.Get());
    REQUIRE(decoded);
    return std::move(*decoded);
}

template <class T>
static T Different(const T _value)
{
    return _value == std::numeric_limits<T>::max() ? static_cast<T>(_value - 1) : static_cast<T>(_value + 1);
}

static uint32_t DifferentNanoseconds(const uint32_t _value)
{
    return _value == 999'999'999 ? _value - 1 : _value + 1;
}

using SealMutation = void (*)(protocol::ObjectSeal &);

constexpr std::array<SealMutation, 14> kEachSealScalar{
    [](protocol::ObjectSeal &_seal) { _seal.device = Different(_seal.device); },
    [](protocol::ObjectSeal &_seal) { _seal.inode = Different(_seal.inode); },
    [](protocol::ObjectSeal &_seal) { _seal.uid = Different(_seal.uid); },
    [](protocol::ObjectSeal &_seal) { _seal.gid = Different(_seal.gid); },
    [](protocol::ObjectSeal &_seal) { _seal.mode ^= S_IRUSR; },
    [](protocol::ObjectSeal &_seal) { _seal.flags = Different(_seal.flags); },
    [](protocol::ObjectSeal &_seal) { _seal.link_count = Different(_seal.link_count); },
    [](protocol::ObjectSeal &_seal) { _seal.byte_size = Different(_seal.byte_size); },
    [](protocol::ObjectSeal &_seal) { _seal.birth_time.seconds = Different(_seal.birth_time.seconds); },
    [](protocol::ObjectSeal &_seal) { _seal.birth_time.nanoseconds = DifferentNanoseconds(_seal.birth_time.nanoseconds); },
    [](protocol::ObjectSeal &_seal) { _seal.modification_time.seconds = Different(_seal.modification_time.seconds); },
    [](protocol::ObjectSeal &_seal) {
        _seal.modification_time.nanoseconds = DifferentNanoseconds(_seal.modification_time.nanoseconds);
    },
    [](protocol::ObjectSeal &_seal) { _seal.status_change_time.seconds = Different(_seal.status_change_time.seconds); },
    [](protocol::ObjectSeal &_seal) {
        _seal.status_change_time.nanoseconds = DifferentNanoseconds(_seal.status_change_time.nanoseconds);
    },
};

TEST_CASE(PREFIX "accepts the exact anchored source and destination-parent seals")
{
    TestDir directory;
    const DescriptorPair pair{directory};
    const auto request = BeginRequest(pair);

    REQUIRE(protocol::Validate(request));
    auto decoded = DecodeBegin(request, pair.Borrow());
    const int validated_source_fd = decoded.descriptors.SourceFD();
    const int validated_destination_parent_fd = decoded.descriptors.DestinationParentFD();
    const auto validated = helper::ValidateBeginDescriptors(std::move(decoded));
    REQUIRE(validated);
    CHECK(validated->Request() == request);
    CHECK((fcntl(validated_source_fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((fcntl(validated_destination_parent_fd, F_GETFD) & FD_CLOEXEC) != 0);
}

TEST_CASE(PREFIX "rejects every source and destination-parent scalar seal drift")
{
    TestDir directory;
    const DescriptorPair pair{directory};
    const auto exact = BeginRequest(pair);
    REQUIRE(protocol::Validate(exact));

    for( const auto mutate : kEachSealScalar ) {
        auto source_drift = exact;
        mutate(source_drift.source);
        REQUIRE(protocol::Validate(source_drift));
        const auto source_result = helper::ValidateBeginDescriptors(DecodeBegin(source_drift, pair.Borrow()));
        REQUIRE_FALSE(source_result);
        CHECK(source_result.error() == helper::BeginDescriptorValidationError::SourceStale);

        auto parent_drift = exact;
        mutate(parent_drift.destination_parent);
        REQUIRE(protocol::Validate(parent_drift));
        const auto parent_result = helper::ValidateBeginDescriptors(DecodeBegin(parent_drift, pair.Borrow()));
        REQUIRE_FALSE(parent_result);
        CHECK(parent_result.error() == helper::BeginDescriptorValidationError::DestinationParentStale);
    }
}

TEST_CASE(PREFIX "rejects stale object types and write-capable or closed descriptor rights without retaining them")
{
    TestDir directory;
    const DescriptorPair pair{directory};
    const auto exact = BeginRequest(pair);

    const int second_source_fd = open(pair.SourcePath().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    REQUIRE(second_source_fd >= 0);
    const auto close_second_source = at_scope_end([second_source_fd] { close(second_source_fd); });
    const auto parent_is_file = helper::ValidateBeginDescriptors(
        DecodeBegin(exact, {.source_fd = pair.SourceFD(), .destination_parent_fd = second_source_fd}));
    REQUIRE_FALSE(parent_is_file);
    CHECK(parent_is_file.error() == helper::BeginDescriptorValidationError::DestinationParentStale);

    const int second_parent_fd = open(directory.directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    REQUIRE(second_parent_fd >= 0);
    const auto close_second_parent = at_scope_end([second_parent_fd] { close(second_parent_fd); });
    const auto source_is_directory = helper::ValidateBeginDescriptors(
        DecodeBegin(exact, {.source_fd = second_parent_fd, .destination_parent_fd = pair.DestinationParentFD()}));
    REQUIRE_FALSE(source_is_directory);
    CHECK(source_is_directory.error() == helper::BeginDescriptorValidationError::SourceStale);

    const int write_only_source = open(pair.SourcePath().c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    REQUIRE(write_only_source >= 0);
    const auto write_only = helper::ValidateBeginDescriptors(
        DecodeBegin(exact, {.source_fd = write_only_source, .destination_parent_fd = pair.DestinationParentFD()}));
    REQUIRE_FALSE(write_only);
    CHECK(write_only.error() == helper::BeginDescriptorValidationError::InvalidRequest);
    CHECK(close(write_only_source) == 0);

    auto closed = DecodeBegin(exact, pair.Borrow());
    const int closed_source = closed.descriptors.SourceFD();
    REQUIRE(close(closed_source) == 0);
    errno = 0;
    CHECK(fcntl(closed_source, F_GETFD) == -1);
    CHECK(errno == EBADF);
    const auto closed_result = helper::ValidateBeginDescriptors(std::move(closed));
    REQUIRE_FALSE(closed_result);
    CHECK(closed_result.error() == helper::BeginDescriptorValidationError::HelperFailure);
}

TEST_CASE(PREFIX "rejects a source descriptor aliasing the destination parent object")
{
    TestDir directory;
    const DescriptorPair pair{directory};
    const auto exact = BeginRequest(pair);
    const int second_parent_fd = open(directory.directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    REQUIRE(second_parent_fd >= 0);
    const auto close_second_parent = at_scope_end([second_parent_fd] { close(second_parent_fd); });
    const auto aliased = helper::ValidateBeginDescriptors(
        DecodeBegin(exact, {.source_fd = pair.DestinationParentFD(), .destination_parent_fd = second_parent_fd}));
    REQUIRE_FALSE(aliased);
    CHECK(aliased.error() == helper::BeginDescriptorValidationError::SourceStale);
}

} // namespace CrossVolumeStagingHelperDescriptorSealValidatorTests

#undef PREFIX
