// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CopyOperationExecutionProduct.h"
#include "ProviderConditionalCopyJournalMapper.h"

#include <VFS/ProviderCapabilities.h>

#include <cstdint>
#include <expected>
#include <memory>

namespace nc::ops {

class ProviderConditionalCopyOperationTesting;
class ReviewedOperationFactory;
struct ProviderConditionalCopyOperationTestHooks;

enum class ProviderConditionalCopyOperationConstructionError : uint8_t {
    MissingTransaction,
    AllocationFailed
};

class ProviderConditionalCopyOperationFactory final
{
private:
    [[nodiscard]] static std::expected<CopyOperationExecutionProduct,
                                       ProviderConditionalCopyOperationConstructionError>
    Create(std::unique_ptr<vfs::ProviderConditionalCopyTransaction> _transaction,
           ProviderConditionalCopyJournalContext _journal_context,
           vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker) noexcept;

    [[nodiscard]] static std::expected<CopyOperationExecutionProduct,
                                       ProviderConditionalCopyOperationConstructionError>
    CreateForTesting(std::unique_ptr<vfs::ProviderConditionalCopyTransaction> _transaction,
                     ProviderConditionalCopyJournalContext _journal_context,
                     vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker,
                     ProviderConditionalCopyOperationTestHooks _hooks) noexcept;

    friend class ProviderConditionalCopyOperationTesting;
    friend class ReviewedOperationFactory;
};

} // namespace nc::ops
