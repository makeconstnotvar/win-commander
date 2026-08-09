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

**Step B1 — DONE: the contract, and the design's own prior was half wrong.** The question was whether a
Move needs a new transaction contract or a mode of the existing one. The prior written above — *the
commit differs entirely, so a shared type would be a switch at the only place that matters* — turned
out to be wrong about the transaction and right about something else.

**The transaction is reused whole, because its commit was already a parameter.**
`ProviderConditionalCopyTransaction` never performs a publication: it is minted with a `CommitHandler`
and an `AbortHandler` and owns the single-use terminal gate, the cached result and the consumed
authority. Those obligations are identical for a Move, and the one thing that differs arrives as a
lambda. Forking the type would have duplicated exactly the parts that are hard to get right — the same
reason the batch operation declined to fork in its own step C.

**What must not be shared is the authority, and that is a safety property rather than tidiness.**
`ProviderConditionalMoveReviewedAuthority` is a distinct type from the Copy one. Were they
interchangeable, an authority minted from a plan the user approved as a *copy* could be handed to a
Move execution and the source would be gone. A shared type could only be defended by a runtime check
on a plan-type field; two types make the substitution unspeakable, and the test asserts that with
`static_assert` rather than with a call that returns an error code.

Claims add the **source parent**, for the reason the rename window above already gave: a rename acts on
a name inside a directory, so the directory holding the source is part of what authorises the
operation rather than an incidental fact about it. Minting therefore also requires the source to be an
exact child of the parent it names, and — unlike a Copy, which left the source's directory alone —
requires *both* ends bound to the provider being asked.

`ProviderConditionalMoveTransactionBeginError` is its own vocabulary because `SourceParentStale` is a
refusal a Copy can never produce, and adding it to the shared enum would oblige every Copy consumer to
handle an unreachable case.

### Verification

Two new `VFSUT` cases: the substitution proof, which is compile-time (`static_assert` over
convertibility, constructibility, and that the authority is move-constructible but not move-assignable,
so single use survives a move); and minting, which refuses an unsealed authority, a source outside the
parent it claims, an unbound source end, and a move onto itself. `nc::vfs::ProviderCapabilities *`
18/18 (573 assertions); full `VFSUT` 198/198 (82,631).

One of those sections had to be rewritten after it passed: the obvious way to express *a move onto
itself* — leave the source where it is and point the destination at it — is refused for being outside
its own claimed parent, so it passed while proving something else. Both ends now live in the same
directory, which leaves the self-move as the only rule left to break.

**Step B2 — DONE: the Native implementation.** Begin anchors three descriptors, and the middle one is
opened in the way that matters: **the source is opened through the anchored source parent**, not by
absolute path. That is what binds the descriptor to the same directory entry the rename will act on,
so the identity checked at Begin and the name published at Commit concern one object rather than two
paths that merely read alike.

Commit re-verifies all three seals, then makes the check a Copy has no use for: **the source's name,
resolved through its anchored parent, must still lead to the object the descriptor holds.** Holding
the source open proves the reviewed object still exists; only this says the name still points at it.
Then cancellation is resampled, `renameatx_np(..., RENAME_EXCL)` publishes, and both namespaces are
made durable — `fsync` then `F_FULLFSYNC` on each parent, because two directories changed and a rename
that survives in one direction only is a lost file rather than a moved one. Nothing syncs the object
itself: no content was written, so there is nothing new on it to persist.

An uncertain rename is resolved more strictly than an uncertain clone. Absence of the destination is
necessary but not sufficient — the source must *also* still be where it was, under the name it had —
because a rename that succeeded and was then undone by someone else looks exactly like one that never
happened if only the destination is probed.

**Two tests corrected the design's expectations, and both corrections are more precise than what they
replaced.** The case for *the source is gone* was written expecting `SourceStale`; the run answered
`SourceParentStale`. The answer is right: removing the source **is** a change to the directory holding
it, and that directory is checked first precisely because the source is opened through it. So a
vanished source is reported as its directory having moved — the more specific fact, since the entry
the rename was going to name is no longer the entry that was reviewed. What actually reaches
`SourceStale` on Begin is the file moving on while its directory does not, which writing into it does
and unlinking it cannot.

The same shape appeared at the other end: a destination created *after* review answers
`DestinationParentStale`, not `DestinationExists`, because creating an entry is a change to the sealed
directory. `DestinationExists` is for the case where the directory is exactly as reviewed and the name
inside it was already taken — a plan that was never publishable rather than one the world overtook.
Both are now cases, because the pair is the point: **the sealed directory is what notices first, and
what it notices is strictly more informative than the collision.**

### Verification

Six new `VFSUT` cases against the real filesystem: a Move publishes and the source ceases to exist in
the same operation; a destination that appeared after review is never replaced and the source stays
put; **a source name re-pointed at another object between Begin and Commit fails closed** — the case
the whole anchoring exists for; either directory changing since review fails closed, the source parent
as source staleness and the destination parent as its own; cancellation at the last point touches
neither directory; and Begin refuses an occupied destination, a changed source, a changed source
parent, a vanished source, and a claimed parent that does not hold the source.
`VFSNative conditional Copy transaction *` 36/36 (757 assertions) and `nc::vfs::ProviderCapabilities *`
18/18 (573) in Debug and under both Release ASAN and Release UBSAN; full `VFSUT` 204/204 (82,809).

One note for whoever runs these next, because it cost a diagnosis here: the sanitizer filters were
first run as two concurrent processes and each reported one unrelated failure. `VFSUT` shares a global
`TestDir` across processes, so independent runs must be sequential — a fact already recorded elsewhere
in this repository's evidence. Run sequentially, all four are clean.

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
