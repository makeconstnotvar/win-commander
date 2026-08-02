// Copyright (C) 2013-2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Host.h"
#include <sys/attr.h>
#include <sys/clonefile.h>
#include <sys/errno.h>
#include <sys/vnode.h>
#include <sys/stat.h>
#include <Base/algo.h>
#include <Utility/PathManip.h>
#include <Utility/FSEventsDirUpdate.h>
#include <Utility/FSEventsFileUpdate.h>
#include <Utility/NativeFSManager.h>
#include <RoutedIO/RoutedIO.h>
#include "DisplayNamesCache.h"
#include "File.h"
#include "ConditionalCopy.h"
#include <VFS/Log.h>
#include "../ListingInput.h"
#include "Fetching.h"
#include "OpenDirectory.h"
#include <Base/DispatchGroup.h>
#include <Base/StackAllocator.h>
#include <Utility/ObjCpp.h>
#include <Utility/Tags.h>
#include <sys/mount.h>
#include <pstld/pstld.h>
#include <fmt/ranges.h>
#include <algorithm>
#include <mutex>

namespace nc::vfs {

namespace {

template <class Callable>
int NativeConditionalCopyRetryInterrupted(Callable &&_call) noexcept
{
    int result = 0;
    do {
        errno = 0;
        result = _call();
    } while( result != 0 && errno == EINTR );
    return result;
}

int NativeConditionalCopyErrno() noexcept
{
    return errno == 0 ? EIO : errno;
}

ProviderConditionalCopyCommitResult NativeConditionalCopyNotPublished(
    ProviderConditionalCopyCommitFailure _failure,
    int _system_error = 0) noexcept
{
    if( _failure == ProviderConditionalCopyCommitFailure::ProviderFailure && _system_error == 0 )
        _system_error = EIO;
    return ProviderConditionalCopyCommitResult{
        .publication = ProviderConditionalCopyPublicationState::NotPublished,
        .failure = _failure,
        .system_error = _system_error,
    };
}

ProviderConditionalCopyCommitResult NativeConditionalCopyUnknown(int _system_error) noexcept
{
    return ProviderConditionalCopyCommitResult{
        .publication = ProviderConditionalCopyPublicationState::Unknown,
        .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
        .system_error = _system_error == 0 ? EIO : _system_error,
    };
}

ProviderConditionalCopyCommitResult NativeConditionalCopyPublished(
    int _metadata_error,
    int _filesystem_sync_error) noexcept
{
    const auto sync_status = _filesystem_sync_error == 0
                                 ? ProviderConditionalCopyFilesystemSyncStatus::Confirmed
                                 : ProviderConditionalCopyFilesystemSyncStatus::Failed;
    if( _metadata_error != 0 ) {
        return ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::MetadataFailed,
            .system_error = _metadata_error,
            .filesystem_sync_status = sync_status,
            .filesystem_sync_system_error = _filesystem_sync_error,
        };
    }
    if( _filesystem_sync_error != 0 ) {
        return ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::FileSystemSyncFailed,
            .system_error = _filesystem_sync_error,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Failed,
            .filesystem_sync_system_error = _filesystem_sync_error,
        };
    }
    return ProviderConditionalCopyCommitResult{
        .publication = ProviderConditionalCopyPublicationState::Published,
        .failure = ProviderConditionalCopyCommitFailure::None,
        .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
    };
}

class NativeConditionalCopyState final
{
public:
    NativeConditionalCopyState(int _source_fd,
                               int _destination_parent_fd,
                               ProviderConditionalCopyExistingExpectation _source,
                               ProviderConditionalCopyExistingExpectation _destination_parent,
                               std::string _destination_name,
                               native::ConditionalCopyMetadataSnapshot _source_metadata,
                               native::ConditionalCopyMetadataSnapshot _destination_parent_metadata,
                               nc::utility::NativeFSManager &_native_fs_manager,
                               std::shared_ptr<native::ConditionalCopyIO> _io) noexcept
        : m_SourceFD{_source_fd},
          m_DestinationParentFD{_destination_parent_fd},
          m_Source{std::move(_source)},
          m_DestinationParent{std::move(_destination_parent)},
          m_DestinationName{std::move(_destination_name)},
          m_SourceMetadata{std::move(_source_metadata)},
          m_DestinationParentMetadata{std::move(_destination_parent_metadata)},
          m_NativeFSManager{_native_fs_manager},
          m_IO{std::move(_io)}
    {
    }

    NativeConditionalCopyState(const NativeConditionalCopyState &) = delete;
    NativeConditionalCopyState &operator=(const NativeConditionalCopyState &) = delete;

    ~NativeConditionalCopyState() { Close(); }

    [[nodiscard]] ProviderConditionalCopyCommitResult Commit() noexcept;

    [[nodiscard]] ProviderConditionalCopyPublicationState Abort() noexcept
    {
        Close();
        return ProviderConditionalCopyPublicationState::NotPublished;
    }

private:
    void Close() noexcept
    {
        const auto lock = std::lock_guard{m_Mutex};
        CloseUnlocked();
    }

    void CloseUnlocked() noexcept
    {
        if( m_SourceFD >= 0 ) {
            m_IO->Close(m_SourceFD);
            m_SourceFD = -1;
        }
        if( m_DestinationParentFD >= 0 ) {
            m_IO->Close(m_DestinationParentFD);
            m_DestinationParentFD = -1;
        }
    }

    int m_SourceFD;
    int m_DestinationParentFD;
    ProviderConditionalCopyExistingExpectation m_Source;
    ProviderConditionalCopyExistingExpectation m_DestinationParent;
    std::string m_DestinationName;
    native::ConditionalCopyMetadataSnapshot m_SourceMetadata;
    native::ConditionalCopyMetadataSnapshot m_DestinationParentMetadata;
    nc::utility::NativeFSManager &m_NativeFSManager;
    std::shared_ptr<native::ConditionalCopyIO> m_IO;
    std::mutex m_Mutex;
};

bool NativeConditionalCopyTimestampMatches(const timespec &_actual,
                                           const ProviderConditionalCopyTimestamp &_expected) noexcept
{
    return _actual.tv_sec == _expected.seconds && _actual.tv_nsec == _expected.nanoseconds;
}

bool NativeConditionalCopyStatMatches(const struct stat &_actual,
                                      const ProviderConditionalCopyExistingExpectation &_expected) noexcept
{
    const bool kind_matches =
        (_expected.kind == ProviderConditionalCopyExpectedKind::RegularFile && S_ISREG(_actual.st_mode)) ||
        (_expected.kind == ProviderConditionalCopyExpectedKind::Directory && S_ISDIR(_actual.st_mode));
    return kind_matches && static_cast<int32_t>(_actual.st_dev) == _expected.device &&
           static_cast<uint64_t>(_actual.st_ino) == _expected.inode &&
           NativeConditionalCopyTimestampMatches(_actual.st_birthtimespec, _expected.birth_time) &&
           static_cast<uint16_t>(_actual.st_mode) == _expected.mode &&
           static_cast<uint64_t>(_actual.st_size) == _expected.byte_size &&
           NativeConditionalCopyTimestampMatches(_actual.st_mtimespec, _expected.modification_time) &&
           NativeConditionalCopyTimestampMatches(_actual.st_ctimespec, _expected.status_change_time);
}

