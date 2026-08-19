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
 * touching what it names between review and publication. `MonotonicGrowth` exists for one case - a
 * destination-parent directory that an authorized batch is itself publishing into. Its own earlier
 * items legitimately advance the parent's size and timestamps; that is not staleness, it is the batch
 * proving its own prior work. `MonotonicShrink` is the mirror case for a Move batch's source-parent
 * directory: a rename indivisibly removes the entry it publishes, so a directory several of a batch's
 * own items are moving *out of* legitimately loses size and link_count as the batch's own earlier items
 * complete - confirmed empirically on APFS, the same way the growth direction was, rather than assumed
 * symmetric. Identity (device, inode, birth time) and the whole ownership/permission surface (mode,
 * uid, gid, BSD flags, ACL, extended attributes) still fail closed on any change under every mode.
 * Exactly four fields may move instead of matching exactly: byte size and link_count, which advance
 * under `MonotonicGrowth` and recede under `MonotonicShrink`, and the two content-derived timestamps,
 * which only ever advance - a clock does not run backward for either direction of content change.
 */
enum class ProviderConditionalCopyExpectationTolerance : uint8_t {
    Exact,
    MonotonicGrowth,
    MonotonicShrink
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

/**
 * The same preliminary question asked of a permanent Delete: one path, no destination side at all -
 * a delete has nothing to publish anywhere, only a source whose continued existence is what a review
 * would attest to. `SameVolumeUnlink` names the interface rather than staying generic, the way
 * `SameVolumeRename` does, because it is a claim about which primitive a reviewed transaction would
 * anchor through - `unlinkat` through an anchored parent, not an anonymous "yes".
 */
enum class ProviderConditionalDeletePathSupport : uint8_t {
    SameVolumeUnlink,
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

/**
 * Exact immutable claims derived from one reviewed create-only Move intent.
 *
 * It carries one expectation a Copy has no use for: the **source parent**. A Copy reads its source
 * through a descriptor and leaves the directory holding it untouched, so that directory is not part of
 * what authorises the publication. A Move removes an entry from it, and - more to the point - a rename
 * has no descriptor form: it acts on a *name inside a directory*. Holding and checking that directory
 * is what makes "the object being moved is the object that was reviewed" a claim the provider can
 * check at all.
 */
struct ProviderConditionalMoveReviewedClaims final {
    std::string plan_id;
    ProviderConditionalCopyBinding source_binding;
    ProviderConditionalCopyBinding destination_binding;
    ProviderConditionalCopyExistingExpectation source;
    ProviderConditionalCopyExistingExpectation source_parent;
    ProviderConditionalCopyExistingExpectation destination_parent;
    ProviderConditionalCopyMissingExpectation destination;

    bool operator==(const ProviderConditionalMoveReviewedClaims &) const noexcept = default;
};

/**
 * Move-only, non-forgeable reviewed Move authority - a distinct type from the Copy one, and that
 * distinction is the safety property rather than tidiness.
 *
 * Were the two interchangeable, an authority minted from a plan the user approved as a *copy* could be
 * handed to a Move execution, and the source would be gone. A shared type could only be defended by a
 * runtime check on a plan-type field; a separate type makes the substitution unspeakable. The reverse
 * direction is harmless by comparison and is refused by the same construction.
 */
class ProviderConditionalMoveReviewedAuthority final
{
public:
    ProviderConditionalMoveReviewedAuthority() = delete;
    ProviderConditionalMoveReviewedAuthority(const ProviderConditionalMoveReviewedAuthority &) = delete;
    ProviderConditionalMoveReviewedAuthority &operator=(const ProviderConditionalMoveReviewedAuthority &) = delete;
    ProviderConditionalMoveReviewedAuthority(ProviderConditionalMoveReviewedAuthority &&) noexcept = default;
    ProviderConditionalMoveReviewedAuthority &operator=(ProviderConditionalMoveReviewedAuthority &&) = delete;
    ~ProviderConditionalMoveReviewedAuthority() = default;

    [[nodiscard]] const ProviderConditionalMoveReviewedClaims &Claims() const noexcept { return m_Claims; }
    [[nodiscard]] bool HasReviewSeal() const noexcept { return static_cast<bool>(m_ReviewSeal); }

private:
    ProviderConditionalMoveReviewedAuthority(ProviderConditionalMoveReviewedClaims _claims,
                                             std::shared_ptr<const void> _review_seal) noexcept
        : m_Claims{std::move(_claims)}, m_ReviewSeal{std::move(_review_seal)}
    {
    }

    ProviderConditionalMoveReviewedClaims m_Claims;
    std::shared_ptr<const void> m_ReviewSeal;

