# Feature: reviewed Copy factory and conditional transaction foundation

> Status: reviewed evidence reaches a private production execution product, journal/Pool orchestrator and bounded `CopyAs` consumer
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 5, 7.2, 13.6, 14, 15, and 32
> Execution tracker: M3 in `Docs/Development-Plan.md`

## Purpose

This slice keeps accepted planning, explicit review, journal admission, provider authority, operation construction and queue admission as distinct capabilities. No plan ID, decoded plan, opened source descriptor or absent-destination check grants mutation authority by itself.

## Implemented chain

1. `MakeVFSOperationPlanningAccessChecker` projects application access into fail-closed planner evidence.
2. `VFSOperationPlanningProbes` binds exact hosts and source/destination version evidence to an accepted preflight.
3. `ReviewedVFSOperationPreflight::Review` creates a move-only approval value only for Copy and requires explicit destructive confirmation when applicable. It rejects an accepted Move preflight with `UnsupportedPlanType` before any factory authority exists.
4. `OperationPlanCodec` serializes the structural plan losslessly; `OperationJournal` durably admits it and issues exact move-only admission/run receipts.
5. `ReviewedOperationFactory` validates one Native-to-Native, create-only regular-file Copy and consumes the reviewed preflight into a private-constructible `ProviderConditionalCopyReviewedAuthority`.
6. The authority retains the moved reviewed preflight as a private seal and exposes immutable exact claims. `BeginConditionalCopyTransaction` consumes it once; a provider may then mint a move-only, single-use `ProviderConditionalCopyTransaction`.
7. `ProviderConditionalCopyOperationFactory` consumes the transaction into a move-only cold operation plus exact terminal journal-result accessor.
8. The process-owned `OperationCenterCoordinator` stages durable admission for bounded `CopyAs`, then passes the exact receipt to the private `CopyOperationOrchestrator` path. It installs weak Start and durable-terminal model reducers while the product is cold; the common path orders Running, enqueue, exact durable-outcome delivery, terminal finalization, reconciliation, and Pool release.

## Provider transaction contract

The authority contains the plan ID, source and destination host identities, exact source evidence, exact destination-parent evidence and an absent exact destination. Production callers cannot default-construct, copy, assign or mint it. Its private review seal owns the consumed exact preflight; the moved-from token cannot issue another authority. VFS validates canonical paths, object kinds/modes, timestamps, direct-child destination structure, seal presence and local host/provider relationships.

The transaction moves through `Pending → Committing → Consumed`. Provider commit/abort handlers execute at most once. Terminal calls replay the cached publication evidence; concurrent or moved-from use returns conservative `Unknown`. An abort can claim `NotPublished` only when the provider explicitly confirms it. Exceptions and ambiguous abort/commit results become `Unknown + ProviderFailure`.

`NativeHost` implements the bounded clone-only transaction from ADR 0001. The private `ReviewedOperationFactory::CreateExecutionProduct` path losslessly joins that transaction to `CopyOperationExecutionProduct`; the production orchestrator is its friend consumer. The public compatibility `ReviewedOperationFactory::Create` deliberately drops the cold product, resolves the transaction without publication, and returns a fail-closed authority/integration error. This prevents direct callers from bypassing durable admission and run-receipt custody.

## Accepted Native publication invariant

[`ADR 0001`](../ADR/0001-native-conditional-copy-publication.md) selects a bounded first provider scope: Native-to-Native, exact same `NativeHost`, exact same internal/local/writable APFS volume, single regular-file, create-only Copy. Review issues a private-constructible move-only authority with immutable semantic claims. Begin consumes it, opens the source with `O_NONBLOCK | O_NOFOLLOW_ANY`, opens the destination parent with `O_DIRECTORY | O_NOFOLLOW_ANY`, validates reviewed evidence and destination absence, verifies volume capabilities, seals supported ownership/mode/timestamps/flags/ACL/xattrs, and performs no namespace mutation. Commit repeats source, destination, parent, metadata-policy and volume checks immediately before one exclusive `fclonefileat(..., CLONE_ACL)` publication step. It then verifies destination metadata/name identity and orders destination fsync, parent fsync, and destination `F_FULLFSYNC`.