bool NativeConditionalCopyMetadataMatchesExpectation(
    const native::ConditionalCopyMetadataSnapshot &_actual,
    const ProviderConditionalCopyExistingExpectation &_expected) noexcept
{
    const bool kind_matches =
        (_expected.kind == ProviderConditionalCopyExpectedKind::RegularFile && S_ISREG(_actual.mode)) ||
        (_expected.kind == ProviderConditionalCopyExpectedKind::Directory && S_ISDIR(_actual.mode));
    return kind_matches && static_cast<int32_t>(_actual.device) == _expected.device &&
           _actual.inode == _expected.inode && _actual.birth_time.seconds == _expected.birth_time.seconds &&
           _actual.birth_time.nanoseconds == _expected.birth_time.nanoseconds &&
           static_cast<uint16_t>(_actual.mode) == _expected.mode && _actual.size == _expected.byte_size &&
           _actual.modification_time.seconds == _expected.modification_time.seconds &&
           _actual.modification_time.nanoseconds == _expected.modification_time.nanoseconds &&
           _actual.change_time.seconds == _expected.status_change_time.seconds &&
           _actual.change_time.nanoseconds == _expected.status_change_time.nanoseconds;
}

bool NativeConditionalCopyVolumesMatch(const nc::utility::NativeFileSystemInfo &_source,
                                       const nc::utility::NativeFileSystemInfo &_destination) noexcept
{
    return _source.mounted_at_path == _destination.mounted_at_path &&
           _source.basic.fs_id.val[0] == _destination.basic.fs_id.val[0] &&
           _source.basic.fs_id.val[1] == _destination.basic.fs_id.val[1];
}

ProviderConditionalCopyCommitResult NativeConditionalCopyState::Commit() noexcept
{
    const auto lock = std::lock_guard{m_Mutex};
    if( m_SourceFD < 0 || m_DestinationParentFD < 0 ) {
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, EBADF);
    }

    struct stat source_stat {};
    if( m_IO->FStat(m_SourceFD, &source_stat) != 0 ) {
        const int stat_error = NativeConditionalCopyErrno();
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, stat_error);
    }
    if( !NativeConditionalCopyStatMatches(source_stat, m_Source) ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::SourceStale,
                                                 ESTALE);
    }
    const auto source_metadata = m_IO->CaptureMetadata(m_SourceFD);
    if( !source_metadata ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure,
                                                 source_metadata.error());
    }
    if( *source_metadata != m_SourceMetadata ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::SourceStale,
                                                 ESTALE);
    }

    struct stat destination_stat {};
    if( m_IO->FStatAt(m_DestinationParentFD,
                      m_DestinationName.c_str(),
                      &destination_stat,
                      AT_SYMLINK_NOFOLLOW) == 0 ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::DestinationExists,
                                                 EEXIST);
    }
    if( errno != ENOENT ) {
        const int stat_error = NativeConditionalCopyErrno();
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, stat_error);
    }

    struct stat destination_parent_stat {};
    if( m_IO->FStat(m_DestinationParentFD, &destination_parent_stat) != 0 ) {
        const int stat_error = NativeConditionalCopyErrno();
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, stat_error);
    }
    if( !NativeConditionalCopyStatMatches(destination_parent_stat, m_DestinationParent) ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(
            ProviderConditionalCopyCommitFailure::DestinationParentStale, ESTALE);
    }
    const auto destination_parent_metadata = m_IO->CaptureMetadata(m_DestinationParentFD);
    if( !destination_parent_metadata ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure,
                                                 destination_parent_metadata.error());
    }
    if( *destination_parent_metadata != m_DestinationParentMetadata ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(
            ProviderConditionalCopyCommitFailure::DestinationParentStale, ESTALE);
    }

    const auto source_volume = m_NativeFSManager.VolumeFromFD(m_SourceFD);
    const auto destination_volume = m_NativeFSManager.VolumeFromFD(m_DestinationParentFD);
    if( !source_volume || !destination_volume || source_stat.st_dev != destination_parent_stat.st_dev ||
        !NativeConditionalCopyVolumesMatch(*source_volume, *destination_volume) ||
        !native::EvaluateConditionalCopyVolume(*source_volume).IsSupported() ||
        !native::EvaluateConditionalCopyVolume(*destination_volume).IsSupported() ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, ENOTSUP);
    }

    if( m_IO->Clone(m_SourceFD, m_DestinationParentFD, m_DestinationName.c_str(), CLONE_ACL) != 0 ) {
        const int clone_error = NativeConditionalCopyErrno();
        if( clone_error == EEXIST ) {
            CloseUnlocked();
            return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::DestinationExists,
                                                     EEXIST);
        }
        struct stat post_clone_stat {};
        const int probe_result = m_IO->FStatAt(m_DestinationParentFD,
                                               m_DestinationName.c_str(),
                                               &post_clone_stat,
                                               AT_SYMLINK_NOFOLLOW);
        const int probe_error = NativeConditionalCopyErrno();
        const auto post_clone_parent_metadata =
            probe_result != 0 && probe_error == ENOENT
                ? m_IO->CaptureMetadata(m_DestinationParentFD)
                : std::expected<native::ConditionalCopyMetadataSnapshot, int>{std::unexpected(EIO)};
        CloseUnlocked();
        // Absence alone is insufficient: a clone may have published and then been removed concurrently. The
        // anchored parent seal must also prove that the namespace did not change across the failed syscall.
        if( probe_result != 0 && probe_error == ENOENT && post_clone_parent_metadata &&
            *post_clone_parent_metadata == m_DestinationParentMetadata ) {
            return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure,
                                                     clone_error);
        }
        return NativeConditionalCopyUnknown(clone_error);
    }

    int metadata_error = 0;
    int filesystem_sync_error = 0;
    const auto remember_metadata_error = [&](int _error) noexcept {
        if( metadata_error == 0 )
            metadata_error = _error == 0 ? EIO : _error;
    };
    const auto remember_sync_error = [&](int _error) noexcept {
        if( filesystem_sync_error == 0 )
            filesystem_sync_error = _error == 0 ? EIO : _error;
    };

    const auto post_clone_source_metadata = m_IO->CaptureMetadata(m_SourceFD);
    if( !post_clone_source_metadata ) {
        remember_metadata_error(post_clone_source_metadata.error());
    } else if( *post_clone_source_metadata != m_SourceMetadata ) {
        // Publication is already known. A source mutation racing the clone invalidates the reviewed metadata proof,
        // so the caller must see a published metadata failure even when the durability barriers still succeed.
        remember_metadata_error(ESTALE);
    }

    const int destination_fd = m_IO->OpenAt(
        m_DestinationParentFD, m_DestinationName.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if( destination_fd < 0 ) {
        remember_metadata_error(NativeConditionalCopyErrno());
        remember_sync_error(EBADF);
    } else {
        const auto destination_metadata = m_IO->CaptureMetadata(destination_fd);
        if( !destination_metadata ) {
            remember_metadata_error(destination_metadata.error());
        } else {
            struct stat named_destination {};
            if( m_IO->FStatAt(m_DestinationParentFD,
                              m_DestinationName.c_str(),
                              &named_destination,
                              AT_SYMLINK_NOFOLLOW) != 0 ) {
                remember_metadata_error(NativeConditionalCopyErrno());
            } else if( static_cast<uint64_t>(named_destination.st_dev) != destination_metadata->device ||
                       static_cast<uint64_t>(named_destination.st_ino) != destination_metadata->inode ||
                       !native::ConditionalCopyMetadataMatchesClone(m_SourceMetadata, *destination_metadata) ) {
                remember_metadata_error(ESTALE);
            }
        }

        if( NativeConditionalCopyRetryInterrupted([&] { return m_IO->FSync(destination_fd); }) != 0 )
            remember_sync_error(NativeConditionalCopyErrno());
    }

    if( NativeConditionalCopyRetryInterrupted([&] { return m_IO->FSync(m_DestinationParentFD); }) != 0 )
        remember_sync_error(NativeConditionalCopyErrno());

    if( destination_fd >= 0 ) {
        if( NativeConditionalCopyRetryInterrupted([&] { return m_IO->FullFSync(destination_fd); }) != 0 )
            remember_sync_error(NativeConditionalCopyErrno());

        struct stat final_named_destination {};
        struct stat final_open_destination {};
        if( m_IO->FStat(destination_fd, &final_open_destination) != 0 ) {
            remember_metadata_error(NativeConditionalCopyErrno());
        } else if( m_IO->FStatAt(m_DestinationParentFD,
                                 m_DestinationName.c_str(),
                                 &final_named_destination,
                                 AT_SYMLINK_NOFOLLOW) != 0 ) {
            remember_metadata_error(NativeConditionalCopyErrno());
        } else if( final_named_destination.st_dev != final_open_destination.st_dev ||
                   final_named_destination.st_ino != final_open_destination.st_ino ) {
            remember_metadata_error(ESTALE);
        }
        m_IO->Close(destination_fd);
    }

    CloseUnlocked();
    return NativeConditionalCopyPublished(metadata_error, filesystem_sync_error);
}

