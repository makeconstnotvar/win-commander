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

**Step A — extract per-item preparation.** The body from `report.items.front()` down to
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

**Step D — lift the gate.** Only `BatchUnsupported` comes down. `MultipleSourcesUnsupported` stays:
several sources need several structural bindings checked against the report, which step A does not
cover. They were separated for exactly this reason.

## What must not be given up

- **One review, one authority per accepted item** — already enforced; the batch path must issue by
  index rather than by counting.
- **Every item's evidence checked before any item is executed.** Checking as you go would let the
  first item's copy happen and the third item's staleness be discovered afterwards.
- **A batch is not a loop of single-item operations.** Each would produce its own journal and its own
  terminal state, and the Operation Center would show N operations where the user asked for one.

## Verification this will need

- The existing factory cases must pass unchanged after step A — that is the whole safety argument for
  the extraction.
- New cases for: a two-item plan preparing and executing both; a second item that is stale rolling
  back the first item's transaction; cancellation between items leaving the first committed and the
  second `Cancelled`; and per-item results landing at the right indices.
- `OperationsIT` under ASAN+UBSAN, since this owns descriptors and transactions across a vector.
  Both sanitizer schemes build now, which they did not at the start of this work.