The reviewed source version is the admission and immediate pre-publication freshness condition. The retained descriptor supplies object identity; the clone syscall supplies a coherent copy-on-write snapshot and atomic destination creation. Post-publication metadata or sync failure remains `Published` with independent typed evidence. Other volumes and scopes return `Unsupported` and continue through the established operation path.

## Application boundary

- Bounded `CopyAs::Perform` builds the exact preflight, shows its summary and issues reviewed authority only after explicit approval.
- The app composes cold-operation hooks through process recovery and dispatches owning durable outcomes to the UI executor.
- Completion presentation uses exact durable terminal, publication, sync, recovery, `Reconcile`, and `ReleaseReconciled` evidence.
- Focused policy/gate and recovery tests pass; the app-owned boundary also proves zero enqueue, exact review projection and UI-dispatch scheduling before non-success Pool release (4 / 70).
- Add provider-owned private/bounded staging for cross-volume data and metadata only through the isolated helper authority in `Docs/ADR/0002-cross-volume-staging-authority.md`; a named in-process stage cannot be connected to this reviewed factory.
- Execute dedicated physical internal/external-volume and power-loss evidence in the required environments.

Known unsupported scopes retain the established Copy path. Once an action selects the reviewed lifecycle, later failure remains in its typed journal/recovery path and cannot fall back to a second legacy mutation attempt.

The narrow Move planner result is intentionally outside this reviewed Copy chain. It supplies exact parent-namespace rename evidence and `RuntimeRevalidationRequired`, but cannot reach `ReviewedOperationFactory`, journal admission or `Pool`; legacy `Copying(docopy = false)` remains its execution route until a dedicated Move factory slice.

## Verified coverage

- `OperationPlan`: 8 / 113; planner: 13 / 228; VFS probes: 5 / 178; codec: 12 / 151.
- Reviewed factory: 8 / 225; VFS planning probes: 5 / 178.
- Provider capabilities: 16 / 549; Native conditional Copy: 16 / 328; combined Debug selection: 32 / 877; latest full Debug `VFSUT` run records 161 / 164 and 46,560 / 46,563 with only the `Application marker`, `FetchUsers` and `FetchGroups` host baselines.
- Earlier staged Native create-copy snapshot: 19 / 924; provider result mapper: 4 / 237; execution product: 9 / 188; journal: 27 / 592; Pool: 17 / 219; orchestrator: 15 / 758, including production construction at 3 / 138.
- Job lifecycle and worker-launch hardening: 10 / 608.
- Previously recorded explicitly instrumented Release ASAN and UBSAN `VFSUT` snapshots pass ProviderCapabilities at 16 / 548 and Native conditional Copy at 15 / 312.
- Historical foundation snapshot: Debug, Release ASAN and Release UBSAN `OperationsUT` passed 170 / 4,748 in each configuration, with both sanitizer runtimes confirmed and no diagnostics. The current coordinator/control subset separately passes Release ASAN and UBSAN at 28 / 999 without diagnostics.
- Current M0: unsigned Debug app and 10 aggregate binaries, 897 / 132,011 in the recorded seeded run.
- Docker-backed Debug ASAN integration: 163 / 89,392; fixture cleanup completed.
- Final Move no-authority evidence (2026-08-06): planner 5 / 69, VFS rename mapping 1 / 4, review rejection 1 / 5 and application access checking 4 / 65 all pass in focused Debug. Full Debug `OperationsUT` records 203 / 204 and 5,379 / 5,383 with only the NativeCreateCopy set-ID metadata host baseline; full Debug `WinCommanderUT` records 330 / 334 and 5,317 / 5,321 with four headless pasteboard host baselines. This verifies rejection before factory authority, not Move execution.

## Next slice

Add physical-volume evidence for the bounded `CopyAs::Perform` consumer. The test-only VFSUT characterization now proves that a same-euid name rebind can make `RENAME_EXCL` publish a replacement rather than the prior validated stage inode (1 / 48 focused Debug); it grants no publisher API or authority. Cross-volume staging remains `Unsupported` until a descriptor-bound namespace primitive or a revised signed-root trust model has root-acquisition/transport proof. Move needs its own reviewed factory and execution authority beyond the verified narrow preflight.

