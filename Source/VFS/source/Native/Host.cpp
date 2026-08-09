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
#include "CrossVolumeStagingAuthority.h"
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
#include <exception>
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

ProviderConditionalCopyCommitResult NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure _failure,
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

ProviderConditionalCopyCommitResult NativeConditionalCopyPublished(int _metadata_error,
                                                                   int _filesystem_sync_error) noexcept
{
    const auto sync_status = _filesystem_sync_error == 0 ? ProviderConditionalCopyFilesystemSyncStatus::Confirmed
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

bool NativeConditionalCopyCancelled(const VFSCancelChecker &_cancel_checker) noexcept;

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
        : m_SourceFD{_source_fd}, m_DestinationParentFD{_destination_parent_fd}, m_Source{std::move(_source)},
          m_DestinationParent{std::move(_destination_parent)}, m_DestinationName{std::move(_destination_name)},
          m_SourceMetadata{std::move(_source_metadata)},
          m_DestinationParentMetadata{std::move(_destination_parent_metadata)}, m_NativeFSManager{_native_fs_manager},
          m_IO{std::move(_io)}
    {
    }

    NativeConditionalCopyState(const NativeConditionalCopyState &) = delete;
    NativeConditionalCopyState &operator=(const NativeConditionalCopyState &) = delete;

    ~NativeConditionalCopyState() { Close(); }

    [[nodiscard]] ProviderConditionalCopyCommitResult
    Commit(const ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) noexcept;

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

/**
 * The Move counterpart, and it anchors one descriptor more than a Copy does.
 *
 * A Copy publishes *from* its source descriptor, so the directory holding the source is nothing to it.
 * A rename publishes *by name inside a directory*, so this holds the source parent open, keeps the
 * source's own descriptor purely as proof of what was reviewed, and checks at commit that the name
 * still resolves to that same object. The residual window between that check and the rename is
 * documented at the provider's own test: it cannot be closed, only bounded.
 */
class NativeConditionalMoveState final
{
public:
    NativeConditionalMoveState(int _source_parent_fd,
                               int _source_fd,
                               int _destination_parent_fd,
                               ProviderConditionalCopyExistingExpectation _source,
                               ProviderConditionalCopyExistingExpectation _source_parent,
                               ProviderConditionalCopyExistingExpectation _destination_parent,
                               std::string _source_name,
                               std::string _destination_name,
                               native::ConditionalCopyMetadataSnapshot _source_metadata,
                               native::ConditionalCopyMetadataSnapshot _source_parent_metadata,
                               native::ConditionalCopyMetadataSnapshot _destination_parent_metadata,
                               nc::utility::NativeFSManager &_native_fs_manager,
                               std::shared_ptr<native::ConditionalCopyIO> _io) noexcept
        : m_SourceParentFD{_source_parent_fd}, m_SourceFD{_source_fd}, m_DestinationParentFD{_destination_parent_fd},
          m_Source{std::move(_source)}, m_SourceParent{std::move(_source_parent)},
          m_DestinationParent{std::move(_destination_parent)}, m_SourceName{std::move(_source_name)},
          m_DestinationName{std::move(_destination_name)}, m_SourceMetadata{std::move(_source_metadata)},
          m_SourceParentMetadata{std::move(_source_parent_metadata)},
          m_DestinationParentMetadata{std::move(_destination_parent_metadata)}, m_NativeFSManager{_native_fs_manager},
          m_IO{std::move(_io)}
    {
    }

    NativeConditionalMoveState(const NativeConditionalMoveState &) = delete;
    NativeConditionalMoveState &operator=(const NativeConditionalMoveState &) = delete;

    ~NativeConditionalMoveState() { Close(); }

    [[nodiscard]] ProviderConditionalCopyCommitResult
    Commit(const ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) noexcept;

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
        for( int *fd : {&m_SourceParentFD, &m_SourceFD, &m_DestinationParentFD} ) {
            if( *fd >= 0 ) {
                m_IO->Close(*fd);
                *fd = -1;
            }
        }
    }

    int m_SourceParentFD;
    int m_SourceFD;
    int m_DestinationParentFD;
    ProviderConditionalCopyExistingExpectation m_Source;
    ProviderConditionalCopyExistingExpectation m_SourceParent;
    ProviderConditionalCopyExistingExpectation m_DestinationParent;
    std::string m_SourceName;
    std::string m_DestinationName;
    native::ConditionalCopyMetadataSnapshot m_SourceMetadata;
    native::ConditionalCopyMetadataSnapshot m_SourceParentMetadata;
    native::ConditionalCopyMetadataSnapshot m_DestinationParentMetadata;
    nc::utility::NativeFSManager &m_NativeFSManager;
    std::shared_ptr<native::ConditionalCopyIO> m_IO;
    std::mutex m_Mutex;
};

/** Owns a granted one-use helper lease until the generic provider transaction takes over. */
class NativeCrossVolumeStagingState final
{
public:
    explicit NativeCrossVolumeStagingState(std::unique_ptr<native::CrossVolumeStagingTransaction> _transaction) noexcept
        : m_Transaction{std::move(_transaction)}
    {
    }

    NativeCrossVolumeStagingState(const NativeCrossVolumeStagingState &) = delete;
    NativeCrossVolumeStagingState &operator=(const NativeCrossVolumeStagingState &) = delete;

    ~NativeCrossVolumeStagingState() { (void)Abort(); }

    [[nodiscard]] ProviderConditionalCopyCommitResult
    Commit(const ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) noexcept
    {
        const auto lock = std::lock_guard{m_Mutex};
        if( m_CommitResult )
            return *m_CommitResult;
        if( m_AbortPublication )
            return NativeConditionalCopyUnknown(EIO);
        if( !m_Transaction )
            return NativeConditionalCopyUnknown(EBADF);
        m_CommitResult = m_Transaction->Commit(_cancel_checker);
        return *m_CommitResult;
    }

    [[nodiscard]] ProviderConditionalCopyPublicationState Abort() noexcept
    {
        const auto lock = std::lock_guard{m_Mutex};
        if( m_AbortPublication )
            return *m_AbortPublication;
        if( m_CommitResult ) {
            m_AbortPublication = m_CommitResult->publication;
            return *m_AbortPublication;
        }
        if( !m_Transaction ) {
            m_AbortPublication = ProviderConditionalCopyPublicationState::Unknown;
            return *m_AbortPublication;
        }
        m_AbortPublication = m_Transaction->Abort();
        return *m_AbortPublication;
    }

private:
    std::unique_ptr<native::CrossVolumeStagingTransaction> m_Transaction;
    std::mutex m_Mutex;
    std::optional<ProviderConditionalCopyCommitResult> m_CommitResult;
    std::optional<ProviderConditionalCopyPublicationState> m_AbortPublication;
};

