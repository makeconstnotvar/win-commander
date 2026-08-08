# Operation Center model and control projection

> Status: value model, schema-v3 allocation, terminal-history hydration, cold exact-storage history refresh, explicit confirmed-recovery projection with one user-invoked projection-only retry, receipt-bound `CopyAs` submission, live executor residency, revision-checked engine cancellation, value-only `operation.cancel`, and bounded static `operationCenter.open` Registry presentation are implemented; the live/full Operation Center and pause/resume/retry remain open
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 13.5 and 14
> Execution tracker: R6 in `Docs/refactor_plan.md` and M3 in `Docs/Development-Plan.md`

## Purpose

`OperationCenterModel` is the engine-owned value-model foundation for queue activity and durable operation history. `OperationCenterCoordinator::Create` imports a startup snapshot of terminal journal history into it; `RefreshColdHistory` can later append only absent terminal or interrupted history from the same reopened storage while the coordinator remains cold. The process-owned coordinator is now the sole production bridge from bounded reviewed `CopyAs` submission to the private receipt-aware Orchestrator path; it projects `Queued`, Start and durable terminal outcomes into immutable records. For a newly admitted live operation it holds an exact process-local `Pool`/`Operation` residency solely for revision-checked engine cancellation. The production Registry owns value-only `operation.cancel` and snapshot-presentation `operationCenter.open` definitions; Explorer consumes them in its compact `More` menu and bounded static panel, while no Registry layer owns a live executor.

The contract gives every submitted execution an opaque `OperationId`, retains its relationship with the immutable `OperationPlanId`, and makes an exact revision part of every state-changing request. `OperationPlanId` remains the identity of a reviewed plan and journal entry; `OperationId` identifies one execution record. Until custody/recovery is re-keyed from plan ID, a user retry must create a new plan as well as a new execution record. Journal-finalization retry remains an internal recovery action over its exact journal receipt.

## Current state and migration boundary

The engine now has a shared `OperationId` value type, `OperationCenterModel`, immutable `OperationRecord`, derived declarative controls and revision-checked lifecycle transitions. `OperationId` is non-default-constructible and uses canonical `op-<positive-decimal-sequence>` serialization. Production model publication accepts an exact `OperationJournalAdmissionReceipt` only: the coordinator prepares a move-only model draft before journal allocation, then binds the record's ID only from the durable receipt. The old model-owned ID issuer is private to `OperationCenterModelTesting`; records contain values only and snapshots return copies. Journal schema v3 owns production ID allocation through a move-only reservation and persists the ID high-water mark alongside every entry; it migrates v1/v2 atomically in the existing filename/lock domain.

The current engine still has durable plan/journal identities, transaction-backed operation lifecycle, Pool finalization retention and legacy compact views that hold `std::shared_ptr<Operation>`. The production Registry defines both `operation.cancel` and `operationCenter.open`. Cancel accepts only a value `{OperationId, expected revision, can-cancel}` target derived from an immutable record, and maps every live coordinator rejection to a typed disabled reason. Open requires a borrowed synchronous Explorer target, obtains a weak-coordinator snapshot and copies the `OperationRecord` vector at the Registry boundary before AppKit receives it. The compact `More` menu still filters to active records; the separate static panel renders active plus terminal/interrupted history with operation type, lifecycle state, opaque IDs and available timestamps. It adds a Cancel button only by rebuilding the same value context. Reopening obtains a fresh snapshot; neither surface observes the model. The model has no Journal, Pool, executor, callback or UI authority. The coordinator takes a value snapshot after `OperationJournal::Open`, maps only terminal or startup-interrupted entries to revision-1 history records, and retains no Journal. Its cold refresh checks the same storage identity, rejects a live journal, staged admission, live residency or active model record, then appends only unknown matching terminal history without altering existing records. The explicit Copy recovery flow invokes that projection only after confirmed reconciliation and required Pool release. An exact `ColdHistoryBusy` result mints only a shared opaque storage identity plus consumed-bit continuation; the user may invoke one projection-only refresh, which rechecks the current journal through `RefreshColdHistory` and never starts another custody pass. For one newly admitted bounded `CopyAs` it retains the exact `shared_ptr<Pool>` plus `shared_ptr<Operation>` only from private pre-enqueue handoff to durable terminal retirement.

## Compact Explorer Operations menu and bounded static snapshot