    friend class nc::ops::ReviewedVFSOperationPreflight;
    friend struct ProviderConditionalCopyTransactionTestAccess;
};

/**
 * Its own vocabulary rather than the Copy one, for the same reason the eligibility answer is its own:
 * `SourceParentStale` is a refusal a Copy can never produce, and adding it to the shared enum would
 * oblige every Copy consumer to handle a case it cannot reach.
 */
enum class ProviderConditionalMoveTransactionBeginError : uint8_t {
    Unsupported,
    InvalidRequest,
    SourceStale,
    SourceParentStale,
    DestinationParentStale,
    DestinationExists,
    Cancelled,
    ProviderFailure
};

/**
 * Exact immutable claims derived from one reviewed permanent-Delete intent.
 *
 * There is no destination side at all - not merely an empty one, an absent one - because a delete
 * publishes nothing anywhere. What authorises `unlinkat` is exactly the same pair a Move's own removal
 * half needs: the source, and the directory holding it, since a delete acts on a *name inside a
 * directory* and has no descriptor form either, for the identical reason a rename does not.
 */
struct ProviderConditionalDeleteReviewedClaims final {
    std::string plan_id;
    ProviderConditionalCopyBinding source_binding;
    ProviderConditionalCopyExistingExpectation source;
    ProviderConditionalCopyExistingExpectation source_parent;

    bool operator==(const ProviderConditionalDeleteReviewedClaims &) const noexcept = default;
};

/**
 * Delete-only, non-forgeable reviewed authority - its own type for the same non-negotiable reason the
 * Move one is: an authority minted from a plan approved as a copy or a move must be structurally unable
 * to authorise a delete, and a shared type could only be defended by a runtime check on a field a
 * mistake could skip.
 */
class ProviderConditionalDeleteReviewedAuthority final
{
public:
    ProviderConditionalDeleteReviewedAuthority() = delete;
    ProviderConditionalDeleteReviewedAuthority(const ProviderConditionalDeleteReviewedAuthority &) = delete;
    ProviderConditionalDeleteReviewedAuthority &
    operator=(const ProviderConditionalDeleteReviewedAuthority &) = delete;
    ProviderConditionalDeleteReviewedAuthority(ProviderConditionalDeleteReviewedAuthority &&) noexcept = default;
    ProviderConditionalDeleteReviewedAuthority &operator=(ProviderConditionalDeleteReviewedAuthority &&) = delete;
    ~ProviderConditionalDeleteReviewedAuthority() = default;

    [[nodiscard]] const ProviderConditionalDeleteReviewedClaims &Claims() const noexcept { return m_Claims; }
    [[nodiscard]] bool HasReviewSeal() const noexcept { return static_cast<bool>(m_ReviewSeal); }

private:
    ProviderConditionalDeleteReviewedAuthority(ProviderConditionalDeleteReviewedClaims _claims,
                                               std::shared_ptr<const void> _review_seal) noexcept
        : m_Claims{std::move(_claims)}, m_ReviewSeal{std::move(_review_seal)}
    {
    }

    ProviderConditionalDeleteReviewedClaims m_Claims;
    std::shared_ptr<const void> m_ReviewSeal;

    friend class nc::ops::ReviewedVFSOperationPreflight;
    friend struct ProviderConditionalCopyTransactionTestAccess;
};

/**
 * Its own vocabulary for the same reason Move's is: no `DestinationParentStale`/`DestinationExists`
 * exist here at all, because there is no destination for either refusal to be about.
 */
enum class ProviderConditionalDeleteTransactionBeginError : uint8_t {
    Unsupported,
    InvalidRequest,
    SourceStale,
    SourceParentStale,
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
    /**
     * The same transaction from a Move authority. One type serves both because what this class owns -
     * the single-use terminal gate, the cached result, the consumed authority - is identical for the
     * two, while the only thing that differs, the publication itself, is already a handler. A second
     * class would have forked exactly the parts that are hard to get right, which is the lesson the
     * batch operation learned when it declined to fork for the same reason.
     */
    [[nodiscard]] static std::expected<std::unique_ptr<ProviderConditionalCopyTransaction>,
                                       ProviderConditionalMoveTransactionBeginError>
    MintForMove(const Host &_provider,
                ProviderConditionalMoveReviewedAuthority _authority,
                CommitHandler _commit,
                AbortHandler _abort) noexcept;
    /**
     * The same transaction from a Delete authority, for the reason `MintForMove` already gives - what
     * this class owns is identical regardless of which of the three this is, and the one place they
     * differ is already a parameter. `ProviderConditionalCopyCommitResult` is reused as-is rather than
     * given a Delete-shaped sibling: `publication` here means "did this transaction's one durable
     * outcome happen", which for a Delete is removal rather than creation - the raw provider type
     * stays generic, and the rename to `source_removal` happens where a Delete's result is mapped into
     * journal evidence, the same boundary where Move's own `destination_publication` reading already
     * gets its meaning from context rather than from the field's name alone.
     */
    [[nodiscard]] static std::expected<std::unique_ptr<ProviderConditionalCopyTransaction>,
                                       ProviderConditionalDeleteTransactionBeginError>
    MintForDelete(const Host &_provider,
                  ProviderConditionalDeleteReviewedAuthority _authority,
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
