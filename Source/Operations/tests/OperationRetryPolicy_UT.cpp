// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include "../source/OperationRetryPolicy.h"

#include <vector>

using namespace nc::ops;

namespace {

OperationJournalItemResult Item(const size_t _index,
                                const OperationJournalItemStatus _status,
                                const OperationJournalItemError _error = OperationJournalItemError::None,
                                const OperationJournalPublicationState _publication =
                                    OperationJournalPublicationState::NotPublished)
{
    OperationJournalItemResult result;
    result.item_index = _index;
    result.status = _status;
    result.error = _error;
    result.destination_publication = _publication;
    return result;
}

} // namespace

#define PREFIX "nc::ops::DecideOperationRetry "

TEST_CASE(PREFIX "never repeats an item that already arrived")
{
    // Copying it again could overwrite something changed at the destination since. A retry that
    // destroys work is worse than one that does nothing.
    const auto decision = DecideOperationItemRetry(
        Item(0, OperationJournalItemStatus::Succeeded, OperationJournalItemError::None,
             OperationJournalPublicationState::Published));
    CHECK_FALSE(decision.Retryable());
    CHECK(decision.refusal == OperationRetryRefusal::AlreadyDone);
    CHECK(decision.item_index == 0);
}

TEST_CASE(PREFIX "does not quietly revisit something that was skipped on purpose")
{
    // Skipping was an answer - from a conflict policy or from the user - and a retry that returns to
    // it overrides a decision already made, without asking again.
    const auto decision = DecideOperationItemRetry(Item(3, OperationJournalItemStatus::Skipped));
    CHECK_FALSE(decision.Retryable());
    CHECK(decision.refusal == OperationRetryRefusal::DeliberatelySkipped);
    CHECK(decision.item_index == 3);
}

TEST_CASE(PREFIX "always offers a cancelled item again")
{
    // Nothing about it was decided; the operation simply stopped.
    const auto decision = DecideOperationItemRetry(Item(1, OperationJournalItemStatus::Cancelled));
    CHECK(decision.Retryable());
}

TEST_CASE(PREFIX "offers a failure again only when a second attempt could differ")
{
    for( const auto error : {OperationJournalItemError::SourceChanged,
                             OperationJournalItemError::DestinationChanged,
                             OperationJournalItemError::Read,
                             OperationJournalItemError::Write,
                             OperationJournalItemError::Commit,
                             OperationJournalItemError::Metadata,
                             OperationJournalItemError::Cleanup} ) {
        const auto decision = DecideOperationItemRetry(Item(0, OperationJournalItemStatus::Failed, error));
        CHECK(decision.Retryable());
    }

    // Permission does not grant itself between attempts, and an error nobody could name is not
    // evidence that a second attempt will go better. Retrying either turns a failure into a loop.
    for( const auto error : {OperationJournalItemError::PermissionDenied, OperationJournalItemError::Unknown} ) {
        const auto decision = DecideOperationItemRetry(Item(0, OperationJournalItemStatus::Failed, error));
        CHECK_FALSE(decision.Retryable());
        CHECK(decision.refusal == OperationRetryRefusal::NotRetryable);
    }
}

TEST_CASE(PREFIX "will not repeat blindly when it does not know what reached the destination")
{
    // Stronger than any reason the attempt would otherwise be allowed: a blind repeat could overwrite
    // what did land, or leave a second copy beside it. "Look at this first" is a different answer
    // from "no".
    const auto after_failure = DecideOperationItemRetry(
        Item(2, OperationJournalItemStatus::Failed, OperationJournalItemError::Write,
             OperationJournalPublicationState::Unknown));
    CHECK_FALSE(after_failure.Retryable());
    CHECK(after_failure.refusal == OperationRetryRefusal::NeedsInspection);

    const auto after_cancel = DecideOperationItemRetry(
        Item(2, OperationJournalItemStatus::Cancelled, OperationJournalItemError::Cancelled,
             OperationJournalPublicationState::Unknown));
    CHECK(after_cancel.refusal == OperationRetryRefusal::NeedsInspection);

    // A known publication state does not block anything: the retryable failure above stays
    // retryable once it is known nothing was published.
    const auto known = DecideOperationItemRetry(
        Item(2, OperationJournalItemStatus::Failed, OperationJournalItemError::Write,
             OperationJournalPublicationState::NotPublished));
    CHECK(known.Retryable());
}

TEST_CASE(PREFIX "a succeeded item stays refused for the reason that matters most")
{
    // Both objections apply here, and the one reported is the one that stops a user reaching for
    // "inspect and retry" on something that is simply finished.
    const auto decision = DecideOperationItemRetry(
        Item(0, OperationJournalItemStatus::Succeeded, OperationJournalItemError::None,
             OperationJournalPublicationState::Unknown));
    CHECK(decision.refusal == OperationRetryRefusal::AlreadyDone);
}

TEST_CASE(PREFIX "reports every item in journal order and answers whether anything can be retried")
{
    const std::vector<OperationJournalItemResult> results{
        Item(0, OperationJournalItemStatus::Succeeded),
        Item(1, OperationJournalItemStatus::Failed, OperationJournalItemError::Read),
        Item(2, OperationJournalItemStatus::Skipped),
        Item(3, OperationJournalItemStatus::Cancelled),
    };

    const auto decisions = DecideOperationRetry(results);
    REQUIRE(decisions.size() == 4);
    CHECK(decisions[0].refusal == OperationRetryRefusal::AlreadyDone);
    CHECK(decisions[1].Retryable());
    CHECK(decisions[2].refusal == OperationRetryRefusal::DeliberatelySkipped);
    CHECK(decisions[3].Retryable());
    for( size_t i = 0; i < decisions.size(); ++i )
        CHECK(decisions[i].item_index == i);

    CHECK(OperationHasRetryableItems(results));
}

TEST_CASE(PREFIX "offers nothing to retry when there is nothing worth retrying")
{
    // What a Retry control is enabled by. Offering it on an operation where every item is finished or
    // deliberately skipped would promise something pressing it cannot deliver.
    CHECK_FALSE(OperationHasRetryableItems({}));
    CHECK_FALSE(OperationHasRetryableItems({
        Item(0, OperationJournalItemStatus::Succeeded),
        Item(1, OperationJournalItemStatus::Skipped),
        Item(2, OperationJournalItemStatus::Failed, OperationJournalItemError::PermissionDenied),
    }));
}

#undef PREFIX
