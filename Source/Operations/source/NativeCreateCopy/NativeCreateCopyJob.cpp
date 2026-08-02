// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NativeCreateCopyJob.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <stdlib.h>
#include <unistd.h>
#include <vector>

namespace nc::ops {
namespace {

constexpr size_t g_NativeCreateCopyBufferSize = 256 * 1024;
constexpr uint32_t g_NativeCreateCopyAllowedUserFlags =
    static_cast<uint32_t>(UF_NODUMP | UF_IMMUTABLE | UF_APPEND | UF_HIDDEN);

bool NativeCreateCopySameTime(const struct timespec &_lhs, const struct timespec &_rhs) noexcept
{
    return _lhs.tv_sec == _rhs.tv_sec && _lhs.tv_nsec == _rhs.tv_nsec;
}

bool NativeCreateCopyMetadataUnsupportedError(int _error_number) noexcept
{
    return _error_number == ENOTSUP || _error_number == EOPNOTSUPP;
}

NativeCreateCopyOutcomeCode NativeCreateCopyMetadataOutcomeForError(int _error_number) noexcept
{
    if( NativeCreateCopyMetadataUnsupportedError(_error_number) )
        return NativeCreateCopyOutcomeCode::MetadataUnsupported;
    if( _error_number == EPERM || _error_number == EACCES )
        return NativeCreateCopyOutcomeCode::MetadataPermissionDenied;
    return NativeCreateCopyOutcomeCode::MetadataFailed;
}

std::string NativeCreateCopyRandomTempName()
{
    std::array<unsigned char, 16> random_bytes{};
    arc4random_buf(random_bytes.data(), random_bytes.size());

    constexpr std::array hex{'0', '1', '2', '3', '4', '5', '6', '7',
                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::array<char, random_bytes.size() * 2> encoded{};
    for( size_t index = 0; index < random_bytes.size(); ++index ) {
        encoded[index * 2] = hex[random_bytes[index] >> 4];
        encoded[index * 2 + 1] = hex[random_bytes[index] & 0x0f];
    }

    std::string name = ".wincommander-copy.";
    name.append(encoded.data(), encoded.size());
    name.append(".tmp");
    return name;
}

bool NativeCreateCopyValidBasename(std::string_view _name) noexcept
{
    return !_name.empty() && _name != "." && _name != ".." && _name.find('/') == std::string_view::npos;
}

bool NativeCreateCopyRetryableFStat(NativeCreateCopyIO &_io, int _fd, struct stat &_stat) noexcept
{
    while( _io.FStat(_fd, &_stat) < 0 ) {
        if( errno != EINTR )
            return false;
    }
    return true;
}

bool NativeCreateCopyRetryableFStatAt(NativeCreateCopyIO &_io,
                                      int _directory_fd,
                                      const char *_name,
                                      struct stat &_stat,
                                      int _flags) noexcept
{
    while( _io.FStatAt(_directory_fd, _name, &_stat, _flags) < 0 ) {
        if( errno != EINTR )
            return false;
    }
    return true;
}

int NativeCreateCopyClose(NativeCreateCopyIO &_io, int _fd) noexcept
{
    return _io.Close(_fd);
}

} // namespace

NativeCreateCopyJob::NativeCreateCopyJob(NativeCreateCopyCapsule _capsule,
                                         std::shared_ptr<NativeCreateCopyIO> _io)
    : m_Capsule{std::move(_capsule)}, m_IO{std::move(_io)}
{
    if( !m_IO )
        m_IO = std::make_shared<NativeCreateCopyIO>();
    Statistics().SetPreferredSource(Statistics::SourceType::Bytes);
}

NativeCreateCopyJob::~NativeCreateCopyJob()
{
    if( m_TempFD >= 0 )
        NativeCreateCopyClose(*m_IO, m_TempFD);
}

NativeCreateCopyOutcome NativeCreateCopyJob::Outcome() const noexcept
{
    const auto guard = std::lock_guard{m_OutcomeMutex};
    return m_Outcome;
}

void NativeCreateCopyJob::Perform()
{
    try {
        Statistics().CommitEstimated(Statistics::SourceType::Bytes, m_Capsule.m_Input.size);
        Statistics().CommitEstimated(Statistics::SourceType::Items, 1);

        if( !Checkpoint(NativeCreateCopyCheckpoint::BeforeValidation) )
            return;
        if( !ValidateSource() || !ValidateDestinationParent() || !ValidateDestinationAbsent() ||
            !CaptureSourceMetadata() )
            return;

        // Allocate before creating a namespace entry so allocation failure cannot strand a temp.
        auto buffer = m_IO->AllocateCopyBuffer(g_NativeCreateCopyBufferSize);
        if( buffer.empty() ) {
            Fail(NativeCreateCopyOutcomeCode::WriteFailed, EIO);
            return;
        }

        if( !CreateTemp() )
            return;
        if( !ClearTempACL() )
            return;
        if( !Checkpoint(NativeCreateCopyCheckpoint::AfterTempCreated) )
            return;
        if( !CopyContents(buffer) || !CopyPrepublishMetadata() || !SyncTemp() || !CaptureTempSeal() )
            return;
        if( !Checkpoint(NativeCreateCopyCheckpoint::BeforeSourceRevalidation) )
            return;
        if( !ValidateSource() || !ValidateDestinationParent() )
            return;
        if( !Checkpoint(NativeCreateCopyCheckpoint::BeforePublish) )
            return;
        if( !TryBeginCommit() ) {
            Fail(NativeCreateCopyOutcomeCode::Cancelled);
            return;
        }
        if( !RevalidateTempSeal() || !Publish() )
            return;
        // The commit gate linearizes cancellation before the exclusive rename. Once the gate is held,
        // cancellation is no longer actionable and publication owns the terminal decision.
        m_IO->Checkpoint(NativeCreateCopyCheckpoint::AfterPublish);
        if( !ValidatePublishedDestination() || !ApplyPublishedMetadata() || !VerifyPublishedMetadata() )
            return;
        CompletePublished();
    }
    catch( const std::bad_alloc & ) {
        Fail(NativeCreateCopyOutcomeCode::WriteFailed, ENOMEM);
    }
    catch( ... ) {
        Fail(NativeCreateCopyOutcomeCode::WriteFailed, EIO);
    }
}

bool NativeCreateCopyJob::OnStopRequested() noexcept
{
    const auto guard = std::lock_guard{m_CommitMutex};
    if( m_CommitStarted )
        return false;
    m_StopAccepted = true;
    return true;
}

bool NativeCreateCopyJob::ValidateSource()
{
    if( m_Capsule.m_Input.source_fd < 0 || m_Capsule.m_Input.size != m_Capsule.m_Input.expected_source.size ||
        m_Capsule.m_Input.size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
        m_Capsule.m_Input.expected_source.type_bits != S_IFREG ||
        (m_Capsule.m_Input.mode & 07777) != (m_Capsule.m_Input.expected_source.mode_bits & 07777) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleSource, ESTALE);
        return false;
    }

