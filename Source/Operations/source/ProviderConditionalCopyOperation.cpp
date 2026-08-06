// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ProviderConditionalCopyOperation.h"
#include "ProviderConditionalCopyOperationTesting.h"

#include "Job.h"

#include <mutex>
#include <optional>
#include <utility>

namespace nc::ops {
namespace {

class ProviderConditionalCopyOperationTerminalState final
{
public:
    [[nodiscard]] std::expected<OperationJournalItemResult, CopyOperationTerminalResultError> Read() const noexcept
    {
        const auto guard = std::lock_guard{m_Mutex};
        if( m_Result )
            return *m_Result;
        if( m_Inconsistent )
            return std::unexpected(CopyOperationTerminalResultError::Inconsistent);
        return std::unexpected(CopyOperationTerminalResultError::Pending);
    }

    [[nodiscard]] std::expected<CopyOperationTerminalEvidence, CopyOperationTerminalResultError>
    ReadEvidence() const
    {
        auto item_result = Read();
        if( !item_result )
            return std::unexpected(item_result.error());

        std::optional<OperationJournalState> state;
        switch( item_result->status ) {
            case OperationJournalItemStatus::Succeeded:
            case OperationJournalItemStatus::Skipped:
                state = OperationJournalState::Completed;
                break;
            case OperationJournalItemStatus::Failed:
                state = OperationJournalState::Failed;
                break;
            case OperationJournalItemStatus::Cancelled:
                state = OperationJournalState::Cancelled;
                break;
        }
        if( !state )
            return std::unexpected(CopyOperationTerminalResultError::Inconsistent);
        return CopyOperationTerminalEvidence{
            .state = *state,
            .item_results = {std::move(*item_result)},
        };
    }

    [[nodiscard]] bool Publish(const vfs::ProviderConditionalCopyCommitResult &_provider_result,
                               ProviderConditionalCopyJournalContext _journal_context) noexcept
    {
        const auto mapped =
            MapProviderConditionalCopyCommitResultToJournalItemResult(_provider_result, _journal_context);
        const auto guard = std::lock_guard{m_Mutex};
        if( m_Result || m_Inconsistent )
            return m_Result && m_Result->status == OperationJournalItemStatus::Cancelled;
        if( !mapped ) {
            m_Inconsistent = true;
            return false;
        }
        m_Result = *mapped;
        return m_Result->status == OperationJournalItemStatus::Cancelled;
    }

private:
    mutable std::mutex m_Mutex;
    std::optional<OperationJournalItemResult> m_Result;
    bool m_Inconsistent{false};
};

class ProviderConditionalCopyOperationJob final : public Job
{
public:
    ProviderConditionalCopyOperationJob(std::unique_ptr<vfs::ProviderConditionalCopyTransaction> _transaction,
                                        ProviderConditionalCopyJournalContext _journal_context,
                                        ProviderConditionalCopyOperationPresentation _presentation,
                                        vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker,
                                        std::shared_ptr<ProviderConditionalCopyOperationTerminalState> _terminal_state,
                                        ProviderConditionalCopyOperationTestHooks _hooks)
        : m_Transaction{std::move(_transaction)}, m_JournalContext{_journal_context},
          m_Presentation{std::move(_presentation)}, m_CancelChecker{std::move(_cancel_checker)},
          m_TerminalState{std::move(_terminal_state)}, m_Hooks{std::move(_hooks)}
    {
        Statistics().SetPreferredSource(Statistics::SourceType::Items);
        Statistics().CommitEstimated(Statistics::SourceType::Items, 1);
    }

    ~ProviderConditionalCopyOperationJob() override { AbortColdTransaction(); }

private:
    enum class GatePhase : uint8_t {
        Pending,
        CommitOwned,
        StopOwned,
        DestructionOwned,
        TerminalCancelled,
        Terminal
    };

    void LaunchWorker(std::shared_ptr<void> _worker_keep_alive) override
    {
        if( m_Hooks.before_worker_launch )
            m_Hooks.before_worker_launch();
        Job::LaunchWorker(std::move(_worker_keep_alive));
    }

    void Perform() override
    {
        if( m_Hooks.before_commit_gate )
            m_Hooks.before_commit_gate();

        {
            const auto guard = std::lock_guard{m_Gate};
            if( m_GatePhase != GatePhase::Pending )
                return;
            m_GatePhase = GatePhase::CommitOwned;
        }

        const auto provider_result = m_Transaction->Commit(m_CancelChecker);
        const bool cancelled = m_TerminalState->Publish(provider_result, m_JournalContext);
        ReportTerminalPresentation();
        {
            const auto guard = std::lock_guard{m_Gate};
            m_GatePhase = cancelled ? GatePhase::TerminalCancelled : GatePhase::Terminal;
        }
        if( cancelled )
            (void)Stop();
    }