The Explorer command bar's `More` control now opens a native menu with an `Operations` section. It obtains one `OperationCenterModel::Snapshot()` only when the menu is built. With an unavailable runtime it renders a disabled availability item; with an empty snapshot it renders a disabled empty item. Otherwise it renders a disabled descriptive line for each current active value record (`type — state (OperationId)`) and a separately presented `Cancel <OperationId>` command.

The menu target owns only the copied `CommandContext` made by `OperationCancelContextFromRecord`: invocation source, immutable `OperationId`, the snapshot's expected revision, and `can_cancel`. Presentation calls `CommandRegistry::QueryState` for that context and applies the shared `CommandPresentationAdapter`, so unavailable cancellation has the same localized tooltip and accessibility help as other Registry controls. A click calls `CommandRegistry::Execute`; it never calls `OperationCenterCoordinator::Cancel`, `Operation::Stop`, a `Pool`, or a Journal directly. The Registry repeats its identity, revision, lifecycle and residency validation through the sealed coordinator port. A live rejection remains visible as a failure alert rather than being reported as a successful cancellation.

`operationCenter.open` uses the same `More` section but opens a native static panel instead of expanding the menu. The command's weak provider returns a value snapshot, the Registry copies that vector again, and the presenter retains only its copy. The panel includes every current record, including terminal and startup-interrupted history, as type/state/operation ID/plan ID/created-started-finished timestamps. It creates one Cancel button only for each snapshot record whose existing control projection allows it, applies the shared disabled tooltip/accessibility adapter, and dispatches through `operation.cancel` with the snapshot ID and revision. There is no observer, timer, progress polling, Journal, Pool, executor, source/destination path, typed terminal detail, log, pause, resume, retry or persistence authority. Reopen is the explicit refresh action. `Executed` means that the exact stop request was accepted, while a subsequent snapshot may already be `Cancelling` or terminal. Legacy `PoolViewController` and `BriefOperationViewController` remain separate Commander compatibility views: they retain raw operations and own their pre-existing direct pause/resume/stop controls, so they are not an authority for this Explorer slice.

## Identity and records

`OperationId` is an opaque execution identifier with a serialized stable representation. Production allocation belongs exclusively to the schema-v3 journal, whose durable high-water mark prevents reuse across restarts and future history retention. The coordinator first allocates every fallible model draft resource, then receives a journal reservation, durably admits the exact plan and publishes `Queued` by binding the receipt's exact ID. The private Orchestrator receipt path validates the receipt's journal instance and full reviewed plan before construction or Pool admission; it never calls `Admit` a second time. A failed draft or journal admission exposes no model record, and a failed model publication finalizes the exact durable admission as `Failed`. The ID is never derived from a pointer or container position and is never reassigned after removal.

An immutable `OperationRecord` contains at least:

- `OperationId`, `OperationPlanId`, optional `retry_of`, and a monotonically increasing `revision`;
- operation kind, source/destination presentation, creation/start/finish timestamps, and owning window or application scope;
- projected lifecycle state, progress/current item, typed item and terminal evidence, and log summary;
- derived control availability and typed disabled reason for cancel, pause, resume and retry;
- durable-history classification, including a startup `Interrupted` marker.

`OperationRecord` stores values and immutable shared value objects only. It carries no `Operation *`, `Job *`, `Pool *`, window, view, callback or executor closure. The implemented model returns complete replacement snapshots and per-record revisions; its future observers will use the same values, so consumers render the matching snapshot and treat a revision gap as a fresh read.

## Ownership and event flow

`OperationCenterCoordinator` provides startup hydration, cold history refresh, exact admission staging and the only production receipt-to-Orchestrator bridge for bounded `CopyAs`. It does not retain the Journal, which preserves the recovery service's exclusive reopen authority, and it rejects live journal entries rather than inferring them at startup. `RefreshColdHistory` accepts only that coordinator's storage identity; a history gate serializes it with public stage/commit paths, and it fails closed before any model mutation when a draft, live residency, active record or active journal entry exists. The explicit Copy recovery composition invokes it synchronously only after `Service` has released its mutex and produced confirmed reconciliation plus any required Pool release; it holds the current Journal only for that call, does not retain it, and reports a projection deferral separately from custody recovery. Its committed receipt reaches the private Orchestrator path after `Queued` is visible. After the run receipt is armed, a coordinator-only pre-enqueue handoff consumes a preallocated `LiveResidencyDraft`, records the exact `(OperationId, Pool, Operation)`, and holds a private recursive admission lease through `BeginEnqueue` and `Pool::TryEnqueue`; external submission hooks never receive queue or executor authority. Handoff failure durably finalizes `Failed` with no Pool admission. The coordinator's preinstalled weak callbacks reduce Start and every exact durable terminal outcome before forwarding the application's observer; a synchronously rejected pre-enqueue submission rereads only the exact Journal entry and reduces it only when terminal persistence succeeded. `Pool` remains the executor for each window. Aggregation across windows remains a separate authority.

