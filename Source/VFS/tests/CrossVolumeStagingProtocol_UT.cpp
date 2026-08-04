// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <RoutedIO/CrossVolumeStagingProtocol.h>

#include <array>
#include <cerrno>
#include <sys/stat.h>

#define PREFIX "RoutedIO cross-volume staging protocol "

namespace CrossVolumeStagingProtocolTests {

namespace protocol = nc::routedio::cross_volume_staging;

static protocol::Header Header()
{
    protocol::Header header;
    header.correlation[0] = 1;
    return header;
}

static protocol::ObjectSeal Seal(const mode_t _type)
{
    return protocol::ObjectSeal{
        .device = 1,
        .inode = 2,
        .uid = 3,
        .gid = 4,
        .mode = static_cast<uint32_t>(_type | 0640),
        .flags = 0,
        .link_count = 1,
        .byte_size = 5,
        .birth_time = {.seconds = 6, .nanoseconds = 7},
        .modification_time = {.seconds = 8, .nanoseconds = 9},
        .status_change_time = {.seconds = 10, .nanoseconds = 11},
    };
}

static protocol::DestinationComponent DestinationName()
{
    constexpr std::array<uint8_t, 10> name{'d', 'e', 's', 't', '.', 't', 'x', 't', 0xFF, 0xFE};
    auto component = protocol::DestinationComponent::Create(name);
    REQUIRE(component);
    return *component;
}

static protocol::BeginRequest BeginRequest()
{
    return protocol::BeginRequest{
        .header = Header(),
        .source = Seal(S_IFREG),
        .destination_parent = Seal(S_IFDIR),
        .destination_name = DestinationName(),
    };
}

static protocol::Lease Lease()
{
    protocol::Lease lease;
    lease.header = Header();
    lease.token.bytes[0] = 1;
    return lease;
}

TEST_CASE(PREFIX "accepts exact descriptor-seal-only begin claims")
{
    const auto request = BeginRequest();
    CHECK(protocol::Validate(request));
    CHECK(request.destination_name.Bytes().size() == 10);
    CHECK(request.destination_name.Bytes()[8] == 0xFF);
}

TEST_CASE(PREFIX "rejects malformed begin claims before helper admission")
{
    SECTION("unknown protocol version") {
        auto request = BeginRequest();
        request.header.version = protocol::kProtocolVersion + 1;
        const auto result = protocol::Validate(request);
        REQUIRE_FALSE(result);
        CHECK(result.error() == protocol::ValidationError::UnsupportedVersion);
    }

    SECTION("empty correlation") {
        auto request = BeginRequest();
        request.header.correlation = {};
        const auto result = protocol::Validate(request);
        REQUIRE_FALSE(result);
        CHECK(result.error() == protocol::ValidationError::InvalidCorrelation);
    }

    SECTION("source is not regular") {
        auto request = BeginRequest();
        request.source.mode = static_cast<uint32_t>(S_IFDIR | 0700);
        const auto result = protocol::Validate(request);
        REQUIRE_FALSE(result);
        CHECK(result.error() == protocol::ValidationError::InvalidSourceSeal);
    }

    SECTION("destination parent is not a directory") {
        auto request = BeginRequest();
        request.destination_parent.mode = static_cast<uint32_t>(S_IFREG | 0600);
        const auto result = protocol::Validate(request);
        REQUIRE_FALSE(result);
        CHECK(result.error() == protocol::ValidationError::InvalidDestinationParentSeal);
    }

    SECTION("zero inode or link count") {
        auto request = BeginRequest();
        request.source.inode = 0;
        CHECK(protocol::Validate(request).error() == protocol::ValidationError::InvalidSourceSeal);
        request = BeginRequest();
        request.destination_parent.link_count = 0;
        CHECK(protocol::Validate(request).error() == protocol::ValidationError::InvalidDestinationParentSeal);
    }

    SECTION("invalid timestamp") {
        auto request = BeginRequest();
        request.source.modification_time.nanoseconds = 1'000'000'000;
        const auto result = protocol::Validate(request);
        REQUIRE_FALSE(result);
        CHECK(result.error() == protocol::ValidationError::InvalidTimestamp);
    }
}

TEST_CASE(PREFIX "bounds destination components as opaque names")
{
    constexpr std::array<uint8_t, 1> empty{};
    constexpr std::array<uint8_t, 1> dot{'.'};
    constexpr std::array<uint8_t, 2> dot_dot{'.', '.'};
    constexpr std::array<uint8_t, 3> slash{'a', '/', 'b'};
    constexpr std::array<uint8_t, 3> embedded_nul{'a', 0, 'b'};
    constexpr std::array<uint8_t, protocol::kMaximumDestinationComponentBytes + 1> overlong{};

    CHECK_FALSE(protocol::DestinationComponent::Create(std::span<const uint8_t>{}));
    CHECK_FALSE(protocol::DestinationComponent::Create(empty));
    CHECK_FALSE(protocol::DestinationComponent::Create(dot));
    CHECK_FALSE(protocol::DestinationComponent::Create(dot_dot));
    CHECK_FALSE(protocol::DestinationComponent::Create(slash));
    CHECK_FALSE(protocol::DestinationComponent::Create(embedded_nul));
    CHECK_FALSE(protocol::DestinationComponent::Create(overlong));
}

TEST_CASE(PREFIX "binds one helper-issued lease to one correlation")
{
    const auto lease = Lease();
    CHECK(protocol::Validate(lease));
    CHECK(protocol::Validate(protocol::CommitRequest{.header = Header(), .lease = lease}));
    CHECK(protocol::Validate(protocol::AbortRequest{.header = Header(), .lease = lease}));

    auto request = protocol::CommitRequest{.header = Header(), .lease = lease};
    request.header.correlation[1] = 2;
    const auto result = protocol::Validate(request);
    REQUIRE_FALSE(result);
    CHECK(result.error() == protocol::ValidationError::InconsistentLease);
}

TEST_CASE(PREFIX "accepts only conservative begin and completion replies")
{
    const auto lease = Lease();
    CHECK(protocol::Validate(protocol::BeginResult{
        .header = Header(), .disposition = protocol::BeginDisposition::Granted, .failure = protocol::BeginFailure::None, .lease = lease}));
    CHECK(protocol::Validate(protocol::BeginResult{
        .header = Header(),
        .disposition = protocol::BeginDisposition::Rejected,
        .failure = protocol::BeginFailure::SourceStale,
    }));

    auto invalid_begin = protocol::BeginResult{
        .header = Header(), .disposition = protocol::BeginDisposition::Granted, .failure = protocol::BeginFailure::None};
    CHECK(protocol::Validate(invalid_begin).error() == protocol::ValidationError::InconsistentBeginResult);

    CHECK(protocol::Validate(protocol::CompletionResult{
        .header = Header(),
        .publication = protocol::Publication::Published,
        .failure = protocol::CompletionFailure::None,
        .filesystem_sync = protocol::FilesystemSync::Confirmed,
    }));
    CHECK(protocol::Validate(protocol::CompletionResult{
        .header = Header(),
        .publication = protocol::Publication::Published,
        .failure = protocol::CompletionFailure::FileSystemSyncFailed,
        .system_error = EIO,
        .filesystem_sync = protocol::FilesystemSync::Failed,
        .filesystem_sync_system_error = EIO,
    }));
    CHECK(protocol::Validate(protocol::CompletionResult{
        .header = Header(),
        .publication = protocol::Publication::Unknown,
        .failure = protocol::CompletionFailure::HelperFailure,
        .system_error = EIO,
    }));
    CHECK(protocol::Validate(protocol::CompletionResult{
        .header = Header(),
        .publication = protocol::Publication::NotPublished,
        .failure = protocol::CompletionFailure::SourceStale,
        .system_error = ESTALE,
    }));

    const auto invalid_completion = protocol::CompletionResult{
        .header = Header(),
        .publication = protocol::Publication::NotPublished,
        .failure = protocol::CompletionFailure::Cancelled,
        .filesystem_sync = protocol::FilesystemSync::Confirmed,
    };
    CHECK(protocol::Validate(invalid_completion).error() == protocol::ValidationError::InconsistentCompletionResult);

    const auto missing_unknown_error = protocol::CompletionResult{
        .header = Header(),
        .publication = protocol::Publication::Unknown,
        .failure = protocol::CompletionFailure::HelperFailure,
    };
    CHECK(protocol::Validate(missing_unknown_error).error() == protocol::ValidationError::InconsistentCompletionResult);
}

} // namespace CrossVolumeStagingProtocolTests

#undef PREFIX
