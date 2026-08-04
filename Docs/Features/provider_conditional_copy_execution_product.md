# Feature: provider conditional Copy execution product

> Status: provider result mapper, transaction-owning operation product, private reviewed-factory construction, production orchestrator composition, restricted submission hooks and bounded `CopyAs` consumer implemented
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 14, 15, 31, and 32
> Execution tracker: M3 in `Docs/Development-Plan.md`

## Purpose

`CopyOperationExecutionProduct` is the narrow ownership join between a provider-minted conditional Copy transaction and the durable `OperationJournal`/`Pool` lifecycle. It moves one cold `Operation` together with one terminal item-result accessor. Construction is private to the provider operation factory, reviewed factory, orchestrator, and their test seams, so application code cannot pair an arbitrary operation with unrelated terminal evidence.

## Lossless provider-result mapping

`MapProviderConditionalCopyCommitResultToJournalItemResult` validates the complete provider terminal tuple before producing journal evidence. The implemented mapping preserves:

- exact item index and source bytes, with bytes recorded only after confirmed publication;
- primary failure and errno;
- `NotPublished`, `Published`, or `Unknown` publication state;
- filesystem-sync status and errno independently from the primary failure;
- recovery guidance derived from the exact publication state.

All twelve executable provider result variants are represented. `Aborted` is a valid provider terminal state before execution and maps to `NonExecutionTerminal`; contradictory enum, errno, publication, or sync combinations map to `InconsistentResult`. The mapper grants no execution or publication authority.

## Transaction-owning operation

`ProviderConditionalCopyOperationFactory` consumes the sole transaction owner and creates a move-only execution product. The internal `Job` serializes worker commit, stop, destruction, and terminal publication through one gate:

- normal execution owns `Commit` exactly once;
- a stop that wins before commit resolves the provider transaction through `Commit` with cancellation asserted;
- dropping a cold product resolves the transaction through `Abort` exactly once;
- a throwing cancellation checker is treated as cancellation;
- the terminal accessor returns `Pending` before provider resolution, the exact mapped result after resolution, or `Inconsistent` after an invalid terminal tuple;
- the operation destructor waits for worker completion, so detached execution cannot outlive its owner.

Worker-launch failure is converted into the same stopped/cancelled terminal path before finish callbacks run. Only an exact mapped cancellation ends the operation as `Stopped`; success and all non-cancellation provider failures end as `Completed`. Consumers must therefore use durable typed terminal evidence for success, failure, publication uncertainty, and recovery presentation rather than treating the generic `Operation` state as the result.

## Factory and orchestrator integration

The private `ReviewedOperationFactory::CreateExecutionProduct` path validates the reviewed plan, consumes its private-sealed authority, begins the exact Native transaction, and creates this product. The production `CopyOperationOrchestrator` constructor uses that friend path; injected execution factories remain test-only. The public compatibility `ReviewedOperationFactory::Create` still drops the product, resolves the cold transaction, and fails closed, so callers cannot bypass journal admission and queue custody.

The orchestrator admits the plan before construction, configures the valid submission hooks while the operation is cold, reserves the exact run-receipt slot before Running, arms it before enqueue, and transfers the same slot to the `Pool` finalizer only after accepted enqueue. Provider evidence is sampled once and durably finalized before `Pool` release.

The hook contract exposes Start, Pause, Resume, Stop and TitleChange lifecycle observations plus the operation's item-status callback. Generic Completion and Finish observations are rejected. Terminal presentation instead receives an owning exact durable outcome after journal finalization or exact reopen reconciliation. Delivery is synchronous on the active Submit, Pool-finalizer, retry or recovery caller, so UI consumers dispatch the owning value to their executor.

Accepted Pool admission preallocates the terminal-finalization wrapper and transfer capacity. Successful durable completion follows the normal Pool completion route. Failed, cancelled and reconciled `Interrupted` outcomes use `ReleaseWithoutCompletion`, which removes terminal work and starts eligible pending work without publishing a generic success callback. Slot locking and observer consumption make durable delivery at-most-once across retry, reconcile and concurrent release.

## Application integration boundary

`CopyAs::Perform` in `States/FilePanels/Actions/CopyFile.mm` submits this product for one regular Native item copied create-only within the same source directory when path eligibility is explicitly `SameVolumeClone`. That route matches the implemented single-item, same-`NativeHost`, internal writable APFS scope.

The application boundary still requires three explicit contracts:

1. an app-owned typed review step that produces the exact `ReviewedVFSOperationPreflight` accepted by the orchestrator;
2. a presenter/coordinator that maps application lifecycle and item-status handling into the implemented restricted hooks, dispatches owning durable outcomes to the UI executor, and drives post-rename `Reconcile`/`ReleaseReconciled` recovery;
3. one bounded `CopyAs::Perform` consumer that enters this lifecycle only after exact user review succeeds.

Process-lifetime composition should own the active journal and `CopyOperationRunReceiptCustodian`; a window-scoped coordinator should own `Pool`, user cancellation, and presentation. A submission may construct a short-lived orchestrator from those owners. Legacy fallback is valid only before the new reviewed lifecycle is selected; no failure after preflight, admission, construction, Running, or enqueue may re-enter the legacy mutation path.

## Verified coverage

- Provider result mapper: 4 Debug cases / 237 assertions.
- Execution product and provider operation: 9 / 188.
- Reviewed factory: 8 / 225.
- Job lifecycle and worker-launch hardening: 10 / 608.
- Copy orchestrator: 17 / 806, including the production factory path at 3 / 138 and receipt-aware no-re-admission.
- Pool: 17 / 219.
- Journal: 27 / 592.
- Historical foundation Debug `OperationsUT`: 170 cases / 4,748 assertions; the current model/coordinator batch is tracked separately in `Development-Plan.md`.

At that foundation snapshot, explicitly instrumented Release ASAN and UBSAN `OperationsUT` each passed 170 / 4,748 with confirmed runtime linkage and no diagnostics; the current coordinator/control subset separately passes Release ASAN and UBSAN at 28 / 999 without diagnostics.

Earlier Native staged-capsule, Pool, VFS, M0, and seeded integration snapshots remain recorded in their owning feature and plan documents.

## Remaining work

The opt-in `OperationsIT` physical-volume fixture is implemented, but the required internal/external physical run and hardware power-loss evidence are not yet recorded; see `Docs/Features/reviewed_copy_as_physical_volume_protocol.md`. Cross-volume Copy still requires provider-owned bounded staging; batches, replacement, directories, symlinks, remote providers and clipboard Paste remain separate gates.

## Related documents

- [`reviewed_copy_factory_foundation.md`](reviewed_copy_factory_foundation.md)
- [`copy_operation_orchestrator_foundation.md`](copy_operation_orchestrator_foundation.md)
- [`copy_operation_submission_hooks.md`](copy_operation_submission_hooks.md)
- [`operation_journal_foundation.md`](operation_journal_foundation.md)
- [`native_create_copy_execution_foundation.md`](native_create_copy_execution_foundation.md)
- [`ADR 0001`](../ADR/0001-native-conditional-copy-publication.md)