```text
exact plan
        -> coordinator allocates unpublished model draft
        -> Journal reserves OperationId and durably admits exact plan
        -> exact receipt binds OperationId <-> OperationPlanId and publishes Queued
        -> private Orchestrator receipt path validates exact plan and arms run receipt
        -> coordinator-only handoff records exact live residency and leases admission
        -> Pool admission
        -> weak Start and durable-terminal reducers update the matching record
        -> OperationCenterModel immutable revisions
        -> Explorer / compact view / CommandRegistry presentation

future CommandRegistry target { OperationId, expected_revision }
        -> coordinator engine control port
        -> exact live residency
        -> Pool / Operation executor
```

`OperationCenterCoordinator::Cancel` resolves the value record and exact residency, confirms that the same `Operation` remains in the matching `Pool`, then invokes `Operation::Stop()` without a coordinator or Pool lock. Its per-residency recursive gate serializes queue admission, cancellation and synchronous terminal reentry; a terminal callback during Stop is deferred until the outer request produces `Cancelling → Finalizing → terminal` and retires the residency.

The command and UI layers hold only `OperationId` plus the snapshot revision that produced their enabled state. Raw executor pointers remain confined to the coordinator and existing migration surfaces.

## Control port

The Registry adapter consumes the implemented engine port without extending its authority:

```text
OperationCenterCoordinator::Cancel(OperationId id, Revision expected_revision)
    -> Accepted | OperationNotFound | StaleRevision(current record)
       | CancelUnavailable(current record) | ResidencyUnavailable(current record when known)
       | CancelInProgress(current record) | StopRejected(current record)
```

`Accepted` means the exact `Operation::Stop()` request returned true. It publishes a newer `Cancelling` revision as soon as the executor acknowledges the request; a synchronous callback can make a follow-up snapshot terminal already. The terminal `Cancelled`, `Failed`, `Completed`, or publication-uncertain result continues through the ordinary typed terminal-finalization path.

The result carries an immutable current record when it is available and never exposes an executor reference. `StaleRevision` describes an extant record whose revision differs from the target; `CancelUnavailable` describes a state with no applicable transition; `ResidencyUnavailable` describes a live-record request without an exact active Pool residency; `CancelInProgress` prevents recursive Stop; `StopRejected` preserves a refused executor request. Registry command-state and user-facing mapping are future consumers.

Cancel is applicable to the coordinator's queued, running and paused record states. It is rejected after finalization begins and for terminal history. The coordinator serializes duplicate and reentrant requests for the same `OperationId`; the first accepted request owns the transition and later requests observe its newer revision. A command availability check remains advisory, and `Cancel` repeats exact identity, revision, residency and lifecycle validation at execution time.

Pause, resume and user retry will use the same `OperationId`/revision control shape when their engine authority is present. User retry creates a new admitted execution record with a new `OperationId`; it retains a value-only link to the prior terminal record. The recovery custodian's `Retry(plan_id)` finalization action stays private to recovery and does not become a Registry command target.

## Durability, finalization and restart

The record relationship becomes durable before queued work is visible. Every potentially allocating model step, including one `LiveResidencyDraft` and its vector capacity, occurs before journal admission; receipt-bound publication is a pre-reserved no-throw move into the model. `Queued` is therefore visible before `Pool::TryEnqueue` can synchronously start or finish an operation. The bounded production coordinator reduces Start and the existing durable-terminal callback into the same record before it forwards application presentation. Pre-enqueue failures are reduced only after the coordinator rereads an exact durably terminal Journal entry; a durable-uncertain rejection stays non-terminal. Progress and item detail remain outside this reducer.

