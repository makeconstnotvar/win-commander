// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "OperationRetryPolicy.h"

#include <algorithm>

namespace nc::ops {

namespace {

bool CouldDifferOnASecondAttempt(const OperationJournalItemError _error) noexcept
{
    switch( _error ) {
        // The world moved under a plan made against an older world; a fresh plan sees the new one.
        case OperationJournalItemError::SourceChanged:
        case OperationJournalItemError::DestinationChanged:
        // These can be transient: a busy disk, a link that came back, a lock that was released.
        case OperationJournalItemError::Read:
        case OperationJournalItemError::Write:
        case OperationJournalItemError::Commit:
        case OperationJournalItemError::Metadata:
        case OperationJournalItemError::Cleanup:
        case OperationJournalItemError::Cancelled:
            return true;
        // Permission does not grant itself between attempts, and an error nobody could name is not
        // evidence that a second attempt will go better. Retrying either turns a failure into a loop.
        case OperationJournalItemError::PermissionDenied:
        case OperationJournalItemError::Unknown:
        case OperationJournalItemError::None:
            return false;
    }
    return false;
}

} // namespace

OperationRetryDecision DecideOperationItemRetry(const OperationJournalItemResult &_result) noexcept
{
    const auto refuse = [&](const OperationRetryRefusal _refusal) {
        return OperationRetryDecision{.item_index = _result.item_index, .refusal = _refusal};
    };

    switch( _result.status ) {
        case OperationJournalItemStatus::Succeeded:
            // Copying it again could overwrite something changed at the destination since. A retry
            // that destroys work is worse than one that does nothing.
            return refuse(OperationRetryRefusal::AlreadyDone);
        case OperationJournalItemStatus::Skipped:
            // Skipping was an answer - from a conflict policy or from the user. Revisiting it
            // silently overrides a decision already made.
            return refuse(OperationRetryRefusal::DeliberatelySkipped);
        case OperationJournalItemStatus::Cancelled:
            break;
        case OperationJournalItemStatus::Failed:
            if( !CouldDifferOnASecondAttempt(_result.error) )
                return refuse(OperationRetryRefusal::NotRetryable);
            break;
    }

    // Checked after the status, and last: not knowing whether the destination was written is a
    // stronger objection than any reason the attempt was allowed. A blind repeat could overwrite
    // what did land, or leave a second copy beside it.
    if( _result.destination_publication == OperationJournalPublicationState::Unknown )
        return refuse(OperationRetryRefusal::NeedsInspection);

    return OperationRetryDecision{.item_index = _result.item_index, .refusal = OperationRetryRefusal::None};
}

std::vector<OperationRetryDecision> DecideOperationRetry(const std::vector<OperationJournalItemResult> &_results)
{
    std::vector<OperationRetryDecision> decisions;
    decisions.reserve(_results.size());
    for( const OperationJournalItemResult &result : _results )
        decisions.push_back(DecideOperationItemRetry(result));
    return decisions;
}

bool OperationHasRetryableItems(const std::vector<OperationJournalItemResult> &_results)
{
    return std::ranges::any_of(_results, [](const OperationJournalItemResult &_result) {
        return DecideOperationItemRetry(_result).Retryable();
    });
}

} // namespace nc::ops
