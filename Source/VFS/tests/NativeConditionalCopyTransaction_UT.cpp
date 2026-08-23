// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"
#include "../source/Native/ConditionalCopy.h"
#include "../source/Native/CrossVolumeStagingAuthority.h"
#include "../source/Native/CrossVolumeStagingClient.h"
#include "../source/ProviderCapabilitiesTesting.h"
#include <VFS/Native.h>
#include <Utility/NativeFSManager.h>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <membership.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#define PREFIX "VFSNative conditional Copy transaction "

namespace NativeConditionalCopyTransactionTests {

using namespace nc::vfs;

static std::string CanonicalDirectoryPath(const TestDir &_test_dir)
{
    return std::filesystem::canonical(_test_dir.directory).string();
}

static ProviderConditionalCopyTimestamp Timestamp(const timespec &_timestamp) noexcept
{
    return ProviderConditionalCopyTimestamp{
        .seconds = _timestamp.tv_sec,
        .nanoseconds = _timestamp.tv_nsec,
    };
}

static ProviderConditionalCopyExistingExpectation
Expectation(const std::string &_path,
           ProviderConditionalCopyExpectedKind _kind,
           ProviderConditionalCopyExpectationTolerance _tolerance = ProviderConditionalCopyExpectationTolerance::Exact)
{
    struct stat value;
    REQUIRE(stat(_path.c_str(), &value) == 0);
    return ProviderConditionalCopyExistingExpectation{
        .absolute_path = _path,
        .kind = _kind,
        .device = static_cast<int32_t>(value.st_dev),
        .inode = static_cast<uint64_t>(value.st_ino),
        .birth_time = Timestamp(value.st_birthtimespec),
        .mode = static_cast<uint16_t>(value.st_mode),
        .byte_size = static_cast<uint64_t>(value.st_size),
        .modification_time = Timestamp(value.st_mtimespec),
        .status_change_time = Timestamp(value.st_ctimespec),
        .tolerance = _tolerance,
    };
}

static ProviderConditionalCopyReviewedClaims
Claims(const std::shared_ptr<NativeHost> &_host,
      const std::string &_source,
      const std::string &_destination_parent,
      const std::string &_destination,
      ProviderConditionalCopyExpectationTolerance _destination_parent_tolerance =
          ProviderConditionalCopyExpectationTolerance::Exact)
{
    const ProviderConditionalCopyBinding binding{
        .provider_id = "native",
        .host = _host,
    };
    return ProviderConditionalCopyReviewedClaims{
        .plan_id = "native-conditional-copy-test",
        .source_binding = binding,
        .destination_binding = binding,
        .source = Expectation(_source, ProviderConditionalCopyExpectedKind::RegularFile),
        .destination_parent = Expectation(_destination_parent,
                                          ProviderConditionalCopyExpectedKind::Directory,
                                          _destination_parent_tolerance),
        .destination = ProviderConditionalCopyMissingExpectation{.absolute_path = _destination},
    };
}

static ProviderConditionalMoveReviewedClaims MoveClaims(const std::shared_ptr<NativeHost> &_host,
                                                        const std::string &_source_parent,
                                                        const std::string &_source,
                                                        const std::string &_destination_parent,
                                                        const std::string &_destination)
{
    const ProviderConditionalCopyBinding binding{
        .provider_id = "native",
        .host = _host,
    };
    return ProviderConditionalMoveReviewedClaims{
        .plan_id = "native-conditional-move-test",
        .source_binding = binding,
        .destination_binding = binding,
        .source = Expectation(_source, ProviderConditionalCopyExpectedKind::RegularFile),
        .source_parent = Expectation(_source_parent, ProviderConditionalCopyExpectedKind::Directory),
        .destination_parent = Expectation(_destination_parent, ProviderConditionalCopyExpectedKind::Directory),
        .destination = ProviderConditionalCopyMissingExpectation{.absolute_path = _destination},
    };
}

static ProviderConditionalMoveReviewedAuthority MoveAuthority(ProviderConditionalMoveReviewedClaims _claims)
{
    return ProviderConditionalCopyTransactionTestAccess::MakeMoveAuthority(std::move(_claims));
}

static ProviderConditionalDeleteReviewedClaims
DeleteClaims(const std::shared_ptr<NativeHost> &_host,
            const std::string &_source_parent,
            const std::string &_source,
            ProviderConditionalCopyExpectationTolerance _source_parent_tolerance =
                ProviderConditionalCopyExpectationTolerance::Exact)
{
    return ProviderConditionalDeleteReviewedClaims{
        .plan_id = "native-conditional-delete-test",
        .source_binding =
            ProviderConditionalCopyBinding{
                .provider_id = "native",
                .host = _host,
            },
        .source = Expectation(_source, ProviderConditionalCopyExpectedKind::RegularFile),
        .source_parent =
            Expectation(_source_parent, ProviderConditionalCopyExpectedKind::Directory, _source_parent_tolerance),
    };
}

static ProviderConditionalDeleteReviewedAuthority DeleteAuthority(ProviderConditionalDeleteReviewedClaims _claims)
{
    return ProviderConditionalCopyTransactionTestAccess::MakeDeleteAuthority(std::move(_claims));
}

static ProviderConditionalCopyReviewedAuthority Authority(ProviderConditionalCopyReviewedClaims _claims)
{
    return ProviderConditionalCopyTransactionTestAccess::MakeAuthority(std::move(_claims));
}

static void Write(const std::string &_path, std::string_view _contents)
{
    auto output = std::ofstream{_path, std::ios::binary | std::ios::trunc};
    REQUIRE(output);
    output.write(_contents.data(), static_cast<std::streamsize>(_contents.size()));
    REQUIRE(output.good());
}

static std::string Read(const std::string &_path)
{
    auto input = std::ifstream{_path, std::ios::binary};
    REQUIRE(input);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

static native::ConditionalCopyMetadataSnapshot CaptureMetadata(const std::string &_path)
{
    native::ConditionalCopyIO io;
    const int fd = open(_path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    REQUIRE(fd >= 0);
    const auto close_fd = at_scope_end([fd] { close(fd); });
    auto metadata = io.CaptureMetadata(fd);
    REQUIRE(metadata);
    return std::move(*metadata);
}

static void SetReadACLForCurrentUser(const std::string &_path)
{
    uuid_t account_uuid{};
    REQUIRE(mbr_uid_to_uuid(geteuid(), account_uuid) == 0);
    acl_t acl = acl_init(1);
    REQUIRE(acl != nullptr);
    const auto free_acl = at_scope_end([&acl] { acl_free(acl); });
    acl_entry_t entry = nullptr;
    REQUIRE(acl_create_entry(&acl, &entry) == 0);
    REQUIRE(acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0);
    REQUIRE(acl_set_qualifier(entry, account_uuid) == 0);
    acl_permset_t permissions = nullptr;
    REQUIRE(acl_get_permset(entry, &permissions) == 0);
    REQUIRE(acl_clear_perms(permissions) == 0);
    REQUIRE(acl_add_perm(permissions, ACL_READ_DATA) == 0);
    REQUIRE(acl_set_file(_path.c_str(), ACL_TYPE_EXTENDED, acl) == 0);
}

static void RequireCloneCapable(const std::string &_path)
{
    const auto volume = TestEnv().native_fs_man->VolumeFromPath(_path);
    REQUIRE(volume);
    if( !native::EvaluateConditionalCopyVolume(*volume).IsSupported() )
        SKIP("test volume does not satisfy the internal APFS conditional-Copy policy");
}

struct ConditionalCopyPaths final {
    ConditionalCopyPaths()
    {
        const auto root = CanonicalDirectoryPath(storage);
        source_parent = root + "/source";
        destination_parent = root + "/destination";
        REQUIRE(std::filesystem::create_directory(source_parent));
        REQUIRE(std::filesystem::create_directory(destination_parent));
        source = source_parent + "/source.txt";
        destination = destination_parent + "/destination.txt";
    }

    TestDir storage;
    std::string source_parent;
    std::string destination_parent;
    std::string source;
    std::string destination;
};

class GateNativeFSManager final : public nc::utility::NativeFSManager
{
public:
    enum class Mode {
        CloneDisabled,
        RenameExclDisabled,
        DifferentVolumes,
        Supported,
        MissingPathVolume
    };

    explicit GateNativeFSManager(Mode _mode) : m_Mode{_mode}
    {
        auto first = std::make_shared<nc::utility::NativeFileSystemInfo>();
        first->mounted_at_path = "/first";
        first->basic.fs_id.val[0] = 1;
        first->fs_type_name = "apfs";
        first->mount_flags.local = true;
        first->mount_flags.internal = true;
        first->interfaces.clone = _mode != Mode::CloneDisabled;
        first->interfaces.rename_excl = _mode != Mode::RenameExclDisabled;
        first->interfaces.attr_list = true;
        first->interfaces.extended_attr = true;
        first->interfaces.extended_security = true;
        m_First = std::move(first);

        auto second = std::make_shared<nc::utility::NativeFileSystemInfo>();
        second->mounted_at_path = "/second";
        second->basic.fs_id.val[0] = 2;
        second->fs_type_name = "apfs";
        second->mount_flags.local = true;
        second->mount_flags.internal = true;
        second->interfaces.clone = true;
        second->interfaces.rename_excl = _mode != Mode::RenameExclDisabled;
        second->interfaces.attr_list = true;
        second->interfaces.extended_attr = true;
        second->interfaces.extended_security = true;
        m_Second = std::move(second);
    }

    [[nodiscard]] std::vector<Info> Volumes() const override { return {m_First, m_Second}; }

    [[nodiscard]] Info VolumeFromFD([[maybe_unused]] int _fd) const noexcept override
    {
        ++m_VolumeFromFDCalls;
        if( m_Mode == Mode::DifferentVolumes && m_VolumeFromFDCalls % 2 == 0 )
            return m_Second;
        return m_First;
    }

    [[nodiscard]] Info VolumeFromPath([[maybe_unused]] std::string_view _path) const noexcept override
    {
        if( m_Mode == Mode::MissingPathVolume )
            return {};
        if( m_Mode == Mode::DifferentVolumes && ++m_VolumeFromPathCalls % 2 == 0 )
            return m_Second;
        return m_First;
    }

    [[nodiscard]] Info VolumeFromPathFast([[maybe_unused]] std::string_view _path) const noexcept override
    {
        return m_First;
    }

    [[nodiscard]] Info VolumeFromMountPoint([[maybe_unused]] std::string_view _mount_point) const noexcept override
    {
        return m_First;
    }

    void UpdateSpaceInformation([[maybe_unused]] const Info &_volume) override {}
    void EjectVolumeContainingPath([[maybe_unused]] const std::string &_path) override {}

    [[nodiscard]] bool IsVolumeContainingPathEjectable([[maybe_unused]] const std::string &_path) override
    {
        return false;
    }

    [[nodiscard]] size_t VolumeFromFDCalls() const noexcept { return m_VolumeFromFDCalls; }

private:
    Mode m_Mode;
    Info m_First;
    Info m_Second;
    mutable size_t m_VolumeFromFDCalls{0};
    mutable size_t m_VolumeFromPathCalls{0};
};

class RecordingCrossVolumeStagingAuthority final : public native::CrossVolumeStagingAuthority
{
public:
    class Transaction final : public native::CrossVolumeStagingTransaction
    {
    public:
        Transaction(size_t &_commit_calls, bool &_cancellation_observed) noexcept
            : m_CommitCalls{_commit_calls}, m_CancellationObserved{_cancellation_observed}
        {
        }

        [[nodiscard]] ProviderConditionalCopyCommitResult
        Commit(const ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) noexcept override
        {
            ++m_CommitCalls;
            try {
                m_CancellationObserved = _cancel_checker && _cancel_checker();
            } catch( ... ) {
                m_CancellationObserved = true;
            }
            if( m_CancellationObserved ) {
                return ProviderConditionalCopyCommitResult{
                    .publication = ProviderConditionalCopyPublicationState::NotPublished,
                    .failure = ProviderConditionalCopyCommitFailure::Cancelled,
                };
            }
            return ProviderConditionalCopyCommitResult{
                .publication = ProviderConditionalCopyPublicationState::NotPublished,
                .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
                .system_error = EIO,
            };
        }

        [[nodiscard]] ProviderConditionalCopyPublicationState Abort() noexcept override
        {
            return ProviderConditionalCopyPublicationState::NotPublished;
        }

    private:
        size_t &m_CommitCalls;
        bool &m_CancellationObserved;
    };

    [[nodiscard]] bool IsAvailable() const noexcept override { return available; }

    [[nodiscard]] std::expected<std::unique_ptr<native::CrossVolumeStagingTransaction>,
                                ProviderConditionalCopyTransactionBeginError>
    Begin(native::CrossVolumeStagingRequest _request,
          const ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) override
    {
        ++begin_calls;
        destination_name = _request.DestinationName();
        source_seal = _request.SourceSeal();
        destination_parent_seal = _request.DestinationParentSeal();
        struct stat source_stat{};
        struct stat destination_parent_stat{};
        descriptor_seals_match = fstat(_request.SourceFD(), &source_stat) == 0 &&
                                 fstat(_request.DestinationParentFD(), &destination_parent_stat) == 0 &&
                                 static_cast<uint64_t>(source_stat.st_dev) == source_seal.device &&
                                 static_cast<uint64_t>(source_stat.st_ino) == source_seal.inode &&
                                 static_cast<uint64_t>(destination_parent_stat.st_dev) == destination_parent_seal.device &&
                                 static_cast<uint64_t>(destination_parent_stat.st_ino) == destination_parent_seal.inode;
        try {
            if( _cancel_checker && _cancel_checker() )
                return std::unexpected(ProviderConditionalCopyTransactionBeginError::Cancelled);
        } catch( ... ) {
            return std::unexpected(ProviderConditionalCopyTransactionBeginError::Cancelled);
        }
        if( fail_begin )
            return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
        return std::make_unique<Transaction>(commit_calls, cancellation_observed);
    }

    bool available{true};
    bool fail_begin{false};
    size_t begin_calls{0};
    size_t commit_calls{0};
    bool cancellation_observed{false};
    bool descriptor_seals_match{false};
    std::string destination_name;
    native::CrossVolumeStagingObjectSeal source_seal;
    native::CrossVolumeStagingObjectSeal destination_parent_seal;
};

namespace staging_protocol = nc::routedio::cross_volume_staging;

class RecordingCrossVolumeStagingTransport final : public native::CrossVolumeStagingTransport
{
public:
    enum class BeginReply : uint8_t {
        Granted,
        RejectedSourceStale,
        MismatchedCorrelation
    };

    enum class CommitReply : uint8_t {
        Published,
        SourceStale,
        TransportFailure,
        MismatchedCorrelation
    };

    [[nodiscard]] bool IsAvailable() const noexcept override { return available; }

    [[nodiscard]] std::expected<staging_protocol::BeginResult, native::CrossVolumeStagingTransportError>
    Begin(native::CrossVolumeStagingTransportBegin _begin) override
    {
        ++begin_calls;
        begin_request = _begin.Request();
        source_fd = _begin.SourceFD();
        destination_parent_fd = _begin.DestinationParentFD();
        source_descriptor_valid = fstat(source_fd, &source_stat) == 0;
        destination_parent_descriptor_valid = fstat(destination_parent_fd, &destination_parent_stat) == 0;
        source_descriptor_cloexec = (fcntl(source_fd, F_GETFD) & FD_CLOEXEC) != 0;
        destination_parent_descriptor_cloexec = (fcntl(destination_parent_fd, F_GETFD) & FD_CLOEXEC) != 0;

        if( begin_reply == BeginReply::RejectedSourceStale ) {
            return staging_protocol::BeginResult{
                .header = begin_request->header,
                .disposition = staging_protocol::BeginDisposition::Rejected,
                .failure = staging_protocol::BeginFailure::SourceStale,
            };
        }

        staging_protocol::Lease lease{.header = begin_request->header};
        lease.token.bytes[0] = 1;
        staging_protocol::BeginResult result{
            .header = begin_request->header,
            .disposition = staging_protocol::BeginDisposition::Granted,
            .failure = staging_protocol::BeginFailure::None,
            .lease = lease,
        };
        if( begin_reply == BeginReply::MismatchedCorrelation )
            result.header.correlation[0] ^= 1;
        return result;
    }

    [[nodiscard]] std::expected<staging_protocol::CompletionResult, native::CrossVolumeStagingTransportError>
    Commit(const staging_protocol::CommitRequest &_request) override
    {
        ++commit_calls;
        commit_request = _request;
        if( commit_reply == CommitReply::TransportFailure )
            return std::unexpected{native::CrossVolumeStagingTransportError::Failure};

        staging_protocol::CompletionResult result{
            .header = _request.header,
            .publication = staging_protocol::Publication::Published,
            .failure = staging_protocol::CompletionFailure::None,
            .filesystem_sync = staging_protocol::FilesystemSync::Confirmed,
        };
        if( commit_reply == CommitReply::SourceStale ) {
            result.publication = staging_protocol::Publication::NotPublished;
            result.failure = staging_protocol::CompletionFailure::SourceStale;
            result.system_error = ESTALE;
            result.filesystem_sync = staging_protocol::FilesystemSync::NotAttempted;
        }
        if( commit_reply == CommitReply::MismatchedCorrelation )
            result.header.correlation[0] ^= 1;
        return result;
    }

    [[nodiscard]] std::expected<staging_protocol::CompletionResult, native::CrossVolumeStagingTransportError>
    Abort(const staging_protocol::AbortRequest &_request) override
    {
        ++abort_calls;
        abort_request = _request;
        return staging_protocol::CompletionResult{
            .header = _request.header,
            .publication = staging_protocol::Publication::NotPublished,
            .failure = staging_protocol::CompletionFailure::Aborted,
        };
    }

    bool available{true};
    BeginReply begin_reply{BeginReply::Granted};
    CommitReply commit_reply{CommitReply::Published};
    size_t begin_calls{0};
    size_t commit_calls{0};
    size_t abort_calls{0};
    int source_fd{-1};
    int destination_parent_fd{-1};
    bool source_descriptor_valid{false};
    bool destination_parent_descriptor_valid{false};
    bool source_descriptor_cloexec{false};
    bool destination_parent_descriptor_cloexec{false};
    struct stat source_stat{};
    struct stat destination_parent_stat{};
    std::optional<staging_protocol::BeginRequest> begin_request;
    std::optional<staging_protocol::CommitRequest> commit_request;
    std::optional<staging_protocol::AbortRequest> abort_request;
};

class FaultConditionalCopyIO final : public native::ConditionalCopyIO
{
public:
    enum class Fault {
        None,
        PostCloneSourceMetadata,
        DestinationMetadata,
        DestinationMetadataAndParentFSync,
        DestinationFSync,
        ParentFSync,
        FullFSync,
        FullFSyncInterruptedOnce,
        CloneDestinationExists,
        CloneErrorWithoutPublication,
        CloneErrorWithPublication,
        CloneErrorAfterPublishAndRemoval
    };

    enum class Step : uint8_t {
        Clone,
        DestinationFSync,
        ParentFSync,
        FullFSync
    };

    explicit FaultConditionalCopyIO(Fault _fault) : fault{_fault} {}

    int Open(const char *_path, int _flags) noexcept override
    {
        const int fd = ConditionalCopyIO::Open(_path, _flags);
        if( fd >= 0 ) {
            if( (_flags & O_DIRECTORY) != 0 )
                destination_parent_fd = fd;
            else
                source_fd = fd;
        }
        return fd;
    }

    int OpenAt(int _directory_fd, const char *_name, int _flags) noexcept override
    {
        destination_fd = ConditionalCopyIO::OpenAt(_directory_fd, _name, _flags);
        return destination_fd;
    }

    int Clone(int _source_fd, int _destination_parent_fd, const char *_name, uint32_t _flags) noexcept override
    {
        Record(Step::Clone);
        if( fault == Fault::CloneDestinationExists ) {
            errno = EEXIST;
            return -1;
        }
        if( fault == Fault::CloneErrorWithoutPublication ) {
            errno = EIO;
            return -1;
        }
        const int result = ConditionalCopyIO::Clone(_source_fd, _destination_parent_fd, _name, _flags);
        if( result == 0 )
            clone_published = true;
        if( result == 0 && fault == Fault::CloneErrorAfterPublishAndRemoval ) {
            const int remove_result = unlinkat(_destination_parent_fd, _name, 0);
            if( remove_result != 0 )
                return remove_result;
            errno = EIO;
            return -1;
        }
        if( result == 0 && fault == Fault::CloneErrorWithPublication ) {
            errno = EIO;
            return -1;
        }
        return result;
    }

    int FSync(int _fd) noexcept override
    {
        if( _fd == destination_fd ) {
            ++destination_fsync_calls;
            Record(Step::DestinationFSync);
            if( fault == Fault::DestinationFSync ) {
                errno = EIO;
                return -1;
            }
        }
        else if( _fd == destination_parent_fd ) {
            ++parent_fsync_calls;
            Record(Step::ParentFSync);
            if( fault == Fault::ParentFSync || fault == Fault::DestinationMetadataAndParentFSync ) {
                errno = ENOSPC;
                return -1;
            }
        }
        return ConditionalCopyIO::FSync(_fd);
    }

    int FullFSync(int _fd) noexcept override
    {
        ++full_fsync_calls;
        Record(Step::FullFSync);
        if( fault == Fault::FullFSync ) {
            errno = EROFS;
            return -1;
        }
        if( fault == Fault::FullFSyncInterruptedOnce && full_fsync_calls == 1 ) {
            errno = EINTR;
            return -1;
        }
        return ConditionalCopyIO::FullFSync(_fd);
    }

    void Checkpoint(native::ConditionalCopyCheckpoint _checkpoint) noexcept override
    {
        if( checkpoint_count < checkpoints.size() ) {
            checkpoints[checkpoint_count] = _checkpoint;
            checkpoint_step_counts[checkpoint_count] = step_count;
            ++checkpoint_count;
        }
    }

    std::expected<native::ConditionalCopyMetadataSnapshot, int> CaptureMetadata(int _fd) noexcept override
    {
        auto metadata = ConditionalCopyIO::CaptureMetadata(_fd);
        if( metadata && clone_published &&
            ((fault == Fault::PostCloneSourceMetadata && _fd == source_fd) ||
             ((fault == Fault::DestinationMetadata || fault == Fault::DestinationMetadataAndParentFSync) &&
              _fd == destination_fd)) ) {
            ++metadata->size;
        }
        return metadata;
    }

    Fault fault;
    int source_fd{-1};
    int destination_parent_fd{-1};
    int destination_fd{-1};
    bool clone_published{false};
    size_t destination_fsync_calls{0};
    size_t parent_fsync_calls{0};
    size_t full_fsync_calls{0};
    std::array<Step, 8> steps{};
    size_t step_count{0};
    std::array<native::ConditionalCopyCheckpoint, 2> checkpoints{};
    std::array<size_t, 2> checkpoint_step_counts{};
    size_t checkpoint_count{0};

private:
    void Record(Step _step) noexcept
    {
        if( step_count < steps.size() )
            steps[step_count++] = _step;
    }
};

static ProviderConditionalCopyCommitResult PublishedSuccess() noexcept
{
    return ProviderConditionalCopyCommitResult{
        .publication = ProviderConditionalCopyPublicationState::Published,
        .failure = ProviderConditionalCopyCommitFailure::None,
        .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
    };
}

static ProviderConditionalCopyCommitResult PublishedMetadataFailure(
    int _metadata_error,
    ProviderConditionalCopyFilesystemSyncStatus _sync_status = ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
    int _sync_error = 0) noexcept
{
    return ProviderConditionalCopyCommitResult{
        .publication = ProviderConditionalCopyPublicationState::Published,
        .failure = ProviderConditionalCopyCommitFailure::MetadataFailed,
        .system_error = _metadata_error,
        .filesystem_sync_status = _sync_status,
        .filesystem_sync_system_error = _sync_error,
    };
}

static ProviderConditionalCopyCommitResult PublishedSyncFailure(int _error) noexcept
{
    return ProviderConditionalCopyCommitResult{
        .publication = ProviderConditionalCopyPublicationState::Published,
        .failure = ProviderConditionalCopyCommitFailure::FileSystemSyncFailed,
        .system_error = _error,
        .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Failed,
        .filesystem_sync_system_error = _error,
    };
}

TEST_CASE(PREFIX "publishes an exclusive clone only at Commit")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "reviewed payload");
    REQUIRE(chmod(paths.source.c_str(), 0640) == 0);
    constexpr char xattr_name[] = "com.duckcommander.conditional-copy-test";
    constexpr char xattr_value[] = "reviewed metadata";
    REQUIRE(setxattr(paths.source.c_str(), xattr_name, xattr_value, sizeof(xattr_value) - 1, 0, 0) == 0);
    SetReadACLForCurrentUser(paths.source);
    REQUIRE(chflags(paths.source.c_str(), UF_HIDDEN) == 0);
    RequireCloneCapable(paths.destination_parent);

    auto transaction = TestEnv().vfs_native->BeginConditionalCopyTransaction(
        Authority(Claims(TestEnv().vfs_native, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    CHECK_FALSE(std::filesystem::exists(paths.destination));

    CHECK((*transaction)->Commit() == PublishedSuccess());
    const auto source_metadata = CaptureMetadata(paths.source);
    const auto destination_metadata = CaptureMetadata(paths.destination);
    CHECK(native::ConditionalCopyMetadataMatchesClone(source_metadata, destination_metadata));
    CHECK(Read(paths.destination) == "reviewed payload");
    CHECK_FALSE((*transaction)->IsPending());
    CHECK((*transaction)->Commit() == PublishedSuccess());
}

TEST_CASE(PREFIX "fails closed when reviewed source evidence becomes stale")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "before");
    RequireCloneCapable(paths.destination_parent);

    auto transaction = TestEnv().vfs_native->BeginConditionalCopyTransaction(
        Authority(Claims(TestEnv().vfs_native, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    Write(paths.source, "after review changed the exact byte size");

    CHECK(((*transaction)->Commit() ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::SourceStale,
                                               ESTALE}));
    CHECK_FALSE(std::filesystem::exists(paths.destination));
}

TEST_CASE(PREFIX "fails closed when reviewed destination parent evidence becomes stale")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto transaction = TestEnv().vfs_native->BeginConditionalCopyTransaction(
        Authority(Claims(TestEnv().vfs_native, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    Write(paths.destination_parent + "/unrelated.txt", "namespace mutation");

    CHECK(((*transaction)->Commit() ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::DestinationParentStale,
                                               ESTALE}));
    CHECK_FALSE(std::filesystem::exists(paths.destination));
}

TEST_CASE(PREFIX "monotonic growth tolerance publishes though the destination parent grew since review")
{
    // What a reviewed batch's own earlier item does to a shared destination directory: its size,
    // content timestamps and (confirmed empirically on APFS) its link_count all advance the moment a
    // sibling is created, whether that sibling is this batch's or not. The mechanism cannot tell the
    // two apart - that is the accepted, documented shape of this tolerance, not a gap closed here -
    // so this stands for either case: a real batch's own growth looks exactly like this to a
    // transaction, which is what makes an authorized batch able to finish at all.
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto transaction = TestEnv().vfs_native->BeginConditionalCopyTransaction(Authority(
        Claims(TestEnv().vfs_native,
              paths.source,
              paths.destination_parent,
              paths.destination,
              ProviderConditionalCopyExpectationTolerance::MonotonicGrowth)));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    Write(paths.destination_parent + "/sibling.txt", "grown before this item's own commit");

    CHECK((*transaction)->Commit() == PublishedSuccess());
    CHECK(Read(paths.destination) == "payload");
}

TEST_CASE(PREFIX "monotonic growth tolerance still fails closed on a destination parent mode change")
{
    // Growth is expected; a changed permission bit is not. Content and permissions are different
    // questions, and tolerating the first must not quietly tolerate the second.
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto transaction = TestEnv().vfs_native->BeginConditionalCopyTransaction(Authority(
        Claims(TestEnv().vfs_native,
              paths.source,
              paths.destination_parent,
              paths.destination,
              ProviderConditionalCopyExpectationTolerance::MonotonicGrowth)));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    REQUIRE(chmod(paths.destination_parent.c_str(), 0700) == 0);

    CHECK(((*transaction)->Commit() ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::DestinationParentStale,
                                               ESTALE}));
    CHECK_FALSE(std::filesystem::exists(paths.destination));
}

TEST_CASE(PREFIX "monotonic growth tolerance still fails closed on a destination parent flags change")
{
    // The narrower stat check has no field for BSD flags at all - only the fuller metadata re-check
    // does - so this exercises that second check specifically, the one place flags are verified for
    // the destination parent.
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto transaction = TestEnv().vfs_native->BeginConditionalCopyTransaction(Authority(
        Claims(TestEnv().vfs_native,
              paths.source,
              paths.destination_parent,
              paths.destination,
              ProviderConditionalCopyExpectationTolerance::MonotonicGrowth)));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    REQUIRE(chflags(paths.destination_parent.c_str(), UF_HIDDEN) == 0);

    CHECK(((*transaction)->Commit() ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::DestinationParentStale,
                                               ESTALE}));
    CHECK_FALSE(std::filesystem::exists(paths.destination));
}

TEST_CASE(PREFIX "rejects an existing destination before minting authority")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "source");
    Write(paths.destination, "existing");
    RequireCloneCapable(paths.destination_parent);

    const auto transaction = TestEnv().vfs_native->BeginConditionalCopyTransaction(
        Authority(Claims(TestEnv().vfs_native, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE_FALSE(transaction);
    CHECK(transaction.error() == ProviderConditionalCopyTransactionBeginError::DestinationExists);
    CHECK(Read(paths.destination) == "existing");
}

TEST_CASE(PREFIX "preserves a destination created after Begin")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "source");
    RequireCloneCapable(paths.destination_parent);

    auto transaction = TestEnv().vfs_native->BeginConditionalCopyTransaction(
        Authority(Claims(TestEnv().vfs_native, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    Write(paths.destination, "racing destination");

    CHECK(((*transaction)->Commit() ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::DestinationExists,
                                               EEXIST}));
    CHECK(Read(paths.destination) == "racing destination");
}

TEST_CASE(PREFIX "reports exact destination-exists evidence from exclusive clone")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "source");
    RequireCloneCapable(paths.destination_parent);

    auto io = std::make_shared<FaultConditionalCopyIO>(FaultConditionalCopyIO::Fault::CloneDestinationExists);
    auto host = std::make_shared<NativeHost>(*TestEnv().native_fs_man, *TestEnv().fsevents_file_update, io);
    auto transaction = host->BeginConditionalCopyTransaction(
        Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);
    REQUIRE(*transaction);

    CHECK(((*transaction)->Commit() ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::DestinationExists,
                                               EEXIST}));
    CHECK_FALSE(std::filesystem::exists(paths.destination));
    CHECK(io->step_count == 1);
    CHECK(io->steps[0] == FaultConditionalCopyIO::Step::Clone);
}

TEST_CASE(PREFIX "rejects symlink resolution while anchoring reviewed descriptors")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    SECTION("source path")
    {
        const auto source_symlink = paths.source_parent + "/source-link.txt";
        std::filesystem::create_symlink(paths.source, source_symlink);
        const auto transaction = TestEnv().vfs_native->BeginConditionalCopyTransaction(
            Authority(Claims(TestEnv().vfs_native, source_symlink, paths.destination_parent, paths.destination)));
        REQUIRE_FALSE(transaction);
        CHECK(transaction.error() == ProviderConditionalCopyTransactionBeginError::SourceStale);
    }

