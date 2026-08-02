# Copy operation orchestrator foundation

## Status

`CopyOperationOrchestrator` is the implemented and unit-tested production composition boundary for one-item reviewed Copy lifecycle orchestration. Its public constructor uses the private reviewed-factory execution-product authority; the injected factory constructor is test-only. No application mutation entry point calls it yet.

This foundation implements the lifecycle ordering required by the canonical specification: intent is durably admitted before execution construction, execution enters `Pool` only after the journal reaches Running, and terminal evidence is durably finalized before the operation is released from the pool lifecycle.

## Implemented contract

For a structurally valid single-item Copy plan, the orchestrator:

1. durably admits the exact serialized plan to `OperationJournal`;
2. requests a transaction-owning execution product through the private `ReviewedOperationFactory` path;
3. converts cancellation or construction rejection before Running into a durable terminal journal record;
4. transitions the admitted entry to Running before enqueue;
5. enqueues the operation with a terminal finalizer that reads the typed item result;
6. persists the item result and terminal state before allowing `Pool` to release the operation;
7. retains operations whose result is unavailable, inconsistent, or cannot be persisted, allowing finalization to be retried;
8. transfers the same preallocated exact run-receipt slot from pre-enqueue custody to the Pool finalizer only after accepted enqueue.

Queue shutdown and enqueue rejection are also represented as durable terminal outcomes. Contract violations and persistence uncertainty fail closed; they do not manufacture publication authority or silently discard unfinished state.

## Run-receipt custody and recovery

The orchestrator reserves a bounded custodian slot for the exact immutable plan, originating journal storage identity and terminal accessor before the Running transition. The returned run receipt is armed into that existing slot without allocation. Cancellation, enqueue exceptions and enqueue rejection record immutable terminal evidence in the slot before finalization.

A pre-rename persistence fault returns `RetryRequired`; `Retry(plan_id)` can invoke only `OperationJournal::Finalize` with the retained exact receipt and cached evidence. Post-rename uncertainty returns `ReconcileRequired`, releases the poisoned journal and live receipt, and requires an independently reopened journal whose parent directory has the exact original device/inode identity. `Reconcile` is read-only and confirms either the exact terminal snapshot or startup-produced `Interrupted`; it never writes a terminal state or re-enqueues work.

Accepted operations keep the slot in `PoolOwned`, where external retry/reconcile calls return `Busy`. A post-rename finalization fault moves that same slot to `ReconcileRequired` while the operation remains in `Pool::Finalizing`. `Reconcile` reports `pool_release_required` when the exact operation is still retained. The separate `ReleaseReconciled(plan_id)` handshake invokes `Pool::RetryFinalization` for that exact operation and removes custody only after the Pool callback latches the matching release. Concurrent release attempts report `InProgress` or remain retained; an expired Pool can release only the already reconciled slot. Terminal evidence, once acquired, is never resampled after a journal rejection.

## Authority boundaries

- Structural validation does not approve execution.
- Accepted preflight and explicit review do not admit work to the queue.
- A journal admission receipt authorizes only state transitions in its originating journal for its exact plan.
- `Pool` admission authorizes scheduling, not provider mutation or publication.
- The private reviewed-factory execution-product path is the production construction authority; injected factories are test seams only.
- Terminal release requires a typed execution result and a successful durable journal finalization.

Publication evidence is tri-state: `NotPublished`, `Published`, or `Unknown`. `Unknown` is preserved for failures where destination inspection is required; current isolated Native execution reports only exact `NotPublished` or `Published` states.

## Production boundary

The Native provider owns a bounded same-host internal-APFS transaction with private-sealed reviewed authority, exclusive clone publication, exact metadata parity, destination verification and ordered durability. The provider mapper, transaction-backed operation/result state, private reviewed-factory construction, and public production orchestrator constructor are joined and tested. No application mutation entry point calls this orchestrator; established production mutation paths remain unchanged.

Application adoption requires an app-owned typed review step, a cold pre-enqueue configurator for callbacks, and an exact durable terminal presenter. The presenter must distinguish journal success, failure, cancellation, publication uncertainty, retry, reconcile, and Pool release; generic `Operation::Completed` is insufficient because non-cancellation provider failures also complete the operation. The narrow first candidate is one create-only regular Native item through `CopyAs::Perform`. Physical-volume fixtures and cross-volume bounded staging remain later gates.

## Verification snapshot

Current recorded evidence for the tree containing this foundation:

- focused Debug journal: 27 / 592; focused Debug orchestrator: 13 / 558, including production construction at 3 / 138;
- provider result mapper: 4 / 237; execution product: 9 / 188; reviewed factory: 8 / 225; Job lifecycle: 10 / 608;
- current full Debug, Release ASAN, and Release UBSAN `OperationsUT`: 165 cases / 4,468 assertions in each configuration;
- current full Debug `VFSUT`: 94 / 43,531;
- current-tree M0 gate: 897 / 132,011 across ten aggregate unit-test binaries;
- seeded ASAN integration total: 163 / 89,392.

These results prove the implemented composition and lifecycle contracts. They do not qualify the new Copy chain as a production mutation consumer.

## Next development slice

Add the typed application review, cold pre-enqueue callback configurator, and exact durable terminal presenter, then wire the bounded `CopyAs::Perform` candidate. The app must keep the journal/custodian alive across recovery and use `Reconcile` plus `ReleaseReconciled` for post-rename uncertainty.

## Related documents

- [`Docs/win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md)
- [`Docs/Development-Plan.md`](../Development-Plan.md)
- [`reviewed_copy_factory_foundation.md`](reviewed_copy_factory_foundation.md)
- [`native_create_copy_execution_foundation.md`](native_create_copy_execution_foundation.md)
- [`provider_conditional_copy_execution_product.md`](provider_conditional_copy_execution_product.md)
- [`operation_journal_foundation.md`](operation_journal_foundation.md)
