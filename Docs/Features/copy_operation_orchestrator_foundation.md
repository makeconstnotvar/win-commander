# Copy operation orchestrator foundation

## Status

`CopyOperationOrchestrator` is the implemented and unit-tested production composition boundary for one-item reviewed Copy lifecycle orchestration. Its public constructor uses the private reviewed-factory execution-product authority; the injected factory constructor is test-only. Restricted cold-operation submission hooks and exact durable terminal delivery are implemented. Bounded `CopyAs::Perform` is its first application caller.

This foundation implements the lifecycle ordering required by the canonical specification: intent is durably admitted before execution construction, execution enters `Pool` only after the journal reaches Running, and terminal evidence is durably finalized before the operation is released from the pool lifecycle.

## Implemented contract

For a structurally valid single-item Copy plan, public compatibility `Submit` durably admits the plan itself; the private coordinator-only `SubmitAdmitted` instead accepts an exact existing receipt, validates its Journal instance and full reviewed plan, and never admits twice. The common continuation then:

1. requests a transaction-owning execution product through the private `ReviewedOperationFactory` path;
2. converts cancellation or construction rejection before Running into a durable terminal journal record;
3. transitions the admitted entry to Running before enqueue;
4. enqueues the operation with a terminal finalizer that reads the typed item result;
5. persists the item result and terminal state before allowing `Pool` to release the operation;
6. retains operations whose result is unavailable, inconsistent, or cannot be persisted, allowing finalization to be retried;
7. transfers the same preallocated exact run-receipt slot from pre-enqueue custody to the Pool finalizer only after accepted enqueue;
8. installs validated lifecycle observations and the item-status callback while the product is cold, before Running and Pool admission;
9. invokes a coordinator-only pre-enqueue handoff after run-receipt arm; its opaque admission lease survives `BeginEnqueue` and `Pool::TryEnqueue`, and a throwing handoff durably fails without Pool admission;
10. repeats cancellation after that handoff and before custody opens enqueue;
11. delivers an owning exact durable terminal outcome at most once, before Pool removal and generic success reporting.

Queue shutdown and enqueue rejection are also represented as durable terminal outcomes. Contract violations and persistence uncertainty fail closed; they do not manufacture publication authority or silently discard unfinished state.

The public lifecycle hook surface accepts only Start, Pause, Resume, Stop and TitleChange observations. Generic Completion and the combined Finish mask are rejected because provider failure may still leave the transport operation in `Completed`. The separate item-status callback is installed on the cold operation and retains its existing worker-thread delivery contract. Public hooks never receive mutable `Pool` or `Operation` authority; the pre-enqueue handoff is private to the coordinator. Invalid hook configuration durably fails the admitted entry without reaching Running or `Pool`.

## Run-receipt custody and recovery

The orchestrator reserves a bounded custodian slot for the exact immutable plan, originating journal storage identity and terminal accessor before the Running transition. The returned run receipt is armed into that existing slot without allocation. Cancellation, enqueue exceptions and enqueue rejection record immutable terminal evidence in the slot before finalization.

A pre-rename persistence fault returns `RetryRequired`; `Retry(plan_id)` can invoke only `OperationJournal::Finalize` with the retained exact receipt and cached evidence. Post-rename uncertainty returns `ReconcileRequired`, releases the poisoned journal and live receipt, and requires an independently reopened journal whose parent directory has the exact original device/inode identity. `Reconcile` is read-only and confirms either the exact terminal snapshot or startup-produced `Interrupted`; it never writes a terminal state or re-enqueues work.

Accepted operations keep the slot in `PoolOwned`, where external retry/reconcile calls return `Busy`. A post-rename finalization fault moves that same slot to `ReconcileRequired` while the operation remains in `Pool::Finalizing`. `Reconcile` reports `pool_release_required` when the exact operation is still retained. The separate `ReleaseReconciled(plan_id)` handshake invokes `Pool::RetryFinalization` for that exact operation and removes custody only after the Pool callback latches the matching release. Concurrent release attempts report `InProgress` or remain retained; an expired Pool can release only the already reconciled slot. Terminal evidence, once acquired, is never resampled after a journal rejection.

