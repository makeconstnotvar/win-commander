# Feature: Copy operation submission hooks

> Status: restricted cold-operation hooks, exact durable terminal delivery and bounded `CopyAs` presenter/consumer implemented
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 14, 15, 31, and 32
> Execution tracker: M3 in `Docs/Development-Plan.md`

## Purpose

`CopyOperationSubmissionHooks` is the narrow application-facing observation boundary of `CopyOperationOrchestrator::Submit`. It configures transport and progress observation while the produced operation is still cold and provides the durable terminal evidence required for user-visible completion. It does not grant planning, review, provider mutation, journal or queue authority.

The coordinator-only receipt path separately has an opaque private pre-enqueue handoff. After the exact run receipt is armed and before custody opens `Pool` enqueue, it registers sealed live residency and holds the admission lease through `TryEnqueue`. It is not part of `CopyOperationSubmissionHooks`, does not expose a mutable `Pool` or `Operation`, and a failing handoff durably records `Failed` with zero Pool admission.

## Cold-operation contract

The lifecycle observation list accepts masks composed only from:

- `Operation::NotifyAboutStart`;
- `Operation::NotifyAboutPause`;
- `Operation::NotifyAboutResume`;
- `Operation::NotifyAboutStop`;
- `Operation::NotifyAboutTitleChange`.

Every mask must be non-zero and every callback must be present. `NotifyAboutCompletion` and the combined `NotifyAboutFinish` mask are rejected because a non-cancellation provider failure may still complete the transport operation. Durable journal evidence, rather than generic operation completion, defines the user-visible terminal result.

The optional item-status callback is installed through the operation's existing item-status channel and runs on its background Job thread. Lifecycle observations and the item-status callback are installed before the journal enters Running and before Pool admission can start the operation. The application must marshal UI work to its own executor.

All lifecycle entries are validated before installation begins. Invalid configuration or an installation failure releases the cold product, attempts to durably finalize the admitted journal entry as `Failed`, and never reaches Running or `Pool`. Successful failure finalization returns `OperationConfigurationFailed`; a journal finalization failure retains its own typed error. The durable terminal observer has not entered run-receipt custody on this path, so the Submit error is the synchronous result.

## Exact durable outcome

The terminal observer receives an owning `CopyOperationDurableTerminalOutcome` containing:

- the exact plan ID;
- the durable journal terminal state;
- the exact item result when one exists;
- `Finalized`, `ReconciledTerminal`, or `ReconciledInterrupted` confirmation.

The value owns its plan ID and item evidence, so a consumer can copy or move it into another executor without retaining orchestrator or custodian storage. Delivery is synchronous on the caller that establishes the terminal fact: the Submit caller for a post-Running pre-enqueue terminal path, a Pool finalizer thread, an explicit retry caller, or a reconciliation/release caller. UI presentation therefore dispatches the owning outcome explicitly.

Delivery occurs only after exact durable finalization or read-only reopen reconciliation and before Pool removal or generic success reporting. Observer exceptions are contained. The custodian consumes the observer under the slot lock before invocation, providing at-most-once delivery across persistence retry, repeated reconciliation, concurrent Pool retry and `ReleaseReconciled` races.

## Pool terminal release

Accepted enqueue preallocates a `FinalizingOperation` owner and sufficient finalizing-container capacity. Operation finish transfers that existing owner into `Pool::Finalizing`; the terminal callback does not construct new finalization authority.

The finalizer maps durable state to two release forms:

- durable `Completed` uses `Release`, removes the operation and permits the generic completion callback;
- failure, cancellation and reconciled `Interrupted` use `ReleaseWithoutCompletion`, remove the operation and start eligible pending work while suppressing the generic completion callback.

If terminal evidence is pending, inconsistent or not durably persisted, the finalizer returns `Retain`. A retry reuses the exact cached receipt and item evidence. Post-rename uncertainty requires an independently reopened journal; `Reconcile` confirms the exact terminal snapshot or startup-produced `Interrupted`, and Pool-owned custody is released separately through `ReleaseReconciled`.

For Pool-owned reconciliation, terminal delivery is deferred until the matching Pool finalizer accepts release. A concurrent release reports `InProgress` while preserving custody. If the Pool has expired, the already reconciled slot can deliver the same owning outcome and release custody without manufacturing a Pool completion event.

## Application integration boundary

`ReviewedCopyAsApplicationBoundary` now owns the app-facing reviewed preflight until a terminal review decision. It exposes an immutable exact-plan presentation, keeps blocked, stale, unpersisted and cancelled paths out of its sole submission port, acquires the window ticket before minting reviewed authority, and installs the item-status and durable-outcome hooks only for the accepted product. Its durable observer copies the owning outcome into a UI task before the finalizer can release Pool residency.

The production `CopyAs::Perform` supplies that seam with process-owned recovery, the window submission gate, item-status deselection and UI presentation. Four focused app-boundary cases / 70 assertions cover the zero-enqueue paths, exact review projection and real `Pool::ReleaseWithoutCompletion` ordering; the same filter passes Release ASAN and UBSAN.

The journal and `CopyOperationRunReceiptCustodian` need process-lifetime ownership across recovery. The coordinator can remain window-scoped around the existing `Pool`. Once the reviewed lifecycle is selected, later failure remains within its typed durable recovery path and does not fall back to a second mutation route.

## Verification snapshot

Current Debug evidence for this snapshot:

- `CopyOperationOrchestrator_UT`: 15 cases / 758 assertions;
- `Pool_UT`: 17 / 219;
- production orchestrator subset: 3 / 138;
- reviewed CopyAs policy and app boundary: 10 / 98, including the app-boundary subset at 4 / 70.

The historical full Debug `OperationsUT` run reproduced one existing host-specific set-ID metadata failure (169 / 170 cases and 4,744 / 4,748 assertions pass). Current full Debug `OperationsUT` evidence is 197 / 5,295. Aggregate ASAN/UBSAN reruns stop at the independent AppKit pasteboard baseline; the changed app-boundary filter passes under both sanitizers without diagnostics.

## Related documents

- [`copy_operation_orchestrator_foundation.md`](copy_operation_orchestrator_foundation.md)
- [`provider_conditional_copy_execution_product.md`](provider_conditional_copy_execution_product.md)
- [`operation_journal_foundation.md`](operation_journal_foundation.md)
- [`ADR 0001`](../ADR/0001-native-conditional-copy-publication.md)
