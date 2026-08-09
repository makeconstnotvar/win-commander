// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "ProviderConditionalCopyOperation.h"

#include <cstddef>
#include <functional>

namespace nc::ops {

struct ProviderConditionalCopyOperationTestHooks final {
    std::function<void()> before_worker_launch;
    std::function<void()> before_commit_gate;
    /**
     * Runs on the worker at the top of each item, before the gate decides whether that item may
     * start. It is the only way to drive the two interleavings that matter and cannot otherwise be
     * reached: a stop landing exactly there, and a throw from that window.
     */
    std::function<void(size_t _index)> before_item_start;
};

class ProviderConditionalCopyOperationTesting final
{
public:
    [[nodiscard]] static std::expected<CopyOperationExecutionProduct, ProviderConditionalCopyOperationConstructionError>
    Create(std::unique_ptr<vfs::ProviderConditionalCopyTransaction> _transaction,
           ProviderConditionalCopyJournalContext _journal_context,
           ProviderConditionalCopyOperationPresentation _presentation,
           vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker = {},
           ProviderConditionalCopyOperationTestHooks _hooks = {}) noexcept;

    [[nodiscard]] static std::expected<CopyOperationExecutionProduct, ProviderConditionalCopyOperationConstructionError>
    CreateBatch(std::vector<ProviderConditionalCopyOperationItem> _items,
                vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker = {},
                ProviderConditionalCopyOperationTestHooks _hooks = {}) noexcept;

    [[nodiscard]] static std::shared_ptr<Operation> &Operation(CopyOperationExecutionProduct &_product) noexcept;
    [[nodiscard]] static CopyOperationExecutionProduct::TerminalItemResultAccessor &
    TerminalItemResult(CopyOperationExecutionProduct &_product) noexcept;
    [[nodiscard]] static CopyOperationExecutionProduct::TerminalEvidenceAccessor &
    TerminalEvidence(CopyOperationExecutionProduct &_product) noexcept;
};

} // namespace nc::ops