    struct stat stat{};
    if( !NativeCreateCopyRetryableFStat(*m_IO, m_Capsule.m_Input.source_fd, stat) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleSource, errno);
        return false;
    }
    if( !m_Capsule.m_Input.expected_source.MatchesSource(stat) || !S_ISREG(stat.st_mode) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleSource, ESTALE);
        return false;
    }
    return true;
}

bool NativeCreateCopyJob::ValidateDestinationParent()
{
    if( m_Capsule.m_Input.destination_parent_fd < 0 ||
        !NativeCreateCopyValidBasename(m_Capsule.m_Input.destination_name) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleDestination, ESTALE);
        return false;
    }

    struct stat stat{};
    if( !NativeCreateCopyRetryableFStat(*m_IO, m_Capsule.m_Input.destination_parent_fd, stat) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleDestination, errno);
        return false;
    }
    if( !m_Capsule.m_Input.expected_destination_parent.MatchesDirectory(stat) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleDestination, ESTALE);
        return false;
    }
    return true;
}

bool NativeCreateCopyJob::ValidateDestinationAbsent()
{
    struct stat stat{};
    if( NativeCreateCopyRetryableFStatAt(*m_IO,
                                         m_Capsule.m_Input.destination_parent_fd,
                                         m_Capsule.m_Input.destination_name.c_str(),
                                         stat,
                                         AT_SYMLINK_NOFOLLOW) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleDestination, EEXIST);
        return false;
    }
    if( errno == ENOENT )
        return true;

    Fail(NativeCreateCopyOutcomeCode::CommitFailed, errno);
    return false;
}

