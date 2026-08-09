// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ProviderConditionalCopyOperation.h"
#include "ProviderConditionalCopyOperationTesting.h"

#include "Job.h"

#include <mutex>
#include <optional>
#include <utility>

namespace nc::ops {
namespace {

/**
 * Terminal outcomes for the whole batch, one slot per item.
 *
 * A slot is resolved once, by whichever path terminated its transaction. A resolved slot may hold no
 * result at all: aborting a transaction that never executed is a legitimate terminal that the journal
 * schema has nothing to say about, which is exactly what the mapper's non-execution refusal means.
 */
class ProviderConditionalCopyOperationTerminalState final
{
public:
    explicit ProviderConditionalCopyOperationTerminalState(size_t _items) : m_Slots(_items) {}

    /**
     * Records item `_index`'s provider outcome and returns the status that stands for that item -
     * nothing when the item ends without a journal result. The first publication for a slot wins, so
     * a later path retracing the same item reads back the outcome that was already recorded.
     */
    [[nodiscard]] std::optional<OperationJournalItemStatus>
    Publish(size_t _index,
            const vfs::ProviderConditionalCopyCommitResult &_provider_result,
            ProviderConditionalCopyJournalContext _journal_context) noexcept
    {
        const auto mapped =
            MapProviderConditionalCopyCommitResultToJournalItemResult(_provider_result, _journal_context);
        const auto guard = std::lock_guard{m_Mutex};
        if( _index >= m_Slots.size() ) {
            m_Inconsistent = true;
            return std::nullopt;
        }
        auto &slot = m_Slots[_index];
        if( slot.resolved )
            return slot.result ? std::optional{slot.result->status} : std::nullopt;
        slot.resolved = true;
        if( !mapped ) {
            // Two refusals with opposite meanings. A result that could not be mapped at all leaves
            // the whole snapshot untrustworthy; a transaction aborted before it executed simply has
            // no item result, and saying so is not the same as saying something went wrong.
            if( mapped.error() != ProviderConditionalCopyJournalMappingError::NonExecutionTerminal )
                m_Inconsistent = true;
            return std::nullopt;
        }
        slot.result = *mapped;
        return slot.result->status;
    }

    [[nodiscard]] std::expected<CopyOperationTerminalEvidence, CopyOperationTerminalResultError>
    ReadEvidence() const
    {
        const auto guard = std::lock_guard{m_Mutex};
        if( m_Inconsistent )
            return std::unexpected(CopyOperationTerminalResultError::Inconsistent);
        for( const auto &slot : m_Slots ) {
            if( !slot.resolved )
                return std::unexpected(CopyOperationTerminalResultError::Pending);
        }

        bool failed = false;
        bool cancelled = false;
        bool resultless = false;
        for( const auto &slot : m_Slots ) {
            if( !slot.result ) {
                resultless = true;
                continue;
            }
            failed = failed || slot.result->status == OperationJournalItemStatus::Failed;
            cancelled = cancelled || slot.result->status == OperationJournalItemStatus::Cancelled;
        }

        std::vector<OperationJournalItemResult> results;
        for( const auto &slot : m_Slots ) {
            if( !slot.result )
                continue;
            // A failure and a cancellation cannot appear in one journal entry, and of the two only
            // the failure carries a consequence: a cancelled item is always NotPublished, so leaving
            // it out withholds nothing about what is on disk, while dropping the failure would hide
            // a destination that may exist behind an outcome that says none does.
            if( failed && cancelled && slot.result->status == OperationJournalItemStatus::Cancelled )
                continue;
            results.push_back(*slot.result);
        }
        // Nothing executed at all. There is no terminal outcome to report - the cold-abort case,
        // which the application boundary reads as its integration blocker rather than as a run.
        //
        // And a run that neither failed nor was cancelled has to account for every item: an entry may
        // legally omit the items behind a failure or a cancellation, but a completed one may not, and
        // an item whose provider answered with a terminal that precedes execution has no result to
        // give. Calling that batch completed would produce an entry the journal refuses - which is
        // not a visible error but a slot latched into contract violation. The single-item path has
        // always answered the same event this way, because there the omitted item was the only one.
        if( results.empty() || (resultless && !failed && !cancelled) )
            return std::unexpected(CopyOperationTerminalResultError::Inconsistent);

        const auto state = failed      ? OperationJournalState::Failed
                           : cancelled ? OperationJournalState::Cancelled
                                       : OperationJournalState::Completed;
        return CopyOperationTerminalEvidence{
            .state = state,
            .item_results = std::move(results),
        };
    }

private:
    struct Slot final {
        bool resolved{false};
        std::optional<OperationJournalItemResult> result;
    };

