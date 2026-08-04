# Operation Center model and control projection

> Status: value model, schema-v3 allocation, terminal-history hydration, cold exact-storage history refresh, explicit confirmed-recovery history projection, receipt-bound `CopyAs` submission, live executor residency, revision-checked engine cancellation, value-only `operation.cancel` Registry adapter and its compact Explorer Operations menu consumer are implemented; the full Operation Center and pause/resume/retry remain open
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 13.5 and 14
> Execution tracker: R6 in `Docs/refactor_plan.md` and M3 in `Docs/Development-Plan.md`

## Purpose

`OperationCenterModel` is the engine-owned value-model foundation for queue activity and durable operation history. `OperationCenterCoordinator::Create` imports a startup snapshot of terminal journal history into it; `RefreshColdHistory` can later append only absent terminal or interrupted history from the same reopened storage while the coordinator remains cold. The process-owned coordinator is now the sole production bridge from bounded reviewed `CopyAs` submission to the private receipt-aware Orchestrator path; it projects `Queued`, Start and durable terminal outcomes into immutable records. For a newly admitted live operation it holds an exact process-local `Pool`/`Operation` residency solely for revision-checked engine cancellation. The production Registry now owns a value-only `operation.cancel` definition, and Explorer has one compact consumer in its `More` menu; no Registry layer owns a live executor.

The contract gives every submitted execution an opaque `OperationId`, retains its relationship with the immutable `OperationPlanId`, and makes an exact revision part of every state-changing request. `OperationPlanId` remains the identity of a reviewed plan and journal entry; `OperationId` identifies one execution record. Until custody/recovery is re-keyed from plan ID, a user retry must create a new plan as well as a new execution record. Journal-finalization retry remains an internal recovery action over its exact journal receipt.

## Current state and migration boundary

The engine now has a shared `OperationId` value type, `OperationCenterModel`, immutable `OperationRecord`, derived declarative controls and revision-checked lifecycle transitions. `OperationId` is non-default-constructible and uses canonical `op-<positive-decimal-sequence>` serialization. Production model publication accepts an exact `OperationJournalAdmissionReceipt` only: the coordinator prepares a move-only model draft before journal allocation, then binds the record's ID only from the durable receipt. The old model-owned ID issuer is private to `OperationCenterModelTesting`; records contain values only and snapshots return copies. Journal schema v3 owns production ID allocation through a move-only reservation and persists the ID high-water mark alongside every entry; it migrates v1/v2 atomically in the existing filename/lock domain.

The current engine still has durable plan/journal identities, transaction-backed operation lifecycle, Pool finalization retention and legacy compact views that hold `std::shared_ptr<Operation>`. `operation.cancel` and `operationCenter.open` are declared command IDs; the production Registry defines only `operation.cancel`. Its context accepts only a value `{OperationId, expected revision, can-cancel}` target derived from an immutable record, and its result handler maps every live coordinator rejection to a typed disabled reason. The Explorer `More` menu opens one immutable snapshot, filters it to active records, renders each active record's operation type, lifecycle state and opaque ID, and adds its separate Cancel item only through that context. Terminal history produces the explicit empty active-operations state rather than a menu history. The model has no Journal, Pool, executor, callback or UI authority. The coordinator takes a value snapshot after `OperationJournal::Open`, maps only terminal or startup-interrupted entries to revision-1 history records, and retains no Journal. Its cold refresh checks the same storage identity, rejects a live journal, staged admission, live residency or active model record, then appends only unknown matching terminal history without altering existing records. The explicit Copy recovery flow invokes that projection only after confirmed reconciliation and required Pool release; a deferred projection never changes the recovery outcome or starts another custody pass. For one newly admitted bounded `CopyAs` it retains the exact `shared_ptr<Pool>` plus `shared_ptr<Operation>` only from private pre-enqueue handoff to durable terminal retirement.

## Compact Explorer Operations menu

The Explorer command bar's `More` control now opens a native menu with an `Operations` section. It obtains one `OperationCenterModel::Snapshot()` only when the menu is built. With an unavailable runtime it renders a disabled availability item; with an empty snapshot it renders a disabled empty item. Otherwise it renders a disabled descriptive line for each current value record (`type — state (OperationId)`) and a separately presented `Cancel <OperationId>` command.

