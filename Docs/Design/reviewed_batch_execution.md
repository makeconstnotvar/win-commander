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

**Step B — DONE: prepare all items, then commit to none of them.** The factory loops `prepare_item`
over `report.items`, collects the prepared transactions, and abandons the whole set before returning
any failure. Three things were decided while writing it:

- **What the rollback adds is not the absence of a leak — it is a readable answer.** A transaction
  aborts itself when destroyed, so dropping the vector would already undo them. What that discards is
  the abort *result*, and the one case worth having is precisely the one it hides: an abort that
  cannot confirm `NotPublished`. So the rollback aborts explicitly, in reverse order — last begun,
  first undone — and reports the item it could not confirm.
- **An unconfirmed rollback outranks the reason for it.** `StaleSource` tells the user the world moved
  and nothing was done. An abort that cannot say `NotPublished` does not support the second half of
  that, so the answer becomes `ConditionalCommitAuthorityUnavailable` carrying the destination that
  may or may not exist. Reporting the original reason would hide a possible file behind an error
  saying there is none. Same rule the cold-abort path already applied at the blocker.
- **Cancellation is now checked after preparation, before handing over.** It is the first failure that
  can occur with a transaction already begun — the note below is why there was none before — so it is
  what makes the rollback reachable rather than dead code waiting for step D. It is also right on its
  own terms: an operation built after a cancellation would carry open transactions into the Pool only
  to abort them, telling the user "cancelled" after the Pool had taken ownership of unwanted work.

Step A's outstanding debt was closed here as well: an item's structural source is matched against
whichever entry of `plan.Sources()` names it, not against `front()`. See step D for why that lookup is
deliberately not positional.

> Visible only now that step A exists: `prepare_item` **cannot leak a transaction today**. Beginning
> the transaction is its last action, so every failure path returns before one exists. The
> all-or-nothing rollback requirement therefore belongs entirely to step B — it is introduced by the
> loop, not inherited from step A. Worth knowing, so the next reader does not go looking for a leak
> that is not there, and does not assume the loop is safe because step A was.

The original description follows, for the steps still to come. Loop step A over `report.items`. If any
item fails, **every already-begun transaction must be rolled back before returning**. This is the
decision that makes the slice non-trivial: a half-prepared batch that returns an error while holding
open transactions leaves temporary state on disk that nothing owns. Preparation must therefore be
all-or-nothing, which means the loop cannot simply propagate the first error with `return`.

**Step C — DONE: the batch operation.** Not a new class. `ProviderConditionalCopyOperation` and its
Job now own a `std::vector` of prepared items and one item is a batch of one, because the parts that
are hard to get right — the termination gate with its four entry points, the cancel-checker
sanitiser, the construction pipeline — are exactly the parts a second class would fork. The existing
suite became the N=1 regression pin at no cost.

What the design said about deriving the state from the whole set turned out to be under-specified,
and the journal decided the rest:

- **`OperationJournalValidEntryLifecycle` cannot express a Failed item and a Cancelled item in one
  entry** (`Completed` forbids both, `Failed` requires no Cancelled, `Cancelled` requires no Failed).
  So "any failure fails it, any cancellation cancels it" is not a fold over statuses — it is also a
  decision about which results are emitted at all.
- **The run therefore stops at the first item that does not succeed.** Continuing would let a later
  cancellation meet an earlier failure and produce a set with no legal state at all. It is also the
  honest reading of the evidence: every item's evidence was checked before execution began, and an
  item that has just proved the world moved is a reason to stop spending it.
- **How the untouched tail ends depends on why the run ended.** A cancellation cancels it — a forced
  commit with an always-true checker, which is what yields a `Cancelled` result and what the design
  meant by "the rest are `Cancelled`". A failure *aborts* it instead, and an aborted transaction has
  no journal result at all; a `Failed` entry may legally omit the items behind the failure, and that
  is exactly what "never attempted" should look like.
- **A failure can still surface during a cancellation wind-down**, when a forced commit's abort
  cannot confirm `NotPublished` and maps to `Failed`. Then the state is `Failed` and the `Cancelled`
  results are dropped: a cancelled item is always `NotPublished`, so leaving it out withholds nothing
  about what is on disk, while dropping the failure would hide a destination that may exist behind an
  outcome saying none does.
- **A completed batch must account for every item.** An entry may omit the items behind a failure or
  a cancellation; a completed one may not, and an item whose provider answers with a terminal that
  precedes execution has no result to give. Such a run is `Inconsistent` — the answer the single-item
  path has always given the same event, where the missing item was the only one. Found by review,
  after the first implementation would have emitted a `Completed` entry the journal refuses.
- **Whoever claims the sequence owes every item a terminal.** A stop finds the sequence already
  claimed and the destructor only acts on an untouched job, so an exception between items must not
  escape: it winds the batch down as cancelled instead. Unresolved slots are evidence that never
  arrives, and the Pool would hold the operation forever waiting for it. Also found by review — the
  two calls that can throw there (the pause wait, the current-path publication) are both new in this
  step, and the region held only `noexcept` calls before it.
