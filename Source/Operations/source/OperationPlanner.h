// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "OperationPlan.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace nc::ops {

struct OperationPlanningPath final {
    std::string provider_id;
    std::string absolute_path;

    bool operator==(const OperationPlanningPath &) const = default;
};

enum class OperationPlanningProbeError : uint8_t {
    Unsupported,
    UnsupportedItem,
    InvalidName,
    Unavailable,
    Cancelled,
    PermissionDenied,
    Failed
};

template <class T>
using OperationPlanningProbeResult = std::expected<T, OperationPlanningProbeError>;

enum class OperationPlanningPathIdentitySemantics : uint8_t {
    ExactBytes,
    ASCIICaseSensitive,
    ASCIICaseInsensitive,
    Unavailable
};

struct OperationPlanningProviderEvidence final {
    bool can_copy_from = false;
    bool can_copy_to = false;
    OperationPlanningPathIdentitySemantics path_identity = OperationPlanningPathIdentitySemantics::Unavailable;
    bool can_replace_file = false;
    bool can_replace_directory = false;
    bool can_copy_symlink_to = false;
    /** Provider-level rename capability. Namespace access remains separately probed on each parent directory. */
    bool can_rename = false;
    /** Provider-level permanent-delete capability. Namespace access is separately probed on the parent. */
    bool can_delete_permanently = false;

    bool operator==(const OperationPlanningProviderEvidence &) const = default;
};

enum class OperationPlanningItemKind : uint8_t {
    Missing,
    File,
    Directory,
    Symlink,
    Other
};

struct OperationPlanningTimestampEvidence final {
    int64_t seconds = 0;
    int64_t nanoseconds = 0;

    bool operator==(const OperationPlanningTimestampEvidence &) const = default;
};

struct OperationPlanningNativeObjectIdentityEvidence final {
    int32_t device = 0;
    uint64_t inode = 0;
    OperationPlanningTimestampEvidence birth_time;

    bool operator==(const OperationPlanningNativeObjectIdentityEvidence &) const = default;
};

struct OperationPlanningNativeObjectVersionEvidence final {
    uint16_t mode = 0;
    uint64_t byte_size = 0;
    OperationPlanningTimestampEvidence modification_time;
    OperationPlanningTimestampEvidence status_change_time;

    bool operator==(const OperationPlanningNativeObjectVersionEvidence &) const = default;
};

struct OperationPlanningItemEvidence final {
    OperationPlanningItemKind kind = OperationPlanningItemKind::Missing;
    std::optional<uint64_t> byte_size;
    std::optional<OperationPlanningNativeObjectIdentityEvidence> native_identity;
    std::optional<OperationPlanningNativeObjectVersionEvidence> native_version;

    OperationPlanningItemEvidence() = default;
    OperationPlanningItemEvidence(
        OperationPlanningItemKind _kind,
        std::optional<uint64_t> _byte_size,
        std::optional<OperationPlanningNativeObjectIdentityEvidence> _native_identity = std::nullopt,
        std::optional<OperationPlanningNativeObjectVersionEvidence> _native_version = std::nullopt)
        : kind(_kind), byte_size(_byte_size), native_identity(std::move(_native_identity)),
          native_version(std::move(_native_version))
    {
    }

    bool operator==(const OperationPlanningItemEvidence &) const = default;
};

struct OperationPlanningNameEvidence final {
    bool valid = false;

    bool operator==(const OperationPlanningNameEvidence &) const = default;
};

enum class OperationPlanningRequiredAccess : uint8_t {
    Read,
    Write,
    ReplaceFile,
    ReplaceDirectory,
    /** Rename mutates the supplied parent namespace; it is distinct from creation access. */
    Rename,
    /** Delete removes an entry from the supplied parent namespace; kept apart from Rename for the same reason. */
    Delete
};

enum class OperationPlanningAccessState : uint8_t {
    Granted,
    PermissionRequired,
    Denied
};

struct OperationPlanningAccessEvidence final {
    OperationPlanningAccessState state = OperationPlanningAccessState::Denied;

    bool operator==(const OperationPlanningAccessEvidence &) const = default;
};

struct OperationPlanningEstimateEvidence final {
    uint64_t files = 0;
    uint64_t bytes = 0;
    bool contains_symlinks = false;