    SECTION("destination parent path")
    {
        const auto destination_parent_symlink =
            std::filesystem::path{paths.destination_parent}.parent_path().string() + "/destination-link";
        std::filesystem::create_directory_symlink(paths.destination_parent, destination_parent_symlink);
        const auto linked_destination = destination_parent_symlink + "/destination.txt";
        const auto transaction = TestEnv().vfs_native->BeginConditionalCopyTransaction(
            Authority(Claims(TestEnv().vfs_native, paths.source, destination_parent_symlink, linked_destination)));
        REQUIRE_FALSE(transaction);
        CHECK(transaction.error() == ProviderConditionalCopyTransactionBeginError::DestinationParentStale);
    }

    CHECK_FALSE(std::filesystem::exists(paths.destination));
}

TEST_CASE(PREFIX "abort and cancellation leave the namespace unchanged")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto transaction = TestEnv().vfs_native->BeginConditionalCopyTransaction(
        Authority(Claims(TestEnv().vfs_native, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    CHECK(((*transaction)->Abort() ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::Aborted}));
    CHECK_FALSE(std::filesystem::exists(paths.destination));

    const auto cancelled = TestEnv().vfs_native->BeginConditionalCopyTransaction(
        Authority(Claims(TestEnv().vfs_native, paths.source, paths.destination_parent, paths.destination)),
        [] { return true; });
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error() == ProviderConditionalCopyTransactionBeginError::Cancelled);
    CHECK_FALSE(std::filesystem::exists(paths.destination));

    auto late_cancelled = TestEnv().vfs_native->BeginConditionalCopyTransaction(
        Authority(Claims(TestEnv().vfs_native, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(late_cancelled);
    REQUIRE(*late_cancelled);
    int cancellation_checks = 0;
    CHECK(((*late_cancelled)->Commit([&] { return ++cancellation_checks >= 2; }) ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::Cancelled}));
    CHECK(cancellation_checks == 2);
    CHECK_FALSE(std::filesystem::exists(paths.destination));
}

TEST_CASE(PREFIX "requires same-volume clone capability from NativeFSManager")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");

    GateNativeFSManager::Mode mode = GateNativeFSManager::Mode::CloneDisabled;
    SECTION("clone capability is absent")
    {
        mode = GateNativeFSManager::Mode::CloneDisabled;
    }
    SECTION("VolumeFromFD resolves different volumes")
    {
        mode = GateNativeFSManager::Mode::DifferentVolumes;
    }

    GateNativeFSManager native_fs_manager{mode};
    auto host = std::make_shared<NativeHost>(native_fs_manager, *TestEnv().fsevents_file_update);
    const auto transaction = host->BeginConditionalCopyTransaction(
        Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE_FALSE(transaction);
    CHECK(transaction.error() == ProviderConditionalCopyTransactionBeginError::Unsupported);
    CHECK(native_fs_manager.VolumeFromFDCalls() == 2);
    CHECK_FALSE(std::filesystem::exists(paths.destination));
}

TEST_CASE(PREFIX "probes path-specific conditional Copy support conservatively")
{
    const ConditionalCopyPaths paths;
    auto mode = GateNativeFSManager::Mode::Supported;
    auto expected = ProviderConditionalCopyPathSupport::SameVolumeClone;

    SECTION("supported internal APFS volume")
    {
    }
    SECTION("clone capability is absent")
    {
        mode = GateNativeFSManager::Mode::CloneDisabled;
        expected = ProviderConditionalCopyPathSupport::Unsupported;
    }
    SECTION("paths resolve to different volumes")
    {
        mode = GateNativeFSManager::Mode::DifferentVolumes;
        expected = ProviderConditionalCopyPathSupport::Unsupported;
    }
    SECTION("path volume cannot be resolved")
    {
        mode = GateNativeFSManager::Mode::MissingPathVolume;
        expected = ProviderConditionalCopyPathSupport::Unavailable;
    }

    GateNativeFSManager native_fs_manager{mode};
    const auto host = std::make_shared<NativeHost>(native_fs_manager, *TestEnv().fsevents_file_update);
    CHECK(host->ConditionalCopyPathSupport(paths.source, paths.destination_parent) == expected);
    CHECK(host->ConditionalCopyPathSupport("relative-source", paths.destination_parent) ==
          ProviderConditionalCopyPathSupport::Unavailable);
}

TEST_CASE(PREFIX "publishes a Move through an atomic exclusive rename")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto transaction = TestEnv().vfs_native->BeginConditionalMoveTransaction(MoveAuthority(MoveClaims(
        TestEnv().vfs_native, paths.source_parent, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);
    REQUIRE(*transaction);

    CHECK((*transaction)->Commit() == PublishedSuccess());
    // One indivisible operation: the destination exists and the source does not, and there is no
    // state in between for a journal result to have to describe.
    CHECK(Read(paths.destination) == "payload");
    CHECK_FALSE(std::filesystem::exists(paths.source));
}

TEST_CASE(PREFIX "refuses a Move onto a destination that appeared after review")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto transaction = TestEnv().vfs_native->BeginConditionalMoveTransaction(MoveAuthority(MoveClaims(
        TestEnv().vfs_native, paths.source_parent, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    Write(paths.destination, "someone else got there first");

    // Create-only is the contract rather than an optimisation, and RENAME_EXCL is how it is kept: the
    // destination is never replaced, and the source stays where it is.
    CHECK(((*transaction)->Commit() ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::DestinationExists,
                                               EEXIST}));
    CHECK(Read(paths.destination) == "someone else got there first");
    CHECK(Read(paths.source) == "payload");
}

TEST_CASE(PREFIX "fails closed when the source name is re-pointed at another object")
{
    // The check a Copy has no need of, and the reason a Move anchors the source's directory. Holding
    // the source open proves the reviewed object still exists; it says nothing about what its *name*
    // leads to now, and the rename would act on the name. Without the name-to-identity check at
    // commit, this would move the substituted file - which is the failure the anchoring exists for.
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto transaction = TestEnv().vfs_native->BeginConditionalMoveTransaction(MoveAuthority(MoveClaims(
        TestEnv().vfs_native, paths.source_parent, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);
    REQUIRE(*transaction);

    REQUIRE(std::filesystem::remove(paths.source));
    Write(paths.source, "a different file wearing the same name");

    CHECK(((*transaction)->Commit() ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::SourceStale,
                                               ESTALE}));
    CHECK_FALSE(std::filesystem::exists(paths.destination));
    CHECK(Read(paths.source) == "a different file wearing the same name");
}

TEST_CASE(PREFIX "fails closed when either directory changed since the Move was reviewed")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto expected_failure = ProviderConditionalCopyCommitFailure::SourceStale;
    std::string changed;
    SECTION("the source parent")
    {
        // Reported as source staleness rather than through a word of its own: the consumer's question
        // is whether the world around the source moved, and a separate term would not change what it
        // has to do about the answer.
        changed = paths.source_parent;
    }
    SECTION("the destination parent")
    {
        changed = paths.destination_parent;
        expected_failure = ProviderConditionalCopyCommitFailure::DestinationParentStale;
    }

    auto transaction = TestEnv().vfs_native->BeginConditionalMoveTransaction(MoveAuthority(MoveClaims(
        TestEnv().vfs_native, paths.source_parent, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    REQUIRE(chmod(changed.c_str(), 0700) == 0);

    CHECK(((*transaction)->Commit() ==
           ProviderConditionalCopyCommitResult{
               ProviderConditionalCopyPublicationState::NotPublished, expected_failure, ESTALE}));
    CHECK_FALSE(std::filesystem::exists(paths.destination));
    CHECK(Read(paths.source) == "payload");
}

TEST_CASE(PREFIX "abandons a Move at its last cancellation point without touching either directory")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto transaction = TestEnv().vfs_native->BeginConditionalMoveTransaction(MoveAuthority(MoveClaims(
        TestEnv().vfs_native, paths.source_parent, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);
    REQUIRE(*transaction);

    CHECK(((*transaction)->Commit([] { return true; }) ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::Cancelled,
                                               0}));
    CHECK_FALSE(std::filesystem::exists(paths.destination));
    CHECK(Read(paths.source) == "payload");
}

TEST_CASE(PREFIX "refuses to begin a Move whose claims no longer describe the disk")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);
    const auto claims = MoveClaims(
        TestEnv().vfs_native, paths.source_parent, paths.source, paths.destination_parent, paths.destination);

    SECTION("the destination appeared after review, which the directory notices first")
    {
        // The same shape as the vanished source below, and correct for the same reason: creating an
        // entry *is* a change to the directory, and the directory is sealed by the review. Reported as
        // the more specific fact rather than as a bare collision.
        Write(paths.destination, "occupied");
        CHECK(TestEnv().vfs_native->BeginConditionalMoveTransaction(MoveAuthority(claims)).error() ==
              ProviderConditionalMoveTransactionBeginError::DestinationParentStale);
    }
    SECTION("the destination was already occupied when the Move was reviewed")
    {
        // What `DestinationExists` is actually for: the directory is exactly as reviewed, and the name
        // inside it is taken. Nothing moved - the plan was never publishable.
        Write(paths.destination, "occupied");
        const auto occupied_claims = MoveClaims(
            TestEnv().vfs_native, paths.source_parent, paths.source, paths.destination_parent, paths.destination);
        CHECK(TestEnv().vfs_native->BeginConditionalMoveTransaction(MoveAuthority(occupied_claims)).error() ==
              ProviderConditionalMoveTransactionBeginError::DestinationExists);
    }
    SECTION("the source is gone, which the directory notices first")
    {
        // Written expecting `SourceStale`, and the run said `SourceParentStale`. It is right and the
        // expectation was wrong: removing the source *is* a change to the directory holding it, and
        // that directory is checked first because the source is opened through it. So a vanished
        // source is reported as its directory having moved, which is true and is the more specific
        // fact - the entry the rename was going to name is no longer the entry that was reviewed.
        REQUIRE(std::filesystem::remove(paths.source));
        CHECK(TestEnv().vfs_native->BeginConditionalMoveTransaction(MoveAuthority(claims)).error() ==
              ProviderConditionalMoveTransactionBeginError::SourceParentStale);
    }
    SECTION("the source changed since review")
    {
        // What reaches `SourceStale` on Begin: the file itself moved on while its directory did not,
        // which writing into it does and unlinking it cannot.
        Write(paths.source, "different payload entirely");
        CHECK(TestEnv().vfs_native->BeginConditionalMoveTransaction(MoveAuthority(claims)).error() ==
              ProviderConditionalMoveTransactionBeginError::SourceStale);
    }
    SECTION("the source parent changed since review")
    {
        REQUIRE(chmod(paths.source_parent.c_str(), 0700) == 0);
        CHECK(TestEnv().vfs_native->BeginConditionalMoveTransaction(MoveAuthority(claims)).error() ==
              ProviderConditionalMoveTransactionBeginError::SourceParentStale);
    }
    SECTION("the claimed source parent does not hold the source")
    {
        auto elsewhere = claims;
        elsewhere.source_parent = Expectation(paths.destination_parent, ProviderConditionalCopyExpectedKind::Directory);
        CHECK(TestEnv().vfs_native->BeginConditionalMoveTransaction(MoveAuthority(std::move(elsewhere))).error() ==
              ProviderConditionalMoveTransactionBeginError::InvalidRequest);
    }
    // No shared closing assertion here: each section leaves the disk in a different state, and one of
    // them puts a file at the destination itself. Every section already asserts the only thing that
    // matters - that Begin refused, so no transaction exists to publish anything.
}

TEST_CASE(PREFIX "publishes a Delete through an anchored unlinkat")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");

    auto transaction = TestEnv().vfs_native->BeginConditionalDeleteTransaction(
        DeleteAuthority(DeleteClaims(TestEnv().vfs_native, paths.source_parent, paths.source)));
    REQUIRE(transaction);
    REQUIRE(*transaction);

    // Reused as-is: `Published` here means the transaction's one durable outcome happened, which for
    // a Delete is removal rather than creation - the same generic result type Copy and Move use.
    CHECK((*transaction)->Commit() == PublishedSuccess());
    CHECK_FALSE(std::filesystem::exists(paths.source));
}

TEST_CASE(PREFIX "fails closed when the source name is re-pointed at another object before a Delete commits")
{
    // The mirror of the identical Move case: holding the source open proves the reviewed object still
    // exists, not that its *name* still leads to it, and unlinkat acts on the name.
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");

    auto transaction = TestEnv().vfs_native->BeginConditionalDeleteTransaction(
        DeleteAuthority(DeleteClaims(TestEnv().vfs_native, paths.source_parent, paths.source)));
    REQUIRE(transaction);
    REQUIRE(*transaction);

    REQUIRE(std::filesystem::remove(paths.source));
    Write(paths.source, "a different file wearing the same name");

    CHECK(((*transaction)->Commit() ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::SourceStale,
                                               ESTALE}));
    CHECK(Read(paths.source) == "a different file wearing the same name");
}

TEST_CASE(PREFIX "fails closed when the source parent changed since the Delete was reviewed")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");

    auto transaction = TestEnv().vfs_native->BeginConditionalDeleteTransaction(
        DeleteAuthority(DeleteClaims(TestEnv().vfs_native, paths.source_parent, paths.source)));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    REQUIRE(chmod(paths.source_parent.c_str(), 0700) == 0);

    CHECK(((*transaction)->Commit() ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::SourceStale,
                                               ESTALE}));
    CHECK(Read(paths.source) == "payload");
}

TEST_CASE(PREFIX "abandons a Delete at its last cancellation point without touching the source")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");

    auto transaction = TestEnv().vfs_native->BeginConditionalDeleteTransaction(
        DeleteAuthority(DeleteClaims(TestEnv().vfs_native, paths.source_parent, paths.source)));
    REQUIRE(transaction);
    REQUIRE(*transaction);

    CHECK(((*transaction)->Commit([] { return true; }) ==
           ProviderConditionalCopyCommitResult{
               ProviderConditionalCopyPublicationState::NotPublished, ProviderConditionalCopyCommitFailure::Cancelled, 0}));
    CHECK(Read(paths.source) == "payload");
}