bool NativeConditionalCopyTimestampMatches(const timespec &_actual,
                                           const ProviderConditionalCopyTimestamp &_expected) noexcept
{
    return _actual.tv_sec == _expected.seconds && _actual.tv_nsec == _expected.nanoseconds;
}

/** Never behind: raw seconds/nanoseconds pairs, so every timestamp type here can share one rule. */
bool NativeConditionalCopySecondsNanosAtLeast(int64_t _actual_seconds,
                                              int64_t _actual_nanoseconds,
                                              int64_t _expected_seconds,
                                              int64_t _expected_nanoseconds) noexcept
{
    return _actual_seconds > _expected_seconds ||
           (_actual_seconds == _expected_seconds && _actual_nanoseconds >= _expected_nanoseconds);
}

bool NativeConditionalCopyContentFieldMatches(uint64_t _actual,
                                              uint64_t _expected,
                                              ProviderConditionalCopyExpectationTolerance _tolerance) noexcept
{
    return _tolerance == ProviderConditionalCopyExpectationTolerance::Exact ? _actual == _expected
                                                                            : _actual >= _expected;
}

bool NativeConditionalCopyContentTimestampMatches(const timespec &_actual,
                                                  const ProviderConditionalCopyTimestamp &_expected,
                                                  ProviderConditionalCopyExpectationTolerance _tolerance) noexcept
{
    if( _tolerance == ProviderConditionalCopyExpectationTolerance::Exact )
        return NativeConditionalCopyTimestampMatches(_actual, _expected);
    return NativeConditionalCopySecondsNanosAtLeast(
        _actual.tv_sec, _actual.tv_nsec, _expected.seconds, _expected.nanoseconds);
}

bool NativeConditionalCopyStatMatches(const struct stat &_actual,
                                      const ProviderConditionalCopyExistingExpectation &_expected) noexcept
{
    const bool kind_matches =
        (_expected.kind == ProviderConditionalCopyExpectedKind::RegularFile && S_ISREG(_actual.st_mode)) ||
        (_expected.kind == ProviderConditionalCopyExpectedKind::Directory && S_ISDIR(_actual.st_mode));
    // Identity and the whole permission surface stay exact under either tolerance: only what a batch's
    // own prior publication predictably advances - size and the two content-derived timestamps - may
    // grow instead of matching. Everything that would signal someone else touched this object (a
    // different inode, a different mode) still refuses immediately.
    return kind_matches && static_cast<int32_t>(_actual.st_dev) == _expected.device &&
           static_cast<uint64_t>(_actual.st_ino) == _expected.inode &&
           NativeConditionalCopyTimestampMatches(_actual.st_birthtimespec, _expected.birth_time) &&
           static_cast<uint16_t>(_actual.st_mode) == _expected.mode &&
           NativeConditionalCopyContentFieldMatches(
               static_cast<uint64_t>(_actual.st_size), _expected.byte_size, _expected.tolerance) &&
           NativeConditionalCopyContentTimestampMatches(
               _actual.st_mtimespec, _expected.modification_time, _expected.tolerance) &&
           NativeConditionalCopyContentTimestampMatches(
               _actual.st_ctimespec, _expected.status_change_time, _expected.tolerance);
}

bool NativeConditionalCopyMetadataMatchesExpectation(
    const native::ConditionalCopyMetadataSnapshot &_actual,
    const ProviderConditionalCopyExistingExpectation &_expected) noexcept
{
    const bool kind_matches =
        (_expected.kind == ProviderConditionalCopyExpectedKind::RegularFile && S_ISREG(_actual.mode)) ||
        (_expected.kind == ProviderConditionalCopyExpectedKind::Directory && S_ISDIR(_actual.mode));
    const bool modification_time_matches =
        _expected.tolerance == ProviderConditionalCopyExpectationTolerance::Exact
           ? (_actual.modification_time.seconds == _expected.modification_time.seconds &&
              _actual.modification_time.nanoseconds == _expected.modification_time.nanoseconds)
           : NativeConditionalCopySecondsNanosAtLeast(_actual.modification_time.seconds,
                                                       _actual.modification_time.nanoseconds,
                                                       _expected.modification_time.seconds,
                                                       _expected.modification_time.nanoseconds);
    const bool change_time_matches =
        _expected.tolerance == ProviderConditionalCopyExpectationTolerance::Exact
           ? (_actual.change_time.seconds == _expected.status_change_time.seconds &&
              _actual.change_time.nanoseconds == _expected.status_change_time.nanoseconds)
           : NativeConditionalCopySecondsNanosAtLeast(_actual.change_time.seconds,
                                                       _actual.change_time.nanoseconds,
                                                       _expected.status_change_time.seconds,
                                                       _expected.status_change_time.nanoseconds);
    return kind_matches && static_cast<int32_t>(_actual.device) == _expected.device &&
           _actual.inode == _expected.inode && _actual.birth_time.seconds == _expected.birth_time.seconds &&
           _actual.birth_time.nanoseconds == _expected.birth_time.nanoseconds &&
           static_cast<uint16_t>(_actual.mode) == _expected.mode &&
           NativeConditionalCopyContentFieldMatches(_actual.size, _expected.byte_size, _expected.tolerance) &&
           modification_time_matches && change_time_matches;
}

/**
 * The full re-check Commit runs against what Begin itself captured on this same anchored descriptor.
 * Identity, ownership, permissions, ACL and extended attributes stay exact regardless of tolerance -
 * this is the one place they are checked at all for the destination parent, since the narrower
 * expectation above never carried them. Only size, link_count and the two content timestamps may
 * have grown.
 */
bool NativeConditionalCopyMetadataSnapshotMatches(const native::ConditionalCopyMetadataSnapshot &_actual,
                                                  const native::ConditionalCopyMetadataSnapshot &_expected,
                                                  ProviderConditionalCopyExpectationTolerance _tolerance) noexcept
{
    if( _tolerance == ProviderConditionalCopyExpectationTolerance::Exact )
        return _actual == _expected;
    // APFS advances a directory's link_count when a regular-file child is added, not only for
    // subdirectories - empirically confirmed, not assumed. It is therefore a content signal for a
    // directory, exactly like size, and belongs with it rather than with the identity/permission
    // fields that must never move.
    return _actual.device == _expected.device && _actual.inode == _expected.inode &&
           _actual.uid == _expected.uid && _actual.gid == _expected.gid && _actual.mode == _expected.mode &&
           _actual.flags == _expected.flags && _actual.access_time == _expected.access_time &&
           _actual.birth_time == _expected.birth_time && _actual.acl == _expected.acl &&
           _actual.extended_attributes == _expected.extended_attributes &&
           _actual.link_count >= _expected.link_count && _actual.size >= _expected.size &&
           NativeConditionalCopySecondsNanosAtLeast(_actual.modification_time.seconds,
                                                     _actual.modification_time.nanoseconds,
                                                     _expected.modification_time.seconds,
                                                     _expected.modification_time.nanoseconds) &&
           NativeConditionalCopySecondsNanosAtLeast(_actual.change_time.seconds,
                                                     _actual.change_time.nanoseconds,
                                                     _expected.change_time.seconds,
                                                     _expected.change_time.nanoseconds);
}