std::optional<std::string> NativeConditionalCopyDestinationName(
    const ProviderConditionalCopyReviewedClaims &_claims) noexcept
{
    const auto &parent = _claims.destination_parent.absolute_path;
    const auto &destination = _claims.destination.absolute_path;
    if( parent.empty() || destination.empty() || parent.front() != '/' || destination.front() != '/' )
        return std::nullopt;
    const size_t name_offset = parent == "/" ? 1 : parent.size() + 1;
    if( destination.size() <= name_offset || destination.substr(0, name_offset) !=
                                                 (parent == "/" ? "/" : parent + "/") )
        return std::nullopt;
    const auto name = destination.substr(name_offset);
    if( name.empty() || name.find('/') != std::string::npos || name == "." || name == ".." ||
        name.find('\0') != std::string::npos )
        return std::nullopt;
    return name;
}

bool NativeConditionalCopyCancelled(const VFSCancelChecker &_cancel_checker) noexcept
{
    try {
        return _cancel_checker && _cancel_checker();
    } catch( ... ) {
        return true;
    }
}

} // namespace

const char *NativeHost::UniqueTag = "native";

class VFSNativeHostConfiguration
{
public:
    [[nodiscard]] static const char *Tag() { return VFSNativeHost::UniqueTag; }

    [[nodiscard]] static const char *Junction() { return ""; }

    bool operator==(const VFSNativeHostConfiguration & /*unused*/) const { return true; }
};

VFSMeta NativeHost::Meta()
{
    VFSMeta m;
    m.Tag = UniqueTag;
    m.SpawnWithConfig = []([[maybe_unused]] const VFSHostPtr &_parent,
                           [[maybe_unused]] const VFSConfiguration &_config,
                           [[maybe_unused]] VFSCancelChecker _cancel_checker) {
        assert(0); // unimplementable without external knoweledge
        return nullptr;
    };
    return m;
}

NativeHost::NativeHost(nc::utility::NativeFSManager &_native_fs_man,
                       nc::utility::FSEventsFileUpdate &_fsevents_file_update)
    : NativeHost(_native_fs_man, _fsevents_file_update, std::make_shared<native::ConditionalCopyIO>())
{
}

NativeHost::NativeHost(nc::utility::NativeFSManager &_native_fs_man,
                       nc::utility::FSEventsFileUpdate &_fsevents_file_update,
                       std::shared_ptr<native::ConditionalCopyIO> _conditional_copy_io)
    : Host("", nullptr, UniqueTag),
      m_NativeFSManager(_native_fs_man),
      m_FSEventsFileUpdate(_fsevents_file_update),
      m_ConditionalCopyIO(_conditional_copy_io ? std::move(_conditional_copy_io)
                                               : std::make_shared<native::ConditionalCopyIO>())
{
    AddFeatures(HostFeatures::FetchUsers | HostFeatures::FetchGroups | HostFeatures::SetOwnership |
                HostFeatures::SetFlags | HostFeatures::SetPermissions | HostFeatures::SetTimes | HostFeatures::Read |
                HostFeatures::CreateFile | HostFeatures::CreateDirectory | HostFeatures::Rename |
                HostFeatures::Unlink | HostFeatures::RemoveDirectory | HostFeatures::Trash |
                HostFeatures::ReadSymlink | HostFeatures::ObserveDirectoryChanges |
                HostFeatures::CreateSymlink);
}

bool NativeHost::ShouldProduceThumbnails() const
{
    return true;
}