    bool OnStopRequested() noexcept override
    {
        {
            const auto guard = std::lock_guard{m_Gate};
            if( m_GatePhase == GatePhase::TerminalCancelled ) {
                m_GatePhase = GatePhase::Terminal;
                return true;
            }
            if( m_GatePhase != GatePhase::Pending )
                return false;
            m_GatePhase = GatePhase::StopOwned;
        }

        const auto provider_result = m_Transaction->Commit([] { return true; });
        (void)m_TerminalState->Publish(provider_result, m_JournalContext);
        ReportTerminalPresentation();
        {
            const auto guard = std::lock_guard{m_Gate};
            m_GatePhase = GatePhase::Terminal;
        }
        return true;
    }

    void OnStopped() override
    {
        {
            const auto guard = std::lock_guard{m_Gate};
            if( m_GatePhase != GatePhase::Pending )
                return;
            m_GatePhase = GatePhase::StopOwned;
        }

        const auto provider_result = m_Transaction->Commit([] { return true; });
        (void)m_TerminalState->Publish(provider_result, m_JournalContext);
        ReportTerminalPresentation();
        const auto guard = std::lock_guard{m_Gate};
        m_GatePhase = GatePhase::Terminal;
    }

    void ReportTerminalPresentation() noexcept
    {
        const auto result = m_TerminalState->Read();
        const bool succeeded = result && result->status == OperationJournalItemStatus::Succeeded;

        try {
            if( succeeded )
                Statistics().CommitProcessed(Statistics::SourceType::Items, 1);
            else
                Statistics().CommitSkipped(Statistics::SourceType::Items, 1);
        } catch( ... ) {
        }

        try {
            TellItemReport(ItemStateReport{
                .host = *m_Presentation.source_host,
                .path = m_Presentation.source_path,
                .status = succeeded ? ItemStatus::Processed : ItemStatus::Skipped,
            });
        } catch( ... ) {
        }
    }

    void AbortColdTransaction() noexcept
    {
        {
            const auto guard = std::lock_guard{m_Gate};
            if( m_GatePhase != GatePhase::Pending )
                return;
            m_GatePhase = GatePhase::DestructionOwned;
        }

        const auto provider_result = m_Transaction->Abort();
        (void)m_TerminalState->Publish(provider_result, m_JournalContext);
        ReportTerminalPresentation();
        const auto guard = std::lock_guard{m_Gate};
        m_GatePhase = GatePhase::Terminal;
    }

    std::unique_ptr<vfs::ProviderConditionalCopyTransaction> m_Transaction;
    const ProviderConditionalCopyJournalContext m_JournalContext;
    const ProviderConditionalCopyOperationPresentation m_Presentation;
    vfs::ProviderConditionalCopyTransaction::CancelChecker m_CancelChecker;
    std::shared_ptr<ProviderConditionalCopyOperationTerminalState> m_TerminalState;
    ProviderConditionalCopyOperationTestHooks m_Hooks;
    std::mutex m_Gate;
    GatePhase m_GatePhase{GatePhase::Pending};
};

class ProviderConditionalCopyOperation final : public Operation
{
public:
    ProviderConditionalCopyOperation(std::unique_ptr<vfs::ProviderConditionalCopyTransaction> _transaction,
                                     ProviderConditionalCopyJournalContext _journal_context,
                                     ProviderConditionalCopyOperationPresentation _presentation,
                                     vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker,
                                     std::shared_ptr<ProviderConditionalCopyOperationTerminalState> _terminal_state,
                                     ProviderConditionalCopyOperationTestHooks _hooks)
        : m_Job{std::make_unique<ProviderConditionalCopyOperationJob>(std::move(_transaction),
                                                                      _journal_context,
                                                                      _presentation,
                                                                      std::move(_cancel_checker),
                                                                      std::move(_terminal_state),
                                                                      std::move(_hooks))}
    {
        SetTitle("Copying " + _presentation.source_path + " \u2192 " + _presentation.destination_path);
    }

    ~ProviderConditionalCopyOperation() override { Wait(); }

private:
    Job *GetJob() noexcept override { return m_Job.get(); }