bool NativeCreateCopyJob::CaptureSourceMetadata()
{
    struct stat source_stat {};
    if( !NativeCreateCopyRetryableFStat(*m_IO, m_Capsule.m_Input.source_fd, source_stat) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleSource, errno);
        return false;
    }
    if( !m_Capsule.m_Input.expected_source.MatchesSource(source_stat) || !S_ISREG(source_stat.st_mode) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleSource, ESTALE);
        return false;
    }

    // Ownership parity is outside the current regular-file contract. Applying set-ID or sticky
    // bits without the reviewed owner/group would create a different security object.
    if( (source_stat.st_mode & 07000) != 0 ) {
        Fail(NativeCreateCopyOutcomeCode::MetadataUnsupported, ENOTSUP);
        return false;
    }

    const uint32_t source_flags = static_cast<uint32_t>(source_stat.st_flags);
    if( (source_flags & ~g_NativeCreateCopyAllowedUserFlags) != 0 ) {
        Fail(NativeCreateCopyOutcomeCode::MetadataUnsupported, ENOTSUP);
        return false;
    }

    MetadataSnapshot snapshot{
        .mode = source_stat.st_mode,
        .access_time = source_stat.st_atimespec,
        .modification_time = source_stat.st_mtimespec,
        .birth_time = source_stat.st_birthtimespec,
        .flags = source_flags,
    };

    int result;
    do {
        result = m_IO->ReadACL(m_Capsule.m_Input.source_fd, snapshot.acl);
    } while( result < 0 && errno == EINTR );
    if( result < 0 ) {
        const int error_number = errno;
        Fail(NativeCreateCopyMetadataOutcomeForError(error_number), error_number);
        return false;
    }

    struct stat revalidated_stat {};
    if( !NativeCreateCopyRetryableFStat(*m_IO, m_Capsule.m_Input.source_fd, revalidated_stat) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleSource, errno);
        return false;
    }
    if( !m_Capsule.m_Input.expected_source.MatchesSource(revalidated_stat) ||
        static_cast<uint32_t>(revalidated_stat.st_flags) != snapshot.flags ||
        !NativeCreateCopySameTime(revalidated_stat.st_atimespec, snapshot.access_time) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleSource, ESTALE);
        return false;
    }

    snapshot.captured = true;
    m_SourceMetadata = std::move(snapshot);
    return true;
}

bool NativeCreateCopyJob::CreateTemp()
{
    for( unsigned attempt = 0; attempt != 128; ++attempt ) {
        m_TempName = NativeCreateCopyRandomTempName();

        m_TempFD = m_IO->OpenAt(m_Capsule.m_Input.destination_parent_fd,
                                m_TempName.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                S_IRUSR | S_IWUSR);

        if( m_TempFD >= 0 )
            break;
        if( errno != EEXIST ) {
            Fail(NativeCreateCopyOutcomeCode::WriteFailed, errno);
            return false;
        }
    }

    if( m_TempFD < 0 ) {
        Fail(NativeCreateCopyOutcomeCode::WriteFailed, EEXIST);
        return false;
    }

    struct stat stat{};
    if( !NativeCreateCopyRetryableFStat(*m_IO, m_TempFD, stat) ) {
        Fail(NativeCreateCopyOutcomeCode::WriteFailed, errno);
        return false;
    }
    if( !S_ISREG(stat.st_mode) || stat.st_nlink != 1 ) {
        Fail(NativeCreateCopyOutcomeCode::WriteFailed, ESTALE);
        return false;
    }
    m_TempIdentity = NativeCreateCopyIdentity::FromStat(stat);
    return true;
}

