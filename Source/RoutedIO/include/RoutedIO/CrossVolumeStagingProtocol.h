// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace nc::routedio::cross_volume_staging {

inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr size_t kCorrelationBytes = 16;
inline constexpr size_t kLeaseTokenBytes = 32;
inline constexpr size_t kMaximumDestinationComponentBytes = 255;

using CorrelationID = std::array<uint8_t, kCorrelationBytes>;

enum class ValidationError : uint8_t;

/** A versioned correlation header present on every helper request and reply. */
struct Header final {
    uint32_t version{kProtocolVersion};
    CorrelationID correlation{};

    bool operator==(const Header &) const noexcept = default;
};

struct Timestamp final {
    int64_t seconds{0};
    uint32_t nanoseconds{0};

    bool operator==(const Timestamp &) const noexcept = default;
};

/**
 * Complete scalar descriptor seal.  The XPC codec carries two duplicated descriptor rights beside a BeginRequest;
 * it never accepts a source, destination-parent, stage or cleanup pathname.
 */
struct ObjectSeal final {
    uint64_t device{0};
    uint64_t inode{0};
    uint32_t uid{0};
    uint32_t gid{0};
    uint32_t mode{0};
    uint32_t flags{0};
    uint64_t link_count{0};
    uint64_t byte_size{0};
    Timestamp birth_time;
    Timestamp modification_time;
    Timestamp status_change_time;

    bool operator==(const ObjectSeal &) const noexcept = default;
};

/**
 * One APFS destination component represented as bytes rather than a Unicode string.  It cannot encode a path,
 * dot component or embedded NUL; the helper owns every staging and retained-artifact name.
 */
class DestinationComponent final
{
public:
    [[nodiscard]] static std::expected<DestinationComponent, ValidationError>
    Create(std::span<const uint8_t> _bytes) noexcept;

    [[nodiscard]] std::span<const uint8_t> Bytes() const noexcept
    {
        return std::span<const uint8_t>{m_Bytes.data(), m_Size};
    }

    bool operator==(const DestinationComponent &) const noexcept = default;

private:
    std::array<uint8_t, kMaximumDestinationComponentBytes> m_Bytes{};
    uint16_t m_Size{0};
};

/** Descriptor rights are deliberately transported outside this value type by the private XPC codec. */
struct BeginRequest final {
    Header header;
    ObjectSeal source;
    ObjectSeal destination_parent;
    DestinationComponent destination_name;

    bool operator==(const BeginRequest &) const noexcept = default;
};

struct LeaseToken final {
    std::array<uint8_t, kLeaseTokenBytes> bytes{};

    bool operator==(const LeaseToken &) const noexcept = default;
};

/** Helper-minted, one-use and opaque authority.  It contains no retained artifact identifier or pathname. */
struct Lease final {
    Header header;
    LeaseToken token;

    bool operator==(const Lease &) const noexcept = default;
};

struct CommitRequest final {
    Header header;
    Lease lease;

    bool operator==(const CommitRequest &) const noexcept = default;
};

struct AbortRequest final {
    Header header;
    Lease lease;

    bool operator==(const AbortRequest &) const noexcept = default;
};

enum class BeginDisposition : uint8_t {
    Granted,
    Rejected
};

enum class BeginFailure : uint8_t {
    None,
    Unsupported,
    InvalidRequest,
    SourceStale,
    DestinationParentStale,
    DestinationExists,
    Cancelled,
    HelperFailure
};

struct BeginResult final {
    Header header;
    BeginDisposition disposition{BeginDisposition::Rejected};
    BeginFailure failure{BeginFailure::HelperFailure};
    Lease lease{};

    bool operator==(const BeginResult &) const noexcept = default;
};

enum class Publication : uint8_t {
    NotPublished,
    Published,
    Unknown
};

enum class CompletionFailure : uint8_t {
    None,
    Aborted,
    Cancelled,
    SourceStale,
    DestinationParentStale,
    DestinationExists,
    MetadataFailed,
    FileSystemSyncFailed,
    HelperFailure
};

enum class FilesystemSync : uint8_t {
    NotAttempted,
    Confirmed,
    Failed
};

/** Conservative reply that maps losslessly into the existing provider tri-state result. */
struct CompletionResult final {
    Header header;
    Publication publication{Publication::Unknown};
    CompletionFailure failure{CompletionFailure::HelperFailure};
    int32_t system_error{0};
    FilesystemSync filesystem_sync{FilesystemSync::NotAttempted};
    int32_t filesystem_sync_system_error{0};

    bool operator==(const CompletionResult &) const noexcept = default;
};

enum class ValidationError : uint8_t {
    UnsupportedVersion,
    InvalidCorrelation,
    InvalidLease,
    InvalidDestinationComponent,
    InvalidTimestamp,
    InvalidSourceSeal,
    InvalidDestinationParentSeal,
    InconsistentLease,
    InconsistentBeginResult,
    InconsistentCompletionResult
};

[[nodiscard]] std::expected<void, ValidationError> Validate(const Header &_header) noexcept;
[[nodiscard]] std::expected<void, ValidationError> Validate(const BeginRequest &_request) noexcept;
[[nodiscard]] std::expected<void, ValidationError> Validate(const Lease &_lease) noexcept;
[[nodiscard]] std::expected<void, ValidationError> Validate(const CommitRequest &_request) noexcept;
[[nodiscard]] std::expected<void, ValidationError> Validate(const AbortRequest &_request) noexcept;
[[nodiscard]] std::expected<void, ValidationError> Validate(const BeginResult &_result) noexcept;
[[nodiscard]] std::expected<void, ValidationError> Validate(const CompletionResult &_result) noexcept;

} // namespace nc::routedio::cross_volume_staging