---

# Q2-8 slice 1: one review, one authority per accepted item

Q2-8 is about carrying batch, Move and Delete through the reviewed engine. Every one of them starts at the same wall: a review could yield **exactly one** authority, ever, so a plan with two accepted items could never be executed no matter what the rest of the engine learned to do.

That one-shot rule was not arbitrary. An authority is proof that a person looked at *this* operation and accepted it, and letting one review mint two authorities would let the second be spent on something nobody saw.

## The rule that replaces it

A review covers one plan, and a plan covers every item its report accepted. So one review yields **one authority per accepted item — no fewer, and emphatically no more.**

Both halves are enforced, and the second is the security-relevant one:

- **An index outside the accepted report is refused.** Nobody reviewed it, so there is nothing to authorise, and an authority minted for it would claim a review that never happened.
- **An index is refused the second time.** Otherwise a caller could ask twice for one reviewed item and come away with a spare authority to spend elsewhere — which is exactly the hole the one-shot rule closed, and it must not reopen merely because a plan may now carry more than one item.

Tying each issue to an item index rather than counting to N is what makes the second guarantee about *identity* and not just arithmetic.

## Every authority from one review shares one seal

The seal is created once, when the review is sealed, rather than per issue. That is what makes the authorities provably the product of the same review rather than of several — and it is why sealing is now an explicit step instead of a side effect of asking for the first authority.

It also survives the object that issued it: an authority in flight holds the seal, so a discarded `SealedReviewedPreflight` cannot leave one pointing at a review that has gone away. A test discards the seal while holding an authority and reads its claims afterwards.

## Nothing else changed

The factory still handles exactly one accepted item; the batch gate above it is what keeps that true, and lifting that gate is what the per-item issue exists for. It asks for index 0 and behaves as it always did, which is why every existing test pins it unchanged.

## Verification

- `OperationsUT`, `OperationsIT`, `WinCommanderUT` and `WinCommander-Unsigned` — all **BUILD SUCCEEDED**; `OperationsUT` also under **ASAN+UBSAN**.
- New `OperationsUT` cases: **3/3, 23 assertions** — one authority per accepted item with a second refused twice over; an out-of-range index refused without spending the one that exists; and the seal outliving the object that issued it.
- Full `OperationsUT`: **233/233, 6,001 assertions**. Full `OperationsIT`: **98 passed, 2 skipped, 973/973 assertions**. Full `WinCommanderUT`: **821/821, 11,816**.

## Slice 2, first step: the gate was two gates

`plan.Sources().size() != 1 || report.items.size() != 1` reported one error for two different limitations. They are not the same wall, and lifting them is not the same work: **several sources** need several structural bindings checked against the report, while **one source that expanded into several accepted items** needs only the per-item loop. Told apart, a caller can say which it ran into — and so can this code, when the loop arrives and only one of the two comes down.

The existing test that pinned "batch" was pinning the multiple-sources case all along; it now says so.

### Coverage gap

**The factory still does not loop.** Validating N items, building N transactions, and producing an execution product that journals N results is the remaining work, and it needs a new operation type: `ProviderConditionalCopyOperationFactory::Create` takes exactly one transaction, and there is no composition of operations in this module to build on. That is design work rather than a loop, which is why it is named here rather than attempted in passing.

**Closed by steps B and C.** The loop and its all-or-nothing rollback are below; the execution product now carries the whole set, through the same operation type generalised rather than a second one — see [`provider_conditional_copy_execution_product.md`](provider_conditional_copy_execution_product.md). What remains is step D, the gate itself.

## Slice 2, step B: prepared as a set, committed to as a set

The factory now loops `prepare_item` over every accepted item and holds the prepared transactions together. If any item refuses, the whole set is abandoned **before** the error is returned: a half-prepared batch that returns while holding open transactions leaves temporary state on disk that nothing owns.

### The rollback is not what prevents a leak — it is what makes the answer readable

