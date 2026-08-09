// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace nc::ops {
class ReviewedVFSOperationPreflight;
}

namespace nc::vfs {

class Host;
struct ProviderConditionalCopyTransactionTestAccess;

/**
 * A conservative projection of provider functionality used by higher-level command validation.
 * Operation capabilities require explicit HostFeatures declarations; host traits remain direct queries.
 */
struct ProviderCapabilities {
    bool can_read = false;
    bool can_write = false;
    bool can_create_file = false;
    bool can_create_folder = false;
    bool can_create_symlink = false;
    bool can_rename = false;
    bool can_delete_permanently = false;
    bool can_trash = false;
    bool can_watch_changes = false;
    bool can_generate_thumbnails = false;
    bool can_resolve_symlink = false;
    bool can_set_permissions = false;
    bool can_set_owner_group = false;
    bool can_set_times = false;
    bool is_native = false;
    bool is_immutable = false;
    bool is_case_sensitive = true;

    bool operator==(const ProviderCapabilities &) const noexcept = default;
};

struct ProviderConditionalCopyTimestamp final {
    int64_t seconds{0};
    int64_t nanoseconds{0};

    bool operator==(const ProviderConditionalCopyTimestamp &) const noexcept = default;
};

enum class ProviderConditionalCopyExpectedKind : uint8_t {
    RegularFile,
    Directory
};

/**
 * How strictly a Commit must find the expected object unchanged.
 *
 * `Exact` is the only mode a single reviewed item ever needs: nothing but this transaction should be
 * touching what it names between review and publication. `MonotonicGrowth` exists for one case only -
 * a destination-parent directory that an authorized batch is itself publishing into. Its own earlier
 * items legitimately advance the parent's size and timestamps; that is not staleness, it is the batch
 * proving its own prior work. Identity (device, inode, birth time) and the whole ownership/permission
 * surface (mode, uid, gid, BSD flags, ACL, extended attributes) still fail closed on any change under
 * either mode. Exactly four fields may advance instead of matching: byte size, the two content-derived
 * timestamps, and link_count - the last because APFS advances a directory's link_count for a
 * regular-file child too, not only for a subdirectory, which is a fact about the filesystem confirmed
 * by running it rather than derived from POSIX convention.
 */
enum class ProviderConditionalCopyExpectationTolerance : uint8_t {
    Exact,
    MonotonicGrowth
};

/** Preliminary, read-only provider eligibility for an exact source and destination parent. */
enum class ProviderConditionalCopyPathSupport : uint8_t {
    SameVolumeClone,
    CrossVolumeStaged,
    Unsupported,
    Unavailable
};

/**
 * The same preliminary question asked of a Move, and it has one fewer answer than Copy on purpose.
 *
 * There is no cross-volume case here, and its absence is a decision rather than an omission. A
 * same-volume Move is one indivisible operation: the destination appears and the source ceases to
 * exist together, so a result saying the destination was published says everything there is to say. A
 * cross-volume Move is copy-then-unlink - two separate events - and the journal's item result has no
 * field that could record "published, and the source is still there". That shape is therefore not
 * merely unimplemented but unrepresentable, and answering `Unsupported` for it is the truth.
 */
enum class ProviderConditionalMovePathSupport : uint8_t {
    SameVolumeRename,
    Unsupported,
    Unavailable
};

struct ProviderConditionalCopyBinding final {
    std::string provider_id;
    std::shared_ptr<Host> host;

    bool operator==(const ProviderConditionalCopyBinding &) const noexcept = default;
};

struct ProviderConditionalCopyExistingExpectation final {
    std::string absolute_path;
    ProviderConditionalCopyExpectedKind kind{ProviderConditionalCopyExpectedKind::RegularFile};
    int32_t device{0};
    uint64_t inode{0};
    ProviderConditionalCopyTimestamp birth_time;
    uint16_t mode{0};
    uint64_t byte_size{0};
    ProviderConditionalCopyTimestamp modification_time;
    ProviderConditionalCopyTimestamp status_change_time;
    ProviderConditionalCopyExpectationTolerance tolerance{ProviderConditionalCopyExpectationTolerance::Exact};

    bool operator==(const ProviderConditionalCopyExistingExpectation &) const noexcept = default;
};

struct ProviderConditionalCopyMissingExpectation final {
    std::string absolute_path;

    bool operator==(const ProviderConditionalCopyMissingExpectation &) const noexcept = default;
};

/** Exact immutable claims derived from one reviewed create-only Copy intent. */
struct ProviderConditionalCopyReviewedClaims final {
    std::string plan_id;
    ProviderConditionalCopyBinding source_binding;
    ProviderConditionalCopyBinding destination_binding;
    ProviderConditionalCopyExistingExpectation source;
    ProviderConditionalCopyExistingExpectation destination_parent;
    ProviderConditionalCopyMissingExpectation destination;

