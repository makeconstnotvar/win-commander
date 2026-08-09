# Q2-8 Move: executing a reviewed Move — a design, not a backlog entry

> Written while the engine was in context, in the same spirit as
> [`reviewed_batch_execution.md`](reviewed_batch_execution.md): the next session should start from a
> decision rather than from a survey.

## What already exists, and what the wall actually is

More is in place than a survey would suggest, and the wall is not where the plan item implies:

- **The preflight already plans a Move.** `OperationPlanner::RunMove` binds an exact one-file rename
  intent: source and destination paths, both parent namespaces, their capability and access evidence.
  It is deliberately intent-only and issues no execution authority.
- **`OperationPlan` already knows what a Move means.** `DeriveIntrinsicEffects` answers `Relocated` /
  `CreateOrUpdate` / no data-loss risk, and `Create` requires the same destination and conflict policy
  it requires of a Copy.
- **The journal already accepts a Move.** `OperationJournalValidItemResult` treats Move as publishing
  its destination, with the same `Published` + `Confirmed` durability requirement a Copy has.
- **The atomic primitive is already used elsewhere in the codebase.** `renameatx_np(..., RENAME_EXCL)`
  is what `NativeCreateCopy` publishes through, and `NativeFSManager` already reports `rename_excl` as
  a distinct volume interface, next to `clone`.

**The wall is the review token.** `ReviewedVFSOperationPreflight::Review` refuses any plan that is not
`Copy`, and a test already pins it: *does not issue a generic review token for an accepted Move*. Below
that refusal, four more places ask the same question — `ReviewedOperationFactory`,
`CopyOperationOrchestrator` (twice) and `OperationCenterCoordinator`. The batch slice's step D is the
precedent: **lifting one of five gates changes nothing**, and the interesting question is what the
gates are hiding.

## What the gates are hiding, and it is not a missing primitive

Two things, and only the second is hard.

**A same-volume Move is one operation, not two, and that is what makes it expressible at all.**
`renameatx_np(from_parent_fd, from_name, to_parent_fd, to_name, RENAME_EXCL)` publishes the
destination and removes the source *indivisibly*, and refuses outright if the destination exists.
The journal's item result can say `destination_publication == Published` and nothing about the source
— which is exactly right here, because there is no state in which the destination exists and the
source also still does. **A cross-volume Move has no such property**: copy-then-unlink is two events,
and `OperationJournalItemResult` has no field that could record "published, but the source is still
there". So cross-volume Move is not merely unimplemented, it is *unrepresentable* in the current
journal — which matches the queue policy that already closed the cross-volume branch as unsupported.
Worth stating in the record rather than rediscovering.

**The hard part is that a rename cannot be anchored to a descriptor the way a clone can.** This is
the real difference between Move and Copy, and it is not removable by better code:

- A Copy publishes *from an open file descriptor*. `Begin` opens the source `O_NOFOLLOW`, seals its
  identity, and `fclonefileat` reads that descriptor. Whatever happens to the source *name* between
  review and commit, the bytes published are the bytes of the object that was reviewed.
- A rename has no descriptor form. It operates on a *name inside a directory*, so the object it moves
  is whatever that name resolves to at the instant the kernel performs it. Holding the source open
  proves the reviewed object still exists; it does not prove the name still points at it.

So a reviewed Move carries a window a reviewed Copy does not: between the last identity check and the
rename, the source name could be re-pointed at another object, and the operation would move that one
instead. `RENAME_EXCL` does not help — it protects the *destination* from being replaced, which is a
different guarantee.

**What that window is worth, stated plainly rather than argued away.** Nothing is overwritten: the
destination is still created only if absent. Nothing is lost: the substituted object is moved, not
deleted. The user gets a file they did not ask to move, in a folder they did ask to move something
into. That is meaningfully smaller than the failure a Copy's anchoring prevents, and it is the same
exposure every `mv` on this platform already has. **The decision this design takes is to accept it and
name it in the provider's own test**, exactly as the batch slice accepted and named the interleaved-
write window under `MonotonicGrowth`, rather than to invent a locking scheme a single-user desktop
file manager does not need.