    mutable std::mutex m_Mutex;
    std::vector<Slot> m_Slots;
    bool m_Inconsistent{false};
};

uint64_t ProviderConditionalCopyOperationTotalBytes(
    const std::vector<ProviderConditionalCopyOperationItem> &_items) noexcept
{
    uint64_t total = 0;
    for( const auto &item : _items )
        total += item.journal_context.exact_source_bytes;
    return total;
}

class ProviderConditionalCopyOperationJob final : public Job
{
public:
    ProviderConditionalCopyOperationJob(std::vector<ProviderConditionalCopyOperationItem> _items,
                                        vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker,
                                        std::shared_ptr<ProviderConditionalCopyOperationTerminalState> _terminal_state,
                                        ProviderConditionalCopyOperationTestHooks _hooks)
        : m_Items{std::move(_items)}, m_CancelChecker{std::move(_cancel_checker)},
          m_TerminalState{std::move(_terminal_state)}, m_Hooks{std::move(_hooks)}
    {
        // Items, not bytes, because a conditional copy publishes atomically: an item's bytes go from
        // none to all at its commit, so a byte fraction would sit still and then jump. The byte total
        // is still committed - it is what tells the user how much the batch weighs.
        Statistics().SetPreferredSource(Statistics::SourceType::Items);
        Statistics().CommitEstimated(Statistics::SourceType::Items, m_Items.size());
        Statistics().CommitEstimated(Statistics::SourceType::Bytes,
                                     ProviderConditionalCopyOperationTotalBytes(m_Items));
    }

    ~ProviderConditionalCopyOperationJob() override { AbortColdItems(); }

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

        bool cancelled = false;
        size_t next = 0;
        try {
            while( next != m_Items.size() ) {
                if( m_Hooks.before_item_start )
                    m_Hooks.before_item_start(next);
                // Between items is the only place a pause can mean anything: a commit cannot be
                // interrupted, so pausing anywhere else would report a state the operation is not in.
                BlockIfPaused();
                bool stop_accepted = false;
                {
                    // Both halves under one lock, because they are one decision. A stop is accepted
                    // on the strength of "this item has not started", so if the answer is read here
                    // and the start published there, a stop can be accepted for an item that is
                    // committed a moment later - and the caller was told it had been cancelled.
                    const auto guard = std::lock_guard{m_Gate};
                    stop_accepted = m_StopAccepted;
                    if( !stop_accepted )
                        m_StartedItems = next + 1;
                }
                if( stop_accepted ) {
                    // Only the worker may terminate a transaction once it owns the sequence, so an
                    // accepted stop leaves the cancelling to here.
                    cancelled = true;
                    break;
                }
                PublishCurrentItemPath(m_Items[next].presentation.source_path);
                const auto status = CommitItem(next, m_CancelChecker);
                ++next;
                if( status == OperationJournalItemStatus::Succeeded )
                    continue;
                // Anything else ends the run. Carrying on after a surprise would spend evidence that
                // was checked before execution began on a world that has just proved it moved - and a
                // batch that failed one item and then cancelled the rest cannot be journalled at all.
                cancelled = status == OperationJournalItemStatus::Cancelled;
                break;
            }
        } catch( ... ) {
            // Whoever claims the sequence owes every item a terminal, and nothing else can pay that
            // debt afterwards: a stop finds the sequence already claimed and the destructor only acts
            // on an untouched job, so an unresolved slot means evidence that never arrives and an
            // operation the Pool holds forever waiting for it. The wind-down below settles them, and
            // it settles them as cancelled: the items that did commit stay committed, and a run that
            // stopped short of the rest is not a completed one.
            cancelled = true;
        }

        // What the untouched items become depends on why the run ended. A cancellation may still say
        // "cancelled" about them; a failure may not, because one journal entry cannot hold both, so
        // they are aborted and report nothing instead.
        for( size_t remaining = next; remaining != m_Items.size(); ++remaining ) {
            if( cancelled )
                (void)CommitItem(remaining, [] { return true; });
            else
                AbandonItem(remaining);
        }