bool NativeCreateCopyJob::ClearTempACL()
{
    // The temp has a discoverable name in the destination parent. Keep it owner-only and clear any
    // inherited ACL before data is written. Final source security metadata stays post-publish; a
    // private bounded staging namespace is required before it can be applied safely pre-publish.
    const std::vector<std::byte> empty_acl;
    int result;
    do {
        result = m_IO->WriteACL(m_TempFD, empty_acl);
    } while( result < 0 && errno == EINTR );
    if( result < 0 ) {
        const int error_number = errno;
        Fail(NativeCreateCopyMetadataOutcomeForError(error_number), error_number);
        return false;
    }

    struct stat temp_stat {};
    if( !NativeCreateCopyRetryableFStat(*m_IO, m_TempFD, temp_stat) ) {
        const int error_number = errno;
        Fail(NativeCreateCopyMetadataOutcomeForError(error_number), error_number);
        return false;
    }
    if( !S_ISREG(temp_stat.st_mode) || temp_stat.st_nlink != 1 ||
        (temp_stat.st_mode & 07777) != 0600 || temp_stat.st_flags != 0 ) {
        Fail(NativeCreateCopyOutcomeCode::MetadataVerificationFailed, ESTALE);
        return false;
    }

    std::vector<std::byte> verified_acl;
    do {
        result = m_IO->ReadACL(m_TempFD, verified_acl);
    } while( result < 0 && errno == EINTR );
    if( result < 0 ) {
        const int error_number = errno;
        Fail(NativeCreateCopyMetadataOutcomeForError(error_number), error_number);
        return false;
    }
    if( !verified_acl.empty() ) {
        Fail(NativeCreateCopyOutcomeCode::MetadataVerificationFailed, ESTALE);
        return false;
    }
    return true;
}

bool NativeCreateCopyJob::CopyContents(std::vector<std::byte> &_buffer)
{
    uint64_t read_offset = 0;

    while( read_offset < m_Capsule.m_Input.size ) {
        if( IsStopped() ) {
            Fail(NativeCreateCopyOutcomeCode::Cancelled);
            return false;
        }
        BlockIfPaused();
        if( IsStopped() ) {
            Fail(NativeCreateCopyOutcomeCode::Cancelled);
            return false;
        }

        const auto remaining = m_Capsule.m_Input.size - read_offset;
        const auto requested = static_cast<size_t>(std::min<uint64_t>(remaining, _buffer.size()));
        ssize_t bytes_read;
        do {
            bytes_read = m_IO->PRead(m_Capsule.m_Input.source_fd,
                                     _buffer.data(),
                                     requested,
                                     static_cast<off_t>(read_offset));
        } while( bytes_read < 0 && errno == EINTR );

        if( bytes_read < 0 ) {
            Fail(NativeCreateCopyOutcomeCode::ReadFailed, errno);
            return false;
        }
        if( bytes_read == 0 ) {
            Fail(NativeCreateCopyOutcomeCode::StaleSource, ESTALE);
            return false;
        }

        size_t written = 0;
        while( written < static_cast<size_t>(bytes_read) ) {
            if( IsStopped() ) {
                Fail(NativeCreateCopyOutcomeCode::Cancelled);
                return false;
            }
            BlockIfPaused();
            if( IsStopped() ) {
                Fail(NativeCreateCopyOutcomeCode::Cancelled);
                return false;
            }

            ssize_t bytes_written;
            do {
                bytes_written = m_IO->Write(
                    m_TempFD, _buffer.data() + written, static_cast<size_t>(bytes_read) - written);
            } while( bytes_written < 0 && errno == EINTR );

            if( bytes_written < 0 ) {
                Fail(NativeCreateCopyOutcomeCode::WriteFailed, errno);
                return false;
            }
            if( bytes_written == 0 ) {
                Fail(NativeCreateCopyOutcomeCode::WriteFailed, EIO);
                return false;
            }
            written += static_cast<size_t>(bytes_written);
            m_BytesCopied += static_cast<uint64_t>(bytes_written);
            Statistics().CommitProcessed(Statistics::SourceType::Bytes, static_cast<uint64_t>(bytes_written));
        }
        read_offset += static_cast<uint64_t>(bytes_read);
    }

    std::byte extra{};
    ssize_t extra_read;
    do {
        extra_read = m_IO->PRead(m_Capsule.m_Input.source_fd,
                                 &extra,
                                 1,
                                 static_cast<off_t>(m_Capsule.m_Input.size));
    } while( extra_read < 0 && errno == EINTR );
    if( extra_read < 0 ) {
        Fail(NativeCreateCopyOutcomeCode::ReadFailed, errno);
        return false;
    }
    if( extra_read != 0 ) {
        Fail(NativeCreateCopyOutcomeCode::StaleSource, ESTALE);
        return false;
    }
    return true;
}

