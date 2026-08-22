// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CopyOperationExecutionProduct.h"
#include "VFSOperationPlanningProbes.h"

#include <Base/Error.h>
#include <VFS/ProviderCapabilities.h>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace nc::ops {

class Operation;
class CopyOperationOrchestrator;
struct ReviewedOperationFactoryTestAccess;

enum class ReviewedOperationFactoryErrorCode : uint8_t {
    UnsupportedPlanType,
    MissingBindings,
    ProviderUnavailable,
    UnsupportedProviderScope,
    ConditionalCommitAuthorityUnavailable,
    ConditionalCommitIntegrationUnavailable,
    EmptyAcceptedPlan,
    /**
     * The report does not account for the plan's sources, one item each. Both former refusals -
     * several sources, and one source expanded into several items - are lifted; what replaces them is
     * the rule the journal actually imposes, since it numbers results in the plan's source space and
     * will not record a completed entry that is missing one.
     */
    IncompleteAcceptedPlan,
    UnsupportedConflictPolicy,
    UnexpectedConflictEvidence,
    InvalidReviewedPlan,
    UnsupportedSourceKind,
    MissingEvidence,
    InvalidEvidence,
    InvalidPath,
    UnsupportedAccessRoute,
    StaleSource,
    /**
     * Move and Delete only. The directory holding the source changed since review - a strictly more
     * informative fact than the source itself having changed, since both a rename and an unlink act on
     * a name inside that directory, and the entry the operation was going to name is no longer the
     * entry that was reviewed. Kept distinct from `StaleSource` rather than collapsed into it, for the
     * same reason the provider layer keeps `SourceParentStale` distinct from `SourceStale`.
     */
    StaleSourceParent,
    StaleDestination,
    Cancelled,
    OpenFailed,
    ConstructionFailed
};

struct ReviewedOperationFactoryError final {
    ReviewedOperationFactoryErrorCode code;
    std::optional<OperationPlanningPath> path;
    std::optional<Error> cause;

    bool operator==(const ReviewedOperationFactoryError &) const = default;
};

/**
 * Validates reviewed create-only Native Copy intent and binds exact provider transaction authority.
 * Public Create is a deliberate compatibility surface that aborts that authority and fails closed; the
 * private friend path constructs the production execution product. Privileged RoutedIO routes are rejected
 * before any direct descriptor access.
 */
class ReviewedOperationFactory final
{
public:
    using CancelChecker = std::function<bool()>;

    [[nodiscard]] static std::expected<std::shared_ptr<Operation>, ReviewedOperationFactoryError>
    Create(ReviewedVFSOperationPreflight _preflight, CancelChecker _cancel_checker = {}) noexcept;

private:
    using DirectAccessChecker = std::function<bool(std::string_view _path, int _mode)>;
    using SourceOpenAt = std::function<int(int _directory_fd, const char *_name, int _flags)>;
    /**
     * Finds the evidence recorded for a path.
     *
     * Injectable so a test can withhold or corrupt one snapshot - the two failures a real planner
     * never produces, and the ones guarding staleness. Deliberately *this* rather than a way to
     * construct an accepted plan: a seam able to forge a review is a seam that could forge one
     * somewhere it matters, while this can only change how already-reviewed evidence is looked up.
     */
    using SnapshotLookup = std::function<const OperationPlanningItemSnapshot *(const OperationPreflightReport &,
                                                                              const OperationPlanningPath &)>;
    using ConditionalCommitTransactionResolver = std::function<
        std::expected<std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction>,
                      nc::vfs::ProviderConditionalCopyTransactionBeginError>(
            nc::vfs::ProviderConditionalCopyReviewedAuthority,
            const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &)>;
    /** The Move counterpart, injectable for the same reason: a test can force a Begin failure. */
    using ConditionalMoveCommitTransactionResolver = std::function<
        std::expected<std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction>,
                      nc::vfs::ProviderConditionalMoveTransactionBeginError>(
            nc::vfs::ProviderConditionalMoveReviewedAuthority,
            const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &)>;
    /** The Delete counterpart, injectable for the same reason. */
    using ConditionalDeleteCommitTransactionResolver = std::function<
        std::expected<std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction>,
                      nc::vfs::ProviderConditionalDeleteTransactionBeginError>(
            nc::vfs::ProviderConditionalDeleteReviewedAuthority,
            const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &)>;

    [[nodiscard]] static std::expected<std::shared_ptr<Operation>, ReviewedOperationFactoryError>
    CreateWithDependencies(ReviewedVFSOperationPreflight _preflight,
                           CancelChecker _cancel_checker,
                           DirectAccessChecker _direct_access_checker,
                           SourceOpenAt _source_open_at,
                           ConditionalCommitTransactionResolver _conditional_commit_transaction_resolver,
                           SnapshotLookup _snapshot_lookup = {},
                           ConditionalMoveCommitTransactionResolver _conditional_move_commit_transaction_resolver =
                               {},
                           ConditionalDeleteCommitTransactionResolver
                               _conditional_delete_commit_transaction_resolver = {}) noexcept;

    [[nodiscard]] static std::expected<CopyOperationExecutionProduct, ReviewedOperationFactoryError>
    CreateExecutionProduct(ReviewedVFSOperationPreflight _preflight,
                           CancelChecker _cancel_checker = {}) noexcept;
    [[nodiscard]] static std::expected<CopyOperationExecutionProduct, ReviewedOperationFactoryError>
    CreateExecutionProductWithDependencies(
        ReviewedVFSOperationPreflight _preflight,
        CancelChecker _cancel_checker,
        DirectAccessChecker _direct_access_checker,
        SourceOpenAt _source_open_at,
        ConditionalCommitTransactionResolver _conditional_commit_transaction_resolver,
        SnapshotLookup _snapshot_lookup = {},
        ConditionalMoveCommitTransactionResolver _conditional_move_commit_transaction_resolver = {},
        ConditionalDeleteCommitTransactionResolver _conditional_delete_commit_transaction_resolver = {}) noexcept;
    [[nodiscard]] static std::expected<std::shared_ptr<Operation>, ReviewedOperationFactoryError>
    BlockExecutionProduct(
        std::expected<CopyOperationExecutionProduct, ReviewedOperationFactoryError> _product) noexcept;

    friend class CopyOperationOrchestrator;
    friend struct ReviewedOperationFactoryTestAccess;
};

} // namespace nc::ops
