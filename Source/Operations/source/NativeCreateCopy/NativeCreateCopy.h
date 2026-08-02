// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "../Operation.h"
#include "../OperationJournal.h"

#include <copyfile.h>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

namespace nc::ops {

enum class NativeCreateCopyOutcomeCode : uint8_t {
    Pending,
    Success,
    Cancelled,
    StaleSource,
    StaleDestination,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    MetadataFailed,
    MetadataUnsupported,
    MetadataPermissionDenied,
    MetadataVerificationFailed,
    CommitFailed,
    FileSystemSyncFailed,
    CleanupFailed
};

enum class NativeCreateCopyPublicationState : uint8_t {
    NotPublished,
    Published,
    Unknown
};

struct NativeCreateCopyOutcome final {
    NativeCreateCopyOutcomeCode code{NativeCreateCopyOutcomeCode::Pending};
    NativeCreateCopyOutcomeCode prior_code{NativeCreateCopyOutcomeCode::Pending};
    int error_number{0};
    uint64_t bytes_copied{0};
    NativeCreateCopyPublicationState destination_publication{
        NativeCreateCopyPublicationState::NotPublished};
    bool recovery_artifact_left{false};
    int filesystem_sync_error_number{0};
    bool filesystem_sync_confirmed{false};

    bool operator==(const NativeCreateCopyOutcome &) const = default;
};

enum class NativeCreateCopyJournalMappingError : uint8_t {
    PendingOutcome,
    InconsistentOutcome
};

[[nodiscard]] std::expected<OperationJournalItemResult, NativeCreateCopyJournalMappingError>
MapNativeCreateCopyOutcomeToJournalItemResult(const NativeCreateCopyOutcome &_outcome,
                                              size_t _item_index) noexcept;

struct NativeCreateCopyIdentity final {
    uint64_t device{0};
    uint64_t inode{0};
    uint32_t type_bits{0};
    uint32_t mode_bits{0};
    uint32_t flags{0};
    uint64_t link_count{0};
    uint64_t size{0};
    int64_t modification_seconds{0};
    int64_t modification_nanoseconds{0};
    int64_t change_seconds{0};
    int64_t change_nanoseconds{0};
    int64_t birth_seconds{0};
    int64_t birth_nanoseconds{0};

    static NativeCreateCopyIdentity FromStat(const struct stat &_stat) noexcept;
    bool MatchesSource(const struct stat &_stat) const noexcept;
    bool MatchesDirectory(const struct stat &_stat) const noexcept;

    bool operator==(const NativeCreateCopyIdentity &) const = default;
};

struct NativeCreateCopyCapsuleInput final {
    int source_fd{-1};
    NativeCreateCopyIdentity expected_source;
    int destination_parent_fd{-1};
    NativeCreateCopyIdentity expected_destination_parent;
    std::string source_display_path;
    std::string destination_display_path;
    std::string destination_name;
    mode_t mode{0};
    uint64_t size{0};
};

// Owns already-resolved descriptors. The execution job never resolves either display path.
class NativeCreateCopyCapsule final
{
public:
    explicit NativeCreateCopyCapsule(NativeCreateCopyCapsuleInput _input) noexcept;
    NativeCreateCopyCapsule(NativeCreateCopyCapsule &&_other) noexcept;
    NativeCreateCopyCapsule &operator=(NativeCreateCopyCapsule &&_other) noexcept;
    NativeCreateCopyCapsule(const NativeCreateCopyCapsule &) = delete;
    NativeCreateCopyCapsule &operator=(const NativeCreateCopyCapsule &) = delete;
    ~NativeCreateCopyCapsule();

private:
    friend class NativeCreateCopy;
    friend class NativeCreateCopyJob;

    void Close() noexcept;

    NativeCreateCopyCapsuleInput m_Input;
};

enum class NativeCreateCopyCheckpoint : uint8_t {
    BeforeValidation,
    AfterTempCreated,
    BeforeSourceRevalidation,
    BeforePublish,
    AfterPublish,
    BeforeRecoveryAbandon
};

enum class NativeCreateCopyDurabilityPolicy : uint8_t {
    FileSystemSyncOnly,
    PowerLossDurable
};

// This slice promises fsync(2) filesystem semantics only. Switching to a power-loss guarantee is
// intentionally a compile-time decision and requires a fail-closed F_FULLFSYNC implementation.
inline constexpr auto g_NativeCreateCopyDurabilityPolicy =
    NativeCreateCopyDurabilityPolicy::FileSystemSyncOnly;

// Narrow syscall seam. Production uses these POSIX implementations; tests override only
// the calls or checkpoints needed to reproduce an execution race or I/O edge case.
class NativeCreateCopyIO
{
public:
    virtual ~NativeCreateCopyIO();

    virtual int FStat(int _fd, struct stat *_stat) noexcept;
    virtual int FStatAt(int _directory_fd, const char *_name, struct stat *_stat, int _flags) noexcept;
    virtual int OpenAt(int _directory_fd, const char *_name, int _flags, mode_t _mode) noexcept;
    virtual std::vector<std::byte> AllocateCopyBuffer(size_t _size);
    virtual ssize_t PRead(int _fd, void *_buffer, size_t _size, off_t _offset) noexcept;
    virtual ssize_t Write(int _fd, const void *_buffer, size_t _size) noexcept;
    virtual int ReadACL(int _fd, std::vector<std::byte> &_acl) noexcept;
    virtual int WriteACL(int _fd, const std::vector<std::byte> &_acl) noexcept;
    virtual int CopyMetadata(int _source_fd,
                             int _destination_fd,
                             copyfile_flags_t _flags) noexcept;
    virtual int FChmod(int _fd, mode_t _mode) noexcept;
    virtual int FUTimens(int _fd, const struct timespec _times[2]) noexcept;
    virtual int FSetBirthTime(int _fd, const struct timespec &_birth_time) noexcept;
    virtual int FChflags(int _fd, uint32_t _flags) noexcept;
    virtual int FSync(int _fd) noexcept;
    virtual int RenameExclusive(int _directory_fd, const char *_from, const char *_to) noexcept;
    virtual int Close(int _fd) noexcept;
    virtual void Checkpoint(NativeCreateCopyCheckpoint _checkpoint);
};

class NativeCreateCopyJob;

// A create-only regular-file copy constructed by ReviewedOperationFactory. It remains isolated
// from production mutation consumers and Pool adoption; execution consumes only the anchored
// immutable capsule above.
class NativeCreateCopy final : public Operation
{
public:
    explicit NativeCreateCopy(NativeCreateCopyCapsule _capsule,
                              std::shared_ptr<NativeCreateCopyIO> _io = {});
    ~NativeCreateCopy() override;

    NativeCreateCopyOutcome Outcome() const noexcept;

private:
    Job *GetJob() noexcept override;

    std::unique_ptr<NativeCreateCopyJob> m_Job;
};

} // namespace nc::ops
