// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CopyOperationOrchestrator.h"

#include <functional>
#include <type_traits>
#include <utility>

namespace nc::ops {

class CopyOperationOrchestratorTesting final
{
public:
    using ExecutionFactory = CopyOperationOrchestrator::ExecutionFactory;
    using ConditionalCommitTransactionResolver =
        CopyOperationOrchestrator::ConditionalCommitTransactionResolver;
    using PreEnqueueHandoff = CopyOperationOrchestrator::PreEnqueueHandoff;

    [[nodiscard]] static CopyOperationExecutionProduct
    MakeExecutionProduct(
        std::shared_ptr<Operation> _operation,
        CopyOperationExecutionProduct::TerminalItemResultAccessor _terminal_item_result)
    {
        return CopyOperationExecutionProduct{
            std::move(_operation), std::move(_terminal_item_result)};
    }

    template <class TerminalAccessor>
        requires std::is_invocable_r_v<std::expected<CopyOperationTerminalEvidence,
                                                     CopyOperationTerminalResultError>,
                                         TerminalAccessor &>
    [[nodiscard]] static CopyOperationExecutionProduct
    MakeExecutionProduct(std::shared_ptr<Operation> _operation, TerminalAccessor &&_terminal_evidence)
    {
        return CopyOperationExecutionProduct{
            std::move(_operation),
            CopyOperationExecutionProduct::TerminalEvidenceAccessor{std::forward<TerminalAccessor>(_terminal_evidence)}};
    }

    [[nodiscard]] static CopyOperationExecutionProduct::TerminalEvidenceAccessor &
    TerminalEvidence(CopyOperationExecutionProduct &_product) noexcept
    {
        return _product.m_TerminalEvidence;
    }

    [[nodiscard]] static CopyOperationOrchestrator
    CreateInjected(std::shared_ptr<OperationJournal> _journal,
                   std::shared_ptr<Pool> _pool,
                   ExecutionFactory _execution_factory,
                   std::shared_ptr<CopyOperationRunReceiptCustodian> _run_receipt_custodian)
    {
        return CopyOperationOrchestrator{std::move(_journal),
                                         std::move(_pool),
                                         std::move(_execution_factory),
                                         std::move(_run_receipt_custodian)};
    }

    [[nodiscard]] static CopyOperationOrchestrator CreateProductionWithResolver(
        std::shared_ptr<OperationJournal> _journal,
        std::shared_ptr<Pool> _pool,
        std::shared_ptr<CopyOperationRunReceiptCustodian> _run_receipt_custodian,
        ConditionalCommitTransactionResolver _resolver)
    {
        auto orchestrator = CopyOperationOrchestrator{
            std::move(_journal), std::move(_pool), std::move(_run_receipt_custodian)};
        orchestrator.m_ConditionalCommitTransactionResolver = std::move(_resolver);
        return orchestrator;
    }

    [[nodiscard]] static std::expected<std::shared_ptr<Operation>, CopyOperationOrchestratorError>
    SubmitAdmitted(CopyOperationOrchestrator &_orchestrator,
                   ReviewedVFSOperationPreflight _reviewed,
                   OperationJournalAdmissionReceipt _admission,
                   CopyOperationOrchestrator::CancelChecker _cancel_checker = {},
                   CopyOperationSubmissionHooks _hooks = {},
                   PreEnqueueHandoff _pre_enqueue_handoff = {})
    {
        return _orchestrator.SubmitAdmitted(
            std::move(_reviewed),
            std::move(_admission),
            std::move(_cancel_checker),
            std::move(_hooks),
            std::move(_pre_enqueue_handoff));
    }
};

class CopyOperationRunReceiptCustodianTesting final
{
public:
    /**
     * Test-only custody path for multi-item terminal evidence. It requires an exact journal-issued Running receipt
     * and is not a production execution route; production retains the Orchestrator singleton-shape admission gate.
     */
    [[nodiscard]] static bool
    EnqueueExactTerminalEvidence(CopyOperationRunReceiptCustodian &_custodian,
                                 OperationPlan _plan,
                                 std::shared_ptr<OperationJournal> _journal,
                                 OperationJournalRunReceipt &&_receipt,
                                 CopyOperationExecutionProduct::TerminalEvidenceAccessor _accessor,
                                 const std::shared_ptr<Pool> &_pool,
                                 const std::shared_ptr<Operation> &_operation,
                                 std::function<void(const CopyOperationDurableTerminalOutcome &)> _terminal_observer = {});

    [[nodiscard]] static bool
    SetReleaseFinalizerBarrier(CopyOperationRunReceiptCustodian &_custodian,
                               std::string_view _plan_id,
                               std::function<void()> _barrier);
};

} // namespace nc::ops