    bool operator==(const ProviderConditionalCopyReviewedClaims &) const noexcept = default;
};

/**
 * Move-only, non-forgeable reviewed authority. Production construction is restricted to the
 * consumed ReviewedVFSOperationPreflight boundary; providers receive only its immutable exact claims.
 */
class ProviderConditionalCopyReviewedAuthority final
{
public:
    ProviderConditionalCopyReviewedAuthority() = delete;
    ProviderConditionalCopyReviewedAuthority(const ProviderConditionalCopyReviewedAuthority &) = delete;
    ProviderConditionalCopyReviewedAuthority &operator=(const ProviderConditionalCopyReviewedAuthority &) = delete;
    ProviderConditionalCopyReviewedAuthority(ProviderConditionalCopyReviewedAuthority &&) noexcept = default;
    ProviderConditionalCopyReviewedAuthority &operator=(ProviderConditionalCopyReviewedAuthority &&) = delete;
    ~ProviderConditionalCopyReviewedAuthority() = default;

    [[nodiscard]] const ProviderConditionalCopyReviewedClaims &Claims() const noexcept { return m_Claims; }
    [[nodiscard]] bool HasReviewSeal() const noexcept { return static_cast<bool>(m_ReviewSeal); }

private:
    ProviderConditionalCopyReviewedAuthority(ProviderConditionalCopyReviewedClaims _claims,
                                             std::shared_ptr<const void> _review_seal) noexcept
        : m_Claims{std::move(_claims)}, m_ReviewSeal{std::move(_review_seal)}
    {
    }

    ProviderConditionalCopyReviewedClaims m_Claims;
    std::shared_ptr<const void> m_ReviewSeal;

    friend class nc::ops::ReviewedVFSOperationPreflight;
    friend struct ProviderConditionalCopyTransactionTestAccess;
};

enum class ProviderConditionalCopyTransactionBeginError : uint8_t {
    Unsupported,
    InvalidRequest,
    SourceStale,
    DestinationParentStale,
    DestinationExists,
    Cancelled,
    ProviderFailure
};

enum class ProviderConditionalCopyPublicationState : uint8_t {
    NotPublished,
    Published,
    Unknown
};

enum class ProviderConditionalCopyCommitFailure : uint8_t {
    None,
    Aborted,
    Cancelled,
    SourceStale,
    DestinationParentStale,
    DestinationExists,
    MetadataFailed,
    FileSystemSyncFailed,
    ProviderFailure
};

enum class ProviderConditionalCopyFilesystemSyncStatus : uint8_t {
    NotAttempted,
    Confirmed,
    Failed
};

struct ProviderConditionalCopyCommitResult final {
    // Publication/failure is the primary outcome. Filesystem sync evidence is independent;
    // successful publication requires Confirmed, and only Failed carries a sync errno.
    ProviderConditionalCopyPublicationState publication{ProviderConditionalCopyPublicationState::NotPublished};
    ProviderConditionalCopyCommitFailure failure{ProviderConditionalCopyCommitFailure::ProviderFailure};
    int system_error{0};
    ProviderConditionalCopyFilesystemSyncStatus filesystem_sync_status{
        ProviderConditionalCopyFilesystemSyncStatus::NotAttempted};
    int filesystem_sync_system_error{0};

    bool operator==(const ProviderConditionalCopyCommitResult &) const noexcept = default;
};

/**
 * Provider-minted, reviewed conditional commit authority. Provider commit/abort executes at most once;
 * later terminal calls replay a cached result without claiming a safer publication state.
 */
class ProviderConditionalCopyTransaction final
{
public:
    using CancelChecker = std::function<bool()>;
    // The checker is borrowed for this synchronous invocation only.  A provider must sample it before its
    // publication barrier and must not retain it.
    using CommitHandler = std::function<ProviderConditionalCopyCommitResult(const CancelChecker &)>;
    using AbortHandler = std::function<ProviderConditionalCopyPublicationState()>;

    ProviderConditionalCopyTransaction() = delete;
    ProviderConditionalCopyTransaction(const ProviderConditionalCopyTransaction &) = delete;
    ProviderConditionalCopyTransaction &operator=(const ProviderConditionalCopyTransaction &) = delete;
    ProviderConditionalCopyTransaction(ProviderConditionalCopyTransaction &&) noexcept;
    ProviderConditionalCopyTransaction &operator=(ProviderConditionalCopyTransaction &&) noexcept;
    ~ProviderConditionalCopyTransaction();

    [[nodiscard]] bool IsPending() const noexcept;
    [[nodiscard]] ProviderConditionalCopyCommitResult Commit(const CancelChecker &_cancel_checker = {}) noexcept;
    [[nodiscard]] ProviderConditionalCopyCommitResult Abort() noexcept;

private:
    struct Impl;

    explicit ProviderConditionalCopyTransaction(std::unique_ptr<Impl> _impl) noexcept;
    [[nodiscard]] static std::expected<std::unique_ptr<ProviderConditionalCopyTransaction>,
                                       ProviderConditionalCopyTransactionBeginError>
    Mint(const Host &_provider,
         ProviderConditionalCopyReviewedAuthority _authority,
         CommitHandler _commit,
         AbortHandler _abort) noexcept;
    void Reset() noexcept;

    std::unique_ptr<Impl> m_Impl;

    friend class Host;
    friend struct ProviderConditionalCopyTransactionTestAccess;
};

class ProviderCapabilitiesResolver
{
public:
    /**
     * Resolves capabilities at an optional provider path. An empty path uses host-wide writability
     * and leaves path-dependent change observation disabled.
     */
    [[nodiscard]] static ProviderCapabilities Resolve(Host &_host, std::string_view _path = {});
};

} // namespace nc::vfs