std::expected<VFSListingPtr, Error> NativeHost::FetchDirectoryListing(std::string_view _path,
                                                                      const unsigned long _flags,
                                                                      const VFSCancelChecker &_cancel_checker)
{
    Log::Trace("NativeHost::FetchDirectoryListing() called with path='{}',_flags: {}", _path, _flags);

    using namespace native;
    if( !_path.starts_with("/") )
        return std::unexpected(nc::Error{nc::Error::POSIX, EINVAL});

    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);

    const auto need_to_add_dot_dot = !(_flags & VFSFlags::F_NoDotDot) && _path != "/";
    auto &io = routedio::RoutedIO::InterfaceForAccess(path.c_str(), R_OK);
    const bool is_native_io = !io.isrouted();
    const int fd = io.open(path.c_str(), O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC);
    if( fd < 0 )
        return std::unexpected(Error{Error::POSIX, errno});
    auto close_fd = at_scope_end([fd] { close(fd); });

    using nc::base::variable_container;
    ListingInput listing_source;
    listing_source.hosts[0] = shared_from_this();
    listing_source.directories[0] = EnsureTrailingSlash(std::string(_path));
    listing_source.inodes.reset(variable_container<>::type::dense);
    listing_source.atimes.reset(variable_container<>::type::dense);
    listing_source.mtimes.reset(variable_container<>::type::dense);
    listing_source.ctimes.reset(variable_container<>::type::dense);
    listing_source.btimes.reset(variable_container<>::type::dense);
    listing_source.add_times.reset(variable_container<>::type::sparse);
    listing_source.unix_flags.reset(variable_container<>::type::dense);
    listing_source.uids.reset(variable_container<>::type::dense);
    listing_source.gids.reset(variable_container<>::type::dense);
    listing_source.sizes.reset(variable_container<>::type::dense);
    listing_source.symlinks.reset(variable_container<>::type::sparse);
    listing_source.display_filenames.reset(variable_container<>::type::sparse);

    std::vector<uint64_t> ext_flags; // store EF_xxx here
    constexpr size_t initial_prealloc_size = 64;
    size_t allocated_size = 0;
    auto resize_dense = [&](size_t _sz) {
        listing_source.filenames.resize(_sz);
        listing_source.inodes.resize(_sz);
        listing_source.unix_types.resize(_sz);
        listing_source.atimes.resize(_sz);
        listing_source.mtimes.resize(_sz);
        listing_source.ctimes.resize(_sz);
        listing_source.btimes.resize(_sz);
        listing_source.unix_modes.resize(_sz);
        listing_source.unix_flags.resize(_sz);
        listing_source.uids.resize(_sz);
        listing_source.gids.resize(_sz);
        listing_source.sizes.resize(_sz);
        ext_flags.resize(_sz);
        allocated_size = _sz;
    };

    // allocate space for up to 64 items upfront
    resize_dense(initial_prealloc_size);

    auto fill = [&](size_t _n, const Fetching::CallbackParams &_params) {
        assert(_n < listing_source.filenames.size());
        listing_source.filenames[_n] = _params.filename;
        listing_source.inodes[_n] = _params.inode;
        listing_source.unix_types[_n] = IFTODT(_params.mode);
        listing_source.atimes[_n] = _params.acc_time;
        listing_source.mtimes[_n] = _params.mod_time;
        listing_source.ctimes[_n] = _params.chg_time;
        listing_source.btimes[_n] = _params.crt_time;
        listing_source.unix_modes[_n] = _params.mode;
        listing_source.unix_flags[_n] = _params.flags;
        listing_source.uids[_n] = _params.uid;
        listing_source.gids[_n] = _params.gid;
        listing_source.sizes[_n] = _params.size;
        if( _params.add_time >= 0 )
            listing_source.add_times.insert(_n, _params.add_time);

        if( _flags & VFSFlags::F_LoadDisplayNames )
            if( S_ISDIR(listing_source.unix_modes[_n]) && !listing_source.filenames[_n].empty() &&
                listing_source.filenames[_n] != ".." ) {
                static auto &dnc = DisplayNamesCache::Instance();
                if( auto display_name = dnc.DisplayName(
                        _params.inode, _params.dev, listing_source.directories[0] + listing_source.filenames[_n]) )
                    listing_source.display_filenames.insert(_n, std::string(*display_name));
            }

        ext_flags[_n] = _params.ext_flags;
    };

    size_t next_entry_index = 0;
    auto cb_param = [&](const Fetching::CallbackParams &_params) { fill(next_entry_index++, _params); };

    if( need_to_add_dot_dot ) {
        Fetching::ReadSingleEntryAttributesByPath(io, path, cb_param);
        listing_source.filenames[0] = "..";
    }

    auto cb_fetch = [&](size_t _fetched_now) {
        // check if final entries count is more than previous approximate
        if( next_entry_index + _fetched_now > allocated_size )
            resize_dense(next_entry_index + _fetched_now);
    };

    // when Admin Mode is on - we use different fetch route
    const int ret =
        is_native_io ? Fetching::ReadDirAttributesBulk(fd, cb_fetch, cb_param)
                     : Fetching::ReadDirAttributesStat(fd, listing_source.directories[0].c_str(), cb_fetch, cb_param);
    if( ret != 0 )
        return std::unexpected(Error{Error::POSIX, ret});

    if( _cancel_checker && _cancel_checker() )
        return std::unexpected(Error{Error::POSIX, ECANCELED});

    // check if final entries count is less than approximate
    if( next_entry_index < allocated_size )
        resize_dense(next_entry_index);

    // Now the main fetching is done there's a second pass to gather optional attributes per item:
    // - symlinks
    // - tags
    // Run the pass in parallel to reduce the latency of the critical path.
    const bool tags_reading_enabled = TagsFetchingAllowed(_flags, _path);
    std::mutex listing_source_tags_mut;     // guard access to 'listing_source.tags'
    std::mutex listing_source_symlinks_mut; // guard access to 'listing_source.symlinks'
    auto epilogue_pass = [fd,
                          tags_reading_enabled,
                          is_native_io,
                          &io,
                          &listing_source,
                          &listing_source_tags_mut,
                          &listing_source_symlinks_mut,
                          &ext_flags](const std::string &filename) {
        // index of the item in the listing
        const size_t n = &filename - listing_source.filenames.data();

        // If this entry is symbolic link - read the target path, stat it and the target into in the listing.
        if( listing_source.unix_types[n] == DT_LNK ) {
            // read an actual link path
            char linkpath[MAXPATHLEN];
            const ssize_t sz = is_native_io
                                   ? readlinkat(fd, listing_source.filenames[n].c_str(), linkpath, MAXPATHLEN)
                                   : io.readlink((listing_source.directories[0] + listing_source.filenames[n]).c_str(),
                                                 linkpath,
                                                 MAXPATHLEN);
            if( sz >= 0 ) {
                linkpath[sz] = 0;
                {
                    const std::lock_guard<std::mutex> lock(listing_source_symlinks_mut);
                    listing_source.symlinks.insert(n, linkpath);
                }

                // stat the target file
                struct ::stat stat_buffer;
                const int stat_ret =
                    is_native_io
                        ? fstatat(fd, listing_source.filenames[n].c_str(), &stat_buffer, 0)
                        : io.stat((listing_source.directories[0] + listing_source.filenames[n]).c_str(), &stat_buffer);
                if( stat_ret == 0 ) {
                    listing_source.unix_modes[n] = stat_buffer.st_mode;
                    listing_source.unix_flags[n] = MergeUnixFlags(listing_source.unix_flags[n], stat_buffer.st_flags);
                    listing_source.uids[n] = stat_buffer.st_uid;
                    listing_source.gids[n] = stat_buffer.st_gid;
                    listing_source.sizes[n] = S_ISDIR(stat_buffer.st_mode) ? -1 : stat_buffer.st_size;
                }
            }
        }

        // Fetch FinderTags if they were requested AND
        // if an entry doesn't have an EF_NO_XATTRS flag (to do less unnecessary syscalls).
        // Tags are stored in xattrs and if we know in advance that there are no xattrs in this entry - there's no
        // point trying. Unfortunately, some filesystems (like SMB) don't report EF_NO_XATTRS, so this algorithm
        // has to check every single item...
        if( tags_reading_enabled && !(ext_flags[n] & EF_NO_XATTRS) ) {
            // TODO: is it worth routing the I/O here? guess not atm
            const int entry_fd = openat(fd, filename.c_str(), O_RDONLY | O_NONBLOCK);
            if( entry_fd >= 0 ) {
                auto close_entry_fd = at_scope_end([entry_fd] { close(entry_fd); });

                if( auto tags = utility::Tags::ReadTags(entry_fd); !tags.empty() ) {
                    Log::Debug("Extracted the tags of the file '{}': {}", filename, fmt::join(tags, ", "));

                    const std::lock_guard<std::mutex> lock(listing_source_tags_mut);
                    listing_source.tags.emplace(n, std::move(tags));
                }
            }
        }
    };
    pstld::for_each_n(listing_source.filenames.begin(), next_entry_index, [epilogue_pass](const std::string &filename) {
        try {
            epilogue_pass(filename);
        } catch( ... ) {
            // PSTL gets very upset when the functor throws an exception, so swallow it silently instead of terminating.
        }
    });

    // And, finally, compose the listing source into a compact immutable listing object.
    return VFSListing::Build(std::move(listing_source));
}