native::CrossVolumeStagingObjectSeal
NativeCrossVolumeStagingSeal(const native::ConditionalCopyMetadataSnapshot &_metadata) noexcept
{
    return native::CrossVolumeStagingObjectSeal{
        .device = _metadata.device,
        .inode = _metadata.inode,
        .birth_time = {.seconds = _metadata.birth_time.seconds, .nanoseconds = _metadata.birth_time.nanoseconds},
        .uid = _metadata.uid,
        .gid = _metadata.gid,
        .mode = _metadata.mode,
        .flags = _metadata.flags,
        .link_count = _metadata.link_count,
        .byte_size = _metadata.size,
        .modification_time = {.seconds = _metadata.modification_time.seconds,
                              .nanoseconds = _metadata.modification_time.nanoseconds},
        .status_change_time = {.seconds = _metadata.change_time.seconds,
                               .nanoseconds = _metadata.change_time.nanoseconds},
    };
}

ProviderConditionalCopyCommitResult
NativeConditionalCopyState::Commit(const ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) noexcept
{
    const auto lock = std::lock_guard{m_Mutex};
    if( m_SourceFD < 0 || m_DestinationParentFD < 0 ) {
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, EBADF);
    }

    struct stat source_stat{};
    if( m_IO->FStat(m_SourceFD, &source_stat) != 0 ) {
        const int stat_error = NativeConditionalCopyErrno();
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, stat_error);
    }
    if( !NativeConditionalCopyStatMatches(source_stat, m_Source) ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::SourceStale, ESTALE);
    }
    const auto source_metadata = m_IO->CaptureMetadata(m_SourceFD);
    if( !source_metadata ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure,
                                                 source_metadata.error());
    }
    if( *source_metadata != m_SourceMetadata ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::SourceStale, ESTALE);
    }

    struct stat destination_stat{};
    if( m_IO->FStatAt(m_DestinationParentFD, m_DestinationName.c_str(), &destination_stat, AT_SYMLINK_NOFOLLOW) == 0 ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::DestinationExists, EEXIST);
    }
    if( errno != ENOENT ) {
        const int stat_error = NativeConditionalCopyErrno();
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, stat_error);
    }

    struct stat destination_parent_stat{};
    if( m_IO->FStat(m_DestinationParentFD, &destination_parent_stat) != 0 ) {
        const int stat_error = NativeConditionalCopyErrno();
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, stat_error);
    }
    if( !NativeConditionalCopyStatMatches(destination_parent_stat, m_DestinationParent) ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::DestinationParentStale, ESTALE);
    }
    const auto destination_parent_metadata = m_IO->CaptureMetadata(m_DestinationParentFD);
    if( !destination_parent_metadata ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure,
                                                 destination_parent_metadata.error());
    }
    if( !NativeConditionalCopyMetadataSnapshotMatches(
            *destination_parent_metadata, m_DestinationParentMetadata, m_DestinationParent.tolerance) ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::DestinationParentStale, ESTALE);
    }

    const auto source_volume = m_NativeFSManager.VolumeFromFD(m_SourceFD);
    const auto destination_volume = m_NativeFSManager.VolumeFromFD(m_DestinationParentFD);
    if( !source_volume || !destination_volume || source_stat.st_dev != destination_parent_stat.st_dev ||
        !native::ConditionalCopyVolumesMatch(*source_volume, *destination_volume) ||
        !native::EvaluateConditionalCopyVolume(*source_volume).IsSupported() ||
        !native::EvaluateConditionalCopyVolume(*destination_volume).IsSupported() ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, ENOTSUP);
    }

    // This is the last cancellation point.  After Clone the result must retain the observed publication state.
    if( NativeConditionalCopyCancelled(_cancel_checker) ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::Cancelled);
    }

    m_IO->Checkpoint(native::ConditionalCopyCheckpoint::BeforePublish);
    if( m_IO->Clone(m_SourceFD, m_DestinationParentFD, m_DestinationName.c_str(), CLONE_ACL) != 0 ) {
        const int clone_error = NativeConditionalCopyErrno();
        if( clone_error == EEXIST ) {
            CloseUnlocked();
            return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::DestinationExists, EEXIST);
        }
        struct stat post_clone_stat{};
        const int probe_result =
            m_IO->FStatAt(m_DestinationParentFD, m_DestinationName.c_str(), &post_clone_stat, AT_SYMLINK_NOFOLLOW);
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
    }
    else if( *post_clone_source_metadata != m_SourceMetadata ) {
        // Publication is already known. A source mutation racing the clone invalidates the reviewed metadata proof,
        // so the caller must see a published metadata failure even when the durability barriers still succeed.
        remember_metadata_error(ESTALE);
    }

    const int destination_fd =
        m_IO->OpenAt(m_DestinationParentFD, m_DestinationName.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if( destination_fd < 0 ) {
        remember_metadata_error(NativeConditionalCopyErrno());
        remember_sync_error(EBADF);
    }
    else {
        const auto destination_metadata = m_IO->CaptureMetadata(destination_fd);
        if( !destination_metadata ) {
            remember_metadata_error(destination_metadata.error());
        }
        else {
            struct stat named_destination{};
            if( m_IO->FStatAt(
                    m_DestinationParentFD, m_DestinationName.c_str(), &named_destination, AT_SYMLINK_NOFOLLOW) != 0 ) {
                remember_metadata_error(NativeConditionalCopyErrno());
            }
            else if( static_cast<uint64_t>(named_destination.st_dev) != destination_metadata->device ||
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
        m_IO->Checkpoint(native::ConditionalCopyCheckpoint::AfterPublishBeforeFullFSync);
        if( NativeConditionalCopyRetryInterrupted([&] { return m_IO->FullFSync(destination_fd); }) != 0 )
            remember_sync_error(NativeConditionalCopyErrno());

        struct stat final_named_destination{};
        struct stat final_open_destination{};
        if( m_IO->FStat(destination_fd, &final_open_destination) != 0 ) {
            remember_metadata_error(NativeConditionalCopyErrno());
        }
        else if( m_IO->FStatAt(
                     m_DestinationParentFD, m_DestinationName.c_str(), &final_named_destination, AT_SYMLINK_NOFOLLOW) !=
                 0 ) {
            remember_metadata_error(NativeConditionalCopyErrno());
        }
        else if( final_named_destination.st_dev != final_open_destination.st_dev ||
                 final_named_destination.st_ino != final_open_destination.st_ino ) {
            remember_metadata_error(ESTALE);
        }
        m_IO->Close(destination_fd);
    }

    CloseUnlocked();
    return NativeConditionalCopyPublished(metadata_error, filesystem_sync_error);
}

ProviderConditionalCopyCommitResult
NativeConditionalMoveState::Commit(const ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) noexcept
{
    const auto lock = std::lock_guard{m_Mutex};
    if( m_SourceParentFD < 0 || m_SourceFD < 0 || m_DestinationParentFD < 0 )
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, EBADF);

    // The object that was reviewed is still the object this descriptor holds.
    struct stat source_stat{};
    if( m_IO->FStat(m_SourceFD, &source_stat) != 0 ) {
        const int stat_error = NativeConditionalCopyErrno();
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, stat_error);
    }
    if( !NativeConditionalCopyStatMatches(source_stat, m_Source) ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::SourceStale, ESTALE);
    }
    const auto source_metadata = m_IO->CaptureMetadata(m_SourceFD);
    if( !source_metadata ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure,
                                                 source_metadata.error());
    }
    if( *source_metadata != m_SourceMetadata ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::SourceStale, ESTALE);
    }

    // The check a Copy never has to make, and the reason the source parent is anchored at all: the
    // rename will act on this *name*, so the name must still lead to the object above. Holding the
    // source open proves the object exists; it says nothing about what the name points at now.
    struct stat named_source{};
    if( m_IO->FStatAt(m_SourceParentFD, m_SourceName.c_str(), &named_source, AT_SYMLINK_NOFOLLOW) != 0 ) {
        const int stat_error = NativeConditionalCopyErrno();
        CloseUnlocked();
        return stat_error == ENOENT
                   ? NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::SourceStale, ESTALE)
                   : NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure,
                                                       stat_error);
    }
    if( named_source.st_dev != source_stat.st_dev || named_source.st_ino != source_stat.st_ino ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::SourceStale, ESTALE);
    }

    // A stale source parent is reported as source staleness: the consumer's question is whether the
    // world around the source moved, and a separate terminal word for the directory would not change
    // what it must do about it.
    struct stat source_parent_stat{};
    if( m_IO->FStat(m_SourceParentFD, &source_parent_stat) != 0 ) {
        const int stat_error = NativeConditionalCopyErrno();
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, stat_error);
    }
    if( !NativeConditionalCopyStatMatches(source_parent_stat, m_SourceParent) ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::SourceStale, ESTALE);
    }
    const auto source_parent_metadata = m_IO->CaptureMetadata(m_SourceParentFD);
    if( !source_parent_metadata ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure,
                                                 source_parent_metadata.error());
    }
    if( !NativeConditionalCopyMetadataSnapshotMatches(
            *source_parent_metadata, m_SourceParentMetadata, m_SourceParent.tolerance) ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::SourceStale, ESTALE);
    }

    struct stat destination_stat{};
    if( m_IO->FStatAt(m_DestinationParentFD, m_DestinationName.c_str(), &destination_stat, AT_SYMLINK_NOFOLLOW) == 0 ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::DestinationExists, EEXIST);
    }
    if( errno != ENOENT ) {
        const int stat_error = NativeConditionalCopyErrno();
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, stat_error);
    }

    struct stat destination_parent_stat{};
    if( m_IO->FStat(m_DestinationParentFD, &destination_parent_stat) != 0 ) {
        const int stat_error = NativeConditionalCopyErrno();
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, stat_error);
    }
    if( !NativeConditionalCopyStatMatches(destination_parent_stat, m_DestinationParent) ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::DestinationParentStale, ESTALE);
    }
    const auto destination_parent_metadata = m_IO->CaptureMetadata(m_DestinationParentFD);
    if( !destination_parent_metadata ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure,
                                                 destination_parent_metadata.error());
    }
    if( !NativeConditionalCopyMetadataSnapshotMatches(
            *destination_parent_metadata, m_DestinationParentMetadata, m_DestinationParent.tolerance) ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::DestinationParentStale, ESTALE);
    }

    const auto source_volume = m_NativeFSManager.VolumeFromFD(m_SourceParentFD);
    const auto destination_volume = m_NativeFSManager.VolumeFromFD(m_DestinationParentFD);
    if( !source_volume || !destination_volume || source_parent_stat.st_dev != destination_parent_stat.st_dev ||
        source_stat.st_dev != source_parent_stat.st_dev ||
        !native::ConditionalCopyVolumesMatch(*source_volume, *destination_volume) ||
        !native::EvaluateConditionalMoveVolume(*source_volume).IsSupported() ||
        !native::EvaluateConditionalMoveVolume(*destination_volume).IsSupported() ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure, ENOTSUP);
    }

    // The last cancellation point. After the rename the result must report what was observed.
    if( NativeConditionalCopyCancelled(_cancel_checker) ) {
        CloseUnlocked();
        return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::Cancelled);
    }

    m_IO->Checkpoint(native::ConditionalCopyCheckpoint::BeforePublish);
    if( m_IO->RenameExclusive(
            m_SourceParentFD, m_SourceName.c_str(), m_DestinationParentFD, m_DestinationName.c_str()) != 0 ) {
        const int rename_error = NativeConditionalCopyErrno();
        if( rename_error == EEXIST ) {
            CloseUnlocked();
            return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::DestinationExists, EEXIST);
        }
        // A rename either happened or did not, but a failed call plus a concurrent world cannot be
        // told apart from a success that was undone. Absence of the destination is necessary and not
        // sufficient: the source must also still be where it was, under the name it had.
        struct stat post_rename_destination{};
        const int destination_probe = m_IO->FStatAt(
            m_DestinationParentFD, m_DestinationName.c_str(), &post_rename_destination, AT_SYMLINK_NOFOLLOW);
        const int destination_probe_error = NativeConditionalCopyErrno();
        struct stat post_rename_source{};
        const int source_probe =
            m_IO->FStatAt(m_SourceParentFD, m_SourceName.c_str(), &post_rename_source, AT_SYMLINK_NOFOLLOW);
        CloseUnlocked();
        if( destination_probe != 0 && destination_probe_error == ENOENT && source_probe == 0 &&
            post_rename_source.st_dev == source_stat.st_dev && post_rename_source.st_ino == source_stat.st_ino ) {
            return NativeConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::ProviderFailure,
                                                     rename_error);
        }
        return NativeConditionalCopyUnknown(rename_error);
    }

    // Both directories changed and both must be durable before this is called complete: a rename that
    // survives in one direction only is a lost file rather than a moved one. No file content was
    // written, so there is nothing to sync on the object itself - only the two namespaces.
    int filesystem_sync_error = 0;
    const auto remember_sync_error = [&](int _error) noexcept {
        if( filesystem_sync_error == 0 )
            filesystem_sync_error = _error == 0 ? EIO : _error;
    };
    if( NativeConditionalCopyRetryInterrupted([&] { return m_IO->FSync(m_DestinationParentFD); }) != 0 )
        remember_sync_error(NativeConditionalCopyErrno());
    if( NativeConditionalCopyRetryInterrupted([&] { return m_IO->FSync(m_SourceParentFD); }) != 0 )
        remember_sync_error(NativeConditionalCopyErrno());

    m_IO->Checkpoint(native::ConditionalCopyCheckpoint::AfterPublishBeforeFullFSync);
    if( NativeConditionalCopyRetryInterrupted([&] { return m_IO->FullFSync(m_DestinationParentFD); }) != 0 )
        remember_sync_error(NativeConditionalCopyErrno());
    if( NativeConditionalCopyRetryInterrupted([&] { return m_IO->FullFSync(m_SourceParentFD); }) != 0 )
        remember_sync_error(NativeConditionalCopyErrno());

    // The published entry must be the reviewed object and nothing else that happened to take the name.
    int metadata_error = 0;
    struct stat published{};
    if( m_IO->FStatAt(m_DestinationParentFD, m_DestinationName.c_str(), &published, AT_SYMLINK_NOFOLLOW) != 0 )
        metadata_error = NativeConditionalCopyErrno();
    else if( published.st_dev != source_stat.st_dev || published.st_ino != source_stat.st_ino )
        metadata_error = ESTALE;

    CloseUnlocked();
    return NativeConditionalCopyPublished(metadata_error, filesystem_sync_error);
}