TEST_CASE(PREFIX "refuses to begin a Delete whose claims no longer describe the disk")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    const auto claims = DeleteClaims(TestEnv().vfs_native, paths.source_parent, paths.source);

    SECTION("the source is gone, which the directory notices first")
    {
        // The same shape Move's own Begin already established: removing the source is a change to the
        // directory holding it, and that directory is checked first because the source is opened
        // through it.
        REQUIRE(std::filesystem::remove(paths.source));
        CHECK(TestEnv().vfs_native->BeginConditionalDeleteTransaction(DeleteAuthority(claims)).error() ==
              ProviderConditionalDeleteTransactionBeginError::SourceParentStale);
    }
    SECTION("the source changed since review")
    {
        Write(paths.source, "different payload entirely");
        CHECK(TestEnv().vfs_native->BeginConditionalDeleteTransaction(DeleteAuthority(claims)).error() ==
              ProviderConditionalDeleteTransactionBeginError::SourceStale);
    }
    SECTION("the source parent changed since review")
    {
        REQUIRE(chmod(paths.source_parent.c_str(), 0700) == 0);
        CHECK(TestEnv().vfs_native->BeginConditionalDeleteTransaction(DeleteAuthority(claims)).error() ==
              ProviderConditionalDeleteTransactionBeginError::SourceParentStale);
    }
    SECTION("the claimed source parent does not hold the source")
    {
        auto elsewhere = claims;
        elsewhere.source_parent = Expectation(paths.destination_parent, ProviderConditionalCopyExpectedKind::Directory);
        CHECK(TestEnv().vfs_native->BeginConditionalDeleteTransaction(DeleteAuthority(std::move(elsewhere))).error() ==
              ProviderConditionalDeleteTransactionBeginError::InvalidRequest);
    }
}