std::expected<VFSListingPtr, Error> NativeHost::FetchSingleItemListing(std::string_view _path,
                                                                       unsigned long _flags,
                                                                       const VFSCancelChecker &_cancel_checker)
{
    Log::Trace("NativeHost::FetchSingleItemListing() called with path='{}',_flags: {}", _path, _flags);

    using namespace native;
    if( !_path.starts_with("/") )
        return std::unexpected(nc::Error{nc::Error::POSIX, EINVAL});

    std::array<char, 512> mem_buffer;
    std::pmr::monotonic_buffer_resource mem_resource(mem_buffer.data(), mem_buffer.size());
    const std::pmr::string path(utility::PathManip::WithoutTrailingSlashes(_path), &mem_resource);
    if( path.empty() )
        return std::unexpected(nc::Error{nc::Error::POSIX, EINVAL});

    const std::string_view directory = utility::PathManip::Parent(path);
    if( directory.empty() )
        return std::unexpected(nc::Error{nc::Error::POSIX, EINVAL});

    const std::string_view filename = utility::PathManip::Filename(path);
    if( filename.empty() )
        return std::unexpected(nc::Error{nc::Error::POSIX, EINVAL});

    if( _cancel_checker && _cancel_checker() )
        return std::unexpected(nc::Error{nc::Error::POSIX, ECANCELED});

    auto &io = routedio::RoutedIO::InterfaceForAccess(path.c_str(), R_OK);

    using nc::base::variable_container;
    uint64_t ext_flags = 0;
    ListingInput listing_source;
    listing_source.hosts[0] = shared_from_this();
    listing_source.directories[0] = directory;
    listing_source.inodes.reset(variable_container<>::type::common);
    listing_source.atimes.reset(variable_container<>::type::common);
    listing_source.mtimes.reset(variable_container<>::type::common);
    listing_source.ctimes.reset(variable_container<>::type::common);
    listing_source.btimes.reset(variable_container<>::type::common);
    listing_source.add_times.reset(variable_container<>::type::sparse);
    listing_source.unix_flags.reset(variable_container<>::type::common);
    listing_source.uids.reset(variable_container<>::type::common);
    listing_source.gids.reset(variable_container<>::type::common);
    listing_source.sizes.reset(variable_container<>::type::common);
    listing_source.symlinks.reset(variable_container<>::type::sparse);
    listing_source.display_filenames.reset(variable_container<>::type::sparse);

    listing_source.unix_modes.resize(1);
    listing_source.unix_types.resize(1);
    listing_source.filenames.emplace_back(filename);

    auto cb_param = [&](const Fetching::CallbackParams &_params) {
        listing_source.inodes[0] = _params.inode;
        listing_source.unix_types[0] = IFTODT(_params.mode);
        listing_source.atimes[0] = _params.acc_time;
        listing_source.mtimes[0] = _params.mod_time;
        listing_source.ctimes[0] = _params.chg_time;
        listing_source.btimes[0] = _params.crt_time;
        listing_source.unix_modes[0] = _params.mode;
        listing_source.unix_flags[0] = _params.flags;
        listing_source.uids[0] = _params.uid;
        listing_source.gids[0] = _params.gid;
        listing_source.sizes[0] = _params.size;
        if( _params.add_time >= 0 )
            listing_source.add_times.insert(0, _params.add_time);

        if( _flags & VFSFlags::F_LoadDisplayNames )
            if( S_ISDIR(listing_source.unix_modes[0]) && !listing_source.filenames[0].empty() &&
                listing_source.filenames[0] != ".." ) {
                static auto &dnc = DisplayNamesCache::Instance();
                if( std::optional<std::string_view> display_name = dnc.DisplayName(_params.inode, _params.dev, path) )
                    listing_source.display_filenames.insert(0, std::string(*display_name));
            }

        ext_flags = _params.ext_flags;
    };

    const int ret = Fetching::ReadSingleEntryAttributesByPath(io, _path, cb_param);
    if( ret != 0 )
        return std::unexpected(Error{Error::POSIX, ret});

    // a little more work with symlink, if any
    if( listing_source.unix_types[0] == DT_LNK ) {
        // read an actual link path
        char linkpath[MAXPATHLEN];
        const ssize_t sz = io.readlink(path.c_str(), linkpath, MAXPATHLEN);
        if( sz != -1 ) {
            linkpath[sz] = 0;
            listing_source.symlinks.insert(0, linkpath);
        }

        // stat the target file
        struct stat stat_buffer;
        const auto stat_ret = io.stat(path.c_str(), &stat_buffer);
        if( stat_ret == 0 ) {
            listing_source.unix_modes[0] = stat_buffer.st_mode;
            listing_source.unix_flags[0] = MergeUnixFlags(listing_source.unix_flags[0], stat_buffer.st_flags);
            listing_source.uids[0] = stat_buffer.st_uid;
            listing_source.gids[0] = stat_buffer.st_gid;
            listing_source.sizes[0] = stat_buffer.st_size;
        }
    }

    // Fetch FinderTags if they were requested AND if an entry doesn't have an EF_NO_XATTRS flag (to do less unnecessary
    // syscalls).
    const bool tags_reading_enabled = TagsFetchingAllowed(_flags, _path);
    if( tags_reading_enabled && !(ext_flags & EF_NO_XATTRS) ) {
        // TODO: is it worth routing the I/O here? guess not atm
        const int entry_fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if( entry_fd >= 0 ) {
            auto close_entry_fd = at_scope_end([entry_fd] { close(entry_fd); });
            if( auto tags = utility::Tags::ReadTags(entry_fd); !tags.empty() )
                listing_source.tags.emplace(0, std::move(tags));
        }
    }

    return VFSListing::Build(std::move(listing_source));
}

std::expected<std::shared_ptr<VFSFile>, Error> NativeHost::CreateFile(std::string_view _path,
                                                                      const VFSCancelChecker &_cancel_checker)
{
    auto file = std::make_shared<native::File>(_path, SharedPtr());
    if( _cancel_checker && _cancel_checker() )
        return std::unexpected(Error{Error::POSIX, ECANCELED});
    return file;
}