    bool operator==(const OperationPlanningEstimateEvidence &) const = default;
};

struct OperationPlanningSpaceEvidence final {
    std::optional<uint64_t> available_bytes;

    bool operator==(const OperationPlanningSpaceEvidence &) const = default;
};

/** Pure probe boundary. Implementations must not retain path references beyond a call. */
class OperationPlanningProbes
{
public:
    virtual ~OperationPlanningProbes() = default;

    virtual OperationPlanningProbeResult<OperationPlanningProviderEvidence>
    ProbeProvider(const OperationPlanningPath &_path) = 0;
    virtual OperationPlanningProbeResult<OperationPlanningItemEvidence>
    ProbeItem(const OperationPlanningPath &_path) = 0;
    virtual OperationPlanningProbeResult<OperationPlanningNameEvidence>
    ProbeDestinationName(const OperationPlanningPath &_path) = 0;
    virtual OperationPlanningProbeResult<OperationPlanningAccessEvidence>
    ProbeAccess(const OperationPlanningPath &_path, OperationPlanningRequiredAccess _required) = 0;
    virtual OperationPlanningProbeResult<OperationPlanningEstimateEvidence>
    ProbeEstimate(const OperationPlanningPath &_source,
                  const OperationPlanningPath &_destination) = 0;
    virtual OperationPlanningProbeResult<OperationPlanningSpaceEvidence>
    ProbeSpace(const OperationPlanningPath &_destination_directory) = 0;
};

enum class OperationPlanningBlockerCode : uint8_t {
    UnsupportedPlanType,
    ProviderCapabilityUnsupported,
    ProviderUnavailable,
    SourceMissing,
    SourceUnreadable,
    DestinationMissing,
    DestinationNotDirectory,
    DestinationNotWritable,
    PermissionRequired,
    PermissionDenied,
    InvalidSourceName,
    InvalidDestinationName,
    DestinationNameEvidenceUnavailable,
    PathIdentityUnavailable,
    SamePath,
    RecursiveDestination,
    DuplicateDestination,
    ConflictDecisionRequired,
    ConflictPolicyUnsupported,
    InsufficientSpace,
    EstimateOverflow,
    NothingToDo,
    ProbeCancelled,
    ProbeFailed
};

struct OperationPlanningBlocker final {
    OperationPlanningBlockerCode code;
    std::optional<OperationPlanningPath> path;

    bool operator==(const OperationPlanningBlocker &) const = default;
};

enum class OperationPlanningWarningCode : uint8_t {
    EstimateUnavailable,
    SpaceUnknown,
    DestructiveReplacement,
    RuntimeRevalidationRequired
};

struct OperationPlanningWarning final {
    OperationPlanningWarningCode code;
    std::optional<OperationPlanningPath> path;

    bool operator==(const OperationPlanningWarning &) const = default;
};

struct OperationPlanningProviderSnapshot final {
    OperationPlanningPath path;
    OperationPlanningProviderEvidence evidence;

    bool operator==(const OperationPlanningProviderSnapshot &) const = default;
};

struct OperationPlanningItemSnapshot final {
    OperationPlanningPath path;
    OperationPlanningItemEvidence evidence;

    bool operator==(const OperationPlanningItemSnapshot &) const = default;
};

struct OperationPlanningAccessSnapshot final {
    OperationPlanningPath path;
    OperationPlanningRequiredAccess required;
    OperationPlanningAccessEvidence evidence;

    bool operator==(const OperationPlanningAccessSnapshot &) const = default;
};

struct OperationPlanningNameSnapshot final {
    OperationPlanningPath path;
    OperationPlanningNameEvidence evidence;

    bool operator==(const OperationPlanningNameSnapshot &) const = default;
};

struct OperationPlanningConflict final {
    OperationPlanningPath source;
    OperationPlanningPath destination;
    OperationPlanConflictDecision decision;

    bool operator==(const OperationPlanningConflict &) const = default;
};

struct OperationPlanningDestructiveEffect final {
    OperationPlanningPath destination;

    bool operator==(const OperationPlanningDestructiveEffect &) const = default;
};

struct OperationPlannedCopyItem final {
    OperationPlanningPath source;
    OperationPlanningPath destination;
    OperationPlanningItemKind source_kind;
    std::optional<OperationPlanningEstimateEvidence> estimate;