        {
            const auto guard = std::lock_guard{m_Gate};
            m_StartedItems = m_Items.size();
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
            if( m_GatePhase == GatePhase::CommitOwned ) {
                // The worker owns the sequence. Accepting the stop is honest exactly while items
                // remain that it has not begun - those it will cancel once it reads the flag set
                // here, under this same lock. With the last item already committing there is
                // nothing left to interrupt, and an irreversible commit owns the terminal decision.
                if( m_StartedItems == m_Items.size() )
                    return false;
                m_StopAccepted = true;
                return true;
            }
            if( m_GatePhase != GatePhase::Pending )
                return false;
            m_GatePhase = GatePhase::StopOwned;
        }

        CancelAllItems();
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

        CancelAllItems();
    }

    /** Commits one item and reports it. Returns the status that stands for it, if it has one. */
    [[nodiscard]] std::optional<OperationJournalItemStatus>
    CommitItem(size_t _index, const vfs::ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) noexcept
    {
        const auto provider_result = m_Items[_index].transaction->Commit(_cancel_checker);
        const auto status =
            m_TerminalState->Publish(_index, provider_result, m_Items[_index].journal_context);
        ReportItemPresentation(_index, status);
        return status;
    }

    /** Ends an item that never ran. An aborted transaction has no journal result to report. */
    void AbandonItem(size_t _index) noexcept
    {
        const auto provider_result = m_Items[_index].transaction->Abort();
        const auto status =
            m_TerminalState->Publish(_index, provider_result, m_Items[_index].journal_context);
        ReportItemPresentation(_index, status);
    }

    void CancelAllItems() noexcept
    {
        for( size_t index = 0; index != m_Items.size(); ++index )
            (void)CommitItem(index, [] { return true; });
        const auto guard = std::lock_guard{m_Gate};
        m_StartedItems = m_Items.size();
        m_GatePhase = GatePhase::Terminal;
    }

    void ReportItemPresentation(size_t _index, std::optional<OperationJournalItemStatus> _status) noexcept
    {
        const auto &item = m_Items[_index];
        const bool succeeded = _status == OperationJournalItemStatus::Succeeded;

        try {
            const auto source = Statistics::SourceType::Items;
            const auto bytes = Statistics::SourceType::Bytes;
            if( succeeded ) {
                Statistics().CommitProcessed(source, 1);
                Statistics().CommitProcessed(bytes, item.journal_context.exact_source_bytes);
            }
            else {
                // Its own byte count, not a share of the total: skipping anything else would leave
                // the estimate describing a batch that was never asked for.
                Statistics().CommitSkipped(source, 1);
                Statistics().CommitSkipped(bytes, item.journal_context.exact_source_bytes);
            }
        } catch( ... ) {
        }

        try {
            TellItemReport(ItemStateReport{
                .host = *item.presentation.source_host,
                .path = item.presentation.source_path,
                .status = succeeded ? ItemStatus::Processed : ItemStatus::Skipped,
            });
        } catch( ... ) {
        }
    }

    void AbortColdItems() noexcept
    {
        {
            const auto guard = std::lock_guard{m_Gate};
            if( m_GatePhase != GatePhase::Pending )
                return;
            m_GatePhase = GatePhase::DestructionOwned;
        }

        for( size_t index = 0; index != m_Items.size(); ++index )
            AbandonItem(index);
        const auto guard = std::lock_guard{m_Gate};
        m_StartedItems = m_Items.size();
        m_GatePhase = GatePhase::Terminal;
    }