The menu target owns only the copied `CommandContext` made by `OperationCancelContextFromRecord`: invocation source, immutable `OperationId`, the snapshot's expected revision, and `can_cancel`. Presentation calls `CommandRegistry::QueryState` for that context and applies the shared `CommandPresentationAdapter`, so unavailable cancellation has the same localized tooltip and accessibility help as other Registry controls. A click calls `CommandRegistry::Execute`; it never calls `OperationCenterCoordinator::Cancel`, `Operation::Stop`, a `Pool`, or a Journal directly. The Registry repeats its identity, revision, lifecycle and residency validation through the sealed coordinator port. A live rejection remains visible as a failure alert rather than being reported as a successful cancellation.

The menu is a bounded snapshot presentation. It does not observe the model while open; reopening it obtains the current immutable snapshot. `Executed` means that the exact stop request was accepted, while a subsequent snapshot may already be `Cancelling` or terminal. This is adequate for the first cancel control but is not a central screen, progress renderer, persistent-log UI, or cross-window aggregate surface. Legacy `PoolViewController` and `BriefOperationViewController` remain separate Commander compatibility views: they retain raw operations and own their pre-existing direct pause/resume/stop controls, so they are not an authority for this Explorer slice.

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

The record relationship becomes durable before queued work is visible. Every potentially allocating model step, including one `LiveResidencyDraft` and its vector capacity, occurs before journal admission; receipt-bound publication is a pre-reserved no-throw move into the model. `Queued` is therefore visible before `Pool::TryEnqueue` can synchronously start or finish an operation. The bounded production coordinator reduces Start and the existing durable-terminal callback into the same record before it forwards application presentation. Pre-enqueue failures are reduced only after the coordinator rereads an exact durably terminal Journal entry; a durable-uncertain rejection stays non-terminal. Progress, item detail and broader recovery-projection retry/defer policy remain outside this reducer.

At startup, the schema-v3 journal preserves each historical `OperationId`, its high-water mark and its conversion of admitted or running work to `Interrupted`. `OperationCenterCoordinator::Create` imports that post-open terminal/interrupted snapshot into the model as immutable revision-1 records. Because the journal does not yet persist a distinct admission timestamp, the hydration projection uses the immutable plan creation time as `created_at`, leaves `started_at` absent, and maps the journal update time to `finished_at`; it must not be presented as a reconstructed live timeline. Execution remains inactive until a separate, explicitly admitted user retry creates its new record. Pool residency, in-memory executor handles, transient callbacks and in-flight cancellation requests are process-local and are reconstructed only for newly admitted work.

After an exclusive recovery reopen, the explicit Copy recovery composition calls `RefreshColdHistory` on the existing coordinator only after the retained receipt confirms terminal/interrupted evidence and any required `ReleaseReconciled` succeeds. It imports the same value projection only from the exact journal storage, validates the whole snapshot before publishing, preserves each already-present record exactly, and appends only unknown terminal/interrupted records. The helper owns neither recovered executor, callback, `Pool` residency nor Journal after it returns; a cold-state rejection remains a presentation deferral and never changes the durable recovery result or reconstructs a live operation.

## Required acceptance evidence