static std::expected<void, Error> CalculateDirectoriesSizesHelper(char *_path,
                                                                  size_t _path_len,
                                                                  std::atomic_bool &_iscancelling,
                                                                  const VFSCancelChecker &_checker,
                                                                  dispatch_queue &_stat_queue,
                                                                  std::atomic_uint64_t &_size_stock)
{
    if( _checker && _checker() ) {
        _iscancelling = true;
        return std::unexpected(nc::Error{nc::Error::POSIX, ECANCELED});
    }

    auto &io = routedio::RoutedIO::InterfaceForAccess(_path, R_OK); // <-- sync IO operation

    const auto dirp = io.opendir(_path); // <-- sync IO operation
    if( dirp == nullptr )
        return std::unexpected(nc::Error{nc::Error::POSIX, errno});

    _path[_path_len] = '/';
    _path[_path_len + 1] = 0;
    char *var = _path + _path_len + 1;

    dirent *entp = nullptr;
    while( (entp = io.readdir(dirp)) != nullptr ) { // <-- sync IO operation
        if( _checker && _checker() ) {
            _iscancelling = true;
            goto cleanup;
        }

        if( entp->d_ino == 0 )
            continue; // apple's documentation suggest to skip such files
        if( entp->d_namlen == 1 && entp->d_name[0] == '.' )
            continue; // do not process self entry
        if( entp->d_namlen == 2 && entp->d_name[0] == '.' && entp->d_name[1] == '.' )
            continue; // do not process parent entry

        memcpy(var, entp->d_name, entp->d_namlen + 1);
        if( entp->d_type == DT_DIR ) {
            std::ignore = CalculateDirectoriesSizesHelper(
                _path, _path_len + entp->d_namlen + 1, _iscancelling, _checker, _stat_queue, _size_stock);
            if( _iscancelling )
                goto cleanup;
        }
        else if( entp->d_type == DT_REG || entp->d_type == DT_LNK ) {
            std::string full_path = _path;
            _stat_queue.async([&, full_path = std::move(full_path)] {
                if( _iscancelling )
                    return;

                struct stat st;
                if( io.lstat(full_path.c_str(), &st) == 0 ) // <-- sync IO operation
                    _size_stock += st.st_size;
            });
        }
        else if( entp->d_type == DT_UNKNOWN ) {
            // some filesystems (e.g. ftp) might provide DT_UNKNOWN via readdir, so
            // need to check them via lstat() before doing further processing
            struct stat st;
            if( io.lstat(_path, &st) == 0 ) { // <-- sync IO operation
                if( S_ISDIR(st.st_mode) ) {
                    std::ignore = CalculateDirectoriesSizesHelper(
                        _path, _path_len + entp->d_namlen + 1, _iscancelling, _checker, _stat_queue, _size_stock);
                    if( _iscancelling )
                        goto cleanup;
                }
                else if( S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) ) {
                    _size_stock += st.st_size;
                }
            }
        }
    }

cleanup:
    io.closedir(dirp); // <-- sync IO operation
    _path[_path_len] = 0;
    return {};
}

std::expected<uint64_t, Error> NativeHost::CalculateDirectorySize(std::string_view _path,
                                                                  const VFSCancelChecker &_cancel_checker)
{
    if( _cancel_checker && _cancel_checker() )
        return std::unexpected(nc::Error{nc::Error::POSIX, ECANCELED});

    if( !_path.starts_with("/") )
        return std::unexpected(nc::Error{nc::Error::POSIX, EINVAL});

    std::atomic_bool iscancelling{false};

    // TODO: rewrite without using C-style shenanigans
    char path[MAXPATHLEN];
    memcpy(path, _path.data(), _path.length());
    path[_path.length()] = 0;

    dispatch_queue stat_queue("VFSNativeHost.CalculateDirectoriesSizes");

    std::atomic_uint64_t size{0};
    const std::expected<void, Error> result =
        CalculateDirectoriesSizesHelper(path, _path.length(), iscancelling, _cancel_checker, stat_queue, size);
    stat_queue.sync([] {});
    if( !result )
        return std::unexpected(result.error());

    return size.load();
}

bool NativeHost::IsDirectoryChangeObservationAvailable(std::string_view _path)
{
    if( _path.empty() )
        return false;

    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);
    return access(path.c_str(), R_OK) == 0; // should use _not_ routed I/O here!
}

HostDirObservationTicket NativeHost::ObserveDirectoryChanges(std::string_view _path, std::function<void()> _handler)
{
    auto &inst = nc::utility::FSEventsDirUpdate::Instance();
    const uint64_t t = inst.AddWatchPath(_path, std::move(_handler));
    return t ? HostDirObservationTicket(t, shared_from_this()) : HostDirObservationTicket();
}

void NativeHost::StopDirChangeObserving(unsigned long _ticket)
{
    auto &inst = nc::utility::FSEventsDirUpdate::Instance();
    inst.RemoveWatchPathWithTicket(_ticket);
}

FileObservationToken NativeHost::ObserveFileChanges(const std::string_view _path, std::function<void()> _handler)
{
    const auto token = m_FSEventsFileUpdate.AddWatchPath(_path, std::move(_handler));
    return {token, SharedPtr()};
}

void NativeHost::StopObservingFileChanges(unsigned long _token)
{
    assert(_token != utility::FSEventsFileUpdate::empty_token);
    m_FSEventsFileUpdate.RemoveWatchPathWithToken(_token);
}

std::expected<VFSStat, Error>
NativeHost::Stat(std::string_view _path, unsigned long _flags, [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);

    auto &io = routedio::RoutedIO::InterfaceForAccess(path.c_str(), R_OK);

    struct stat st;
    const int ret = (_flags & VFSFlags::F_NoFollow) ? io.lstat(path.c_str(), &st) : io.stat(path.c_str(), &st);
    if( ret != 0 ) {
        return std::unexpected(Error{Error::POSIX, errno});
    }

    VFSStat vfs_stat;
    VFSStat::FromSysStat(st, vfs_stat);
    return vfs_stat;
}

std::expected<void, Error>
NativeHost::IterateDirectoryListing(std::string_view _path,
                                    const std::function<bool(const VFSDirEnt &_dirent)> &_handler)
{
    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);

    auto &io = routedio::RoutedIO::InterfaceForAccess(path.c_str(), R_OK);

    DIR *dirp = io.opendir(path.c_str());
    if( dirp == nullptr )
        return std::unexpected(Error{Error::POSIX, errno});
    const auto close_dirp = at_scope_end([&] { io.closedir(dirp); });

    while( true ) {
        errno = 0;
        dirent *const entp = io.readdir(dirp);
        if( entp == nullptr ) {
            if( errno != 0 )
                return std::unexpected(Error{Error::POSIX, errno});
            break;
        }
        if( (entp->d_namlen == 1 && entp->d_name[0] == '.') ||
            (entp->d_namlen == 2 && entp->d_name[0] == '.' && entp->d_name[1] == '.') )
            continue;

        VFSDirEnt vfs_dirent;
        vfs_dirent.type = static_cast<VFSDirEnt::Type>(entp->d_type);
        vfs_dirent.name = std::string_view{entp->d_name, entp->d_namlen};

        if( !_handler(vfs_dirent) )
            return {};
    }

    return {};
}

