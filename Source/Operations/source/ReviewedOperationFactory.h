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
    BatchUnsupported,
    UnsupportedConflictPolicy,
    UnexpectedConflictEvidence,
    InvalidReviewedPlan,
    UnsupportedSourceKind,
    MissingEvidence,
    InvalidEvidence,
    InvalidPath,
    UnsupportedAccessRoute,
    StaleSource,
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
    using ConditionalCommitTransactionResolver = std::function<
        std::expected<std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction>,
                      nc::vfs::ProviderConditionalCopyTransactionBeginError>(
            nc::vfs::ProviderConditionalCopyReviewedAuthority,
            const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &)>;

    [[nodiscard]] static std::expected<std::shared_ptr<Operation>, ReviewedOperationFactoryError>
    CreateWithDependencies(ReviewedVFSOperationPreflight _preflight,
                           CancelChecker _cancel_checker,
                           DirectAccessChecker _direct_access_checker,
                           SourceOpenAt _source_open_at,
                           ConditionalCommitTransactionResolver _conditional_commit_transaction_resolver) noexcept;

    [[nodiscard]] static std::expected<CopyOperationExecutionProduct, ReviewedOperationFactoryError>
    CreateExecutionProduct(ReviewedVFSOperationPreflight _preflight,
                           CancelChecker _cancel_checker = {}) noexcept;
    [[nodiscard]] static std::expected<CopyOperationExecutionProduct, ReviewedOperationFactoryError>
    CreateExecutionProductWithDependencies(
        ReviewedVFSOperationPreflight _preflight,
        CancelChecker _cancel_checker,
        DirectAccessChecker _direct_access_checker,
        SourceOpenAt _source_open_at,
        ConditionalCommitTransactionResolver _conditional_commit_transaction_resolver) noexcept;
    [[nodiscard]] static std::expected<std::shared_ptr<Operation>, ReviewedOperationFactoryError>
    BlockExecutionProduct(
        std::expected<CopyOperationExecutionProduct, ReviewedOperationFactoryError> _product) noexcept;

    friend class CopyOperationOrchestrator;
    friend struct ReviewedOperationFactoryTestAccess;
};

} // namespace nc::ops