At startup, the schema-v3 journal preserves each historical `OperationId`, its high-water mark and its conversion of admitted or running work to `Interrupted`. `OperationCenterCoordinator::Create` imports that post-open terminal/interrupted snapshot into the model as immutable revision-1 records. Because the journal does not yet persist a distinct admission timestamp, the hydration projection uses the immutable plan creation time as `created_at`, leaves `started_at` absent, and maps the journal update time to `finished_at`; it must not be presented as a reconstructed live timeline. Execution remains inactive until a separate, explicitly admitted user retry creates its new record. Pool residency, in-memory executor handles, transient callbacks and in-flight cancellation requests are process-local and are reconstructed only for newly admitted work.

After an exclusive recovery reopen, the explicit Copy recovery composition calls `RefreshColdHistory` on the existing coordinator only after the retained receipt confirms terminal/interrupted evidence and any required `ReleaseReconciled` succeeds. It imports the same value projection only from the exact journal storage, validates the whole snapshot before publishing, preserves each already-present record exactly, and appends only unknown terminal/interrupted records. On exactly `ColdHistoryBusy`, it mints an opaque shared continuation that contains only the coordinator's value storage identity and an atomic consumed bit. The alert offers one explicit `Refresh history` action. Its retry first obtains the current recovery journal, verifies the retained Center identity, and calls only `RefreshColdHistory`; that method repeats the exact journal-storage validation. A second busy response becomes `RetryExhausted`, and a non-busy error becomes `ProjectionFailed`; neither result mints another continuation. The helper owns neither recovered executor, callback, `Pool` residency nor Journal after it returns, and this path never calls custody retry, reopen, reconcile or Pool release.

## Required acceptance evidence

The pure model foundation passes 4 cases / 64 assertions in Debug. It covers opaque serializable non-default `OperationId`, test-only isolated model allocation, value-only initial records, exact stale-revision rejection, allowed finalization ordering, terminal control suppression and immutable snapshot copies. Schema-v3 journal coverage passes 33 / 752, including journal-owned reservation, durable high-water persistence, exact-ID receipt propagation, strict decode and normal/uncertain v1/v2 migration. Coordinator coverage passes 15 / 284: terminal/interrupted hydration, active-entry rejection, invisible pre-admission drafts, capacity for multiple simultaneous drafts, exact receipt-bound `Queued` publication, journal-admission rollback, post-admission foreign-draft compensation, same-storage append-only/idempotent cold refresh, foreign/active-journal rejection and staged-admission rejection. The focused coordinator/orchestrator/control subset passes 31 / 1,060, covering `Queued` before Pool addition, Start reduction, durable terminal reduction, stale revision rejection, exact live cancellation, reentrant cancellation rejection, private pre-enqueue handoff failure with zero Pool admission, no pending custody and cold-refresh serialization. The explicit recovery/projection bridge passes 12 / 197 in Debug, Release ASAN and Release UBSAN: it proves a required release before deferral, one successful projection-only retry, retry exhaustion while the Center remains busy, no continuation for a non-busy failure, and rejection after a fresh journal-identity mismatch. The current full Debug `OperationsUT` result is 210 / 211 cases and 5,662 / 5,666 assertions; its only failure remains the host-specific NativeCreateCopy set-ID metadata baseline. The focused coordinator/orchestrator/control subset passes Release ASAN and UBSAN without diagnostics. The app-boundary filter passes 4 / 70 after the shared API update.

After the bounded static-panel increment, `WinCommanderUT 'nc::core::CommandRegistry*' --rng-seed 424242` passes 17 cases / 183 assertions, `WinCommanderUT '*operation.cancel*' --rng-seed 424242` passes 3 / 51, the compact-menu filter passes 4 / 70, and `WinCommanderUT 'Explorer presentation geometry Operation Center*' --rng-seed 424242` passes 3 / 104. Core coverage proves missing definition, target/provider/presenter failure, runtime snapshot/presenter rejection, weak-provider unavailability before invocation, and copied value boundaries. The AppKit cases prove terminal-history rendering, an unchanged already-open panel after the provider source changes, disabled expired-coordinator presentation, and that a retained old Cancel control preserves its original ID/revision through a reopen instead of addressing a new record at its old index. Fresh Release ASAN and Release UBSAN builds of the affected UnitTests target followed by the core and panel filters pass without diagnostics. The existing `CommandPresentationAdapter` test covers disabled button/menu presentation, localized tooltip and accessibility help. Full Debug `WinCommanderUT --rng-seed 424242` records 342 passed / 346 cases and 5,580 passed / 5,584 assertions; the four failures are the known headless pasteboard host baselines. This adapter is registered during application composition and holds only a weak coordinator control port. Neither Explorer surface has a raw executor path.