TEST_CASE(PREFIX "tolerates its own batch removing several entries from the shared source parent")
{
    // The Delete-only mirror of the Copy/Move batch's shared destination-parent growth tolerance: a
    // batch removing several files from one folder legitimately shrinks it as its own earlier items
    // complete, and `MonotonicShrink` is what tells that apart from someone else having touched it.
    const ConditionalCopyPaths paths;
    const auto second_source = paths.source_parent + "/second.txt";
    Write(paths.source, "first payload");
    Write(second_source, "second payload");

    auto first_transaction = TestEnv().vfs_native->BeginConditionalDeleteTransaction(
        DeleteAuthority(DeleteClaims(TestEnv().vfs_native, paths.source_parent, paths.source)));
    REQUIRE(first_transaction);
    REQUIRE(*first_transaction);
    auto second_transaction = TestEnv().vfs_native->BeginConditionalDeleteTransaction(DeleteAuthority(DeleteClaims(
        TestEnv().vfs_native,
        paths.source_parent,
        second_source,
        ProviderConditionalCopyExpectationTolerance::MonotonicShrink)));
    REQUIRE(second_transaction);
    REQUIRE(*second_transaction);

    CHECK((*first_transaction)->Commit() == PublishedSuccess());
    CHECK_FALSE(std::filesystem::exists(paths.source));
    // The second item's own review sealed the parent before the first item's removal shrank it - an
    // `Exact` expectation here would now read that shrink as someone else having touched the folder.
    CHECK((*second_transaction)->Commit() == PublishedSuccess());
    CHECK_FALSE(std::filesystem::exists(second_source));
}