- **The stop handshake is one decision under one lock.** A stop is accepted on the strength of "this
  item has not started"; reading that answer anywhere other than where the start is published lets a
  stop be accepted for an item committed a moment later. Third finding from review, and the one whose
  race cannot be driven from outside once fixed — the rule is pinned, the window is argued.
- **Evidence is `Pending` until every item is resolved and `Inconsistent` when no item produced a
  result at all.** The Pool reads the accessor once and latches the first non-`Pending` answer, so a
  partial snapshot would be recorded durably and a permanently-`Pending` one would retain the
  operation forever. The zero-results case is the cold abort, which the application boundary already
  reads as its integration blocker.
- **The journal's numbering is checked at construction, not at the terminal.** A set whose indices do
  not strictly increase is refused before anything is copied, rather than after — a `Finalize` the
  journal rejects is not a visible error but a slot latched into contract violation.
- **Statistics estimate both timelines; the preferred source stays Items.** A conditional copy
  publishes atomically, so an item's bytes go from none to all at its commit and a byte fraction
  would sit still and then jump. The byte total is still what says how much the batch weighs.
- **Only the worker terminates transactions while it owns the sequence.** A stop arriving mid-run is
  accepted exactly while items remain unstarted — those the worker cancels when it sees the stop —
  and refused once the last item is committing, which is the single-item rule unchanged. Having the
  stop path terminate them inline would run N provider commits on the UI thread under Job's state
  mutex, and could touch a transaction the worker is already inside.
- **Pausing between items is now real.** It is the only safe pause point (a commit cannot be
  interrupted), and until now `Pause` flipped a flag while nothing ever waited.

**Step D — DONE, and the gate was not the wall.** Both refusals are gone, and what replaced them is
not a limitation but the rule the journal imposes: a report must account for the plan's sources, one
item each, because results are numbered in the source space and a completed entry missing one cannot
be recorded. The same rule now stands at all three outer gates — `Submit`, `SubmitAdmitted` and the
Operation Center coordinator each refused several sources, so lifting only the factory's would have
changed nothing — and a batch cancelled before it runs now records that against **every** source it
named rather than fabricating a statement about the first one.

**What the gate was hiding.** Executed against the real Native provider rather than a test mint, a
two-item batch copies the first file and refuses the second with `ESTALE`. Each item carries the same
reviewed expectation of the destination directory — including its size and both timestamps — and
publishing the first item changes all three. The provider refuses to publish into a directory that
changed at all since review, deliberately and with its own test (`NativeConditionalCopyTransaction:
fails closed when reviewed destination parent evidence becomes stale`), and a batch is a writer into
its own destination. Nothing is written wrongly; the refusal is fail-closed and the rest of the batch
is abandoned. But **a multi-file copy into one folder cannot complete until that contract separates
what authorises a publication — the directory's identity and permissions — from what the batch itself
is expected to change: its contents.** That is a provider-contract slice, not a gate, and it is now
pinned by a test so the wall is visible rather than inferred.

Nothing user-visible changed either way: the only production producer of a reviewed plan is
`Copy As…`, which builds a single-source `ExactItem` plan and whose application boundary refuses
anything else before submission. That is what makes deferring the single-item terminal presenter
honest rather than convenient — it cannot be reached with a set until a producer exists.

The original description follows, for the record.

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

**Done in step B, and not the way this paragraph assumed.** "Its own entry" cannot mean "the entry at
its own index": `OperationPlanner::PlanSource` returns without emplacing an item when a source's
destination already exists under a `Skip` policy — which the factory accepts — so the report can be
shorter than the plan and every later index off by one. The match is therefore a lookup: the item's
source must be named by *some* entry of `plan.Sources()`. That is weaker than a bijection, and step D
should decide whether it wants one — two accepted items naming the same source would each need their
own destination, and identical destinations are what the review and `DestinationExists` already
refuse, so the weaker check may well be enough. Worth deciding rather than inheriting.

Confirm before building on this: whether any planner path produces several accepted items from one
source. If one does, `BatchUnsupported` becomes reachable and both gates need lifting; if none does,
it is dead and should be said so rather than left looking like a limitation someone might try to
lift.

### What step C found that step D has to answer

The journal numbers item results in the **plan's source space**, not the report's:
`OperationJournalValidItemResult` refuses `item_index >= plan.Sources().size()`, and `Completed`
additionally requires `item_results.size() == plan.Sources().size()`. Three consequences:

1. **The two spaces coincide for every plan this factory can accept, and that is provable rather than
   assumed.** A source is dropped from the report only by `Skip` on an occupied destination, and the
   planner emplaces a conflict *before* that switch — a report carrying a conflict is refused at
   review. Sources are planned in order, so accepted item *i* comes from source *i*. The lookup
   introduced in step B is what keeps this true instead of trusting it.