std::expected<VFSStatFS, Error> NativeHost::StatFS(std::string_view _path,
                                                   [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);

    struct statfs info;
    if( statfs(path.c_str(), &info) < 0 )
        return std::unexpected(Error{Error::POSIX, errno});

    auto volume = m_NativeFSManager.VolumeFromMountPoint(info.f_mntonname);
    if( !volume )
        return std::unexpected(Error{Error::POSIX, ENOENT});

    m_NativeFSManager.UpdateSpaceInformation(volume);

    VFSStatFS stat;
    stat.volume_name = volume->verbose.name;
    stat.total_bytes = volume->basic.total_bytes;
    stat.free_bytes = volume->basic.free_bytes;
    stat.avail_bytes = volume->basic.available_bytes;
    return stat;
}

std::expected<void, Error> NativeHost::Unlink(std::string_view _path,
                                              [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);
    auto &io = routedio::RoutedIO::Default;

    if( io.unlink(path.c_str()) != 0 )
        return std::unexpected(nc::Error{nc::Error::POSIX, errno});

    return {};
}

bool NativeHost::IsWritable() const
{
    return true; // dummy now
}

std::expected<void, Error>
NativeHost::CreateDirectory(std::string_view _path, int _mode, [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);
    auto &io = routedio::RoutedIO::Default;
    const int ret = io.mkdir(path.c_str(), mode_t(_mode));
    if( ret == 0 )
        return {};

    return std::unexpected(nc::Error{nc::Error::POSIX, errno});
}

std::expected<void, Error> NativeHost::RemoveDirectory(std::string_view _path,
                                                       [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);
    auto &io = routedio::RoutedIO::Default;
    const int ret = io.rmdir(path.c_str());
    if( ret == 0 )
        return {};

    return std::unexpected(nc::Error{nc::Error::POSIX, errno});
}

std::expected<std::string, Error> NativeHost::ReadSymlink(std::string_view _path,
                                                          [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);

    auto &io = routedio::RoutedIO::Default;
    char buffer[8192];
    const ssize_t sz = io.readlink(path.c_str(), buffer, sizeof(buffer));
    if( sz < 0 )
        return std::unexpected(nc::Error{nc::Error::POSIX, errno});

    if( sz >= static_cast<long>(sizeof(buffer)) )
        return std::unexpected(nc::Error{nc::Error::POSIX, ENOMEM});

    return std::string(buffer, sz);
}