bool NativeCreateCopyJob::CopyPrepublishMetadata()
{
    int result;
    do {
        result = m_IO->CopyMetadata(m_Capsule.m_Input.source_fd, m_TempFD, COPYFILE_XATTR);
    } while( result < 0 && errno == EINTR );
    if( result < 0 ) {
        const int error_number = errno;
        Fail(NativeCreateCopyMetadataOutcomeForError(error_number), error_number);
        return false;
    }
    return true;
}

bool NativeCreateCopyJob::ApplyPublishedMetadata()
{
    if( !m_SourceMetadata.captured ) {
        Fail(NativeCreateCopyOutcomeCode::MetadataVerificationFailed, ESTALE);
        return false;
    }

    int result;
    do {
        result = m_IO->WriteACL(m_TempFD, m_SourceMetadata.acl);
    } while( result < 0 && errno == EINTR );
    if( result < 0 ) {
        const int error_number = errno;
        Fail(NativeCreateCopyMetadataOutcomeForError(error_number), error_number);
        return false;
    }

    do {
        result = m_IO->FChmod(m_TempFD, m_SourceMetadata.mode & 0777);
    } while( result < 0 && errno == EINTR );
    if( result < 0 ) {
        const int error_number = errno;
        Fail(NativeCreateCopyMetadataOutcomeForError(error_number), error_number);
        return false;
    }

    const struct timespec times[2]{m_SourceMetadata.access_time, m_SourceMetadata.modification_time};
    do {
        result = m_IO->FUTimens(m_TempFD, times);
    } while( result < 0 && errno == EINTR );
    if( result < 0 ) {
        const int error_number = errno;
        Fail(NativeCreateCopyMetadataOutcomeForError(error_number), error_number);
        return false;
    }

    do {
        result = m_IO->FSetBirthTime(m_TempFD, m_SourceMetadata.birth_time);
    } while( result < 0 && errno == EINTR );
    if( result < 0 ) {
        const int error_number = errno;
        Fail(NativeCreateCopyMetadataOutcomeForError(error_number), error_number);
        return false;
    }

    do {
        result = m_IO->FChflags(m_TempFD, m_SourceMetadata.flags);
    } while( result < 0 && errno == EINTR );
    if( result < 0 ) {
        const int error_number = errno;
        Fail(NativeCreateCopyMetadataOutcomeForError(error_number), error_number);
        return false;
    }
    return true;
}

bool NativeCreateCopyJob::VerifyPublishedMetadata()
{
    struct stat destination_stat {};
    if( !NativeCreateCopyRetryableFStat(*m_IO, m_TempFD, destination_stat) ) {
        const int error_number = errno;
        Fail(NativeCreateCopyMetadataOutcomeForError(error_number), error_number);
        return false;
    }
    if( !S_ISREG(destination_stat.st_mode) ||
        static_cast<uint64_t>(destination_stat.st_size) != m_Capsule.m_Input.size ||
        (destination_stat.st_mode & 0777) != (m_SourceMetadata.mode & 0777) ||
        !NativeCreateCopySameTime(destination_stat.st_atimespec, m_SourceMetadata.access_time) ||
        !NativeCreateCopySameTime(destination_stat.st_mtimespec, m_SourceMetadata.modification_time) ||
        !NativeCreateCopySameTime(destination_stat.st_birthtimespec, m_SourceMetadata.birth_time) ||
        static_cast<uint32_t>(destination_stat.st_flags) != m_SourceMetadata.flags ) {
        Fail(NativeCreateCopyOutcomeCode::MetadataVerificationFailed, ESTALE);
        return false;
    }

    std::vector<std::byte> destination_acl;
    int result;
    do {
        result = m_IO->ReadACL(m_TempFD, destination_acl);
    } while( result < 0 && errno == EINTR );
    if( result < 0 ) {
        const int error_number = errno;
        Fail(NativeCreateCopyMetadataOutcomeForError(error_number), error_number);
        return false;
    }
    if( destination_acl != m_SourceMetadata.acl ) {
        Fail(NativeCreateCopyOutcomeCode::MetadataVerificationFailed, ESTALE);
        return false;
    }
    return true;
}