2. ~~**One hole remains: a cancelled preflight can still be accepted.**~~ **False, and step D
   disproved it.** The Copy finisher does break out of the source loop on cancellation, but
   `m_Cancelled` is assigned in exactly one place — inside `AddProbeBlocker`, immediately after the
   blocker is recorded — and a preflight with any blocker is never accepted. So the report can never
   be a strict prefix of the sources, and the refusal step D added for it is defence in depth rather
   than a reachable case. Recorded here rather than quietly deleted: the assumption was written from
   reading the loop and not its only exit.
3. **The derivation is a lookup, so it is many-to-one — and nothing can drive it there.** `PlanSource`
   has exactly one site that appends an item and every other exit returns before it, so no source
   expands into several. `OperationPlan::Create` refuses duplicate sources, and the planner refuses
   duplicate destinations whenever there is more than one source. If that ever changes, the colliding
   indices are refused at the operation's construction — fail-closed, but the wrong answer to give a
   user, so it would need deciding rather than inheriting.
4. **The application's terminal presenter is single-item.** `CopyOperationDurableTerminalOutcome::
   SingleItemResult()` returns nullptr for N != 1 and the copy presenter turns that into a
   "requires reconciliation" alert, so lifting the gate without teaching that surface about a set
   would report a successful batch as a failure needing recovery.

## Provider-contract slice — DONE: separating identity from a batch's own growth

The wall step D pinned: every item in a Directory-kind batch shares one destination-parent
expectation, captured once at review. Publishing the first item advances that directory's size and
both content timestamps, so the second item's commit found the directory "changed since review" and
refused — correctly, since the check could not tell its own batch's growth from tampering.

**The fix is a per-expectation tolerance, not a rewrite of when anything is captured or checked.**
`ProviderConditionalCopyExistingExpectation` now carries a `tolerance`: `Exact` (unchanged default,
the only mode a lone reviewed item ever needs) or `MonotonicGrowth`. Under growth tolerance, size and
the two content timestamps may be *at least* the reviewed value instead of *equal* to it; identity
(device, inode, birth time) and mode are checked exactly either way, in both places the destination
parent is checked (the narrow per-field stat comparison, and the fuller metadata-snapshot re-check
Commit runs against what Begin itself captured — the only place ownership, BSD flags, ACL and extended
attributes are verified for the destination parent at all, so those stayed exact there too).

**Which items get which tolerance is knowable entirely at prepare time**, before any item executes:
the factory tracks which destination-parent paths it has already assigned to an earlier item in this
same batch, by canonical path. The first item to name a given directory gets `Exact` — nothing should
find that directory touched at all, exactly as a lone reviewed copy has always required. Every later
item naming the *same* directory gets `MonotonicGrowth`, because by the time its own commit runs, the
directory has necessarily and legitimately grown by exactly this batch's own prior, authorized
publications. No value observed at runtime is threaded forward between items; the assignment is a set
membership check over paths the plan already names.

**One assumption broke on the real provider and had to be corrected empirically, not reasoned out:**
APFS advances a directory's `link_count` when a regular-file child is added, not only for
subdirectories, contrary to traditional POSIX directory semantics. The first version of this fix
treated `link_count` as an identity field and kept it exact, which meant the second item still failed
— now correctly relaxed alongside size and the two timestamps. Found by running the real transaction
against a live batch and reading exactly which field-equality failed, the same method that found the
wall in the first place; reasoning about filesystem semantics in the abstract had already gotten this
one wrong once.

**What tolerance does not and cannot cover, by design:** an unrelated write landing between two items
of an authorized batch — after the first item has already made the directory a moving target — is
indistinguishable from the batch's own growth, since both only advance size, the content timestamps
and `link_count`. This is not a gap left open by an incomplete fix; it is the accepted, load-bearing
shape of the mechanism, and it is narrower than it first looked: identity and the whole
ownership/permission surface (mode, uid, gid, BSD flags, ACL, extended attributes) still refuse on any
change, for every item, always — an attacker who cannot touch those cannot escalate privilege or
redirect what this operation writes, and the residual is bounded by the same non-atomicity every
sequential multi-file copy on this platform already accepts without a directory lock. A local,
single-user desktop tool was judged not to need one to close this last inch. Documented directly at
the provider level rather than left to be rediscovered, in
`NativeConditionalCopyTransaction_UT.cpp`.

### Verification

Three new `VFSUT` cases: growth tolerance publishes though the parent grew since review; it still
fails closed on a mode change (the narrow check); it still fails closed on a flags change (the fuller
metadata check, the only place flags are verified for the destination parent at all). Two new
`OperationsUT` cases at the `ReviewedOperationFactory` level, both against the real Native transaction
rather than the test mint: a two-item batch into one directory now completes and both files land on
disk; a directory tampered with *before* review still refuses the whole batch before any item runs,
unchanged from the single-item behavior. `VFSNative conditional Copy transaction *` 28/28 (570
assertions) in Debug and the same under Release ASAN and Release UBSAN; full `VFSUT` 194/194 (82,617);
full `OperationsUT` 260/260 (6,414) on three seeds; `OperationsIT` 98 passed / 2 skipped (973);
`WinCommanderUT` full run unchanged.

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
