// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "ProviderConditionalCopyOperation.h"

#include <functional>

namespace nc::ops {

struct ProviderConditionalCopyOperationTestHooks final {
    std::function<void()> before_worker_launch;
    std::function<void()> before_commit_gate;
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

    [[nodiscard]] static std::shared_ptr<Operation> &Operation(CopyOperationExecutionProduct &_product) noexcept;
    [[nodiscard]] static CopyOperationExecutionProduct::TerminalItemResultAccessor &
    TerminalItemResult(CopyOperationExecutionProduct &_product) noexcept;
    [[nodiscard]] static CopyOperationExecutionProduct::TerminalEvidenceAccessor &
    TerminalEvidence(CopyOperationExecutionProduct &_product) noexcept;
};

} // namespace nc::ops
