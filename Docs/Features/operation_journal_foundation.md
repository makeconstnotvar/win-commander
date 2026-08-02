# Feature: durable operation journal foundation

> Status: schema-v1 journal, exact run authority, atomic terminal evidence, production Copy orchestration, and reconciled Pool release implemented; application mutation consumer remains open
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 14 and 31
> Execution tracker: M3 in `Docs/Development-Plan.md`

## Purpose

`OperationJournal` persists immutable plans and typed item outcomes before lifecycle authority is released. It provides restart classification and durable evidence without automatically resuming filesystem mutations.

## Durability and authority contract

- The journal lives in an existing absolute directory owned by the effective user and not writable by group or others.
- Traversal, lock, journal and staging access is descriptor anchored, no-follow and nonblocking. One process owns an exclusive lock.
- Every mutation builds a complete candidate snapshot in an exclusive `0600` temporary file, syncs it, atomically renames it, then syncs the parent before changing in-memory state.
- Pre-publication persistence faults retain the exact receipt for retry. Post-rename uncertainty returns no new authority and poisons the handle; reopen reconciles durable state.
- Startup durably converts `Admitted` and `Running` entries to `Interrupted`; work is visible and never auto-resumed.
- Corruption, schema mismatch, duplicate plans, invalid lifecycle/item evidence, unsafe file identity/mode and resource limits fail closed.
- Move-only RAII owns every descriptor across callbacks, exceptions and early returns.

`Admit` returns a move-only journal- and exact-plan-bound admission receipt. `TransitionToRunning` consumes it and returns a separate move-only run receipt. `FinalizeAdmission` terminates work that never started. `Finalize(run, item, terminal)` atomically persists one exact item result and its terminal entry state; the run receipt is consumed only after the durable snapshot succeeds.

## Item evidence

Schema v1 stores ordered typed evidence without collapsing ambiguity:

- item status and phase-specific error;
- primary errno plus prior error/errno for cleanup failure;
- bytes copied;
- destination publication as `not_published | published | unknown`;
- filesystem-sync status and errno;
- recovery action.

`Unknown` is valid only for `Failed + Commit + InspectDestination` with sync not attempted. Success, cancellation, skip and cleanup require exact publication evidence. Retry and temporary-removal guidance require `NotPublished`; a confirmed published result cannot be represented as safely retryable.

## Current composition boundary

`CopyOperationOrchestrator` is the production consumer of journal admission/run receipts and the `Pool` finalization barrier. Its public constructor privately joins `ReviewedOperationFactory` to the transaction-backed execution product. It durably terminates all pre-running failures, records one item and terminal state atomically, retains `Finalizing` residency on persistence failure and reuses immutable typed evidence on retry. Injected factories remain test-only.

`CopyOperationRunReceiptCustodian` closes the post-Running ownership gap. Before the transition it reserves a bounded slot for the exact immutable plan, journal storage identity and terminal accessor. After `TransitionToRunning` the slot is armed without allocation. Pre-enqueue cancellation or rejection persists one immutable result; a pre-rename fault retains the exact receipt for `Retry(plan_id)`, which can only call `Finalize`. Accepted enqueue keeps the same slot Pool-owned, so runtime retry cannot race a live operation.

After post-rename uncertainty the custodian drops the poisoned journal, live receipt and accessor while retaining the exact plan and terminal evidence. Recovery requires teardown of the old journal owner, reopening the same parent directory device/inode, and read-only `Reconcile`. Reconciliation confirms only an exact persisted terminal state/result or startup-converted `Interrupted` with no item result. It never enqueues work, calls the provider, or synthesizes a terminal state.

When the operation is still Pool-owned, reconciliation returns `pool_release_required`. `ReleaseReconciled(plan_id)` performs the exact `Pool::RetryFinalization` handshake and releases custody only after the matching finalizer callback confirms release. The slot latches that callback, so synchronous completion, concurrent release attempts, retained finalization, and Pool teardown cannot be confused with a different operation. Public ID-addressed journal mutation is removed; compatibility calls exist only through `OperationJournalTesting`.

Temporary artifacts remain manual evidence until provider-owned bounded staging supplies safe cleanup authority. Operation Center projection, retention/redaction policy and user-driven retry/reconcile UI are also open.

## Verified coverage

- Journal: 27 Debug cases / 592 assertions.
- Copy orchestrator: 13 / 558, including production private-factory construction at 3 / 138, pre-running rejection, shutdown cancellation, typed-outcome retention, exact retry, both post-rename reconciliation outcomes, wrong-storage rejection, exact reconciled Pool release, concurrent release gating and synchronous completion before enqueue returns.
- Pool: 15 / 190.
- Provider result mapper: 4 / 237; transaction-backed execution product: 9 / 188; reviewed factory: 8 / 225; Job lifecycle: 10 / 608.
- Full Debug, Release ASAN and Release UBSAN `OperationsUT`: 165 / 4,468 in each configuration.
- Current M0: 897 / 132,011 in the recorded seeded run.
- Docker-backed Debug ASAN integration: 163 / 89,392.