The narrowing that *is* available and should be taken: open the source parent directory and hold it,
open the source by name *within it* with `O_NOFOLLOW`, seal the source's identity, and re-verify that
seal immediately before the rename. That does not close the window — it bounds it to the interval
between the final `fstatat` and the `renameatx_np`, and it removes the entire class of failures where
the *parent path* was swapped, since the rename then runs against a held descriptor.

## The decomposition

**Step A — DONE: provider eligibility, read-only.** `ConditionalMovePathSupport(source,
destination_parent)` answers `SameVolumeRename` / `Unsupported` / `Unavailable`, and the missing fourth
answer is the design: there is no cross-volume case, for the reason above.

Three things were decided while writing it:

- **The volume rule is one rule with a parameter, not two rules that must be kept in step.**
  `EvaluateConditionalCopyVolumeImpl` already took a `_requires_clone` flag for the staging helper's
  sake; that boolean became a named `ConditionalPublicationInterface` (`None` / `Clone` /
  `AtomicExclusiveRename`) and Move is its third caller. Everything the Copy policy demands — APFS,
  local, internal non-removable media, writable, known permissions, a complete metadata API — is
  demanded of a Move for the same reasons, because publishing through a rename changes neither the
  durability contract nor the evidence it rests on. **Exactly one clause differs**, and a test pins
  that it is exactly one.
- **Neither eligibility may be inferred from the other, and the test proves it in both directions.**
  A volume with `clone` and no `rename_excl` is eligible for a Copy and refused for a Move; a volume
  with `rename_excl` and no `clone` is the reverse. Asserting only the first would have left the
  suspicion that Move eligibility is Copy eligibility with an extra condition, which it is not.
- **`AtomicRenameUnavailable` is a Move-only disposition.** A Copy never asks for that interface, so
  it can never produce that answer — the refusal names which interface was missing rather than
  collapsing into a generic unsupported.

### Verification

Two new `VFSUT` cases: eligibility answered without inference from Copy, in both directions, plus the
definitive two-volume refusal and the unresolvable/relative non-answers; and the volume policy held to
the Copy discipline with exactly one differing clause. `VFSNative conditional Copy transaction *`
30/30 (599 assertions); full `VFSUT` 196/196 (82,668) in Debug.

**Step B — the provider transaction.** A move-only, single-use transaction with the same tri-state
publication evidence, whose claims add one expectation Copy has no need of: the **source parent**.
Begin anchors source parent, source and destination parent; Commit re-verifies all three, resamples
cancellation, publishes through `renameatx_np(..., RENAME_EXCL)`, then orders
`fsync(source parent) → fsync(destination parent)` — both, because two directories changed, and in
that order because the source parent is the one that loses an entry.

Whether this is a new contract or a mode of the existing one is the question step B has to answer with
code rather than in advance. The honest prior: the *claims* differ by one expectation and the *commit*
differs entirely, so a shared transaction type would be a switch at the only place that matters.

**Step C — review, factory, orchestrator, coordinator.** All five gates at once, since step D of the
batch slice already demonstrated that lifting them one at a time proves nothing.

**Step D — the producer.** `MoveTo` / `MoveAs`, mirroring what `Copy To` now does, reusing
`SelectBatch`-shaped policy and the terminal presentation, both of which are already type-agnostic.

## What must not be given up

- **One review, one authority per accepted item** — unchanged; a Move authority must be as
  single-use as a Copy one.
- **Create-only.** `RENAME_EXCL` is not an optimisation here, it is the contract: a reviewed Move
  never replaces an existing destination, exactly as a reviewed Copy never does.
- **Both parents are durable before the operation is called complete.** A rename that survives only in
  one direction after a power loss is a lost file, not a moved one.
