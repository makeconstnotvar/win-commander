// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NativeCreateCopy.h"
#include "NativeCreateCopyJob.h"

#include <cerrno>
#include <copyfile.h>
#include <fcntl.h>
#include <sys/acl.h>
#include <sys/attr.h>
#include <unistd.h>

namespace nc::ops {

namespace {

bool NativeCreateCopyOutcomeCodeIsFailure(NativeCreateCopyOutcomeCode _code) noexcept
{
    switch( _code ) {
        case NativeCreateCopyOutcomeCode::StaleSource:
        case NativeCreateCopyOutcomeCode::StaleDestination:
        case NativeCreateCopyOutcomeCode::ReadFailed:
        case NativeCreateCopyOutcomeCode::WriteFailed:
        case NativeCreateCopyOutcomeCode::SyncFailed:
        case NativeCreateCopyOutcomeCode::MetadataFailed:
        case NativeCreateCopyOutcomeCode::MetadataUnsupported:
        case NativeCreateCopyOutcomeCode::MetadataPermissionDenied:
        case NativeCreateCopyOutcomeCode::MetadataVerificationFailed:
        case NativeCreateCopyOutcomeCode::CommitFailed:
            return true;
        case NativeCreateCopyOutcomeCode::Pending:
        case NativeCreateCopyOutcomeCode::Success:
        case NativeCreateCopyOutcomeCode::Cancelled:
        case NativeCreateCopyOutcomeCode::FileSystemSyncFailed:
        case NativeCreateCopyOutcomeCode::CleanupFailed:
            return false;
    }
    return false;
}

OperationJournalItemError
NativeCreateCopyPriorJournalError(NativeCreateCopyOutcomeCode _code) noexcept
{
    switch( _code ) {
        case NativeCreateCopyOutcomeCode::Cancelled:
            return OperationJournalItemError::Cancelled;
        case NativeCreateCopyOutcomeCode::StaleSource:
            return OperationJournalItemError::SourceChanged;
        case NativeCreateCopyOutcomeCode::StaleDestination:
            return OperationJournalItemError::DestinationChanged;
        case NativeCreateCopyOutcomeCode::ReadFailed:
            return OperationJournalItemError::Read;
        case NativeCreateCopyOutcomeCode::WriteFailed:
            return OperationJournalItemError::Write;
        case NativeCreateCopyOutcomeCode::SyncFailed:
            return OperationJournalItemError::Write;
        case NativeCreateCopyOutcomeCode::MetadataFailed:
        case NativeCreateCopyOutcomeCode::MetadataUnsupported:
        case NativeCreateCopyOutcomeCode::MetadataVerificationFailed:
            return OperationJournalItemError::Metadata;
        case NativeCreateCopyOutcomeCode::MetadataPermissionDenied:
            return OperationJournalItemError::PermissionDenied;
        case NativeCreateCopyOutcomeCode::CommitFailed:
            return OperationJournalItemError::Commit;
        case NativeCreateCopyOutcomeCode::Pending:
        case NativeCreateCopyOutcomeCode::Success:
        case NativeCreateCopyOutcomeCode::FileSystemSyncFailed:
        case NativeCreateCopyOutcomeCode::CleanupFailed:
            return OperationJournalItemError::None;
    }
    return OperationJournalItemError::None;
}

} // namespace

std::expected<OperationJournalItemResult, NativeCreateCopyJournalMappingError>
MapNativeCreateCopyOutcomeToJournalItemResult(const NativeCreateCopyOutcome &_outcome,
                                              size_t _item_index) noexcept
{
    using MappingError = NativeCreateCopyJournalMappingError;

    if( _outcome.code == NativeCreateCopyOutcomeCode::Pending )
        return std::unexpected(MappingError::PendingOutcome);

    const bool destination_not_published =
        _outcome.destination_publication == NativeCreateCopyPublicationState::NotPublished;
    const bool destination_is_published =
        _outcome.destination_publication == NativeCreateCopyPublicationState::Published;
    const bool destination_unknown =
        _outcome.destination_publication == NativeCreateCopyPublicationState::Unknown;
    switch( _outcome.destination_publication ) {
        case NativeCreateCopyPublicationState::NotPublished:
        case NativeCreateCopyPublicationState::Unknown:
            if( _outcome.filesystem_sync_error_number != 0 || _outcome.filesystem_sync_confirmed )
                return std::unexpected(MappingError::InconsistentOutcome);
            break;
        case NativeCreateCopyPublicationState::Published:
            if( _outcome.filesystem_sync_confirmed != (_outcome.filesystem_sync_error_number == 0) )
                return std::unexpected(MappingError::InconsistentOutcome);
            break;
        default:
            return std::unexpected(MappingError::InconsistentOutcome);
    }
    if( destination_unknown &&
        (_outcome.code != NativeCreateCopyOutcomeCode::CommitFailed || _outcome.recovery_artifact_left) )
        return std::unexpected(MappingError::InconsistentOutcome);

    if( _outcome.code != NativeCreateCopyOutcomeCode::CleanupFailed ) {
        if( _outcome.prior_code != NativeCreateCopyOutcomeCode::Pending || _outcome.recovery_artifact_left )
            return std::unexpected(MappingError::InconsistentOutcome);
    }

    OperationJournalItemStatus status = OperationJournalItemStatus::Failed;
    OperationJournalItemError error = OperationJournalItemError::Unknown;
    int system_error = _outcome.error_number;
    OperationJournalItemError prior_error = OperationJournalItemError::None;
    int prior_system_error = 0;
    OperationJournalRecoveryAction recovery = OperationJournalRecoveryAction::None;
    switch( _outcome.code ) {
        case NativeCreateCopyOutcomeCode::Pending:
            return std::unexpected(MappingError::PendingOutcome);
        case NativeCreateCopyOutcomeCode::Success:
            if( _outcome.error_number != 0 || !destination_is_published ||
                !_outcome.filesystem_sync_confirmed )
                return std::unexpected(MappingError::InconsistentOutcome);
            status = OperationJournalItemStatus::Succeeded;
            error = OperationJournalItemError::None;
            break;
        case NativeCreateCopyOutcomeCode::Cancelled:
            if( _outcome.error_number != 0 || !destination_not_published )
                return std::unexpected(MappingError::InconsistentOutcome);
            status = OperationJournalItemStatus::Cancelled;
            error = OperationJournalItemError::Cancelled;
            break;
        case NativeCreateCopyOutcomeCode::StaleSource:
            error = OperationJournalItemError::SourceChanged;
            break;
        case NativeCreateCopyOutcomeCode::StaleDestination:
            error = OperationJournalItemError::DestinationChanged;
            break;
        case NativeCreateCopyOutcomeCode::ReadFailed:
            error = OperationJournalItemError::Read;
            break;
        case NativeCreateCopyOutcomeCode::WriteFailed:
            error = OperationJournalItemError::Write;
            break;
        case NativeCreateCopyOutcomeCode::SyncFailed:
            error = destination_is_published ? OperationJournalItemError::Commit
                                             : OperationJournalItemError::Write;
            break;
        case NativeCreateCopyOutcomeCode::MetadataFailed:
        case NativeCreateCopyOutcomeCode::MetadataUnsupported:
        case NativeCreateCopyOutcomeCode::MetadataVerificationFailed:
            error = OperationJournalItemError::Metadata;
            break;
        case NativeCreateCopyOutcomeCode::MetadataPermissionDenied:
            error = OperationJournalItemError::PermissionDenied;
            break;
        case NativeCreateCopyOutcomeCode::CommitFailed:
            error = OperationJournalItemError::Commit;
            break;
        case NativeCreateCopyOutcomeCode::FileSystemSyncFailed:
            if( !destination_is_published || _outcome.error_number == 0 ||
                _outcome.filesystem_sync_error_number != _outcome.error_number ||
                _outcome.filesystem_sync_confirmed )
                return std::unexpected(MappingError::InconsistentOutcome);
            error = OperationJournalItemError::Commit;
            break;
        case NativeCreateCopyOutcomeCode::CleanupFailed: {
            const bool valid_prior = _outcome.prior_code == NativeCreateCopyOutcomeCode::Cancelled ||
                                     NativeCreateCopyOutcomeCodeIsFailure(_outcome.prior_code);
            const bool valid_prior_error = _outcome.prior_code == NativeCreateCopyOutcomeCode::Cancelled
                                               ? _outcome.error_number == 0
                                               : _outcome.error_number != 0;
            if( !valid_prior || !valid_prior_error || !_outcome.recovery_artifact_left ||
                !destination_not_published )
                return std::unexpected(MappingError::InconsistentOutcome);
            error = OperationJournalItemError::Cleanup;
            system_error = 0;
            prior_error = NativeCreateCopyPriorJournalError(_outcome.prior_code);
            prior_system_error = _outcome.error_number;
            // The journal carries no descriptor, temporary name, or inode identity that could
            // authorize removal after the execution capsule has closed its descriptor. Preserve
            // the cleanup failure for manual investigation without advertising an unsafe action.
            recovery = OperationJournalRecoveryAction::None;
            break;
        }
        default:
            return std::unexpected(MappingError::InconsistentOutcome);
    }

    if( status == OperationJournalItemStatus::Failed ) {
        if( _outcome.code != NativeCreateCopyOutcomeCode::CleanupFailed && _outcome.error_number == 0 )
            return std::unexpected(MappingError::InconsistentOutcome);
        if( _outcome.code != NativeCreateCopyOutcomeCode::CleanupFailed &&
            recovery == OperationJournalRecoveryAction::None )
            recovery = destination_is_published || destination_unknown
                           ? OperationJournalRecoveryAction::InspectDestination
                           : OperationJournalRecoveryAction::Retry;
    }

    const auto filesystem_sync_status = destination_is_published
                                            ? (_outcome.filesystem_sync_confirmed
                                                   ? OperationJournalFilesystemSyncStatus::Confirmed
                                                   : OperationJournalFilesystemSyncStatus::Failed)
                                            : OperationJournalFilesystemSyncStatus::NotAttempted;
    const auto destination_publication = destination_not_published
                                             ? OperationJournalPublicationState::NotPublished
                                         : destination_is_published
                                             ? OperationJournalPublicationState::Published
                                             : OperationJournalPublicationState::Unknown;

    return OperationJournalItemResult{.item_index = _item_index,
                                      .status = status,
                                      .error = error,
                                      .system_error = system_error,
                                      .prior_error = prior_error,
                                      .prior_system_error = prior_system_error,
                                      .bytes = _outcome.bytes_copied,
                                      .destination_publication = destination_publication,
                                      .filesystem_sync_status = filesystem_sync_status,
                                      .filesystem_sync_system_error = _outcome.filesystem_sync_error_number,
                                      .recovery_action = recovery};
}

NativeCreateCopyIdentity NativeCreateCopyIdentity::FromStat(const struct stat &_stat) noexcept
{
    return NativeCreateCopyIdentity{
        .device = static_cast<uint64_t>(_stat.st_dev),
        .inode = static_cast<uint64_t>(_stat.st_ino),
        .type_bits = static_cast<uint32_t>(_stat.st_mode & S_IFMT),
        .mode_bits = static_cast<uint32_t>(_stat.st_mode),
        .flags = static_cast<uint32_t>(_stat.st_flags),
        .link_count = static_cast<uint64_t>(_stat.st_nlink),
        .size = static_cast<uint64_t>(_stat.st_size),
        .modification_seconds = _stat.st_mtimespec.tv_sec,
        .modification_nanoseconds = _stat.st_mtimespec.tv_nsec,
        .change_seconds = _stat.st_ctimespec.tv_sec,
        .change_nanoseconds = _stat.st_ctimespec.tv_nsec,
        .birth_seconds = _stat.st_birthtimespec.tv_sec,
        .birth_nanoseconds = _stat.st_birthtimespec.tv_nsec,
    };
}

bool NativeCreateCopyIdentity::MatchesSource(const struct stat &_stat) const noexcept
{
    return *this == FromStat(_stat);
}

bool NativeCreateCopyIdentity::MatchesDirectory(const struct stat &_stat) const noexcept
{
    return device == static_cast<uint64_t>(_stat.st_dev) && inode == static_cast<uint64_t>(_stat.st_ino) &&
           type_bits == static_cast<uint32_t>(_stat.st_mode & S_IFMT) && S_ISDIR(_stat.st_mode);
}

NativeCreateCopyCapsule::NativeCreateCopyCapsule(NativeCreateCopyCapsuleInput _input) noexcept
    : m_Input{std::move(_input)}
{
}

NativeCreateCopyCapsule::NativeCreateCopyCapsule(NativeCreateCopyCapsule &&_other) noexcept
    : m_Input{std::move(_other.m_Input)}
{
    _other.m_Input.source_fd = -1;
    _other.m_Input.destination_parent_fd = -1;
}

NativeCreateCopyCapsule &NativeCreateCopyCapsule::operator=(NativeCreateCopyCapsule &&_other) noexcept
{
    if( this != &_other ) {
        Close();
        m_Input = std::move(_other.m_Input);
        _other.m_Input.source_fd = -1;
        _other.m_Input.destination_parent_fd = -1;
    }
    return *this;
}

NativeCreateCopyCapsule::~NativeCreateCopyCapsule()
{
    Close();
}

void NativeCreateCopyCapsule::Close() noexcept
{
    if( m_Input.source_fd >= 0 ) {
        (void)close(m_Input.source_fd);
        m_Input.source_fd = -1;
    }
    if( m_Input.destination_parent_fd >= 0 ) {
        (void)close(m_Input.destination_parent_fd);
        m_Input.destination_parent_fd = -1;
    }
}

NativeCreateCopyIO::~NativeCreateCopyIO() = default;

int NativeCreateCopyIO::FStat(int _fd, struct stat *_stat) noexcept
{
    return fstat(_fd, _stat);
}

int NativeCreateCopyIO::FStatAt(int _directory_fd, const char *_name, struct stat *_stat, int _flags) noexcept
{
    return fstatat(_directory_fd, _name, _stat, _flags);
}

int NativeCreateCopyIO::OpenAt(int _directory_fd, const char *_name, int _flags, mode_t _mode) noexcept
{
    return openat(_directory_fd, _name, _flags, _mode);
}

std::vector<std::byte> NativeCreateCopyIO::AllocateCopyBuffer(size_t _size)
{
    return std::vector<std::byte>(_size);
}

ssize_t NativeCreateCopyIO::PRead(int _fd, void *_buffer, size_t _size, off_t _offset) noexcept
{
    return pread(_fd, _buffer, _size, _offset);
}

ssize_t NativeCreateCopyIO::Write(int _fd, const void *_buffer, size_t _size) noexcept
{
    return write(_fd, _buffer, _size);
}

int NativeCreateCopyIO::ReadACL(int _fd, std::vector<std::byte> &_acl) noexcept
{
    acl_t acl = acl_get_fd_np(_fd, ACL_TYPE_EXTENDED);
    if( acl == nullptr && errno == ENOENT ) {
        _acl.clear();
        return 0;
    }
    if( acl == nullptr )
        return -1;

    const ssize_t size = acl_size(acl);
    if( size < 0 ) {
        const int error_number = errno;
        (void)acl_free(acl);
        errno = error_number;
        return -1;
    }

    try {
        _acl.resize(static_cast<size_t>(size));
    }
    catch( ... ) {
        (void)acl_free(acl);
        errno = ENOMEM;
        return -1;
    }

    const ssize_t copied = acl_copy_ext(_acl.data(), acl, size);
    const int error_number = copied == size ? 0 : (errno == 0 ? EIO : errno);
    (void)acl_free(acl);
    if( copied != size ) {
        _acl.clear();
        errno = error_number;
        return -1;
    }
    return 0;
}

int NativeCreateCopyIO::WriteACL(int _fd, const std::vector<std::byte> &_acl) noexcept
{
    if( _acl.empty() ) {
        acl_t existing = acl_get_fd_np(_fd, ACL_TYPE_EXTENDED);
        if( existing == nullptr && errno == ENOENT )
            return 0;
        if( existing == nullptr )
            return -1;
        (void)acl_free(existing);
    }

    acl_t acl = _acl.empty() ? acl_init(0) : acl_copy_int(_acl.data());
    if( acl == nullptr )
        return -1;
    int result = 0;
    if( !_acl.empty() )
        result = acl_valid_fd_np(_fd, ACL_TYPE_EXTENDED, acl);
    if( result == 0 )
        result = acl_set_fd_np(_fd, acl, ACL_TYPE_EXTENDED);
    const int error_number = errno;
    (void)acl_free(acl);
    errno = error_number;
    return result;
}

int NativeCreateCopyIO::CopyMetadata(int _source_fd,
                                     int _destination_fd,
                                     copyfile_flags_t _flags) noexcept
{
    return fcopyfile(_source_fd, _destination_fd, nullptr, _flags);
}

int NativeCreateCopyIO::FChmod(int _fd, mode_t _mode) noexcept
{
    return fchmod(_fd, _mode);
}

int NativeCreateCopyIO::FUTimens(int _fd, const struct timespec _times[2]) noexcept
{
    return futimens(_fd, _times);
}

int NativeCreateCopyIO::FSetBirthTime(int _fd, const struct timespec &_birth_time) noexcept
{
    struct attrlist attributes {};
    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    attributes.commonattr = ATTR_CMN_CRTIME;
    auto birth_time = _birth_time;
    return fsetattrlist(_fd, &attributes, &birth_time, sizeof(birth_time), 0);
}

int NativeCreateCopyIO::FChflags(int _fd, uint32_t _flags) noexcept
{
    return fchflags(_fd, _flags);
}

int NativeCreateCopyIO::FSync(int _fd) noexcept
{
    static_assert(g_NativeCreateCopyDurabilityPolicy ==
                      NativeCreateCopyDurabilityPolicy::FileSystemSyncOnly,
                  "Power-loss durability requires fail-closed F_FULLFSYNC support");
    return fsync(_fd);
}

int NativeCreateCopyIO::RenameExclusive(int _directory_fd, const char *_from, const char *_to) noexcept
{
#if defined(__APPLE__)
    return renameatx_np(_directory_fd, _from, _directory_fd, _to, RENAME_EXCL);
#else
    errno = ENOTSUP;
    return -1;
#endif
}

int NativeCreateCopyIO::Close(int _fd) noexcept
{
    return close(_fd);
}

void NativeCreateCopyIO::Checkpoint(NativeCreateCopyCheckpoint)
{
}

NativeCreateCopy::NativeCreateCopy(NativeCreateCopyCapsule _capsule, std::shared_ptr<NativeCreateCopyIO> _io)
{
    const auto source_display_path = _capsule.m_Input.source_display_path;
    m_Job = std::make_unique<NativeCreateCopyJob>(std::move(_capsule), std::move(_io));
    SetTitle("Copying " + source_display_path);
}

NativeCreateCopy::~NativeCreateCopy()
{
    Wait();
}

NativeCreateCopyOutcome NativeCreateCopy::Outcome() const noexcept
{
    return m_Job->Outcome();
}

Job *NativeCreateCopy::GetJob() noexcept
{
    return m_Job.get();
}

} // namespace nc::ops
