// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "OperationJournal.h"

#include <cstdint>
#include <vector>

namespace nc::ops {

/** Why an item from a finished operation may not be retried. */
enum class OperationRetryRefusal : uint8_t {
    /** It may. */
    None,
    /** It already succeeded - repeating it could undo work done since. */
    AlreadyDone,
    /** It was skipped by a decision, not by a failure. */
    DeliberatelySkipped,
    /** Nothing about this failure would differ on a second attempt. */
    NotRetryable,
    /**
     * It failed partway through publishing, and whether anything reached the destination is unknown.
     * Repeating it blind could overwrite or duplicate.
     */
    NeedsInspection
};

struct OperationRetryDecision {
    size_t item_index{0};
    OperationRetryRefusal refusal{OperationRetryRefusal::None};

    [[nodiscard]] bool Retryable() const noexcept { return refusal == OperationRetryRefusal::None; }

    friend bool operator==(const OperationRetryDecision &, const OperationRetryDecision &) = default;
};

/**
 * Whether one finished item may be attempted again, and why not when it may not.
 *
 * The rules, and what each is protecting:
 *
 * - **A succeeded item is never retried.** The file arrived; copying it again could overwrite
 *   something the user has since changed at the destination, and "retry" would have destroyed work
 *   rather than recovered it.
 * - **A skipped item is never retried either.** Skipping was a decision - a conflict policy or the
 *   user - and a retry that quietly revisits it overrides an answer already given.
 * - **A cancelled item is always retryable.** Nothing about it was decided; the operation simply
 *   stopped.
 * - **A failure is retryable only when a second attempt could differ.** `SourceChanged` and
 *   `DestinationChanged` mean the world moved under a plan, and a fresh plan sees the new world.
 *   `Read`, `Write` and `Commit` can be transient. `PermissionDenied` cannot resolve itself, and
 *   neither can an error nobody could name - retrying either is how a failure becomes a loop.
 * - **An unknown publication state overrides all of that.** If it is not known whether the
 *   destination was written, a blind retry can overwrite or duplicate. That has to be looked at
 *   before it can be repeated, which is a different answer from "no".
 */
[[nodiscard]] OperationRetryDecision DecideOperationItemRetry(const OperationJournalItemResult &_result) noexcept;

/** Every item of a finished operation that may be attempted again, in journal order. */
[[nodiscard]] std::vector<OperationRetryDecision>
DecideOperationRetry(const std::vector<OperationJournalItemResult> &_results);

/** True when at least one item may be attempted again - what a Retry control should be enabled by. */
[[nodiscard]] bool OperationHasRetryableItems(const std::vector<OperationJournalItemResult> &_results);

} // namespace nc::ops