std::optional<std::string> NativeConditionalCopyChildName(const std::string &_parent,
                                                          const std::string &_child) noexcept
{
    if( _parent.empty() || _child.empty() || _parent.front() != '/' || _child.front() != '/' )
        return std::nullopt;
    const size_t name_offset = _parent == "/" ? 1 : _parent.size() + 1;
    if( _child.size() <= name_offset || _child.substr(0, name_offset) != (_parent == "/" ? "/" : _parent + "/") )
        return std::nullopt;
    const auto name = _child.substr(name_offset);
    if( name.empty() || name.find('/') != std::string::npos || name == "." || name == ".." ||
        name.find('\0') != std::string::npos )
        return std::nullopt;
    return name;
}

std::optional<std::string>
NativeConditionalCopyDestinationName(const ProviderConditionalCopyReviewedClaims &_claims) noexcept
{
    return NativeConditionalCopyChildName(_claims.destination_parent.absolute_path, _claims.destination.absolute_path);
}

bool NativeConditionalCopyCancelled(const VFSCancelChecker &_cancel_checker) noexcept
{
    try {
        return _cancel_checker && _cancel_checker();
    } catch( ... ) {
        return true;
    }
}

template <class T>
void CopyNativeListingVariableRange(const base::variable_container<T> &_source,
                                    base::variable_container<T> &_destination,
                                    size_t _begin,
                                    size_t _end)
{
    _destination.reset(base::variable_container<>::type::sparse);
    for( size_t source_index = _begin, destination_index = 0; source_index != _end;
         ++source_index, ++destination_index )
        if( _source.has(source_index) )
            _destination.insert(destination_index, _source[source_index]);
}

