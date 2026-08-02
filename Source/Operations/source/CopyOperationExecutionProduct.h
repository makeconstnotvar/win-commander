// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "Operation.h"
#include "OperationJournal.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <utility>

namespace nc::ops {

class CopyOperationOrchestrator;
class CopyOperationOrchestratorTesting;
class ProviderConditionalCopyOperationFactory;
class ProviderConditionalCopyOperationTesting;
class ReviewedOperationFactory;
struct ReviewedOperationFactoryTestAccess;

enum class CopyOperationTerminalResultError : uint8_t {
    Pending,
    Inconsistent
};

class CopyOperationExecutionProduct final
{
public:
    using TerminalItemResultAccessor =
        std::function<std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>()>;

    CopyOperationExecutionProduct(const CopyOperationExecutionProduct &) = delete;
    CopyOperationExecutionProduct &operator=(const CopyOperationExecutionProduct &) = delete;
    CopyOperationExecutionProduct(CopyOperationExecutionProduct &&) noexcept = default;
    CopyOperationExecutionProduct &operator=(CopyOperationExecutionProduct &&) noexcept = default;
    ~CopyOperationExecutionProduct() = default;

private:
    CopyOperationExecutionProduct(std::shared_ptr<Operation> _operation,
                                  TerminalItemResultAccessor _terminal_item_result) noexcept
        : m_Operation{std::move(_operation)}, m_TerminalItemResult{std::move(_terminal_item_result)}
    {
    }

    std::shared_ptr<Operation> m_Operation;
    TerminalItemResultAccessor m_TerminalItemResult;

    friend class CopyOperationOrchestrator;
    friend class CopyOperationOrchestratorTesting;
    friend class ProviderConditionalCopyOperationFactory;
    friend class ProviderConditionalCopyOperationTesting;
    friend class ReviewedOperationFactory;
    friend struct ReviewedOperationFactoryTestAccess;
};

} // namespace nc::ops