std::expected<void, Error> NativeHost::CreateSymlink(std::string_view _symlink_path,
                                                     std::string_view _symlink_value,
                                                     [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    StackAllocator alloc;
    const std::pmr::string symlink_path(_symlink_path, &alloc);
    const std::pmr::string symlink_value(_symlink_value, &alloc);

    auto &io = routedio::RoutedIO::Default;
    const int result = io.symlink(symlink_value.c_str(), symlink_path.c_str());
    if( result != 0 )
        return std::unexpected(nc::Error{nc::Error::POSIX, errno});

    return {};
}

std::expected<void, Error> NativeHost::SetTimes(const std::string_view _path,
                                                const std::optional<time_t> _birth_time,
                                                const std::optional<time_t> _mod_time,
                                                const std::optional<time_t> _chg_time,
                                                const std::optional<time_t> _acc_time,
                                                [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    if( _path.empty() )
        return std::unexpected(nc::Error{nc::Error::POSIX, EINVAL});

    if( !_birth_time && !_mod_time && !_chg_time && !_acc_time )
        return {};

    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);

    auto &io = routedio::RoutedIO::Default;
    if( _birth_time && io.chbtime(path.c_str(), *_birth_time) != 0 )
        return std::unexpected(nc::Error{nc::Error::POSIX, errno});
    if( _mod_time && io.chmtime(path.c_str(), *_mod_time) != 0 )
        return std::unexpected(nc::Error{nc::Error::POSIX, errno});
    if( _chg_time && io.chctime(path.c_str(), *_chg_time) != 0 )
        return std::unexpected(nc::Error{nc::Error::POSIX, errno});
    if( _acc_time && io.chatime(path.c_str(), *_acc_time) != 0 )
        return std::unexpected(nc::Error{nc::Error::POSIX, errno});

    return {};
}

std::expected<void, Error> NativeHost::Rename(std::string_view _old_path,
                                              std::string_view _new_path,
                                              [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    StackAllocator alloc;
    const std::pmr::string old_path(_old_path, &alloc);
    const std::pmr::string new_path(_new_path, &alloc);

    auto &io = routedio::RoutedIO::Default;
    const int ret = io.rename(old_path.c_str(), new_path.c_str());
    if( ret == 0 )
        return {};

    return std::unexpected(nc::Error{nc::Error::POSIX, errno});
}

bool NativeHost::IsNativeFS() const noexcept
{
    return true;
}

VFSConfiguration NativeHost::Configuration() const
{
    static const auto aa = VFSNativeHostConfiguration();
    return aa;
}

std::expected<void, nc::Error> NativeHost::Trash(std::string_view _path,
                                                 [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    if( _path.empty() )
        return std::unexpected(nc::Error{nc::Error::POSIX, EINVAL});

    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);

    auto &io = routedio::RoutedIO::Default;
    const auto ret = io.trash(path.c_str());
    if( ret != 0 )
        return std::unexpected(nc::Error{nc::Error::POSIX, errno});

    return {};
}

std::expected<void, Error> NativeHost::SetPermissions(std::string_view _path,
                                                      uint16_t _mode,
                                                      [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    if( _path.empty() )
        return std::unexpected(nc::Error{nc::Error::POSIX, EINVAL});

    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);

    auto &io = routedio::RoutedIO::Default;
    const auto ret = io.chmod(path.c_str(), _mode);
    if( ret != 0 )
        return std::unexpected(nc::Error{nc::Error::POSIX, errno});

    return {};
}

std::expected<void, Error> NativeHost::SetFlags(std::string_view _path,
                                                uint32_t _flags,
                                                uint64_t _vfs_options,
                                                [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    if( _path.empty() )
        return std::unexpected(nc::Error{nc::Error::POSIX, EINVAL});

    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);

    auto &io = routedio::RoutedIO::Default;
    const bool no_follow = _vfs_options & Flags::F_NoFollow;
    const auto ret = no_follow ? io.lchflags(path.c_str(), _flags) : io.chflags(path.c_str(), _flags);
    if( ret != 0 )
        return std::unexpected(nc::Error{nc::Error::POSIX, errno});

    return {};
}

std::expected<void, Error> NativeHost::SetOwnership(std::string_view _path,
                                                    unsigned _uid,
                                                    unsigned _gid,
                                                    [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    if( _path.empty() )
        return std::unexpected(nc::Error{nc::Error::POSIX, EINVAL});

    StackAllocator alloc;
    const std::pmr::string path(_path, &alloc);

    auto &io = routedio::RoutedIO::Default;
    const auto ret = io.chown(path.c_str(), _uid, _gid);
    if( ret != 0 )
        return std::unexpected(nc::Error{nc::Error::POSIX, errno});

    return {};
}

std::expected<std::vector<VFSUser>, Error>
NativeHost::FetchUsers([[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    return native::FetchUsers();
}

std::expected<std::vector<VFSGroup>, Error>
NativeHost::FetchGroups([[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    return native::FetchGroups();
}

bool NativeHost::IsCaseSensitiveAtPath(std::string_view _dir) const
{
    return CaseSensitivityAtPath(_dir).value_or(true);
}

std::optional<bool> NativeHost::CaseSensitivityAtPath(std::string_view _dir) const
{
    if( _dir.empty() || _dir[0] != '/' )
        return std::nullopt;
    if( const auto fs_info = m_NativeFSManager.VolumeFromPath(_dir) )
        return fs_info->format.case_sensitive;
    return std::nullopt;
}

std::optional<std::string> NativeHost::SemanticNamespaceIdentity() const
{
    return std::string{UniqueTag};
}

std::expected<std::unique_ptr<ProviderConditionalCopyTransaction>,
              ProviderConditionalCopyTransactionBeginError>
NativeHost::BeginConditionalCopyTransaction(ProviderConditionalCopyReviewedAuthority _authority,
                                            const VFSCancelChecker &_cancel_checker)
{
    if( NativeConditionalCopyCancelled(_cancel_checker) )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::Cancelled);

    const auto &claims = _authority.Claims();
    const auto destination_name = NativeConditionalCopyDestinationName(claims);
    if( claims.destination_binding.host.get() != this || claims.source_binding.host.get() != this ||
        claims.source.kind != ProviderConditionalCopyExpectedKind::RegularFile ||
        claims.destination_parent.kind != ProviderConditionalCopyExpectedKind::Directory || !destination_name ||
        !ValidateFilename(*destination_name) ) {
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::InvalidRequest);
    }

    const int source_fd = m_ConditionalCopyIO->Open(
        claims.source.absolute_path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW_ANY | O_CLOEXEC);
    if( source_fd < 0 ) {
        const int open_error = errno;
        return std::unexpected(open_error == ENOENT || open_error == ENOTDIR || open_error == ELOOP
                                   ? ProviderConditionalCopyTransactionBeginError::SourceStale
                                   : ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    }
    auto close_source = at_scope_end([source_fd, io = m_ConditionalCopyIO] { io->Close(source_fd); });

    struct stat source_stat {};
    if( m_ConditionalCopyIO->FStat(source_fd, &source_stat) != 0 )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    if( !NativeConditionalCopyStatMatches(source_stat, claims.source) )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::SourceStale);

    const int destination_parent_fd = m_ConditionalCopyIO->Open(
        claims.destination_parent.absolute_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
    if( destination_parent_fd < 0 ) {
        const int open_error = errno;
        return std::unexpected(open_error == ENOENT || open_error == ENOTDIR || open_error == ELOOP
                                   ? ProviderConditionalCopyTransactionBeginError::DestinationParentStale
                                   : ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    }
    auto close_destination_parent =
        at_scope_end([destination_parent_fd, io = m_ConditionalCopyIO] { io->Close(destination_parent_fd); });

    struct stat destination_parent_stat {};
    if( m_ConditionalCopyIO->FStat(destination_parent_fd, &destination_parent_stat) != 0 )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    if( !NativeConditionalCopyStatMatches(destination_parent_stat, claims.destination_parent) )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::DestinationParentStale);

    struct stat destination_stat {};
    if( m_ConditionalCopyIO->FStatAt(
            destination_parent_fd, destination_name->c_str(), &destination_stat, AT_SYMLINK_NOFOLLOW) == 0 )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::DestinationExists);
    if( errno != ENOENT )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);

    const auto source_volume = m_NativeFSManager.VolumeFromFD(source_fd);
    const auto destination_volume = m_NativeFSManager.VolumeFromFD(destination_parent_fd);
    if( !source_volume || !destination_volume )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    if( source_stat.st_dev != destination_parent_stat.st_dev ||
        !NativeConditionalCopyVolumesMatch(*source_volume, *destination_volume) ||
        !native::EvaluateConditionalCopyVolume(*source_volume).IsSupported() ||
        !native::EvaluateConditionalCopyVolume(*destination_volume).IsSupported() ) {
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::Unsupported);
    }

    auto source_metadata = m_ConditionalCopyIO->CaptureMetadata(source_fd);
    if( !source_metadata )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    if( !NativeConditionalCopyMetadataMatchesExpectation(*source_metadata, claims.source) )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::SourceStale);

    auto destination_parent_metadata = m_ConditionalCopyIO->CaptureMetadata(destination_parent_fd);
    if( !destination_parent_metadata )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    if( !NativeConditionalCopyMetadataMatchesExpectation(*destination_parent_metadata, claims.destination_parent) )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::DestinationParentStale);
    if( !native::ValidateConditionalCopyMetadataPolicy(*source_metadata, *destination_parent_metadata) )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::Unsupported);

    if( NativeConditionalCopyCancelled(_cancel_checker) )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::Cancelled);

    std::shared_ptr<NativeConditionalCopyState> state;
    try {
        state = std::make_shared<NativeConditionalCopyState>(source_fd,
                                                             destination_parent_fd,
                                                             claims.source,
                                                             claims.destination_parent,
                                                             *destination_name,
                                                             std::move(*source_metadata),
                                                             std::move(*destination_parent_metadata),
                                                             m_NativeFSManager,
                                                             m_ConditionalCopyIO);
    } catch( ... ) {
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    }
    close_source.disengage();
    close_destination_parent.disengage();
    return MintConditionalCopyTransaction(
        std::move(_authority),
        [state] { return state->Commit(); },
        [state] { return state->Abort(); });
}

nc::utility::NativeFSManager &NativeHost::NativeFSManager() const noexcept
{
    return m_NativeFSManager;
}

uint32_t NativeHost::MergeUnixFlags(uint32_t _symlink_flags, uint32_t _target_flags) noexcept
{
    const uint32_t hidden_flag = _symlink_flags & UF_HIDDEN;
    return _target_flags | hidden_flag;
}

bool NativeHost::TagsFetchingAllowed(unsigned long _fetch_flags, std::string_view _path) const
{
    if( !(_fetch_flags & Flags::F_LoadTags) )
        return false; // was not originally requested - nothing to think about.

    // Disallow fetching tags on remote volumes - use a fast No-I/O approximation to check that.
    const std::shared_ptr<const utility::NativeFileSystemInfo> fast_fs_info =
        m_NativeFSManager.VolumeFromPathFast(_path);
    if( !fast_fs_info )
        return false; // Something is truly fishy going on with the path - let's be on the safe side.

    // If the FS is local => it is not network => allow it.
    return fast_fs_info->mount_flags.local;
}

std::shared_ptr<const NativeHost> NativeHost::SharedPtr() const
{
    return std::static_pointer_cast<const NativeHost>(Host::SharedPtr());
}

std::shared_ptr<NativeHost> NativeHost::SharedPtr()
{
    return std::static_pointer_cast<NativeHost>(Host::SharedPtr());
}

} // namespace nc::vfs
