// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"
#include "../source/Native/ConditionalCopy.h"
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

static ProviderConditionalCopyExistingExpectation Expectation(const std::string &_path,
                                                              ProviderConditionalCopyExpectedKind _kind)
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
    };
}

static ProviderConditionalCopyReviewedClaims Claims(const std::shared_ptr<NativeHost> &_host,
                                                    const std::string &_source,
                                                    const std::string &_destination_parent,
                                                    const std::string &_destination)
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
        .destination_parent = Expectation(_destination_parent, ProviderConditionalCopyExpectedKind::Directory),
        .destination = ProviderConditionalCopyMissingExpectation{.absolute_path = _destination},
    };
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
    constexpr char xattr_name[] = "com.magnumbytes.NimbleCommander.conditional-copy-test";
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
    auto expected = ProviderConditionalCopyPathSupport::Supported;

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
    CHECK(std::filesystem::exists(paths.destination) == destination_exists);
}

} // namespace NativeConditionalCopyTransactionTests

#undef PREFIX
