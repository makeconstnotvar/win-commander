// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "ReviewedOperationFactory.h"

namespace nc::ops {

struct ReviewedOperationFactoryTestAccess final {
    using DirectAccessChecker = std::function<bool(std::string_view _path, int _mode)>;
    using SourceOpenAt = std::function<int(int _directory_fd, const char *_name, int _flags)>;
    using ConditionalCommitTransactionResolver =
        ReviewedOperationFactory::ConditionalCommitTransactionResolver;
    using ConditionalMoveCommitTransactionResolver =
        ReviewedOperationFactory::ConditionalMoveCommitTransactionResolver;
    using SnapshotLookup = ReviewedOperationFactory::SnapshotLookup;

    [[nodiscard]] static std::expected<std::shared_ptr<Operation>, ReviewedOperationFactoryError>
    Create(ReviewedVFSOperationPreflight _preflight,
           ConditionalCommitTransactionResolver _conditional_commit_transaction_resolver,
           ReviewedOperationFactory::CancelChecker _cancel_checker = {},
           DirectAccessChecker _direct_access_checker = {},
           SourceOpenAt _source_open_at = {},
           SnapshotLookup _snapshot_lookup = {},
           ConditionalMoveCommitTransactionResolver _conditional_move_commit_transaction_resolver = {}) noexcept
    {
        return ReviewedOperationFactory::CreateWithDependencies(
            std::move(_preflight),
            std::move(_cancel_checker),
            std::move(_direct_access_checker),
            std::move(_source_open_at),
            std::move(_conditional_commit_transaction_resolver),
            std::move(_snapshot_lookup),
            std::move(_conditional_move_commit_transaction_resolver));
    }

    [[nodiscard]] static std::expected<CopyOperationExecutionProduct, ReviewedOperationFactoryError>
    CreateExecutionProduct(
        ReviewedVFSOperationPreflight _preflight,
        ConditionalCommitTransactionResolver _conditional_commit_transaction_resolver,
        ReviewedOperationFactory::CancelChecker _cancel_checker = {},
        DirectAccessChecker _direct_access_checker = {},
        SourceOpenAt _source_open_at = {},
        ConditionalMoveCommitTransactionResolver _conditional_move_commit_transaction_resolver = {}) noexcept
    {
        return ReviewedOperationFactory::CreateExecutionProductWithDependencies(
            std::move(_preflight),
            std::move(_cancel_checker),
            std::move(_direct_access_checker),
            std::move(_source_open_at),
            std::move(_conditional_commit_transaction_resolver),
            {},
            std::move(_conditional_move_commit_transaction_resolver));
    }

    [[nodiscard]] static std::shared_ptr<Operation> &
    Operation(CopyOperationExecutionProduct &_product) noexcept
    {
        return _product.m_Operation;
    }

    [[nodiscard]] static CopyOperationExecutionProduct::TerminalItemResultAccessor &
    TerminalItemResult(CopyOperationExecutionProduct &_product) noexcept
    {
        return _product.m_TerminalItemResult;
    }

    [[nodiscard]] static CopyOperationExecutionProduct::TerminalEvidenceAccessor &
    TerminalEvidence(CopyOperationExecutionProduct &_product) noexcept
    {
        return _product.m_TerminalEvidence;
    }
};

} // namespace nc::ops