    std::unique_ptr<ProviderConditionalCopyOperationJob> m_Job;
};

bool ProviderConditionalCopyOperationPresentationIsValid(
    const ProviderConditionalCopyOperationPresentation &_presentation) noexcept
{
    const auto valid_path = [](const std::string &_path) noexcept {
        return !_path.empty() && _path.front() == '/' && _path.find('\0') == std::string::npos;
    };
    return _presentation.source_host && valid_path(_presentation.source_path) &&
           valid_path(_presentation.destination_path);
}

vfs::ProviderConditionalCopyTransaction::CancelChecker ProviderConditionalCopyOperationSanitizeCancelChecker(
    vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker)
{
    return [cancel_checker = std::move(_cancel_checker)]() noexcept {
        if( !cancel_checker )
            return false;
        try {
            return cancel_checker();
        } catch( ... ) {
            return true;
        }
    };
}

} // namespace

std::expected<CopyOperationExecutionProduct, ProviderConditionalCopyOperationConstructionError>
ProviderConditionalCopyOperationFactory::Create(
    std::unique_ptr<vfs::ProviderConditionalCopyTransaction> _transaction,
    ProviderConditionalCopyJournalContext _journal_context,
    ProviderConditionalCopyOperationPresentation _presentation,
    vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker) noexcept
{
    return CreateForTesting(
        std::move(_transaction), _journal_context, std::move(_presentation), std::move(_cancel_checker), {});
}

std::expected<CopyOperationExecutionProduct, ProviderConditionalCopyOperationConstructionError>
ProviderConditionalCopyOperationFactory::CreateForTesting(
    std::unique_ptr<vfs::ProviderConditionalCopyTransaction> _transaction,
    ProviderConditionalCopyJournalContext _journal_context,
    ProviderConditionalCopyOperationPresentation _presentation,
    vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker,
    ProviderConditionalCopyOperationTestHooks _hooks) noexcept
{
    if( !_transaction )
        return std::unexpected(ProviderConditionalCopyOperationConstructionError::MissingTransaction);
    if( !ProviderConditionalCopyOperationPresentationIsValid(_presentation) )
        return std::unexpected(ProviderConditionalCopyOperationConstructionError::InvalidPresentation);

    try {
        auto terminal_state = std::make_shared<ProviderConditionalCopyOperationTerminalState>();
        auto operation = std::make_shared<ProviderConditionalCopyOperation>(
            std::move(_transaction),
            _journal_context,
            std::move(_presentation),
            ProviderConditionalCopyOperationSanitizeCancelChecker(std::move(_cancel_checker)),
            terminal_state,
            std::move(_hooks));
        auto terminal_evidence =
            [terminal_state = std::move(terminal_state)] { return terminal_state->ReadEvidence(); };
        return CopyOperationExecutionProduct{std::move(operation), std::move(terminal_evidence)};
    } catch( ... ) {
        return std::unexpected(ProviderConditionalCopyOperationConstructionError::AllocationFailed);
    }
}

std::expected<CopyOperationExecutionProduct, ProviderConditionalCopyOperationConstructionError>
ProviderConditionalCopyOperationTesting::Create(std::unique_ptr<vfs::ProviderConditionalCopyTransaction> _transaction,
                                                ProviderConditionalCopyJournalContext _journal_context,
                                                ProviderConditionalCopyOperationPresentation _presentation,
                                                vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker,
                                                ProviderConditionalCopyOperationTestHooks _hooks) noexcept
{
    return ProviderConditionalCopyOperationFactory::CreateForTesting(std::move(_transaction),
                                                                     _journal_context,
                                                                     std::move(_presentation),
                                                                     std::move(_cancel_checker),
                                                                     std::move(_hooks));
}

std::shared_ptr<Operation> &
ProviderConditionalCopyOperationTesting::Operation(CopyOperationExecutionProduct &_product) noexcept
{
    return _product.m_Operation;
}

CopyOperationExecutionProduct::TerminalItemResultAccessor &
ProviderConditionalCopyOperationTesting::TerminalItemResult(CopyOperationExecutionProduct &_product) noexcept
{
    return _product.m_TerminalItemResult;
}

CopyOperationExecutionProduct::TerminalEvidenceAccessor &
ProviderConditionalCopyOperationTesting::TerminalEvidence(CopyOperationExecutionProduct &_product) noexcept
{
    return _product.m_TerminalEvidence;
}

} // namespace nc::ops
