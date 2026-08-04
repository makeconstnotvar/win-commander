// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "../../RoutedIO/source/CrossVolumeStagingXPCCodec.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define PREFIX "RoutedIO cross-volume staging XPC codec "

namespace CrossVolumeStagingXPCCodecTests {

namespace protocol = nc::routedio::cross_volume_staging;
namespace codec = protocol::xpc_codec;

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
        .flags = 5,
        .link_count = 1,
        .byte_size = 6,
        .birth_time = {.seconds = 7, .nanoseconds = 8},
        .modification_time = {.seconds = 9, .nanoseconds = 10},
        .status_change_time = {.seconds = 11, .nanoseconds = 12},
    };
}

static protocol::DestinationComponent DestinationName()
{
    constexpr std::array<uint8_t, 5> bytes{'a', 0xFF, '.', 't', 'x'};
    auto result = protocol::DestinationComponent::Create(bytes);
    REQUIRE(result);
    return *result;
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

static void Write(const std::filesystem::path &_path)
{
    const int fd = open(_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    REQUIRE(fd >= 0);
    const char contents[] = "payload";
    REQUIRE(write(fd, contents, sizeof(contents)) == sizeof(contents));
    REQUIRE(close(fd) == 0);
}

TEST_CASE(PREFIX "round-trips exactly two duplicated descriptor rights")
{
    TestDir directory;
    const auto source_path = directory.directory / "source";
    Write(source_path);
    const int source_fd = open(source_path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    const int destination_parent_fd = open(directory.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    REQUIRE(source_fd >= 0);
    REQUIRE(destination_parent_fd >= 0);
    const auto close_source = at_scope_end([source_fd] { close(source_fd); });
    const auto close_parent = at_scope_end([destination_parent_fd] { close(destination_parent_fd); });

    const auto request = BeginRequest();
    auto encoded = codec::EncodeBegin(request, {.source_fd = source_fd, .destination_parent_fd = destination_parent_fd});
    REQUIRE(encoded);
    ScopedDictionary message{*encoded};
    CHECK(xpc_dictionary_get_count(message.Get()) == 8);
    CHECK(xpc_get_type(xpc_dictionary_get_value(message.Get(), "source-fd")) == XPC_TYPE_FD);
    CHECK(xpc_get_type(xpc_dictionary_get_value(message.Get(), "destination-parent-fd")) == XPC_TYPE_FD);
    CHECK(fcntl(source_fd, F_GETFD) >= 0);
    CHECK(fcntl(destination_parent_fd, F_GETFD) >= 0);

    auto decoded = codec::DecodeBegin(message.Get());
    REQUIRE(decoded);
    CHECK(decoded->request == request);
    CHECK(decoded->descriptors.SourceFD() != source_fd);
    CHECK(decoded->descriptors.DestinationParentFD() != destination_parent_fd);

    struct stat original_source;
    struct stat decoded_source;
    struct stat original_parent;
    struct stat decoded_parent;
    REQUIRE(fstat(source_fd, &original_source) == 0);
    REQUIRE(fstat(decoded->descriptors.SourceFD(), &decoded_source) == 0);
    REQUIRE(fstat(destination_parent_fd, &original_parent) == 0);
    REQUIRE(fstat(decoded->descriptors.DestinationParentFD(), &decoded_parent) == 0);
    CHECK(decoded_source.st_dev == original_source.st_dev);
    CHECK(decoded_source.st_ino == original_source.st_ino);
    CHECK(decoded_parent.st_dev == original_parent.st_dev);
    CHECK(decoded_parent.st_ino == original_parent.st_ino);
}

TEST_CASE(PREFIX "rejects legacy, malformed and non-two-FD begin dictionaries")
{
    const auto request = BeginRequest();
    CHECK_FALSE(codec::EncodeBegin(request, {.source_fd = -1, .destination_parent_fd = 4}));
    CHECK_FALSE(codec::EncodeBegin(request, {.source_fd = 4, .destination_parent_fd = 4}));

    ScopedDictionary legacy{xpc_dictionary_create(nullptr, nullptr, 0)};
    REQUIRE(legacy.Get() != nullptr);
    xpc_dictionary_set_string(legacy.Get(), "operation", "rename");
    const auto legacy_result = codec::DecodeBegin(legacy.Get());
    REQUIRE_FALSE(legacy_result);
    CHECK(legacy_result.error() == codec::Error::UnknownMember);

    TestDir directory;
    const auto source_path = directory.directory / "source";
    Write(source_path);
    const int source_fd = open(source_path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    const int destination_parent_fd = open(directory.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    REQUIRE(source_fd >= 0);
    REQUIRE(destination_parent_fd >= 0);
    const auto close_source = at_scope_end([source_fd] { close(source_fd); });
    const auto close_parent = at_scope_end([destination_parent_fd] { close(destination_parent_fd); });

    auto encoded = codec::EncodeBegin(request, {.source_fd = source_fd, .destination_parent_fd = destination_parent_fd});
    REQUIRE(encoded);
    ScopedDictionary unknown_member{*encoded};
    xpc_dictionary_set_uint64(unknown_member.Get(), "unexpected", 1);
    CHECK_FALSE(codec::DecodeBegin(unknown_member.Get()));

    auto malformed = codec::EncodeBegin(request, {.source_fd = source_fd, .destination_parent_fd = destination_parent_fd});
    REQUIRE(malformed);
    ScopedDictionary wrong_descriptor{*malformed};
    xpc_dictionary_set_string(wrong_descriptor.Get(), "source-fd", "path");
    const auto malformed_result = codec::DecodeBegin(wrong_descriptor.Get());
    REQUIRE_FALSE(malformed_result);
    CHECK(malformed_result.error() == codec::Error::InvalidDescriptor);
}

TEST_CASE(PREFIX "round-trips correlation-bound lease and conservative replies")
{
    const auto lease = Lease();
    const auto commit = protocol::CommitRequest{.header = Header(), .lease = lease};
    const auto abort = protocol::AbortRequest{.header = Header(), .lease = lease};

    auto encoded_commit = codec::EncodeCommit(commit);
    REQUIRE(encoded_commit);
    ScopedDictionary commit_message{*encoded_commit};
    const auto decoded_commit = codec::DecodeCommit(commit_message.Get());
    REQUIRE(decoded_commit);
    CHECK(*decoded_commit == commit);

    auto encoded_abort = codec::EncodeAbort(abort);
    REQUIRE(encoded_abort);
    ScopedDictionary abort_message{*encoded_abort};
    const auto decoded_abort = codec::DecodeAbort(abort_message.Get());
    REQUIRE(decoded_abort);
    CHECK(*decoded_abort == abort);

    const auto begin_result = protocol::BeginResult{
        .header = Header(),
        .disposition = protocol::BeginDisposition::Granted,
        .failure = protocol::BeginFailure::None,
        .lease = lease,
    };
    auto encoded_begin_result = codec::EncodeBeginResult(begin_result);
    REQUIRE(encoded_begin_result);
    ScopedDictionary begin_result_message{*encoded_begin_result};
    const auto decoded_begin_result = codec::DecodeBeginResult(begin_result_message.Get());
    REQUIRE(decoded_begin_result);
    CHECK(*decoded_begin_result == begin_result);

    const auto completion = protocol::CompletionResult{
        .header = Header(),
        .publication = protocol::Publication::Published,
        .failure = protocol::CompletionFailure::FileSystemSyncFailed,
        .system_error = EIO,
        .filesystem_sync = protocol::FilesystemSync::Failed,
        .filesystem_sync_system_error = EIO,
    };
    auto encoded_completion = codec::EncodeCompletionResult(completion);
    REQUIRE(encoded_completion);
    ScopedDictionary completion_message{*encoded_completion};
    const auto decoded_completion = codec::DecodeCompletionResult(completion_message.Get());
    REQUIRE(decoded_completion);
    CHECK(*decoded_completion == completion);
}

TEST_CASE(PREFIX "classifies only request envelopes and populates reply dictionaries")
{
    const auto request = BeginRequest();
    TestDir directory;
    const auto source_path = directory.directory / "source";
    Write(source_path);
    const int source_fd = open(source_path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    const int destination_parent_fd = open(directory.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    REQUIRE(source_fd >= 0);
    REQUIRE(destination_parent_fd >= 0);
    const auto close_source = at_scope_end([source_fd] { close(source_fd); });
    const auto close_parent = at_scope_end([destination_parent_fd] { close(destination_parent_fd); });

    auto begin = codec::EncodeBegin(request, {.source_fd = source_fd, .destination_parent_fd = destination_parent_fd});
    REQUIRE(begin);
    ScopedDictionary begin_message{*begin};
    CHECK(*codec::DecodeRequestKind(begin_message.Get()) == codec::RequestKind::Begin);

    const auto lease = Lease();
    auto commit = codec::EncodeCommit({.header = Header(), .lease = lease});
    REQUIRE(commit);
    ScopedDictionary commit_message{*commit};
    CHECK(*codec::DecodeRequestKind(commit_message.Get()) == codec::RequestKind::Commit);

    auto abort = codec::EncodeAbort({.header = Header(), .lease = lease});
    REQUIRE(abort);
    ScopedDictionary abort_message{*abort};
    CHECK(*codec::DecodeRequestKind(abort_message.Get()) == codec::RequestKind::Abort);

    const auto begin_result = protocol::BeginResult{
        .header = Header(), .disposition = protocol::BeginDisposition::Rejected,
        .failure = protocol::BeginFailure::Unsupported, .lease = {.header = Header()}};
    ScopedDictionary begin_reply{xpc_dictionary_create(nullptr, nullptr, 0)};
    REQUIRE(begin_reply.Get() != nullptr);
    REQUIRE(codec::PopulateBeginResultReply(begin_reply.Get(), begin_result));
    CHECK(*codec::DecodeBeginResult(begin_reply.Get()) == begin_result);
    CHECK_FALSE(codec::DecodeRequestKind(begin_reply.Get()));
    CHECK_FALSE(codec::PopulateBeginResultReply(begin_reply.Get(), begin_result));

    const auto completion = protocol::CompletionResult{
        .header = Header(), .publication = protocol::Publication::NotPublished,
        .failure = protocol::CompletionFailure::Aborted, .system_error = 0,
        .filesystem_sync = protocol::FilesystemSync::NotAttempted, .filesystem_sync_system_error = 0};
    ScopedDictionary completion_reply{xpc_dictionary_create(nullptr, nullptr, 0)};
    REQUIRE(completion_reply.Get() != nullptr);
    REQUIRE(codec::PopulateCompletionResultReply(completion_reply.Get(), completion));
    CHECK(*codec::DecodeCompletionResult(completion_reply.Get()) == completion);

    ScopedDictionary missing_kind{xpc_dictionary_create(nullptr, nullptr, 0)};
    REQUIRE(missing_kind.Get() != nullptr);
    CHECK_FALSE(codec::DecodeRequestKind(missing_kind.Get()));
}

} // namespace CrossVolumeStagingXPCCodecTests

#undef PREFIX