bool NativeCreateCopyJob::SyncTemp()
{
    int result;
    do {
        result = m_IO->FSync(m_TempFD);
    } while( result < 0 && errno == EINTR );
    if( result < 0 ) {
        Fail(NativeCreateCopyOutcomeCode::SyncFailed, errno);
        return false;
    }
    return true;
}

bool NativeCreateCopyJob::CaptureTempSeal()
{
    struct stat stat {};
    if( !NativeCreateCopyRetryableFStat(*m_IO, m_TempFD, stat) ) {
        Fail(NativeCreateCopyOutcomeCode::CommitFailed, errno);
        return false;
    }
    if( !S_ISREG(stat.st_mode) || stat.st_nlink != 1 ||
        static_cast<uint64_t>(stat.st_size) != m_Capsule.m_Input.size ) {
        Fail(NativeCreateCopyOutcomeCode::CommitFailed, ESTALE);
        return false;
    }
    m_TempSeal = NativeCreateCopyIdentity::FromStat(stat);
    m_TempSealValid = true;
    return true;
}

bool NativeCreateCopyJob::RevalidateTempSeal()
{
    if( !m_TempSealValid ) {
        Fail(NativeCreateCopyOutcomeCode::CommitFailed, ESTALE);
        return false;
    }

    struct stat named_stat {};
    if( !NativeCreateCopyRetryableFStatAt(*m_IO,
                                         m_Capsule.m_Input.destination_parent_fd,
                                         m_TempName.c_str(),
                                         named_stat,
                                         AT_SYMLINK_NOFOLLOW) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleDestination, errno);
        return false;
    }
    if( !S_ISREG(named_stat.st_mode) || named_stat.st_nlink != 1 ||
        !m_TempSeal.MatchesSource(named_stat) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleDestination, ESTALE);
        return false;
    }

    struct stat descriptor_stat {};
    if( !NativeCreateCopyRetryableFStat(*m_IO, m_TempFD, descriptor_stat) ) {
        Fail(NativeCreateCopyOutcomeCode::CommitFailed, errno);
        return false;
    }
    if( !S_ISREG(descriptor_stat.st_mode) || descriptor_stat.st_nlink != 1 ||
        !m_TempSeal.MatchesSource(descriptor_stat) ) {
        Fail(NativeCreateCopyOutcomeCode::CommitFailed, ESTALE);
        return false;
    }
    // POSIX exposes validation and rename as separate namespace operations. A same-UID actor can
    // still replace this entry after the check and before renameatx_np; the provider must supply a
    // conditional commit primitive before this operation can be connected to production consumers.
    return true;
}

bool NativeCreateCopyJob::TryBeginCommit() noexcept
{
    const auto guard = std::lock_guard{m_CommitMutex};
    if( m_StopAccepted )
        return false;
    m_CommitStarted = true;
    return true;
}

void NativeCreateCopyJob::ReleaseCommitForFailure() noexcept
{
    const auto guard = std::lock_guard{m_CommitMutex};
    m_CommitStarted = false;
}

bool NativeCreateCopyJob::Publish()
{
    const int result = m_IO->RenameExclusive(m_Capsule.m_Input.destination_parent_fd,
                                             m_TempName.c_str(),
                                             m_Capsule.m_Input.destination_name.c_str());
    if( result < 0 ) {
        const int error_number = errno;
        if( error_number == EINTR ) {
            struct stat destination_stat {};
            if( NativeCreateCopyRetryableFStatAt(*m_IO,
                                                 m_Capsule.m_Input.destination_parent_fd,
                                                 m_Capsule.m_Input.destination_name.c_str(),
                                                 destination_stat,
                                                 AT_SYMLINK_NOFOLLOW) &&
                static_cast<uint64_t>(destination_stat.st_dev) == m_TempIdentity.device &&
                static_cast<uint64_t>(destination_stat.st_ino) == m_TempIdentity.inode ) {
                m_Published = true;
                m_TempName.clear();
                return true;
            }
        }
        Fail(error_number == EEXIST || error_number == ENOTEMPTY
                 ? NativeCreateCopyOutcomeCode::StaleDestination
                 : NativeCreateCopyOutcomeCode::CommitFailed,
             error_number);
        return false;
    }
    m_Published = true;
    m_TempName.clear();
    return true;
}