    std::vector<ProviderConditionalCopyOperationItem> m_Items;
    vfs::ProviderConditionalCopyTransaction::CancelChecker m_CancelChecker;
    std::shared_ptr<ProviderConditionalCopyOperationTerminalState> m_TerminalState;
    ProviderConditionalCopyOperationTestHooks m_Hooks;
    std::mutex m_Gate;
    GatePhase m_GatePhase{GatePhase::Pending};
    size_t m_StartedItems{0};
    bool m_StopAccepted{false};
};

class ProviderConditionalCopyOperation final : public Operation
{
public:
    ProviderConditionalCopyOperation(std::vector<ProviderConditionalCopyOperationItem> _items,
                                     std::string _title,
                                     vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker,
                                     std::shared_ptr<ProviderConditionalCopyOperationTerminalState> _terminal_state,
                                     ProviderConditionalCopyOperationTestHooks _hooks)
        : m_Job{std::make_unique<ProviderConditionalCopyOperationJob>(std::move(_items),
                                                                      std::move(_cancel_checker),
                                                                      std::move(_terminal_state),
                                                                      std::move(_hooks))}
    {
        SetTitle(std::move(_title));
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

std::string ProviderConditionalCopyOperationTitle(const std::vector<ProviderConditionalCopyOperationItem> &_items)
{
    // One item keeps naming both ends, which is what a title is for when there is only one of each.
    // Several of them name no path at all rather than one item's: picking the first would describe
    // the batch by whichever item happens to lead it. The file being copied right now travels on the
    // current-item channel instead, where a display can follow it as it changes.
    if( _items.size() == 1 ) {
        return "Copying " + _items.front().presentation.source_path + " \u2192 " +
               _items.front().presentation.destination_path;
    }
    return "Copying " + std::to_string(_items.size()) + " items";
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
ProviderConditionalCopyOperationFactory::CreateBatch(
    std::vector<ProviderConditionalCopyOperationItem> _items,
    vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker) noexcept
{
    return CreateBatchForTesting(std::move(_items), std::move(_cancel_checker), {});
}

std::expected<CopyOperationExecutionProduct, ProviderConditionalCopyOperationConstructionError>
ProviderConditionalCopyOperationFactory::CreateForTesting(
    std::unique_ptr<vfs::ProviderConditionalCopyTransaction> _transaction,
    ProviderConditionalCopyJournalContext _journal_context,
    ProviderConditionalCopyOperationPresentation _presentation,
    vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker,
    ProviderConditionalCopyOperationTestHooks _hooks) noexcept
{
    // One item is a batch of one, and goes through the same construction: two paths would be two
    // places for the terminal contract to drift apart.
    if( !_transaction )
        return std::unexpected(ProviderConditionalCopyOperationConstructionError::MissingTransaction);
    try {
        std::vector<ProviderConditionalCopyOperationItem> items;
        items.push_back(ProviderConditionalCopyOperationItem{
            .transaction = std::move(_transaction),
            .journal_context = _journal_context,
            .presentation = std::move(_presentation),
        });
        return CreateBatchForTesting(std::move(items), std::move(_cancel_checker), std::move(_hooks));
    } catch( ... ) {
        return std::unexpected(ProviderConditionalCopyOperationConstructionError::AllocationFailed);
    }
}

std::expected<CopyOperationExecutionProduct, ProviderConditionalCopyOperationConstructionError>
ProviderConditionalCopyOperationFactory::CreateBatchForTesting(
    std::vector<ProviderConditionalCopyOperationItem> _items,
    vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker,
    ProviderConditionalCopyOperationTestHooks _hooks) noexcept
{
    if( _items.empty() )
        return std::unexpected(ProviderConditionalCopyOperationConstructionError::EmptyBatch);
    std::optional<size_t> previous_journal_index;
    for( const auto &item : _items ) {
        if( !item.transaction )
            return std::unexpected(ProviderConditionalCopyOperationConstructionError::MissingTransaction);
        if( !ProviderConditionalCopyOperationPresentationIsValid(item.presentation) )
            return std::unexpected(ProviderConditionalCopyOperationConstructionError::InvalidPresentation);
        // The numbering the journal will accept, checked before a single transaction is committed.
        // Asked here and only here: the contexts do not change afterwards, so a set that passes this
        // cannot fail it later - and asking again at the terminal would be asking after the copies.
        if( previous_journal_index && *previous_journal_index >= item.journal_context.item_index )
            return std::unexpected(ProviderConditionalCopyOperationConstructionError::InvalidJournalIndices);
        previous_journal_index = item.journal_context.item_index;
    }

    try {
        auto terminal_state = std::make_shared<ProviderConditionalCopyOperationTerminalState>(_items.size());
        auto title = ProviderConditionalCopyOperationTitle(_items);
        auto operation = std::make_shared<ProviderConditionalCopyOperation>(
            std::move(_items),
            std::move(title),
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

std::expected<CopyOperationExecutionProduct, ProviderConditionalCopyOperationConstructionError>
ProviderConditionalCopyOperationTesting::CreateBatch(
    std::vector<ProviderConditionalCopyOperationItem> _items,
    vfs::ProviderConditionalCopyTransaction::CancelChecker _cancel_checker,
    ProviderConditionalCopyOperationTestHooks _hooks) noexcept
{
    return ProviderConditionalCopyOperationFactory::CreateBatchForTesting(
        std::move(_items), std::move(_cancel_checker), std::move(_hooks));
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
