// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Operations/CopyOperationOrchestrator.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nc::panel::actions::reviewed_copy_as {

/**
 * What the application must do about a durable terminal outcome, decided over the whole set of item
 * results rather than over one of them.
 *
 * This exists because the projection it replaces could not answer the question for a batch at all:
 * `SingleItemResult()` is a compatibility accessor that returns nothing whenever the outcome carries
 * other than exactly one result, and the presenter read that nothing as "the journal has no terminal
 * item result", which is the sentence it says when a run must be reconciled by hand. A batch in which
 * every item published would have been announced to the user as a copy that failed and needs recovery.
 * Nothing produces such an outcome in production today - `Copy As` is the only producer of a reviewed
 * plan and it builds a single source - so this is the surface being made ready for a producer, not a
 * defect anyone can currently reach.
 */
enum class DurableCopyOutcomeKind : uint8_t {
    /** Every result accounted for, every one published. The panel catches up; nothing is said. */
    Published,
    /** Nothing to tell the user: the cancellation is the answer they already have. */
    Silent,
    /** Something must be said, and `attention_indices` says which results are worth saying it about. */
    Attention
};

struct DurableCopyOutcomePresentation final {
    DurableCopyOutcomeKind kind{DurableCopyOutcomeKind::Attention};

    /** Something may exist on disk that the panel is not showing - including a partially run batch. */
    bool refresh_panel{false};

    /**
     * Only ever true for a single published item. A batch has no one destination to reveal, and
     * picking one of several would be a guess dressed as an answer; the refresh alone is honest.
     */
    bool focus_single_publication{false};

    /** Positions in `item_results` - not journal item indices - of the results worth reporting. */
    std::vector<size_t> attention_indices;

    /**
     * The terminal carries no item result at all. Distinct from an empty `attention_indices`: this is
     * the run whose evidence cannot be read item by item, and the only case that still deserves the
     * reconciliation sentence.
     */
    bool without_item_results{false};

    size_t published_items{0};
    size_t total_items{0};

    bool operator==(const DurableCopyOutcomePresentation &) const = default;
};

/**
 * One item is worth reporting unless it published exactly as asked, or was cancelled - a cancelled
 * item is always `NotPublished`, so saying nothing about it withholds nothing about the disk.
 */
[[nodiscard]] inline bool DurableCopyItemNeedsAttention(const nc::ops::OperationJournalItemResult &_result) noexcept
{
    if( _result.status == nc::ops::OperationJournalItemStatus::Cancelled )
        return false;
    return _result.status != nc::ops::OperationJournalItemStatus::Succeeded ||
           _result.destination_publication != nc::ops::OperationJournalPublicationState::Published;
}

/** True while anything in this outcome may have reached the destination directory. */
[[nodiscard]] inline bool DurableCopyItemMayExist(const nc::ops::OperationJournalItemResult &_result) noexcept
{
    return _result.destination_publication != nc::ops::OperationJournalPublicationState::NotPublished;
}

[[nodiscard]] inline DurableCopyOutcomePresentation
ClassifyDurableCopyOutcome(const nc::ops::CopyOperationDurableTerminalOutcome &_outcome)
{
    DurableCopyOutcomePresentation presentation;
    presentation.total_items = _outcome.item_results.size();
    presentation.without_item_results = _outcome.item_results.empty();
    for( const auto &result : _outcome.item_results ) {
        if( result.status == nc::ops::OperationJournalItemStatus::Succeeded &&
            result.destination_publication == nc::ops::OperationJournalPublicationState::Published )
            ++presentation.published_items;
    }

    // Cancellation is answered first, and it is never an alert: the user asked for this and telling
    // them again is noise. What changes for a set is the refresh - a batch stopped part-way has
    // legitimately published everything before the stop, and those files have to appear. A lone
    // cancelled item is always `NotPublished`, so this keeps the single-item behaviour exactly.
    if( _outcome.state == nc::ops::OperationJournalState::Cancelled ) {
        presentation.kind = DurableCopyOutcomeKind::Silent;
        for( const auto &result : _outcome.item_results )
            presentation.refresh_panel = presentation.refresh_panel || DurableCopyItemMayExist(result);
        return presentation;
    }

    // Success is the state and the items agreeing. `Completed` is what says the entry accounts for
    // every source the plan named - an entry reporting a failure may legally omit the items behind
    // it, so all-published results under any other state describe a set that is not the whole set.
    if( _outcome.state == nc::ops::OperationJournalState::Completed && !_outcome.item_results.empty() &&
        presentation.published_items == _outcome.item_results.size() ) {
        presentation.kind = DurableCopyOutcomeKind::Published;
        presentation.refresh_panel = true;
        presentation.focus_single_publication = _outcome.item_results.size() == 1;
        return presentation;
    }

    presentation.kind = DurableCopyOutcomeKind::Attention;
    // A terminal with no results at all cannot say what is on disk, so the panel refreshes on the
    // possibility. With results, only a publication - or one that cannot be ruled out - earns it.
    presentation.refresh_panel = _outcome.item_results.empty();
    for( size_t index = 0; index != _outcome.item_results.size(); ++index ) {
        const auto &result = _outcome.item_results[index];
        presentation.refresh_panel = presentation.refresh_panel || DurableCopyItemMayExist(result);
        if( DurableCopyItemNeedsAttention(result) )
            presentation.attention_indices.push_back(index);
    }
    return presentation;
}

} // namespace nc::panel::actions::reviewed_copy_as