    bool operator==(const OperationPlannedCopyItem &) const = default;
};

/**
 * A planned Delete item has no destination at all - `OperationPlan::Create` refuses one for
 * `Trash`/`PermanentDelete` - so it is its own type rather than `OperationPlannedCopyItem` with an
 * unused field. Kept in the report separately from `items` for the same reason: a Delete plan and a
 * Copy/Move plan are never the same report, and giving them a shared vector would mean one of the two
 * always carries a field that means nothing for it.
 */
struct OperationPlannedDeleteItem final {
    OperationPlanningPath source;
    OperationPlanningItemKind source_kind;
    std::optional<OperationPlanningEstimateEvidence> estimate;

    bool operator==(const OperationPlannedDeleteItem &) const = default;
};

struct OperationPreflightReport final {
    std::vector<OperationPlannedCopyItem> items;
    std::vector<OperationPlannedDeleteItem> deleted_items;
    std::vector<OperationPlanningProviderSnapshot> provider_evidence;
    std::vector<OperationPlanningItemSnapshot> item_evidence;
    std::vector<OperationPlanningNameSnapshot> name_evidence;
    std::vector<OperationPlanningAccessSnapshot> access_evidence;
    std::vector<OperationPlanningConflict> conflicts;
    std::vector<OperationPlanningDestructiveEffect> destructive_effects;
    std::vector<OperationPlanningWarning> warnings;
    std::optional<OperationPlanningSpaceEvidence> destination_space;
    std::optional<uint64_t> estimated_files;
    std::optional<uint64_t> estimated_bytes;
    bool requires_confirmation = false;

    bool operator==(const OperationPreflightReport &) const = default;
};

class OperationPlanningRun;

class AcceptedOperationPlan final
{
public:
    AcceptedOperationPlan() = delete;

    [[nodiscard]] const OperationPlan &Plan() const noexcept { return m_Plan; }
    [[nodiscard]] const OperationPreflightReport &Report() const noexcept { return m_Report; }
    bool operator==(const AcceptedOperationPlan &) const = default;

private:
    AcceptedOperationPlan(OperationPlan _plan, OperationPreflightReport _report);

    OperationPlan m_Plan;
    OperationPreflightReport m_Report;

    friend class OperationPlanner;
    friend class OperationPlanningRun;
};

class BlockedOperationPlan final
{
public:
    BlockedOperationPlan() = delete;

    [[nodiscard]] const OperationPlan &Plan() const noexcept { return m_Plan; }
    [[nodiscard]] const OperationPreflightReport &Report() const noexcept { return m_Report; }
    [[nodiscard]] const std::vector<OperationPlanningBlocker> &Blockers() const noexcept { return m_Blockers; }
    bool operator==(const BlockedOperationPlan &) const = default;

private:
    BlockedOperationPlan(OperationPlan _plan,
                         OperationPreflightReport _report,
                         std::vector<OperationPlanningBlocker> _blockers);

    OperationPlan m_Plan;
    OperationPreflightReport m_Report;
    std::vector<OperationPlanningBlocker> m_Blockers;

    friend class OperationPlanner;
    friend class OperationPlanningRun;
};

using OperationPreflightResult = std::variant<AcceptedOperationPlan, BlockedOperationPlan>;

/** Copy-first pure preflight. It does not approve, queue, or execute an operation. */
class OperationPlanner final
{
public:
    [[nodiscard]] static OperationPreflightResult
    Preflight(OperationPlan _plan, OperationPlanningProbes &_probes);
};

/**
 * How many items an accepted report actually covers for a plan of the given type - `deleted_items`
 * for a plan that removes a source instead of publishing a destination, `items` for everything else.
 * Every boundary that checks a report against `plan.Sources().size()` (the journal's own per-source
 * numbering) has to ask this, not assume `items`: a Delete plan's own items live in the other vector,
 * and a boundary that read `items.size()` for one would see zero regardless of how many sources were
 * actually accepted.
 */
[[nodiscard]] inline size_t OperationPlanningAcceptedItemCount(OperationPlanType _type,
                                                               const OperationPreflightReport &_report) noexcept
{
    return _type == OperationPlanType::PermanentDelete || _type == OperationPlanType::Trash
               ? _report.deleted_items.size()
               : _report.items.size();
}

} // namespace nc::ops