bool NativeCreateCopyJob::ValidatePublishedDestination()
{
    struct stat descriptor_stat {};
    if( !NativeCreateCopyRetryableFStat(*m_IO, m_TempFD, descriptor_stat) ) {
        Fail(NativeCreateCopyOutcomeCode::CommitFailed, errno);
        return false;
    }
    const auto descriptor_identity = NativeCreateCopyIdentity::FromStat(descriptor_stat);
    if( descriptor_identity.device != m_TempIdentity.device || descriptor_identity.inode != m_TempIdentity.inode ||
        descriptor_stat.st_nlink != 1 ) {
        Fail(NativeCreateCopyOutcomeCode::CommitFailed, ESTALE);
        return false;
    }

    struct stat named_stat {};
    if( !NativeCreateCopyRetryableFStatAt(*m_IO,
                                         m_Capsule.m_Input.destination_parent_fd,
                                         m_Capsule.m_Input.destination_name.c_str(),
                                         named_stat,
                                         AT_SYMLINK_NOFOLLOW) ) {
        Fail(NativeCreateCopyOutcomeCode::StaleDestination, errno);
        return false;
    }
    if( static_cast<uint64_t>(named_stat.st_dev) != m_TempIdentity.device ||
        static_cast<uint64_t>(named_stat.st_ino) != m_TempIdentity.inode || !S_ISREG(named_stat.st_mode) ||
        named_stat.st_nlink != 1 ) {
        Fail(NativeCreateCopyOutcomeCode::StaleDestination, ESTALE);
        return false;
    }

    return true;
}

void NativeCreateCopyJob::ClosePublishedDescriptor() noexcept
{
    if( m_TempFD >= 0 ) {
        (void)NativeCreateCopyClose(*m_IO, m_TempFD);
        m_TempFD = -1;
    }
}

NativeCreateCopyJob::FileSystemSyncResult NativeCreateCopyJob::FinalizePublishedStorage() noexcept
{
    FileSystemSyncResult filesystem_sync;

    if( m_TempFD >= 0 ) {
        int file_sync_result;
        do {
            file_sync_result = m_IO->FSync(m_TempFD);
        } while( file_sync_result < 0 && errno == EINTR );
        if( file_sync_result < 0 )
            filesystem_sync.error_number = errno;

        const int close_result = NativeCreateCopyClose(*m_IO, m_TempFD);
        m_TempFD = -1;
        if( close_result < 0 && filesystem_sync.error_number == 0 )
            filesystem_sync.error_number = errno;
    }

    int parent_sync_result;
    do {
        parent_sync_result = m_IO->FSync(m_Capsule.m_Input.destination_parent_fd);
    } while( parent_sync_result < 0 && errno == EINTR );
    if( parent_sync_result < 0 && filesystem_sync.error_number == 0 )
        filesystem_sync.error_number = errno;

    filesystem_sync.confirmed = filesystem_sync.error_number == 0;
    return filesystem_sync;
}

void NativeCreateCopyJob::CompletePublished()
{
    const auto filesystem_sync = FinalizePublishedStorage();
    if( !filesystem_sync.confirmed ) {
        ReleaseCommitForFailure();
        StoreOutcome(NativeCreateCopyOutcome{
            .code = NativeCreateCopyOutcomeCode::FileSystemSyncFailed,
            .prior_code = NativeCreateCopyOutcomeCode::Pending,
            .error_number = filesystem_sync.error_number,
            .bytes_copied = m_BytesCopied,
            .destination_publication = NativeCreateCopyPublicationState::Published,
            .recovery_artifact_left = false,
            .filesystem_sync_error_number = filesystem_sync.error_number,
            .filesystem_sync_confirmed = false,
        });
        Stop();
        return;
    }

    Statistics().CommitProcessed(Statistics::SourceType::Items, 1);
    Succeed();
}