TEST_CASE(PREFIX "answers conditional Move eligibility without inferring it from Copy")
{
    // The point of the case: neither answer may be derived from the other. A Move publishes through
    // atomic exclusive rename and a Copy through cloning, and a volume can offer one interface and not
    // the other - so the two sections below deliberately disagree in opposite directions on the same
    // volume, which is the only way to show the question is really being asked twice.
    const ConditionalCopyPaths paths;

    SECTION("a supported volume answers both")
    {
        GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::Supported};
        const auto host = std::make_shared<NativeHost>(native_fs_manager, *TestEnv().fsevents_file_update);
        CHECK(host->ConditionalMovePathSupport(paths.source, paths.destination_parent) ==
              ProviderConditionalMovePathSupport::SameVolumeRename);
        CHECK(host->ConditionalCopyPathSupport(paths.source, paths.destination_parent) ==
              ProviderConditionalCopyPathSupport::SameVolumeClone);
    }

    SECTION("no atomic exclusive rename refuses the Move and leaves the Copy eligible")
    {
        GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::RenameExclDisabled};
        const auto host = std::make_shared<NativeHost>(native_fs_manager, *TestEnv().fsevents_file_update);
        CHECK(host->ConditionalMovePathSupport(paths.source, paths.destination_parent) ==
              ProviderConditionalMovePathSupport::Unsupported);
        CHECK(host->ConditionalCopyPathSupport(paths.source, paths.destination_parent) ==
              ProviderConditionalCopyPathSupport::SameVolumeClone);
    }

    SECTION("no clone refuses the Copy and leaves the Move eligible")
    {
        GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::CloneDisabled};
        const auto host = std::make_shared<NativeHost>(native_fs_manager, *TestEnv().fsevents_file_update);
        CHECK(host->ConditionalMovePathSupport(paths.source, paths.destination_parent) ==
              ProviderConditionalMovePathSupport::SameVolumeRename);
        CHECK(host->ConditionalCopyPathSupport(paths.source, paths.destination_parent) ==
              ProviderConditionalCopyPathSupport::Unsupported);
    }

    SECTION("two volumes are a definitive refusal, not a staged scope")
    {
        // A cross-volume Move is copy-then-unlink, and a journal item result has no way to say
        // "published, and the source is still there". There is nothing to stage towards, so this is
        // `Unsupported` rather than the `CrossVolumeStaged` answer the Copy question can give.
        GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::DifferentVolumes};
        const auto host = std::make_shared<NativeHost>(native_fs_manager, *TestEnv().fsevents_file_update);
        CHECK(host->ConditionalMovePathSupport(paths.source, paths.destination_parent) ==
              ProviderConditionalMovePathSupport::Unsupported);
    }

    SECTION("an unresolvable volume or a relative path is not an answer at all")
    {
        GateNativeFSManager missing{GateNativeFSManager::Mode::MissingPathVolume};
        const auto missing_host = std::make_shared<NativeHost>(missing, *TestEnv().fsevents_file_update);
        CHECK(missing_host->ConditionalMovePathSupport(paths.source, paths.destination_parent) ==
              ProviderConditionalMovePathSupport::Unavailable);

        GateNativeFSManager supported{GateNativeFSManager::Mode::Supported};
        const auto host = std::make_shared<NativeHost>(supported, *TestEnv().fsevents_file_update);
        CHECK(host->ConditionalMovePathSupport("relative-source", paths.destination_parent) ==
              ProviderConditionalMovePathSupport::Unavailable);
        CHECK(host->ConditionalMovePathSupport(paths.source, "relative-parent") ==
              ProviderConditionalMovePathSupport::Unavailable);
    }
}

