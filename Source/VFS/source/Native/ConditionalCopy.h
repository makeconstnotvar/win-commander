// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Utility/NativeFSManager.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace nc::vfs::native {

enum class ConditionalCopyVolumeDisposition : uint8_t {
    Supported,
    UnsupportedFilesystem,
    NonLocal,
    UnsupportedExternalMedia,
    ReadOnly,
    UnknownPermissions,
    CloneUnavailable,
    MetadataUnavailable,
    /** Atomic exclusive rename is missing - a Move-only refusal, since a Copy never asks for it. */
    AtomicRenameUnavailable
};

enum class ConditionalCopyVolumeMedia : uint8_t {
    Internal,
    External
};

struct ConditionalCopyVolumeDecision final {
    ConditionalCopyVolumeDisposition disposition{ConditionalCopyVolumeDisposition::UnsupportedFilesystem};
    ConditionalCopyVolumeMedia media{ConditionalCopyVolumeMedia::External};

    [[nodiscard]] bool IsSupported() const noexcept
    {
        return disposition == ConditionalCopyVolumeDisposition::Supported;
    }

    bool operator==(const ConditionalCopyVolumeDecision &) const noexcept = default;
};

[[nodiscard]] ConditionalCopyVolumeDecision
EvaluateConditionalCopyVolume(const nc::utility::NativeFileSystemInfo &_volume) noexcept;

/** The cross-volume helper keeps the same durability/metadata restrictions but does not require clone support. */
[[nodiscard]] ConditionalCopyVolumeDecision
EvaluateConditionalCopyStagingVolume(const nc::utility::NativeFileSystemInfo &_volume) noexcept;

/**
 * A conditional Move needs every durability and metadata restriction a Copy needs, and one different
 * interface: atomic exclusive rename instead of cloning. Neither eligibility implies the other, so a
 * volume can be usable for exactly one of them and this must be asked separately.
 */
[[nodiscard]] ConditionalCopyVolumeDecision
EvaluateConditionalMoveVolume(const nc::utility::NativeFileSystemInfo &_volume) noexcept;

[[nodiscard]] bool ConditionalCopyVolumesMatch(const nc::utility::NativeFileSystemInfo &_source,
                                               const nc::utility::NativeFileSystemInfo &_destination) noexcept;

struct ConditionalCopyTimestamp final {
    int64_t seconds{0};
    int64_t nanoseconds{0};

    bool operator==(const ConditionalCopyTimestamp &) const noexcept = default;
};

struct ConditionalCopyMetadataSnapshot final {
    uint64_t device{0};
    uint64_t inode{0};
    uint32_t uid{0};
    uint32_t gid{0};
    uint32_t mode{0};
    uint32_t flags{0};
    uint64_t link_count{0};
    uint64_t size{0};
    ConditionalCopyTimestamp access_time;
    ConditionalCopyTimestamp modification_time;
    ConditionalCopyTimestamp change_time;
    ConditionalCopyTimestamp birth_time;
    std::vector<std::byte> acl;
    std::vector<std::pair<std::string, std::vector<std::byte>>> extended_attributes;

    bool operator==(const ConditionalCopyMetadataSnapshot &) const = default;
};

enum class ConditionalCopyMetadataPolicyError : uint8_t {
    UnsupportedOwnership,
    UnsupportedMode,
    UnsupportedFlags,
    DestinationParentACL
};

[[nodiscard]] std::expected<void, ConditionalCopyMetadataPolicyError>
ValidateConditionalCopyMetadataPolicy(const ConditionalCopyMetadataSnapshot &_source,
                                      const ConditionalCopyMetadataSnapshot &_destination_parent) noexcept;

[[nodiscard]] bool ConditionalCopyMetadataMatchesClone(const ConditionalCopyMetadataSnapshot &_source,
                                                       const ConditionalCopyMetadataSnapshot &_destination) noexcept;

// Test-only instrumentation for the physical power-loss harness. Production IO leaves these checkpoints inert.
enum class ConditionalCopyCheckpoint : uint8_t {
    BeforePublish,
    AfterPublishBeforeFullFSync
};

class ConditionalCopyIO
{
public:
    static constexpr size_t MaxACLBytes = 1024 * 1024;
    static constexpr size_t MaxExtendedAttributeNameBytes = 1024 * 1024;
    static constexpr size_t MaxExtendedAttributeValueBytes = 16 * 1024 * 1024;

    virtual ~ConditionalCopyIO();

    virtual int Open(const char *_path, int _flags) noexcept;
    virtual int OpenAt(int _directory_fd, const char *_name, int _flags) noexcept;
    virtual int FStat(int _fd, struct stat *_stat) noexcept;
    virtual int FStatAt(int _directory_fd, const char *_name, struct stat *_stat, int _flags) noexcept;
    virtual int Clone(int _source_fd, int _destination_parent_fd, const char *_name, uint32_t _flags) noexcept;
    /**
     * The Move publication: atomic, and exclusive so it can never replace an existing destination.
     * Unlike Clone it names its source by directory and entry rather than by descriptor - which is the
     * whole reason a Move must anchor the source's parent and re-verify the name against it.
     */
    virtual int RenameExclusive(int _source_parent_fd,
                                const char *_source_name,
                                int _destination_parent_fd,
                                const char *_destination_name) noexcept;
    virtual int FSync(int _fd) noexcept;
    virtual int FullFSync(int _fd) noexcept;
    virtual int Close(int _fd) noexcept;
    virtual void Checkpoint(ConditionalCopyCheckpoint _checkpoint) noexcept { (void)_checkpoint; }

    [[nodiscard]] virtual std::expected<ConditionalCopyMetadataSnapshot, int> CaptureMetadata(int _fd) noexcept;
};

} // namespace nc::vfs::native