The pure model foundation passes 4 cases / 64 assertions in Debug. It covers opaque serializable non-default `OperationId`, test-only isolated model allocation, value-only initial records, exact stale-revision rejection, allowed finalization ordering, terminal control suppression and immutable snapshot copies. Schema-v3 journal coverage passes 33 / 752, including journal-owned reservation, durable high-water persistence, exact-ID receipt propagation, strict decode and normal/uncertain v1/v2 migration. Coordinator coverage passes 15 / 284: terminal/interrupted hydration, active-entry rejection, invisible pre-admission drafts, capacity for multiple simultaneous drafts, exact receipt-bound `Queued` publication, journal-admission rollback, post-admission foreign-draft compensation, same-storage append-only/idempotent cold refresh, foreign/active-journal rejection and staged-admission rejection. The focused coordinator/orchestrator/control subset passes 31 / 1,060, covering `Queued` before Pool addition, Start reduction, durable terminal reduction, stale revision rejection, exact live cancellation, reentrant cancellation rejection, private pre-enqueue handoff failure with zero Pool admission, no pending custody and cold-refresh serialization. The explicit recovery/projection bridge passes 8 / 107 in Debug, Release ASAN and Release UBSAN: confirmed reopen appends only `Interrupted` history to a cold Center, while a busy Center defers projection without changing custody evidence. The pre-checkpoint isolated Debug `OperationsUT` result was 195 / 196 cases and 5,270 / 5,274 assertions; its only failure remains the host-specific NativeCreateCopy set-ID metadata baseline. The current full Debug `OperationsUT` result is 197 / 5,295. The focused coordinator/orchestrator/control subset passes Release ASAN and UBSAN without diagnostics. The app-boundary filter passes 4 / 70 after the shared API update.

After the compact-menu rebuild, `WinCommanderUT 'nc::core::CommandRegistry*' --rng-seed 424242` passes 12 cases / 115 assertions, `WinCommanderUT '*operation.cancel*' --rng-seed 424242` passes 3 / 51, and the focused non-modal AppKit `WinCommanderUT 'Explorer presentation geometry compact Operations menu*' --rng-seed 424242` passes 4 cases / 69 assertions. The pure and Registry cases cover missing-Registry fail-closed behavior, value-only context projection, exact ID/revision execution and every typed live rejection. The AppKit builder/menu cases cover unavailable and empty branches, value record type/state/ID rendering, and Registry-gated Cancel presentation without a modal popup. The existing `CommandPresentationAdapter` test covers disabled button/menu presentation, localized tooltip and accessibility help. Full Debug `WinCommanderUT --rng-seed 424242` passes 333 cases / 5,376 assertions on the current host. This adapter is registered during application composition and holds only a weak coordinator control port. The compact Explorer menu has no raw executor path.

The following evidence remains for the full contract:

- model observers receive immutable value-only snapshots; the compact menu intentionally snapshots only at open, while a full Center needs live update semantics; stale, missing and reused-looking targets cannot address another execution.
- recovery projection retries/defer policy gives a bounded user-visible history update when the Center is busy, without reconstructing a live executor or pre-existing callback.
- full Operation Center presentation derives availability from the same `OperationRecord` revision passed to the engine port; the compact menu already uses this rule for Cancel, and direct command execution continues to revalidate the target without a raw pointer.
- pause, resume and user retry gain their own exact engine authority and lifecycle/teardown coverage.
- typed terminal evidence is durable and visible before Pool removal; persisted history remains readable across restart and interrupted entries never re-enqueue automatically.
- multi-window aggregation, bounded retention/redaction, compact-view migration and full Explorer Operation Center rendering receive focused tests as each consumer lands.

Because the remaining boundary combines persistent state, Pool lifecycle, control races and ownership, its implementation requires focused `OperationsUT` and `WinCommanderUT` coverage, a full affected Debug suite, and Release ASAN/UBSAN evidence for the final concurrency-bearing batch.

## Out of scope for this foundation

This contract selects the model/control authority and cancellation semantics. The implemented state enum is the narrow Copy-foundation subset (`Queued`, `Running`, `Paused`, `Cancelling`, `Finalizing` and terminal outcomes); canonical planning/preparation/wait/retry/rollback states remain future work. It leaves bounded recovery-projection retry/defer UX, the full Explorer Operation Center screen, retention/redaction policy, progress/source/destination/result/log projection, pause/resume engine support, user retry UX, remote transfer adoption and broad mutation-consumer migration to later R6 slices.

## Related documents

- [`Docs/win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md)
- [`Docs/Development-Plan.md`](../Development-Plan.md)
- [`Docs/refactor_plan.md`](../refactor_plan.md)
- [`operation_journal_foundation.md`](operation_journal_foundation.md)
- [`copy_operation_orchestrator_foundation.md`](copy_operation_orchestrator_foundation.md)