The following evidence remains for the full contract:

- model observers receive immutable value-only snapshots; both current Explorer surfaces intentionally snapshot at open, while a full Center needs live update semantics; stale, missing and reused-looking targets cannot address another execution.
- live Operation Center presentation derives availability from the same `OperationRecord` revision passed to the engine port; the compact menu and static panel already use this rule for Cancel, and direct command execution continues to revalidate the target without a raw pointer.
- pause, resume and user retry gain their own exact engine authority and lifecycle/teardown coverage.
- typed terminal evidence is durable and visible before Pool removal; persisted history remains readable across restart and interrupted entries never re-enqueue automatically.
- multi-window aggregation, bounded retention/redaction, compact-view migration and full Explorer Operation Center rendering receive focused tests as each consumer lands.

Because the remaining boundary combines persistent state, Pool lifecycle, control races and ownership, its implementation requires focused `OperationsUT` and `WinCommanderUT` coverage, a full affected Debug suite, and Release ASAN/UBSAN evidence for the final concurrency-bearing batch.

## Out of scope for this foundation

This contract selects the model/control authority and cancellation semantics. The implemented state enum is the narrow Copy-foundation subset (`Queued`, `Running`, `Paused`, `Cancelling`, `Finalizing` and terminal outcomes); canonical planning/preparation/wait/retry/rollback states remain future work. It leaves the full Explorer Operation Center screen, retention/redaction policy, progress/source/destination/result/log projection, pause/resume engine support, user retry UX, remote transfer adoption and broad mutation-consumer migration to later R6 slices.

## Related documents

- [`Docs/win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md)
- [`Docs/Development-Plan.md`](../Development-Plan.md)
- [`Docs/refactor_plan.md`](../refactor_plan.md)
- [`operation_journal_foundation.md`](operation_journal_foundation.md)
- [`copy_operation_orchestrator_foundation.md`](copy_operation_orchestrator_foundation.md)

---

# OC-x: what "Retry" may actually offer

The Operation Center could show a finished operation and its journal. What it had no model for was the control a user reaches for next: **Retry**. Offering it means deciding, per item, whether a second attempt is safe — and the interesting answers are all refusals.

## A succeeded item is never retried

The file arrived. Copying it again could overwrite something the user has changed at the destination since, so "retry" would have destroyed work rather than recovered it. This is the one that matters most, and it is why a succeeded item is refused even when the publication state is also unknown: reporting *that* would send someone to inspect a file that is simply finished.

## A skipped item is never retried either

Skipping was an answer — from a conflict policy, or from the user. A retry that quietly revisits it overrides a decision already made, without asking again.

## A failure is retryable only when a second attempt could differ

`SourceChanged` and `DestinationChanged` mean the world moved under a plan; a fresh plan sees the new world. `Read`, `Write`, `Commit`, `Metadata` and `Cleanup` can be transient — a busy disk, a link that came back, a lock released.

`PermissionDenied` cannot resolve itself, and an error nobody could name is not evidence that a second attempt goes better. Retrying either is how a failure becomes a loop — the same reasoning `IsRetryableRemoteFailure` applies to connections, arrived at independently for a different subsystem.

## An unknown publication state outranks permission to try

If it is not known whether the destination was written, a blind repeat could overwrite what did land or leave a second copy beside it. So this is checked *after* the status decides an attempt would be allowed, and it turns a yes into **NeedsInspection** — which is a different answer from "no", and should reach the user as one.

## Verification

- `OperationsUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- New `OperationsUT 'nc::ops::DecideOperationRetry*'`: **8/8 cases, 35 assertions** — a succeeded item refused, and refused as *already done* even when its publication state is unknown; a skipped item refused as a decision; a cancelled item always offered; every retryable failure kind and both non-retryable ones; an unknown publication state overriding a permitted attempt after both a failure and a cancellation, and not blocking one where the state is known; journal order preserved with indices intact; and nothing offered when every item is finished, skipped, or refused permission.
- Full `OperationsUT --rng-seed 424242`: **241/241 cases, 6,036 assertions**.

### Coverage gap

**Nothing acts on the decision.** A retry has to become a *new plan* over the retryable items and go through review again, because the evidence the original plan was accepted against is exactly what a failure like `SourceChanged` says is stale. No control is wired, and log capture and persistent history beyond the current hydration remain open in Q2-4.
