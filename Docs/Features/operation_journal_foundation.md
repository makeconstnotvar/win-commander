# Feature: durable operation journal foundation

> Status: schema-v3 journal-owned execution-ID allocation, exact run authority, atomic terminal evidence, production Copy orchestration, bounded `CopyAs` consumer and reconciled Pool release implemented
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 14 and 31
> Execution tracker: M3 in `Docs/Development-Plan.md`

The physical-volume fixture uses a fresh exact journal under a descriptor-anchored internal test workspace. Hardware power-loss recovery must reopen before retry or cleanup and relies on the existing startup conversion of `Admitted`/`Running` to `Interrupted`; the required checkpoint protocol is `Docs/Features/reviewed_copy_as_physical_volume_protocol.md`.

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

Schema v3 stores a required canonical `OperationId` beside every immutable plan and a root `next_operation_sequence` high-water mark. `ReserveOperationId()` returns a move-only, process-local journal reservation; only `Admit(reservation, plan)` can consume it, atomically persisting the exact entry and high-water mark. This removes raw production ID admission. `TransitionToRunning` consumes the resulting exact admission receipt and returns a separate move-only run receipt. `FinalizeAdmission` terminates work that never started. `Finalize(run, item, terminal)` atomically persists one exact item result and its terminal entry state; the run receipt is consumed only after the durable snapshot succeeds. Strict v1 and v2 decoding migrates in place before `Open` exposes a snapshot: v1 derives `op-1…op-N` by entry order, while v2 derives the high-water mark from persisted IDs. The filename and lock domain remain `operation-journal-v1.*` for migration safety.

## Item evidence

The schema-v3 entry keeps ordered typed evidence without collapsing ambiguity:

- item status and phase-specific error;
- primary errno plus prior error/errno for cleanup failure;
- bytes copied;
- destination publication as `not_published | published | unknown`;
- filesystem-sync status and errno;
- recovery action.

`Unknown` is valid only for `Failed + Commit + InspectDestination` with sync not attempted. Success, cancellation, skip and cleanup require exact publication evidence. Retry and temporary-removal guidance require `NotPublished`; a confirmed published result cannot be represented as safely retryable.

## Current composition boundary

`CopyOperationOrchestrator` is the production consumer of journal admission/run receipts and the `Pool` finalization barrier. Its public constructor privately joins `ReviewedOperationFactory` to the transaction-backed execution product. It durably terminates all pre-running failures, records one item and terminal state atomically, retains `Finalizing` residency on persistence failure and reuses immutable typed evidence on retry. Restricted hooks are installed while cold, and an owning exact durable outcome is delivered at most once after finalization or reconciliation. Pool terminal-transition authority is preallocated before start; durable non-success releases through `ReleaseWithoutCompletion`. Injected factories remain test-only.

`CopyOperationRunReceiptCustodian` closes the post-Running ownership gap. Before the transition it reserves a bounded slot for the exact immutable plan, journal storage identity and terminal accessor. After `TransitionToRunning` the slot is armed without allocation. Pre-enqueue cancellation or rejection persists one immutable result; a pre-rename fault retains the exact receipt for `Retry(plan_id)`, which can only call `Finalize`. Accepted enqueue keeps the same slot Pool-owned, so runtime retry cannot race a live operation.

After post-rename uncertainty the custodian drops the poisoned journal, live receipt and accessor while retaining the exact plan and terminal evidence. Recovery requires teardown of the old journal owner, reopening the same parent directory device/inode, and read-only `Reconcile`. Reconciliation confirms only an exact persisted terminal state/result or startup-converted `Interrupted` with no item result. It never enqueues work, calls the provider, or synthesizes a terminal state.

When the operation is still Pool-owned, reconciliation returns `pool_release_required`. `ReleaseReconciled(plan_id)` performs the exact `Pool::RetryFinalization` handshake and releases custody only after the matching finalizer callback confirms release. The slot latches that callback, so synchronous completion, concurrent release attempts, retained finalization, and Pool teardown cannot be confused with a different operation. Public ID-addressed journal mutation is removed; compatibility calls exist only through `OperationJournalTesting`.

Temporary artifacts remain manual evidence until provider-owned bounded staging supplies safe cleanup authority. ADR 0002 reserves that authority for a helper-owned descriptor/lease protocol; the journal must not infer cleanup from a stored pathname. Operation Center projection, retention/redaction policy and user-driven retry/reconcile UI are also open.

## Verified coverage

- Journal: 33 Debug cases / 752 assertions, including journal-issued move-only reservations, durable high-water persistence, exact-ID forged-receipt rejection, strict schema-v3 IDs, and deterministic v1/v2 migration under normal and post-rename-uncertain persistence.
- Copy orchestrator: 19 / 849, including production private-factory construction at 3 / 138, restricted hook validation/installation, exact durable-outcome delivery, pre-running rejection, shutdown cancellation, exact receipt validation/no-re-admission, typed-outcome retention, exact retry, both post-rename reconciliation outcomes, wrong-storage rejection, exact reconciled Pool release, concurrent release gating and synchronous completion before enqueue returns. Coordinator/orchestrator/control integration passes 28 / 999 and covers `Queued` before Pool addition, sealed pre-enqueue residency, Start/durable-terminal reduction, stale revision rejection, exact cancellation, reentrant cancellation rejection, and a failed handoff without Pool side effects.
- Pool: 17 / 219, including preallocated terminal transition, `ReleaseWithoutCompletion`, next-start/removal semantics and fail-closed unknown finalizer decisions.
- Provider result mapper: 4 / 237; transaction-backed execution product: 9 / 188; reviewed factory: 8 / 225; Job lifecycle: 10 / 608.
- Production CopyAs policy and app-boundary seam: 10 / 98; the 4 / 70 app-boundary subset proves exact review projection, zero enqueue on blocked/stale/unpersisted/cancelled paths and owning durable failure dispatch before `ReleaseWithoutCompletion` removes the operation.
- Historical isolated Debug `OperationsUT` snapshot: 192 / 193 cases and 5,209 / 5,213 assertions; the only failure remains the host-specific NativeCreateCopy set-ID metadata baseline. Current full Debug `OperationsUT` evidence is 197 / 5,295. The current coordinator/control subset passes Release ASAN and UBSAN at 28 / 999 without diagnostics.
- Current M0: 897 / 132,011 in the recorded seeded run.
- Docker-backed Debug ASAN integration: 163 / 89,392.