A transaction aborts itself when it is destroyed, so dropping the vector already undoes them. What that loses is the abort *result*, and the single case worth having is exactly the one silence hides: an abort that cannot confirm `NotPublished`. So the rollback aborts explicitly, in reverse order — the transaction begun last is undone first, so the provider unwinds in the order it built the state up — and it reports the item whose undoing it could not confirm.

### An unconfirmed rollback outranks the reason for it

`StaleSource` tells the user the world moved and **nothing was done**. An abort that cannot say `NotPublished` does not support the second half of that, so the answer becomes `ConditionalCommitAuthorityUnavailable` carrying the destination that may or may not exist. Answering with the original reason would leave a possible file behind an error stating there is none — and this is the same rule the cold-abort path already applies at the application blocker, reached here from the other side.

### Cancellation is checked after preparation, before handing over

An operation built after a cancellation would carry open transactions into the Pool only to abort them, so the user would be told "cancelled" after the Pool had taken ownership of work nobody wants. That is reason enough on its own, and it has a second effect: it is the **first failure that can happen with a transaction already begun** — beginning one is the last thing preparing an item does — which is what gives the rollback a reachable path today instead of leaving it dead until the several-sources gate comes down.

### Step A's outstanding debt, closed here

An item's structural source is matched against whichever entry of `plan.Sources()` names it, rather than against `Sources().front()`. The lookup is deliberately **not** positional: `PlanSource` drops a source whose destination already exists under a `Skip` policy, so the report can be shorter than the plan and accepted item *i* need not come from source *i*. Pairing by index would refuse a sound report as readily as it accepted a mismatched one. With one source the two formulations are the same check, which is why every existing case pins it unchanged.

### Verification

- `OperationsUT` (Debug), `OperationsUT` under **Release ASAN** and **Release UBSAN**, `OperationsIT` and `WinCommanderUT` — all **BUILD SUCCEEDED**.
- New `OperationsUT` cases: **2/2** — a confirmed rollback leaves the reason for it standing (`Cancelled`, one abort, nothing published); an unconfirmed one outranks it (`ConditionalCommitAuthorityUnavailable` naming the destination).
- `ReviewedOperationFactory:` **12/12, 283 assertions** in Debug, and the same 12/283 under **Release ASAN** and **Release UBSAN** with no diagnostics.
- Full `OperationsUT`: **245/245, 6,094 assertions**. Full `OperationsIT`: **98 passed, 2 skipped, 973/973 assertions** — the physical two-volume fixture remains the recorded skip. Full `WinCommanderUT`: **890/890, 12,150 assertions**, unchanged from the recorded baseline.

## Slice 2, step C: the factory hands over a set

The prepared items now become one `ProviderConditionalCopyOperationItem` each and are handed to the batch operation together, so a reviewed batch is one operation with one journal entry rather than N operations the user never asked for. The operation's own decisions are recorded in [`provider_conditional_copy_execution_product.md`](provider_conditional_copy_execution_product.md); two things belong to the factory.

**Two index spaces meet here, and they are not the same map.** An authority is issued for the item's place in the reviewed report, because that is what the review covered. A journal result is numbered by the item's place in `plan.Sources()`, because that is the space the journal validates against — it refuses a result whose index is not a source of the plan, and requires one result per source before an entry may be `Completed`. They coincide for every plan this factory can accept, and provably rather than by assumption: a source leaves the report only through `Skip` on an occupied destination, and the planner records the conflict before that decision, so the report carries a conflict the review refuses. The lookup added in step B is what keeps the derivation honest if that ever stops being true.

**Step D lifted both gates, and what replaced them is the journal's rule.** Several sources are executed; one source expanding into several items needs only the loop that exists. In their place the factory refuses a report that does not account for the plan's sources, one item each — results are numbered in the source space and a completed entry missing one cannot be recorded, so such a plan is unexecutable and this is the last place that can see both sides. It is defence in depth: the only path that stops planning part-way is a cancelled probe, which records a blocker, and a blocked preflight is never accepted. A test pins that reason rather than the unreachable branch.

**The compatibility surface now reads the evidence rather than its single-item projection.** That projection reports any set other than exactly one item as inconsistent, so once a product may carry several it would have been answering the batch's size instead of what happened to it. For one item the two are the same answer, which is why every existing case pins it unchanged.
