// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CopyOperationExecutionProduct.h"
#include "ProviderConditionalCopyJournalMapper.h"

#include <VFS/ProviderCapabilities.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace nc::ops {

class ProviderConditionalCopyOperationTesting;
class ReviewedOperationFactory;
struct ProviderConditionalCopyOperationTestHooks;

enum class ProviderConditionalCopyOperationConstructionError : uint8_t {
    MissingTransaction,
    InvalidPresentation,
    /** A batch with no items at all. Distinct from a missing transaction: nothing was asked for. */
    EmptyBatch,
    /**
     * The items are not numbered as one journal entry can carry them - the indices must strictly
     * increase. Refused here rather than at the terminal, because a set that cannot be recorded is
     * better rejected before anything is copied than after.
     */
    InvalidJournalIndices,
    AllocationFailed
};

struct ProviderConditionalCopyOperationPresentation final {
    std::shared_ptr<vfs::Host> source_host;
    std::string source_path;
    std::string destination_path;
};

/**
 * One prepared item as the operation receives it: what to commit, how to journal the outcome, and
 * what to show while it runs. The operation owns the whole set and is one operation over it - not a
 * loop of single-item operations, which would produce one journal and one terminal state each and
 * show N operations where the user asked for one.
 */
struct ProviderConditionalCopyOperationItem final {
    std::unique_ptr<vfs::ProviderConditionalCopyTransaction> transaction;
    ProviderConditionalCopyJournalContext journal_context;
    ProviderConditionalCopyOperationPresentation presentation;
};

class ProviderConditionalCopyOperationFactory final
{
private:
    [[nodiscard]] static std::expected<CopyOperationExecutionProduct, ProviderConditionalCopyOperationConstructionError>
    Create(std::unique_ptr<vfs::ProviderConditionalCopyTransaction> _transaction,
           ProviderConditionalCopyJournalContext _journal_context,
           ProviderConditionalCopyOperationPresentation _presentation,
           vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker) noexcept;

    [[nodiscard]] static std::expected<CopyOperationExecutionProduct, ProviderConditionalCopyOperationConstructionError>
    CreateBatch(std::vector<ProviderConditionalCopyOperationItem> _items,
                vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker) noexcept;

    [[nodiscard]] static std::expected<CopyOperationExecutionProduct, ProviderConditionalCopyOperationConstructionError>
    CreateForTesting(std::unique_ptr<vfs::ProviderConditionalCopyTransaction> _transaction,
                     ProviderConditionalCopyJournalContext _journal_context,
                     ProviderConditionalCopyOperationPresentation _presentation,
                     vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker,
                     ProviderConditionalCopyOperationTestHooks _hooks) noexcept;

    [[nodiscard]] static std::expected<CopyOperationExecutionProduct, ProviderConditionalCopyOperationConstructionError>
    CreateBatchForTesting(std::vector<ProviderConditionalCopyOperationItem> _items,
                          vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker,
                          ProviderConditionalCopyOperationTestHooks _hooks) noexcept;

    friend class ProviderConditionalCopyOperationTesting;
    friend class ReviewedOperationFactory;
};

} // namespace nc::ops