VFSListingPtr BuildNativeListingRange(const ListingInput &_source, size_t _begin, size_t _end)
{
    assert(_begin < _end);

    ListingInput destination;
    destination.hosts[0] = _source.hosts[_begin];
    destination.directories[0] = _source.directories[_begin];
    destination.filenames.reserve(_end - _begin);
    destination.unix_modes.reserve(_end - _begin);
    destination.unix_types.reserve(_end - _begin);

    for( size_t source_index = _begin; source_index != _end; ++source_index ) {
        destination.filenames.emplace_back(_source.filenames[source_index]);
        destination.unix_modes.emplace_back(_source.unix_modes[source_index]);
        destination.unix_types.emplace_back(_source.unix_types[source_index]);
    }

    CopyNativeListingVariableRange(_source.display_filenames, destination.display_filenames, _begin, _end);
    CopyNativeListingVariableRange(_source.sizes, destination.sizes, _begin, _end);
    CopyNativeListingVariableRange(_source.inodes, destination.inodes, _begin, _end);
    CopyNativeListingVariableRange(_source.atimes, destination.atimes, _begin, _end);
    CopyNativeListingVariableRange(_source.mtimes, destination.mtimes, _begin, _end);
    CopyNativeListingVariableRange(_source.ctimes, destination.ctimes, _begin, _end);
    CopyNativeListingVariableRange(_source.btimes, destination.btimes, _begin, _end);
    CopyNativeListingVariableRange(_source.add_times, destination.add_times, _begin, _end);
    CopyNativeListingVariableRange(_source.uids, destination.uids, _begin, _end);
    CopyNativeListingVariableRange(_source.gids, destination.gids, _begin, _end);
    CopyNativeListingVariableRange(_source.unix_flags, destination.unix_flags, _begin, _end);
    CopyNativeListingVariableRange(_source.symlinks, destination.symlinks, _begin, _end);

    for( size_t source_index = _begin, destination_index = 0; source_index != _end;
         ++source_index, ++destination_index )
        if( const auto tag = _source.tags.find(source_index); tag != _source.tags.end() )
            destination.tags.emplace(destination_index, tag->second);

    return VFSListing::Build(std::move(destination));
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
                       std::shared_ptr<native::ConditionalCopyIO> _conditional_copy_io,
                       std::shared_ptr<native::CrossVolumeStagingAuthority> _cross_volume_staging_authority)
    : Host("", nullptr, UniqueTag), m_NativeFSManager(_native_fs_man), m_FSEventsFileUpdate(_fsevents_file_update),
      m_ConditionalCopyIO(_conditional_copy_io ? std::move(_conditional_copy_io)
                                               : std::make_shared<native::ConditionalCopyIO>()),
      m_CrossVolumeStagingAuthority(std::move(_cross_volume_staging_authority))
{
    AddFeatures(HostFeatures::FetchUsers | HostFeatures::FetchGroups | HostFeatures::SetOwnership |
                HostFeatures::SetFlags | HostFeatures::SetPermissions | HostFeatures::SetTimes | HostFeatures::Read |
                HostFeatures::CreateFile | HostFeatures::CreateDirectory | HostFeatures::Rename | HostFeatures::Unlink |
                HostFeatures::RemoveDirectory | HostFeatures::Trash | HostFeatures::ReadSymlink |
                HostFeatures::ObserveDirectoryChanges | HostFeatures::CreateSymlink);
}

bool NativeHost::ShouldProduceThumbnails() const
{
    return true;
}

std::expected<VFSListingPtr, Error> NativeHost::FetchDirectoryListing(std::string_view _path,
                                                                      const unsigned long _flags,
                                                                      const VFSCancelChecker &_cancel_checker)
{
    return FetchDirectoryListingProgressively(_path, _flags, {}, _cancel_checker);
}

