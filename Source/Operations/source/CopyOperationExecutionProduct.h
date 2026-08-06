// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "Operation.h"
#include "OperationJournal.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

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

/**
 * Immutable terminal evidence emitted by an execution product.
 *
 * `item_results` preserves the execution's source-item order.  Journal-level
 * validation remains the authority for the vector's cardinality, indices, and
 * relationship to `state`; this value only carries the atomically observed
 * terminal snapshot across the execution boundary.
 */
struct CopyOperationTerminalEvidence final {
    OperationJournalState state;
    std::vector<OperationJournalItemResult> item_results;

    bool operator==(const CopyOperationTerminalEvidence &) const = default;
};

class CopyOperationExecutionProduct final
{
public:
    using TerminalItemResultAccessor =
        std::function<std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>()>;
    using TerminalEvidenceAccessor =
        std::function<std::expected<CopyOperationTerminalEvidence, CopyOperationTerminalResultError>()>;

    CopyOperationExecutionProduct(const CopyOperationExecutionProduct &) = delete;
    CopyOperationExecutionProduct &operator=(const CopyOperationExecutionProduct &) = delete;
    CopyOperationExecutionProduct(CopyOperationExecutionProduct &&) noexcept = default;
    CopyOperationExecutionProduct &operator=(CopyOperationExecutionProduct &&) noexcept = default;
    ~CopyOperationExecutionProduct() = default;

private:
    CopyOperationExecutionProduct(std::shared_ptr<Operation> _operation,
                                  TerminalEvidenceAccessor _terminal_evidence)
        : m_Operation{std::move(_operation)}, m_TerminalEvidence{std::move(_terminal_evidence)},
          m_TerminalItemResult{MakeTerminalItemResultAccessor(m_TerminalEvidence)}
    {
    }

    CopyOperationExecutionProduct(std::shared_ptr<Operation> _operation,
                                  TerminalItemResultAccessor _terminal_item_result)
        : m_Operation{std::move(_operation)},
          m_TerminalEvidence{MakeTerminalEvidenceAccessor(_terminal_item_result)},
          m_TerminalItemResult{std::move(_terminal_item_result)}
    {
    }

    [[nodiscard]] static std::optional<OperationJournalState>
    TerminalStateForItemResult(const OperationJournalItemResult &_item_result) noexcept
    {
        switch( _item_result.status ) {
            case OperationJournalItemStatus::Succeeded:
            case OperationJournalItemStatus::Skipped:
                return OperationJournalState::Completed;
            case OperationJournalItemStatus::Failed:
                return OperationJournalState::Failed;
            case OperationJournalItemStatus::Cancelled:
                return OperationJournalState::Cancelled;
        }
        return std::nullopt;
    }

    [[nodiscard]] static TerminalEvidenceAccessor
    MakeTerminalEvidenceAccessor(TerminalItemResultAccessor _terminal_item_result)
    {
        return [_terminal_item_result = std::move(_terminal_item_result)]()
                   -> std::expected<CopyOperationTerminalEvidence, CopyOperationTerminalResultError> {
            if( !_terminal_item_result )
                return std::unexpected(CopyOperationTerminalResultError::Inconsistent);

            auto item_result = _terminal_item_result();
            if( !item_result )
                return std::unexpected(item_result.error());

            const auto state = TerminalStateForItemResult(*item_result);
            if( !state )
                return std::unexpected(CopyOperationTerminalResultError::Inconsistent);

            return CopyOperationTerminalEvidence{
                .state = *state,
                .item_results = {std::move(*item_result)},
            };
        };
    }

    [[nodiscard]] static TerminalItemResultAccessor
    MakeTerminalItemResultAccessor(TerminalEvidenceAccessor _terminal_evidence)
    {
        return [_terminal_evidence = std::move(_terminal_evidence)]()
                   -> std::expected<OperationJournalItemResult, CopyOperationTerminalResultError> {
            if( !_terminal_evidence )
                return std::unexpected(CopyOperationTerminalResultError::Inconsistent);

            auto evidence = _terminal_evidence();
            if( !evidence )
                return std::unexpected(evidence.error());
            if( evidence->item_results.size() != 1 )
                return std::unexpected(CopyOperationTerminalResultError::Inconsistent);

            const auto state = TerminalStateForItemResult(evidence->item_results.front());
            if( !state || *state != evidence->state )
                return std::unexpected(CopyOperationTerminalResultError::Inconsistent);
            return std::move(evidence->item_results.front());
        };
    }

    std::shared_ptr<Operation> m_Operation;
    TerminalEvidenceAccessor m_TerminalEvidence;
    TerminalItemResultAccessor m_TerminalItemResult;

    friend class CopyOperationOrchestrator;
    friend class CopyOperationOrchestratorTesting;
    friend class ProviderConditionalCopyOperationFactory;
    friend class ProviderConditionalCopyOperationTesting;
    friend class ReviewedOperationFactory;
    friend struct ReviewedOperationFactoryTestAccess;
};

} // namespace nc::ops
