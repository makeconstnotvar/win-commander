# Q2-8 slice 2: executing N items under one review — a design, not a backlog entry

> Written while the engine was in context. The point is that the next session starts from a decision
> rather than from a survey.

## Where the wall actually is

`ReviewedOperationFactory::CreateExecutionProductWithDependencies` validates one accepted item and
ends by calling `ProviderConditionalCopyOperationFactory::Create` with **one**
`ProviderConditionalCopyTransaction`. There is no composition of operations in `nc::ops` to layer on
top of that, so "loop over the items" is not the change: the change is that something has to own
several transactions and journal several results.

Three things are already right and should not be disturbed:

- `CopyOperationTerminalEvidence` carries a **vector** of item results and a single journal state.
- `ProviderConditionalCopyJournalContext` already has `item_index`.
- `SealedReviewedPreflight` issues **one authority per accepted item**, refusing an index twice or
  out of range (slice 1). The batch path needs no new authority rule.

So the missing piece is exactly one: an operation that holds N prepared transactions.

## The decomposition

**Step A — DONE.** One item's work is now `prepare_item(index)`: a named unit inside the factory
taking an item index, returning the transaction plus what the operation needs, or the same error it
always returned. Nothing about the work changed - what changed is that it is no longer a
three-hundred-line stretch that only ever ran for `front()`. It is called once, because the batch
gate above still admits exactly one accepted item; lifting that gate is now a loop over this rather
than a rewrite of it.

Deliberately a lambda inside the function rather than a free function with eight parameters: every
dependency it needs is already a local here, and threading them through a signature would have been
a larger edit with more places to get wrong, for no gain the loop can use.

The original description follows, for the steps still to come.

**Step A (as planned) — extract per-item preparation.** The body from `report.items.front()` down to
`BeginConditionalCopyTransaction` is one item's worth of work: canonical paths, structural match,
host resolution, snapshot lookup, evidence match, descriptor opens, claims, authority, transaction.
Lift it to a function returning a prepared item or a `ReviewedOperationFactoryError`, with the item
index passed in. No behaviour change; the existing 8 cases and 225 assertions pin it.

The two `_direct_access_checker` / `_source_open_at` seams must be passed through, since the tests
drive failure paths through them.

**Step B — prepare all items, then commit to none of them.** Loop step A over `report.items`. If any
item fails, **every already-begun transaction must be rolled back before returning**. This is the
decision that makes the slice non-trivial: a half-prepared batch that returns an error while holding
open transactions leaves temporary state on disk that nothing owns. Preparation must therefore be
all-or-nothing, which means the loop cannot simply propagate the first error with `return`.

**Step C — the batch operation.** A new `nc::ops::Operation` owning a `std::vector` of prepared
items, running them in order, writing one journal item result per item at its own index, and
producing terminal evidence whose `state` is derived from the whole set: any failure fails the
operation, any cancellation cancels it, otherwise completed. Per-item `exact_source_bytes` sums into
the operation's total for progress.

Cancellation between items is a real case: the items already committed stay committed, the rest are
`Cancelled`. That is what the journal's per-item result exists to express.

**Step D — lift the gate.** This is where the design was wrong when first written, and a test
corrected it.

A test was added to pin `BatchUnsupported` — one source accepted as several items — using a
directory source. It failed: a directory source is accepted as **one item of kind `Directory`**, so
it stops at the source-kind gate and never reaches the batch one. Nothing at this layer expands one
source into several accepted items, which means **`BatchUnsupported` appears unreachable today**,
and the gate actually standing between here and batching is `MultipleSourcesUnsupported`.

So step D lifts the several-sources gate, and step A's per-item extraction has to cover what that
gate was protecting: **each item's structural source must be matched against its own entry in
`plan.Sources()`**, not against `Sources().front()` as the single-item path does. That check is part
of step A, not something to bolt on afterwards.

Confirm before building on this: whether any planner path produces several accepted items from one
source. If one does, `BatchUnsupported` becomes reachable and both gates need lifting; if none does,
it is dead and should be said so rather than left looking like a limitation someone might try to
lift.

## What must not be given up

- **One review, one authority per accepted item** — already enforced; the batch path must issue by
  index rather than by counting.
- **Every item's evidence checked before any item is executed.** Checking as you go would let the
  first item's copy happen and the third item's staleness be discovered afterwards.
- **A batch is not a loop of single-item operations.** Each would produce its own journal and its own
  terminal state, and the Operation Center would show N operations where the user asked for one.

## Verification this will need

- **The safety argument for the extraction is weaker than it looks**, and the gaps are not all the
  same kind. Of the factory's 21 error codes, nine were raised and asserted by nothing. Sorting them
  by *why*:

  - **`OpenFailed` — now covered.** Raised at eight sites and asserted nowhere, and precisely what a
    per-item extraction moves. A test now drives the `SourceOpenAt` seam to fail with `EACCES` and
    checks the factory reports a failure rather than staleness — the missing-file errnos mean the
    world moved and are deliberately mapped to `StaleSource`, which is a different thing to tell the
    user.
  - **`MissingEvidence` and `InvalidEvidence` — now covered, through the seam that was the decision
    rather than the obvious one.** They need a report whose snapshots are absent or wrong, which a
    real planner never produces. The obvious answer — let a test construct an
    `AcceptedOperationPlan` — was rejected: **a seam able to forge a reviewed plan is a seam that
    could manufacture a review that never happened.** What is injectable instead is the *snapshot
    lookup*, which can only change how already-reviewed evidence is found. One test withholds a
    snapshot, the other returns one claiming the source is a directory; both refuse, and neither
    needed a way to fake a review.
  - **Six remain, and two of them are now known to be unreachable rather than merely unchecked.**
    `BatchUnsupported`, as shown above. And `UnexpectedConflictEvidence`: a test written to reach it
    found that a plan whose destination is already occupied is **refused at review**, so the
    factory's own conflict check is defence in depth and nobody can drive it. That test now pins the
    *reason* — otherwise the next person counting untested paths tries the same thing and learns it
    again. The remaining four are the same shape and should be confirmed the same way rather than
    chased with contrived seams.
  - **Defence in depth.** `Review()` already refuses a non-Copy
    plan with the same code, so the factory's own check cannot be reached through it. Unreachable by
    construction is a different thing from untested, and should be recorded as such rather than
    chased.
- The existing factory cases passed unchanged after step A, as did the operations integration suite
  (98 of 100, 2 skipped, 973 assertions) and the factory suite under ASAN+UBSAN. That was the whole
  safety argument, and it held.
- New cases for: a two-item plan preparing and executing both; a second item that is stale rolling
  back the first item's transaction; cancellation between items leaving the first committed and the
  second `Cancelled`; and per-item results landing at the right indices.
- `OperationsIT` under ASAN+UBSAN, since this owns descriptors and transactions across a vector.
  Both sanitizer schemes build now, which they did not at the start of this work.