TEST_CASE(PREFIX "answers conditional Delete eligibility independently of Copy's and Move's own interfaces")
{
    // A Delete asks for neither `clone` nor `rename_excl` - `unlinkat` removes an entry, it does not
    // write or rename one - so a volume missing either interface must still answer Delete-eligible,
    // which is the only way to show the question is not silently inheriting Copy's or Move's answer.
    const ConditionalCopyPaths paths;

    SECTION("a supported volume answers eligible")
    {
        GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::Supported};
        const auto host = std::make_shared<NativeHost>(native_fs_manager, *TestEnv().fsevents_file_update);
        CHECK(host->ConditionalDeletePathSupport(paths.source) ==
              ProviderConditionalDeletePathSupport::SameVolumeUnlink);
    }

    SECTION("no atomic exclusive rename still leaves Delete eligible")
    {
        GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::RenameExclDisabled};
        const auto host = std::make_shared<NativeHost>(native_fs_manager, *TestEnv().fsevents_file_update);
        CHECK(host->ConditionalDeletePathSupport(paths.source) ==
              ProviderConditionalDeletePathSupport::SameVolumeUnlink);
    }

    SECTION("no clone still leaves Delete eligible")
    {
        GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::CloneDisabled};
        const auto host = std::make_shared<NativeHost>(native_fs_manager, *TestEnv().fsevents_file_update);
        CHECK(host->ConditionalDeletePathSupport(paths.source) ==
              ProviderConditionalDeletePathSupport::SameVolumeUnlink);
    }

    SECTION("an unresolvable volume or a relative path is not an answer at all")
    {
        GateNativeFSManager missing{GateNativeFSManager::Mode::MissingPathVolume};
        const auto missing_host = std::make_shared<NativeHost>(missing, *TestEnv().fsevents_file_update);
        CHECK(missing_host->ConditionalDeletePathSupport(paths.source) ==
              ProviderConditionalDeletePathSupport::Unavailable);

        GateNativeFSManager supported{GateNativeFSManager::Mode::Supported};
        const auto host = std::make_shared<NativeHost>(supported, *TestEnv().fsevents_file_update);
        CHECK(host->ConditionalDeletePathSupport("relative-path") ==
              ProviderConditionalDeletePathSupport::Unavailable);
        CHECK(host->ConditionalDeletePathSupport("") == ProviderConditionalDeletePathSupport::Unavailable);
    }
}

TEST_CASE(PREFIX "keeps staged cross-volume eligibility behind its distinct helper authority")
{
    const ConditionalCopyPaths paths;
    GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::DifferentVolumes};

    SECTION("no staged authority preserves the legacy unsupported route")
    {
        const auto host = std::make_shared<NativeHost>(native_fs_manager, *TestEnv().fsevents_file_update);
        CHECK(host->ConditionalCopyPathSupport(paths.source, paths.destination_parent) ==
              ProviderConditionalCopyPathSupport::Unsupported);
    }

    SECTION("unavailable installed authority fails closed before reviewed selection")
    {
        auto authority = std::make_shared<RecordingCrossVolumeStagingAuthority>();
        authority->available = false;
        const auto host = std::make_shared<NativeHost>(native_fs_manager,
                                                       *TestEnv().fsevents_file_update,
                                                       std::make_shared<native::ConditionalCopyIO>(),
                                                       authority);
        CHECK(host->ConditionalCopyPathSupport(paths.source, paths.destination_parent) ==
              ProviderConditionalCopyPathSupport::Unavailable);
    }

    SECTION("available authority advertises the separate staged scope")
    {
        auto authority = std::make_shared<RecordingCrossVolumeStagingAuthority>();
        const auto host = std::make_shared<NativeHost>(native_fs_manager,
                                                       *TestEnv().fsevents_file_update,
                                                       std::make_shared<native::ConditionalCopyIO>(),
                                                       authority);
        CHECK(host->ConditionalCopyPathSupport(paths.source, paths.destination_parent) ==
              ProviderConditionalCopyPathSupport::CrossVolumeStaged);
    }
}

TEST_CASE(PREFIX "passes only anchored descriptors and scalar seals to cross-volume staging")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::DifferentVolumes};
    auto authority = std::make_shared<RecordingCrossVolumeStagingAuthority>();
    const auto host = std::make_shared<NativeHost>(native_fs_manager,
                                                   *TestEnv().fsevents_file_update,
                                                   std::make_shared<native::ConditionalCopyIO>(),
                                                   authority);
    const auto claims = Claims(host, paths.source, paths.destination_parent, paths.destination);

    auto transaction = host->BeginConditionalCopyTransaction(Authority(claims));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    CHECK(authority->begin_calls == 1);
    CHECK(authority->descriptor_seals_match);
    CHECK(authority->destination_name == "destination.txt");
    CHECK(authority->source_seal.device == static_cast<uint64_t>(claims.source.device));
    CHECK(authority->source_seal.inode == claims.source.inode);
    CHECK(authority->destination_parent_seal.device == static_cast<uint64_t>(claims.destination_parent.device));
    CHECK(authority->destination_parent_seal.inode == claims.destination_parent.inode);
    struct stat source_stat{};
    struct stat destination_parent_stat{};
    REQUIRE(stat(paths.source.c_str(), &source_stat) == 0);
    REQUIRE(stat(paths.destination_parent.c_str(), &destination_parent_stat) == 0);
    CHECK(authority->source_seal.uid == static_cast<uint32_t>(source_stat.st_uid));
    CHECK(authority->source_seal.gid == static_cast<uint32_t>(source_stat.st_gid));
    CHECK(authority->source_seal.mode == static_cast<uint32_t>(source_stat.st_mode));
    CHECK(authority->source_seal.flags == static_cast<uint32_t>(source_stat.st_flags));
    CHECK(authority->source_seal.link_count == static_cast<uint64_t>(source_stat.st_nlink));
    CHECK(authority->destination_parent_seal.uid == static_cast<uint32_t>(destination_parent_stat.st_uid));
    CHECK(authority->destination_parent_seal.gid == static_cast<uint32_t>(destination_parent_stat.st_gid));
    CHECK((*transaction)->Abort() ==
          ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                              ProviderConditionalCopyCommitFailure::Aborted});
    CHECK_FALSE(std::filesystem::exists(paths.destination));
}

TEST_CASE(PREFIX "forwards late cancellation to the cross-volume helper before publication")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::DifferentVolumes};
    auto authority = std::make_shared<RecordingCrossVolumeStagingAuthority>();
    const auto host = std::make_shared<NativeHost>(native_fs_manager,
                                                   *TestEnv().fsevents_file_update,
                                                   std::make_shared<native::ConditionalCopyIO>(),
                                                   authority);
    auto transaction = host->BeginConditionalCopyTransaction(
        Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);
    REQUIRE(*transaction);

    int cancellation_checks = 0;
    CHECK(((*transaction)->Commit([&] { return ++cancellation_checks >= 2; }) ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::Cancelled}));
    CHECK(cancellation_checks == 2);
    CHECK(authority->commit_calls == 1);
    CHECK(authority->cancellation_observed);
    CHECK_FALSE(std::filesystem::exists(paths.destination));
}

TEST_CASE(PREFIX "never invokes cross-volume staging before exact Native validation")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::DifferentVolumes};
    auto authority = std::make_shared<RecordingCrossVolumeStagingAuthority>();
    const auto host = std::make_shared<NativeHost>(native_fs_manager,
                                                   *TestEnv().fsevents_file_update,
                                                   std::make_shared<native::ConditionalCopyIO>(),
                                                   authority);

    SECTION("existing destination")
    {
        Write(paths.destination, "existing");
        const auto transaction = host->BeginConditionalCopyTransaction(
            Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
        REQUIRE_FALSE(transaction);
        CHECK(transaction.error() == ProviderConditionalCopyTransactionBeginError::DestinationExists);
        CHECK(authority->begin_calls == 0);
        CHECK(std::filesystem::exists(paths.destination));
    }

    SECTION("authority loss after reviewed selection")
    {
        authority->fail_begin = true;
        const auto transaction = host->BeginConditionalCopyTransaction(
            Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
        REQUIRE_FALSE(transaction);
        CHECK(transaction.error() == ProviderConditionalCopyTransactionBeginError::ProviderFailure);
        CHECK(authority->begin_calls == 1);
        CHECK_FALSE(std::filesystem::exists(paths.destination));
    }
}

TEST_CASE(PREFIX "staging client transfers two anchored descriptors and scalar V1 claims")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::DifferentVolumes};
    auto transport = std::make_shared<RecordingCrossVolumeStagingTransport>();
    auto io = std::make_shared<FaultConditionalCopyIO>(FaultConditionalCopyIO::Fault::None);
    auto client = std::make_shared<native::CrossVolumeStagingClient>(transport);
    const auto host = std::make_shared<NativeHost>(native_fs_manager, *TestEnv().fsevents_file_update, io, client);
    const auto claims = Claims(host, paths.source, paths.destination_parent, paths.destination);

    auto transaction = host->BeginConditionalCopyTransaction(Authority(claims));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    REQUIRE(transport->begin_request);
    CHECK(transport->begin_calls == 1);
    CHECK(transport->source_descriptor_valid);
    CHECK(transport->destination_parent_descriptor_valid);
    CHECK(transport->source_descriptor_cloexec);
    CHECK(transport->destination_parent_descriptor_cloexec);
    CHECK(transport->source_fd != io->source_fd);
    CHECK(transport->destination_parent_fd != io->destination_parent_fd);
    CHECK(transport->source_stat.st_dev == transport->destination_parent_stat.st_dev);
    CHECK(transport->begin_request->header.version == staging_protocol::kProtocolVersion);
    CHECK(transport->begin_request->header.correlation != staging_protocol::CorrelationID{});
    CHECK(transport->begin_request->source.device == static_cast<uint64_t>(transport->source_stat.st_dev));
    CHECK(transport->begin_request->source.inode == static_cast<uint64_t>(transport->source_stat.st_ino));
    CHECK(transport->begin_request->destination_parent.device ==
          static_cast<uint64_t>(transport->destination_parent_stat.st_dev));
    CHECK(transport->begin_request->destination_parent.inode ==
          static_cast<uint64_t>(transport->destination_parent_stat.st_ino));
    const auto name = transport->begin_request->destination_name.Bytes();
    CHECK(std::string{reinterpret_cast<const char *>(name.data()), name.size()} == "destination.txt");
    CHECK_FALSE(std::filesystem::exists(paths.destination));

    CHECK(((*transaction)->Abort() ==
           ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                               ProviderConditionalCopyCommitFailure::Aborted}));
    CHECK(transport->abort_calls == 1);
    REQUIRE(transport->abort_request);
    CHECK(transport->abort_request->header == transport->begin_request->header);
}