The terminal observer receives an owning `CopyOperationDurableTerminalOutcome`: exact plan ID, journal terminal state, optional exact item result and the `Finalized`, `ReconciledTerminal` or `ReconciledInterrupted` confirmation. Delivery is synchronous on whichever Submit, Pool-finalizer, retry or recovery caller establishes the terminal fact. UI consumers copy or move the owning outcome to their executor. The slot serializes delivery and consumes the observer before invocation, so retry, reconciliation and concurrent release cannot deliver it twice; observer exceptions are contained.

`Pool` preallocates the accepted operation's terminal-finalization wrapper and the capacity required to transfer it into `Finalizing`. Durable `Completed` releases use the normal completion route. Failed, cancelled and reconciled `Interrupted` outcomes use `ReleaseWithoutCompletion`: the operation is removed and pending work can start, while the generic success callback remains suppressed.

## Authority boundaries

- Structural validation does not approve execution.
- Accepted preflight and explicit review do not admit work to the queue.
- A journal admission receipt authorizes only state transitions in its originating journal for its exact plan.
- `Pool` admission authorizes scheduling, not provider mutation or publication.
- The private reviewed-factory execution-product path is the production construction authority; injected factories are test seams only.
- Terminal release requires a typed execution result and a successful durable journal finalization.

Publication evidence is tri-state: `NotPublished`, `Published`, or `Unknown`. `Unknown` is preserved for failures where destination inspection is required; current isolated Native execution reports only exact `NotPublished` or `Published` states.

## Production boundary

The Native provider owns a bounded same-host internal-APFS transaction with private-sealed reviewed authority, exclusive clone publication, exact metadata parity, destination verification and ordered durability. The provider mapper, transaction-backed operation/result state, private reviewed-factory construction, public production orchestrator constructor and bounded `CopyAs` caller are joined and tested.

The application supplies an exact bound-plan review, process-owned recovery coordinator, process-owned `OperationCenterCoordinator`, window submission gate and UI dispatch of item status plus owning durable outcomes for one create-only regular Native item through `CopyAs::Perform`. The coordinator owns the exact receipt handoff, preallocates and registers exact live residency before Pool admission, observes `Queued → Running` and durable terminal reduction before forwarding application hooks, and preserves the full typed orchestrator error for UI recovery presentation. The app-boundary seam proves zero enqueue for blocked, stale, unpersisted and cancelled intent, exact review projection and durable dispatch before `ReleaseWithoutCompletion` removes the operation. An opt-in physical-volume fixture is implemented; its physical execution/power-loss evidence and cross-volume bounded staging remain later gates.

## Verification snapshot

Current recorded Debug evidence for the tree containing this foundation:

- focused Debug journal: 33 / 752; focused Debug coordinator/orchestrator/control integration: 28 / 999, including production construction at 3 / 138, receipt-aware no-re-admission, `Queued` before Pool addition, private pre-enqueue failure, Start/durable-terminal reduction, exact cancellation and reentrant cancellation rejection;
- focused Debug Pool: 17 / 219;
- provider result mapper: 4 / 237; execution product: 9 / 188; reviewed factory: 8 / 225; Job lifecycle: 10 / 608;
- full Debug `OperationsUT`: 192 / 193 passing cases and 5,209 / 5,213 passing assertions; the sole set-ID metadata failure is the existing host-specific NativeCreateCopy baseline.

Separate evidence is the latest full Debug `VFSUT` run at 95 / 43,566, recorded M0 at 897 / 132,011 across ten aggregate unit-test binaries, and seeded ASAN integration at 163 / 89,392.

At the previous foundation snapshot, full `OperationsUT` passed 170 / 4,748 in Debug and explicitly instrumented Release ASAN/UBSAN. The current coordinator/control subset passes Release ASAN and UBSAN at 28 / 999 with no diagnostics. App policy/gate and recovery coordination pass 6 / 28 and 6 / 67.

## Next development slice

Run the physical-volume protocol for the bounded `CopyAs::Perform` consumer. Cross-volume support requires a separate provider-owned staging authority.

## Related documents

- [`Docs/win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md)
- [`Docs/Development-Plan.md`](../Development-Plan.md)
- [`reviewed_copy_factory_foundation.md`](reviewed_copy_factory_foundation.md)
- [`native_create_copy_execution_foundation.md`](native_create_copy_execution_foundation.md)
- [`provider_conditional_copy_execution_product.md`](provider_conditional_copy_execution_product.md)
- [`copy_operation_submission_hooks.md`](copy_operation_submission_hooks.md)
- [`operation_journal_foundation.md`](operation_journal_foundation.md)