bool NativeCreateCopyJob::Checkpoint(NativeCreateCopyCheckpoint _checkpoint)
{
    if( IsStopped() ) {
        Fail(NativeCreateCopyOutcomeCode::Cancelled);
        return false;
    }
    BlockIfPaused();
    if( IsStopped() ) {
        Fail(NativeCreateCopyOutcomeCode::Cancelled);
        return false;
    }
    m_IO->Checkpoint(_checkpoint);
    if( IsStopped() ) {
        Fail(NativeCreateCopyOutcomeCode::Cancelled);
        return false;
    }
    return true;
}

void NativeCreateCopyJob::Fail(NativeCreateCopyOutcomeCode _code, int _error_number)
{
    // A failed rename or a post-publication durability check needs the ordinary stopped terminal
    // state. Release the commit gate before the internal Stop() request reaches OnStopRequested().
    ReleaseCommitForFailure();
    NativeCreateCopyOutcome outcome{
        .code = _code,
        .prior_code = NativeCreateCopyOutcomeCode::Pending,
        .error_number = _error_number,
        .bytes_copied = m_BytesCopied,
        .destination_publication = m_Published ? NativeCreateCopyPublicationState::Published
                                               : NativeCreateCopyPublicationState::NotPublished,
        .recovery_artifact_left = false,
    };
    if( m_Published ) {
        const auto filesystem_sync = FinalizePublishedStorage();
        outcome.filesystem_sync_error_number = filesystem_sync.error_number;
        outcome.filesystem_sync_confirmed = filesystem_sync.confirmed;
    }
    else if( AbandonTempForRecovery() ) {
        outcome.prior_code = outcome.code;
        outcome.code = NativeCreateCopyOutcomeCode::CleanupFailed;
        outcome.recovery_artifact_left = true;
    }
    StoreOutcome(outcome);
    Stop();
}

void NativeCreateCopyJob::Succeed()
{
    StoreOutcome(NativeCreateCopyOutcome{
        .code = NativeCreateCopyOutcomeCode::Success,
        .prior_code = NativeCreateCopyOutcomeCode::Pending,
        .error_number = 0,
        .bytes_copied = m_BytesCopied,
        .destination_publication = NativeCreateCopyPublicationState::Published,
        .recovery_artifact_left = false,
        .filesystem_sync_error_number = 0,
        .filesystem_sync_confirmed = true,
    });
    // A stop racing after the exclusive rename cannot roll the committed file back. Make the
    // externally visible state agree with the typed successful outcome at that commit boundary.
    SetCompleted();
}

bool NativeCreateCopyJob::AbandonTempForRecovery() noexcept
{
    if( m_Published ) {
        ClosePublishedDescriptor();
        return false;
    }
    if( m_TempFD < 0 || m_TempName.empty() )
        return false;

    // POSIX has no unlink-by-descriptor primitive. Resolving the generated name and unlinking it
    // would leave a substitution window that can delete an unrelated entry. The production path
    // therefore performs no namespace mutation after failure. It closes the owned descriptor and
    // reports a recovery artifact only when that descriptor proves the owned inode remains linked.
    try {
        m_IO->Checkpoint(NativeCreateCopyCheckpoint::BeforeRecoveryAbandon);
    }
    catch( ... ) {
    }

    struct stat descriptor_stat {};
    const bool linked_artifact_left =
        NativeCreateCopyRetryableFStat(*m_IO, m_TempFD, descriptor_stat) && descriptor_stat.st_nlink > 0;
    if( m_TempFD >= 0 ) {
        (void)NativeCreateCopyClose(*m_IO, m_TempFD);
        m_TempFD = -1;
    }
    return linked_artifact_left;
}

void NativeCreateCopyJob::StoreOutcome(NativeCreateCopyOutcome _outcome) noexcept
{
    const auto guard = std::lock_guard{m_OutcomeMutex};
    m_Outcome = _outcome;
}

} // namespace nc::ops