TEST_CASE(PREFIX "staging client binds one granted lease to exactly one terminal commit")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::DifferentVolumes};
    auto transport = std::make_shared<RecordingCrossVolumeStagingTransport>();
    auto client = std::make_shared<native::CrossVolumeStagingClient>(transport);
    const auto host = std::make_shared<NativeHost>(native_fs_manager,
                                                   *TestEnv().fsevents_file_update,
                                                   std::make_shared<native::ConditionalCopyIO>(),
                                                   client);

    auto transaction = host->BeginConditionalCopyTransaction(
        Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    CHECK((*transaction)->Commit() == PublishedSuccess());
    CHECK((*transaction)->Commit() == PublishedSuccess());
    CHECK(transport->commit_calls == 1);
    CHECK(transport->abort_calls == 0);
    REQUIRE(transport->begin_request);
    REQUIRE(transport->commit_request);
    CHECK(transport->commit_request->header == transport->begin_request->header);
    CHECK(transport->commit_request->lease.header == transport->begin_request->header);
    CHECK(transport->commit_request->lease.token.bytes[0] == 1);
    CHECK_FALSE(std::filesystem::exists(paths.destination));
}

TEST_CASE(PREFIX "staging client preserves exact begin failures and fails closed after dispatch")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");

    SECTION("an unavailable transport leaves staged support unavailable")
    {
        GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::DifferentVolumes};
        auto transport = std::make_shared<RecordingCrossVolumeStagingTransport>();
        transport->available = false;
        auto client = std::make_shared<native::CrossVolumeStagingClient>(transport);
        const auto host = std::make_shared<NativeHost>(native_fs_manager,
                                                       *TestEnv().fsevents_file_update,
                                                       std::make_shared<native::ConditionalCopyIO>(),
                                                       client);
        CHECK(host->ConditionalCopyPathSupport(paths.source, paths.destination_parent) ==
              ProviderConditionalCopyPathSupport::Unavailable);
        CHECK(transport->begin_calls == 0);
    }

    SECTION("a valid helper rejection retains its exact stale-source result")
    {
        GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::DifferentVolumes};
        auto transport = std::make_shared<RecordingCrossVolumeStagingTransport>();
        transport->begin_reply = RecordingCrossVolumeStagingTransport::BeginReply::RejectedSourceStale;
        auto client = std::make_shared<native::CrossVolumeStagingClient>(transport);
        const auto host = std::make_shared<NativeHost>(native_fs_manager,
                                                       *TestEnv().fsevents_file_update,
                                                       std::make_shared<native::ConditionalCopyIO>(),
                                                       client);
        const auto transaction = host->BeginConditionalCopyTransaction(
            Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
        REQUIRE_FALSE(transaction);
        CHECK(transaction.error() == ProviderConditionalCopyTransactionBeginError::SourceStale);
        CHECK(transport->begin_calls == 1);
        CHECK(transport->abort_calls == 0);
    }

    SECTION("a lost completion after Commit has unknown publication and no second terminal request")
    {
        GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::DifferentVolumes};
        auto transport = std::make_shared<RecordingCrossVolumeStagingTransport>();
        transport->commit_reply = RecordingCrossVolumeStagingTransport::CommitReply::TransportFailure;
        auto client = std::make_shared<native::CrossVolumeStagingClient>(transport);
        const auto host = std::make_shared<NativeHost>(native_fs_manager,
                                                       *TestEnv().fsevents_file_update,
                                                       std::make_shared<native::ConditionalCopyIO>(),
                                                       client);
        auto transaction = host->BeginConditionalCopyTransaction(
            Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
        REQUIRE(transaction);
        REQUIRE(*transaction);
        CHECK(((*transaction)->Commit() ==
               ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::Unknown,
                                                   ProviderConditionalCopyCommitFailure::ProviderFailure,
                                                   EIO}));
        CHECK(transport->commit_calls == 1);
        CHECK(transport->abort_calls == 0);
    }

    SECTION("a terminal not-published Commit is not followed by an illegal Abort")
    {
        GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::DifferentVolumes};
        auto transport = std::make_shared<RecordingCrossVolumeStagingTransport>();
        transport->commit_reply = RecordingCrossVolumeStagingTransport::CommitReply::SourceStale;
        auto client = std::make_shared<native::CrossVolumeStagingClient>(transport);
        const auto host = std::make_shared<NativeHost>(native_fs_manager,
                                                       *TestEnv().fsevents_file_update,
                                                       std::make_shared<native::ConditionalCopyIO>(),
                                                       client);
        auto transaction = host->BeginConditionalCopyTransaction(
            Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
        REQUIRE(transaction);
        REQUIRE(*transaction);
        CHECK(((*transaction)->Commit() ==
               ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                                   ProviderConditionalCopyCommitFailure::SourceStale,
                                                   ESTALE}));
        CHECK(transport->commit_calls == 1);
        CHECK(transport->abort_calls == 0);
    }
}

TEST_CASE(PREFIX "staging client aborts a granted lease on cancellation and abandoned ownership")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");

    SECTION("cancellation after Granted Begin consumes the lease through Abort")
    {
        GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::DifferentVolumes};
        auto transport = std::make_shared<RecordingCrossVolumeStagingTransport>();
        auto client = std::make_shared<native::CrossVolumeStagingClient>(transport);
        const auto host = std::make_shared<NativeHost>(native_fs_manager,
                                                       *TestEnv().fsevents_file_update,
                                                       std::make_shared<native::ConditionalCopyIO>(),
                                                       client);
        int cancellation_checks = 0;
        const auto transaction = host->BeginConditionalCopyTransaction(
            Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)),
            [&] { return ++cancellation_checks >= 4; });
        REQUIRE_FALSE(transaction);
        CHECK(transaction.error() == ProviderConditionalCopyTransactionBeginError::Cancelled);
        CHECK(cancellation_checks >= 4);
        CHECK(transport->begin_calls == 1);
        CHECK(transport->commit_calls == 0);
        CHECK(transport->abort_calls == 1);
    }

    SECTION("generic transaction destruction aborts an uncommitted granted lease")
    {
        GateNativeFSManager native_fs_manager{GateNativeFSManager::Mode::DifferentVolumes};
        auto transport = std::make_shared<RecordingCrossVolumeStagingTransport>();
        auto client = std::make_shared<native::CrossVolumeStagingClient>(transport);
        const auto host = std::make_shared<NativeHost>(native_fs_manager,
                                                       *TestEnv().fsevents_file_update,
                                                       std::make_shared<native::ConditionalCopyIO>(),
                                                       client);
        {
            auto transaction = host->BeginConditionalCopyTransaction(
                Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
            REQUIRE(transaction);
            REQUIRE(*transaction);
        }
        CHECK(transport->begin_calls == 1);
        CHECK(transport->commit_calls == 0);
        CHECK(transport->abort_calls == 1);
    }
}

TEST_CASE(PREFIX "classifies the internal APFS durability policy explicitly")
{
    auto volume = std::make_shared<nc::utility::NativeFileSystemInfo>();
    volume->fs_type_name = "apfs";
    volume->mount_flags.local = true;
    volume->mount_flags.internal = true;
    volume->interfaces.clone = true;
    volume->interfaces.attr_list = true;
    volume->interfaces.extended_attr = true;
    volume->interfaces.extended_security = true;

    auto expected = native::ConditionalCopyVolumeDisposition::Supported;
    auto expected_media = native::ConditionalCopyVolumeMedia::Internal;
    SECTION("supported internal APFS")
    {
    }
    SECTION("different filesystem")
    {
        volume->fs_type_name = "hfs";
        expected = native::ConditionalCopyVolumeDisposition::UnsupportedFilesystem;
    }
    SECTION("non-local")
    {
        volume->mount_flags.local = false;
        expected = native::ConditionalCopyVolumeDisposition::NonLocal;
    }
    SECTION("external media")
    {
        volume->mount_flags.internal = false;
        expected = native::ConditionalCopyVolumeDisposition::UnsupportedExternalMedia;
        expected_media = native::ConditionalCopyVolumeMedia::External;
    }
    SECTION("read-only")
    {
        volume->mount_flags.read_only = true;
        expected = native::ConditionalCopyVolumeDisposition::ReadOnly;
    }
    SECTION("permissions are ignored")
    {
        volume->format.no_permissions = true;
        expected = native::ConditionalCopyVolumeDisposition::UnknownPermissions;
    }
    SECTION("clone is unavailable")
    {
        volume->interfaces.clone = false;
        expected = native::ConditionalCopyVolumeDisposition::CloneUnavailable;
    }
    SECTION("metadata API is incomplete")
    {
        volume->interfaces.extended_security = false;
        expected = native::ConditionalCopyVolumeDisposition::MetadataUnavailable;
    }

    const auto decision = native::EvaluateConditionalCopyVolume(*volume);
    CHECK(decision.disposition == expected);
    CHECK(decision.media == expected_media);
}