std::expected<VFSListingPtr, Error>
NativeHost::FetchDirectoryListingProgressively(std::string_view _path,
                                               const unsigned long _flags,
                                               DirectoryListingBatchCallback _callback,
                                               const VFSCancelChecker &_cancel_checker)
{
    Log::Trace("NativeHost::FetchDirectoryListingProgressively() called with path='{}',_flags: {}", _path, _flags);

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

    // Enrich every row before it can be published. The same enriched storage is retained for the authoritative final
    // listing, so progressive delivery never performs a second directory enumeration or weakens listing semantics.
    const bool tags_reading_enabled = TagsFetchingAllowed(_flags, _path);
    std::mutex listing_source_tags_mut;
    std::mutex listing_source_symlinks_mut;
    auto epilogue_pass = [fd,
                          tags_reading_enabled,
                          is_native_io,
                          &io,
                          &listing_source,
                          &listing_source_tags_mut,
                          &listing_source_symlinks_mut,
                          &ext_flags](const std::string &filename) {
        const size_t n = &filename - listing_source.filenames.data();

        if( listing_source.unix_types[n] == DT_LNK ) {
            char linkpath[MAXPATHLEN];
            const ssize_t sz = is_native_io
                                   ? readlinkat(fd, listing_source.filenames[n].c_str(), linkpath, MAXPATHLEN)
                                   : io.readlink((listing_source.directories[0] + listing_source.filenames[n]).c_str(),
                                                 linkpath,
                                                 MAXPATHLEN);
            if( sz >= 0 && sz < MAXPATHLEN ) {
                linkpath[sz] = 0;
                {
                    const std::lock_guard<std::mutex> lock(listing_source_symlinks_mut);
                    listing_source.symlinks.insert(n, linkpath);
                }

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

        if( tags_reading_enabled && !(ext_flags[n] & EF_NO_XATTRS) ) {
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

    auto cancelled = [&] {
        try {
            return _cancel_checker && _cancel_checker();
        } catch( ... ) {
            return true;
        }
    };
    auto enrich_range = [&](size_t _begin, size_t _end) -> bool {
        constexpr size_t enrichment_chunk_size = 256;
        while( _begin != _end ) {
            if( cancelled() )
                return false;
            const size_t count = std::min(enrichment_chunk_size, _end - _begin);
            pstld::for_each_n(listing_source.filenames.begin() + static_cast<ptrdiff_t>(_begin),
                              count,
                              [epilogue_pass](const std::string &filename) {
                                  try {
                                      epilogue_pass(filename);
                                  } catch( ... ) {
                                      // PSTL gets very upset when the functor throws; an unavailable optional attribute
                                      // is represented by its absence, matching the existing full-listing behaviour.
                                  }
                              });
            _begin += count;
            if( cancelled() )
                return false;
        }
        return true;
    };

    size_t enriched_entry_index = 0;
    size_t published_entry_index = 0;
    bool publishing_enabled = static_cast<bool>(_callback);
    std::exception_ptr callback_exception;
    auto publish_pending = [&]() -> bool {
        if( cancelled() )
            return false;
        const size_t end = next_entry_index;
        if( !publishing_enabled || published_entry_index == end )
            return true;

        if( !enrich_range(enriched_entry_index, end) )
            return false;
        enriched_entry_index = end;

        DirectoryListingBatch batch;
        batch.first_index = published_entry_index;
        batch.entries = BuildNativeListingRange(listing_source, published_entry_index, end);
        DirectoryListingBatchDisposition disposition = DirectoryListingBatchDisposition::Cancel;
        try {
            disposition = _callback(std::move(batch));
        } catch( ... ) {
            callback_exception = std::current_exception();
            return false;
        }
        if( disposition == DirectoryListingBatchDisposition::Cancel )
            return false;

        published_entry_index = end;
        if( disposition == DirectoryListingBatchDisposition::StopPublishing )
            publishing_enabled = false;
        return !cancelled();
    };

    auto cb_fetch = [&](size_t _fetched_now) {
        // check if final entries count is more than previous approximate
        if( next_entry_index + _fetched_now > allocated_size )
            resize_dense(next_entry_index + _fetched_now);
    };

    auto cb_batch_drained = [&](size_t) { return publish_pending(); };

    // when Admin Mode is on - we use different fetch route
    const int ret =
        is_native_io ? Fetching::ReadDirAttributesBulk(fd, cb_fetch, cb_param, cb_batch_drained)
                     : Fetching::ReadDirAttributesStat(
                           fd, listing_source.directories[0].c_str(), cb_fetch, cb_param, cb_batch_drained);
    if( callback_exception )
        std::rethrow_exception(callback_exception);
    if( ret != 0 )
        return std::unexpected(Error{Error::POSIX, ret});

    if( cancelled() )
        return std::unexpected(Error{Error::POSIX, ECANCELED});

    // check if final entries count is less than approximate
    if( next_entry_index < allocated_size )
        resize_dense(next_entry_index);

    if( publishing_enabled ) {
        if( !publish_pending() )
            return std::unexpected(Error{Error::POSIX, ECANCELED});
    }

    if( !enrich_range(enriched_entry_index, next_entry_index) )
        return std::unexpected(Error{Error::POSIX, ECANCELED});

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

ProviderConditionalCopyPathSupport
NativeHost::ConditionalCopyPathSupport(const std::string_view _source_path,
                                       const std::string_view _destination_parent_path) const noexcept
{
    if( _source_path.empty() || _source_path.front() != '/' || _destination_parent_path.empty() ||
        _destination_parent_path.front() != '/' )
        return ProviderConditionalCopyPathSupport::Unavailable;

    const auto source_volume = m_NativeFSManager.VolumeFromPath(_source_path);
    const auto destination_volume = m_NativeFSManager.VolumeFromPath(_destination_parent_path);
    if( !source_volume || !destination_volume )
        return ProviderConditionalCopyPathSupport::Unavailable;
    if( native::ConditionalCopyVolumesMatch(*source_volume, *destination_volume) ) {
        if( !native::EvaluateConditionalCopyVolume(*source_volume).IsSupported() ||
            !native::EvaluateConditionalCopyVolume(*destination_volume).IsSupported() )
            return ProviderConditionalCopyPathSupport::Unsupported;
        return ProviderConditionalCopyPathSupport::SameVolumeClone;
    }

    if( !native::EvaluateConditionalCopyVolume(*source_volume).IsSupported() ||
        !native::EvaluateConditionalCopyStagingVolume(*destination_volume).IsSupported() )
        return ProviderConditionalCopyPathSupport::Unsupported;
    if( !m_CrossVolumeStagingAuthority )
        return ProviderConditionalCopyPathSupport::Unsupported;
    if( !m_CrossVolumeStagingAuthority->IsAvailable() )
        return ProviderConditionalCopyPathSupport::Unavailable;
    return ProviderConditionalCopyPathSupport::CrossVolumeStaged;
}

ProviderConditionalMovePathSupport
NativeHost::ConditionalMovePathSupport(const std::string_view _source_path,
                                       const std::string_view _destination_parent_path) const noexcept
{
    if( _source_path.empty() || _source_path.front() != '/' || _destination_parent_path.empty() ||
        _destination_parent_path.front() != '/' )
        return ProviderConditionalMovePathSupport::Unavailable;

    const auto source_volume = m_NativeFSManager.VolumeFromPath(_source_path);
    const auto destination_volume = m_NativeFSManager.VolumeFromPath(_destination_parent_path);
    if( !source_volume || !destination_volume )
        return ProviderConditionalMovePathSupport::Unavailable;
    // Two volumes is a definitive refusal rather than a future scope: a cross-volume Move is
    // copy-then-unlink, which is two events, and a journal item result cannot express "published, and
    // the source is still there". There is nothing to stage towards here.
    if( !native::ConditionalCopyVolumesMatch(*source_volume, *destination_volume) )
        return ProviderConditionalMovePathSupport::Unsupported;
    // Asked of both, though they are one volume: the two evaluations read the same record, and
    // spelling it out keeps this identical in shape to the Copy answer above rather than relying on
    // the match check having already made them interchangeable.
    if( !native::EvaluateConditionalMoveVolume(*source_volume).IsSupported() ||
        !native::EvaluateConditionalMoveVolume(*destination_volume).IsSupported() )
        return ProviderConditionalMovePathSupport::Unsupported;
    return ProviderConditionalMovePathSupport::SameVolumeRename;
}

std::expected<std::unique_ptr<ProviderConditionalCopyTransaction>, ProviderConditionalCopyTransactionBeginError>
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

    const int source_fd = m_ConditionalCopyIO->Open(claims.source.absolute_path.c_str(),
                                                    O_RDONLY | O_NONBLOCK | O_NOFOLLOW_ANY | O_CLOEXEC);
    if( source_fd < 0 ) {
        const int open_error = errno;
        return std::unexpected(open_error == ENOENT || open_error == ENOTDIR || open_error == ELOOP
                                   ? ProviderConditionalCopyTransactionBeginError::SourceStale
                                   : ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    }
    auto close_source = at_scope_end([source_fd, io = m_ConditionalCopyIO] { io->Close(source_fd); });

    struct stat source_stat{};
    if( m_ConditionalCopyIO->FStat(source_fd, &source_stat) != 0 )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    if( !NativeConditionalCopyStatMatches(source_stat, claims.source) )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::SourceStale);

    const int destination_parent_fd = m_ConditionalCopyIO->Open(claims.destination_parent.absolute_path.c_str(),
                                                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
    if( destination_parent_fd < 0 ) {
        const int open_error = errno;
        return std::unexpected(open_error == ENOENT || open_error == ENOTDIR || open_error == ELOOP
                                   ? ProviderConditionalCopyTransactionBeginError::DestinationParentStale
                                   : ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    }
    auto close_destination_parent =
        at_scope_end([destination_parent_fd, io = m_ConditionalCopyIO] { io->Close(destination_parent_fd); });

    struct stat destination_parent_stat{};
    if( m_ConditionalCopyIO->FStat(destination_parent_fd, &destination_parent_stat) != 0 )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    if( !NativeConditionalCopyStatMatches(destination_parent_stat, claims.destination_parent) )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::DestinationParentStale);

    struct stat destination_stat{};
    if( m_ConditionalCopyIO->FStatAt(
            destination_parent_fd, destination_name->c_str(), &destination_stat, AT_SYMLINK_NOFOLLOW) == 0 )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::DestinationExists);
    if( errno != ENOENT )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);

    const auto source_volume = m_NativeFSManager.VolumeFromFD(source_fd);
    const auto destination_volume = m_NativeFSManager.VolumeFromFD(destination_parent_fd);
    if( !source_volume || !destination_volume )
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    const bool same_volume = source_stat.st_dev == destination_parent_stat.st_dev &&
                             native::ConditionalCopyVolumesMatch(*source_volume, *destination_volume);
    if( same_volume ) {
        if( !native::EvaluateConditionalCopyVolume(*source_volume).IsSupported() ||
            !native::EvaluateConditionalCopyVolume(*destination_volume).IsSupported() )
            return std::unexpected(ProviderConditionalCopyTransactionBeginError::Unsupported);
    }
    else if( native::ConditionalCopyVolumesMatch(*source_volume, *destination_volume) ||
             !native::EvaluateConditionalCopyVolume(*source_volume).IsSupported() ||
             !native::EvaluateConditionalCopyStagingVolume(*destination_volume).IsSupported() ||
             !m_CrossVolumeStagingAuthority || !m_CrossVolumeStagingAuthority->IsAvailable() ) {
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

    if( !same_volume ) {
        std::expected<std::unique_ptr<native::CrossVolumeStagingTransaction>,
                      ProviderConditionalCopyTransactionBeginError>
            staging = std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
        try {
            staging = m_CrossVolumeStagingAuthority->Begin(
                native::CrossVolumeStagingRequest{source_fd,
                                                   destination_parent_fd,
                                                   *destination_name,
                                                   NativeCrossVolumeStagingSeal(*source_metadata),
                                                   NativeCrossVolumeStagingSeal(*destination_parent_metadata)},
                _cancel_checker);
        } catch( ... ) {
            return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
        }
        if( !staging || !*staging )
            return std::unexpected(staging ? ProviderConditionalCopyTransactionBeginError::ProviderFailure
                                            : staging.error());
        std::shared_ptr<NativeCrossVolumeStagingState> state;
        try {
            state = std::shared_ptr<NativeCrossVolumeStagingState>{
                new NativeCrossVolumeStagingState{std::move(*staging)}};
        } catch( ... ) {
            if( *staging )
                (void)(*staging)->Abort();
            return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
        }
        auto transaction = MintConditionalCopyTransaction(
            std::move(_authority),
            [state](const auto &_commit_cancel_checker) { return state->Commit(_commit_cancel_checker); },
            [state] { return state->Abort(); });
        if( transaction )
            return transaction;
        if( state->Abort() != ProviderConditionalCopyPublicationState::NotPublished )
            return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
        return std::unexpected(transaction.error());
    }

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
        [state](const auto &_commit_cancel_checker) { return state->Commit(_commit_cancel_checker); },
        [state] { return state->Abort(); });
}

std::expected<std::unique_ptr<ProviderConditionalCopyTransaction>, ProviderConditionalMoveTransactionBeginError>
NativeHost::BeginConditionalMoveTransaction(ProviderConditionalMoveReviewedAuthority _authority,
                                            const VFSCancelChecker &_cancel_checker)
{
    if( NativeConditionalCopyCancelled(_cancel_checker) )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::Cancelled);

    const auto &claims = _authority.Claims();
    const auto source_name =
        NativeConditionalCopyChildName(claims.source_parent.absolute_path, claims.source.absolute_path);
    const auto destination_name =
        NativeConditionalCopyChildName(claims.destination_parent.absolute_path, claims.destination.absolute_path);
    if( claims.destination_binding.host.get() != this || claims.source_binding.host.get() != this ||
        claims.source.kind != ProviderConditionalCopyExpectedKind::RegularFile ||
        claims.source_parent.kind != ProviderConditionalCopyExpectedKind::Directory ||
        claims.destination_parent.kind != ProviderConditionalCopyExpectedKind::Directory || !source_name ||
        !destination_name || !ValidateFilename(*source_name) || !ValidateFilename(*destination_name) ) {
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::InvalidRequest);
    }

    const int source_parent_fd = m_ConditionalCopyIO->Open(claims.source_parent.absolute_path.c_str(),
                                                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
    if( source_parent_fd < 0 ) {
        const int open_error = errno;
        return std::unexpected(open_error == ENOENT || open_error == ENOTDIR || open_error == ELOOP
                                   ? ProviderConditionalMoveTransactionBeginError::SourceParentStale
                                   : ProviderConditionalMoveTransactionBeginError::ProviderFailure);
    }
    auto close_source_parent =
        at_scope_end([source_parent_fd, io = m_ConditionalCopyIO] { io->Close(source_parent_fd); });

    struct stat source_parent_stat{};
    if( m_ConditionalCopyIO->FStat(source_parent_fd, &source_parent_stat) != 0 )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::ProviderFailure);
    if( !NativeConditionalCopyStatMatches(source_parent_stat, claims.source_parent) )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::SourceParentStale);

    // Opened *through* the anchored parent rather than by absolute path. That is what binds the
    // descriptor to the same directory entry the rename will act on, so the identity checked here and
    // the name published later are about one object rather than two paths that merely read alike.
    const int source_fd = m_ConditionalCopyIO->OpenAt(
        source_parent_fd, source_name->c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if( source_fd < 0 ) {
        const int open_error = errno;
        return std::unexpected(open_error == ENOENT || open_error == ENOTDIR || open_error == ELOOP
                                   ? ProviderConditionalMoveTransactionBeginError::SourceStale
                                   : ProviderConditionalMoveTransactionBeginError::ProviderFailure);
    }
    auto close_source = at_scope_end([source_fd, io = m_ConditionalCopyIO] { io->Close(source_fd); });

    struct stat source_stat{};
    if( m_ConditionalCopyIO->FStat(source_fd, &source_stat) != 0 )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::ProviderFailure);
    if( !NativeConditionalCopyStatMatches(source_stat, claims.source) )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::SourceStale);

    const int destination_parent_fd = m_ConditionalCopyIO->Open(claims.destination_parent.absolute_path.c_str(),
                                                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
    if( destination_parent_fd < 0 ) {
        const int open_error = errno;
        return std::unexpected(open_error == ENOENT || open_error == ENOTDIR || open_error == ELOOP
                                   ? ProviderConditionalMoveTransactionBeginError::DestinationParentStale
                                   : ProviderConditionalMoveTransactionBeginError::ProviderFailure);
    }
    auto close_destination_parent =
        at_scope_end([destination_parent_fd, io = m_ConditionalCopyIO] { io->Close(destination_parent_fd); });

    struct stat destination_parent_stat{};
    if( m_ConditionalCopyIO->FStat(destination_parent_fd, &destination_parent_stat) != 0 )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::ProviderFailure);
    if( !NativeConditionalCopyStatMatches(destination_parent_stat, claims.destination_parent) )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::DestinationParentStale);

    struct stat destination_stat{};
    if( m_ConditionalCopyIO->FStatAt(
            destination_parent_fd, destination_name->c_str(), &destination_stat, AT_SYMLINK_NOFOLLOW) == 0 )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::DestinationExists);
    if( errno != ENOENT )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::ProviderFailure);

    // One volume, and eligible for a rename rather than for a clone. There is no staged branch to fall
    // into: two volumes is a definitive refusal for a Move.
    const auto source_volume = m_NativeFSManager.VolumeFromFD(source_parent_fd);
    const auto destination_volume = m_NativeFSManager.VolumeFromFD(destination_parent_fd);
    if( !source_volume || !destination_volume )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::ProviderFailure);
    if( source_stat.st_dev != source_parent_stat.st_dev ||
        source_parent_stat.st_dev != destination_parent_stat.st_dev ||
        !native::ConditionalCopyVolumesMatch(*source_volume, *destination_volume) ||
        !native::EvaluateConditionalMoveVolume(*source_volume).IsSupported() ||
        !native::EvaluateConditionalMoveVolume(*destination_volume).IsSupported() ) {
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::Unsupported);
    }

    auto source_metadata = m_ConditionalCopyIO->CaptureMetadata(source_fd);
    if( !source_metadata )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::ProviderFailure);
    if( !NativeConditionalCopyMetadataMatchesExpectation(*source_metadata, claims.source) )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::SourceStale);

    auto source_parent_metadata = m_ConditionalCopyIO->CaptureMetadata(source_parent_fd);
    if( !source_parent_metadata )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::ProviderFailure);
    if( !NativeConditionalCopyMetadataMatchesExpectation(*source_parent_metadata, claims.source_parent) )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::SourceParentStale);

    auto destination_parent_metadata = m_ConditionalCopyIO->CaptureMetadata(destination_parent_fd);
    if( !destination_parent_metadata )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::ProviderFailure);
    if( !NativeConditionalCopyMetadataMatchesExpectation(*destination_parent_metadata, claims.destination_parent) )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::DestinationParentStale);
    if( !native::ValidateConditionalCopyMetadataPolicy(*source_metadata, *destination_parent_metadata) )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::Unsupported);

    if( NativeConditionalCopyCancelled(_cancel_checker) )
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::Cancelled);

    std::shared_ptr<NativeConditionalMoveState> state;
    try {
        state = std::make_shared<NativeConditionalMoveState>(source_parent_fd,
                                                             source_fd,
                                                             destination_parent_fd,
                                                             claims.source,
                                                             claims.source_parent,
                                                             claims.destination_parent,
                                                             *source_name,
                                                             *destination_name,
                                                             std::move(*source_metadata),
                                                             std::move(*source_parent_metadata),
                                                             std::move(*destination_parent_metadata),
                                                             m_NativeFSManager,
                                                             m_ConditionalCopyIO);
    } catch( ... ) {
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::ProviderFailure);
    }
    close_source_parent.disengage();
    close_source.disengage();
    close_destination_parent.disengage();
    return MintConditionalMoveTransaction(
        std::move(_authority),
        [state](const auto &_commit_cancel_checker) { return state->Commit(_commit_cancel_checker); },
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