TEST_CASE(PREFIX "holds a Move to the same volume policy as a Copy, differing only in the interface it needs")
{
    // Everything the Copy policy demands - APFS, local, internal non-removable media, writable, known
    // permissions, a complete metadata API - is demanded of a Move for the same reasons, since the
    // durability contract and the evidence it rests on are unchanged by publishing through a rename.
    // Exactly one clause differs, and this pins that it is exactly one.
    auto volume = std::make_shared<nc::utility::NativeFileSystemInfo>();
    volume->fs_type_name = "apfs";
    volume->mount_flags.local = true;
    volume->mount_flags.internal = true;
    volume->interfaces.rename_excl = true;
    volume->interfaces.attr_list = true;
    volume->interfaces.extended_attr = true;
    volume->interfaces.extended_security = true;

    auto expected = native::ConditionalCopyVolumeDisposition::Supported;
    SECTION("supported internal APFS, with no clone interface anywhere in sight")
    {
        // Deliberately left false: a Move never asks for it, and a policy that quietly required it
        // would refuse volumes that can perform the operation perfectly well.
        CHECK_FALSE(volume->interfaces.clone);
    }
    SECTION("different filesystem")
    {
        volume->fs_type_name = "hfs";
        expected = native::ConditionalCopyVolumeDisposition::UnsupportedFilesystem;
    }
    SECTION("non-local")
    {
        volume->mount_flags.local = false;
        expected = native::ConditionalCopyVolumeDisposition::NonLocal;
    }
    SECTION("external media")
    {
        volume->mount_flags.internal = false;
        expected = native::ConditionalCopyVolumeDisposition::UnsupportedExternalMedia;
    }
    SECTION("read-only")
    {
        volume->mount_flags.read_only = true;
        expected = native::ConditionalCopyVolumeDisposition::ReadOnly;
    }
    SECTION("permissions are ignored")
    {
        volume->format.no_permissions = true;
        expected = native::ConditionalCopyVolumeDisposition::UnknownPermissions;
    }
    SECTION("atomic exclusive rename is unavailable")
    {
        volume->interfaces.rename_excl = false;
        expected = native::ConditionalCopyVolumeDisposition::AtomicRenameUnavailable;
    }
    SECTION("metadata API is incomplete")
    {
        volume->interfaces.extended_security = false;
        expected = native::ConditionalCopyVolumeDisposition::MetadataUnavailable;
    }

    CHECK(native::EvaluateConditionalMoveVolume(*volume).disposition == expected);
}

TEST_CASE(PREFIX "fails closed for clonefile metadata transformations outside the supported policy")
{
    native::ConditionalCopyMetadataSnapshot source;
    native::ConditionalCopyMetadataSnapshot destination_parent;
    source.uid = static_cast<uint32_t>(geteuid());
    source.gid = 20;
    source.mode = S_IFREG | 0640;
    destination_parent.gid = 20;

    std::optional<native::ConditionalCopyMetadataPolicyError> expected;
    SECTION("supported source")
    {
    }
    SECTION("ownership would be normalized")
    {
        ++source.uid;
        expected = native::ConditionalCopyMetadataPolicyError::UnsupportedOwnership;
    }
    SECTION("set-ID bits would be cleared")
    {
        source.mode |= S_ISUID;
        expected = native::ConditionalCopyMetadataPolicyError::UnsupportedMode;
    }
    SECTION("unsupported flags")
    {
        source.flags = SF_ARCHIVED;
        expected = native::ConditionalCopyMetadataPolicyError::UnsupportedFlags;
    }
    SECTION("destination ACL inheritance would transform the clone")
    {
        destination_parent.acl.push_back(std::byte{1});
        expected = native::ConditionalCopyMetadataPolicyError::DestinationParentACL;
    }

    const auto result = native::ValidateConditionalCopyMetadataPolicy(source, destination_parent);
    if( expected ) {
        REQUIRE_FALSE(result);
        CHECK(result.error() == *expected);
    }
    else {
        CHECK(result.has_value());
    }
}

TEST_CASE(PREFIX "reports known publication when metadata verification fails")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto fault = FaultConditionalCopyIO::Fault::DestinationMetadata;
    auto expected = PublishedMetadataFailure(ESTALE);
    SECTION("source changes during clone")
    {
        fault = FaultConditionalCopyIO::Fault::PostCloneSourceMetadata;
    }
    SECTION("destination metadata differs")
    {
        fault = FaultConditionalCopyIO::Fault::DestinationMetadata;
    }
    SECTION("metadata and parent durability both fail")
    {
        fault = FaultConditionalCopyIO::Fault::DestinationMetadataAndParentFSync;
        expected = PublishedMetadataFailure(ESTALE, ProviderConditionalCopyFilesystemSyncStatus::Failed, ENOSPC);
    }

    auto io = std::make_shared<FaultConditionalCopyIO>(fault);
    auto host = std::make_shared<NativeHost>(*TestEnv().native_fs_man, *TestEnv().fsevents_file_update, io);
    auto transaction = host->BeginConditionalCopyTransaction(
        Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);

    CHECK((*transaction)->Commit() == expected);
    CHECK(std::filesystem::exists(paths.destination));
    CHECK(io->destination_fsync_calls == 1);
    CHECK(io->parent_fsync_calls == 1);
    CHECK(io->full_fsync_calls == 1);
}

TEST_CASE(PREFIX "preserves published evidence for every durability barrier failure")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto fault = FaultConditionalCopyIO::Fault::DestinationFSync;
    int expected_error = EIO;
    SECTION("destination fsync")
    {
        fault = FaultConditionalCopyIO::Fault::DestinationFSync;
        expected_error = EIO;
    }
    SECTION("parent fsync")
    {
        fault = FaultConditionalCopyIO::Fault::ParentFSync;
        expected_error = ENOSPC;
    }
    SECTION("destination full fsync")
    {
        fault = FaultConditionalCopyIO::Fault::FullFSync;
        expected_error = EROFS;
    }

    auto io = std::make_shared<FaultConditionalCopyIO>(fault);
    auto host = std::make_shared<NativeHost>(*TestEnv().native_fs_man, *TestEnv().fsevents_file_update, io);
    auto transaction = host->BeginConditionalCopyTransaction(
        Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);

    CHECK((*transaction)->Commit() == PublishedSyncFailure(expected_error));
    CHECK(std::filesystem::exists(paths.destination));
    CHECK(io->destination_fsync_calls == 1);
    CHECK(io->parent_fsync_calls == 1);
    CHECK(io->full_fsync_calls == 1);
    REQUIRE(io->step_count >= 4);
    CHECK(io->steps[0] == FaultConditionalCopyIO::Step::Clone);
    CHECK(io->steps[1] == FaultConditionalCopyIO::Step::DestinationFSync);
    CHECK(io->steps[2] == FaultConditionalCopyIO::Step::ParentFSync);
    CHECK(io->steps[3] == FaultConditionalCopyIO::Step::FullFSync);
}

TEST_CASE(PREFIX "exposes power-loss checkpoints around publication and the final durability barrier")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto io = std::make_shared<FaultConditionalCopyIO>(FaultConditionalCopyIO::Fault::None);
    auto host = std::make_shared<NativeHost>(*TestEnv().native_fs_man, *TestEnv().fsevents_file_update, io);
    auto transaction = host->BeginConditionalCopyTransaction(
        Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);

    CHECK((*transaction)->Commit() == PublishedSuccess());
    REQUIRE(io->checkpoint_count == 2);
    CHECK(io->checkpoints[0] == native::ConditionalCopyCheckpoint::BeforePublish);
    CHECK(io->checkpoint_step_counts[0] == 0);
    CHECK(io->checkpoints[1] == native::ConditionalCopyCheckpoint::AfterPublishBeforeFullFSync);
    CHECK(io->checkpoint_step_counts[1] == 3);
    REQUIRE(io->step_count == 4);
    CHECK(io->steps[0] == FaultConditionalCopyIO::Step::Clone);
    CHECK(io->steps[3] == FaultConditionalCopyIO::Step::FullFSync);
}

TEST_CASE(PREFIX "retries an interrupted full filesystem sync")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto io = std::make_shared<FaultConditionalCopyIO>(FaultConditionalCopyIO::Fault::FullFSyncInterruptedOnce);
    auto host = std::make_shared<NativeHost>(*TestEnv().native_fs_man, *TestEnv().fsevents_file_update, io);
    auto transaction = host->BeginConditionalCopyTransaction(
        Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);

    CHECK((*transaction)->Commit() == PublishedSuccess());
    CHECK(io->full_fsync_calls == 2);
}

TEST_CASE(PREFIX "probes ambiguous clone errors before classifying publication")
{
    const ConditionalCopyPaths paths;
    Write(paths.source, "payload");
    RequireCloneCapable(paths.destination_parent);

    auto fault = FaultConditionalCopyIO::Fault::CloneErrorWithoutPublication;
    auto expected = ProviderConditionalCopyCommitResult{
        .publication = ProviderConditionalCopyPublicationState::NotPublished,
        .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
        .system_error = EIO,
    };
    bool destination_exists = false;
    SECTION("absence is proven")
    {
    }
    SECTION("publication cannot be disproved")
    {
        fault = FaultConditionalCopyIO::Fault::CloneErrorWithPublication;
        expected.publication = ProviderConditionalCopyPublicationState::Unknown;
        destination_exists = true;
    }
    SECTION("an absent name with a changed parent cannot disprove publication")
    {
        fault = FaultConditionalCopyIO::Fault::CloneErrorAfterPublishAndRemoval;
        expected.publication = ProviderConditionalCopyPublicationState::Unknown;
    }

    auto io = std::make_shared<FaultConditionalCopyIO>(fault);
    auto host = std::make_shared<NativeHost>(*TestEnv().native_fs_man, *TestEnv().fsevents_file_update, io);
    auto transaction = host->BeginConditionalCopyTransaction(
        Authority(Claims(host, paths.source, paths.destination_parent, paths.destination)));
    REQUIRE(transaction);

    CHECK((*transaction)->Commit() == expected);
    if( fault == FaultConditionalCopyIO::Fault::CloneErrorWithoutPublication ) {
        REQUIRE(io->checkpoint_count == 1);
        CHECK(io->checkpoints[0] == native::ConditionalCopyCheckpoint::BeforePublish);
    }
    CHECK(std::filesystem::exists(paths.destination) == destination_exists);
}

} // namespace NativeConditionalCopyTransactionTests

#undef PREFIX
