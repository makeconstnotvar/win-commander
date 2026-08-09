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

## One operation over a batch (Q2-8 slice 2, step C)

The operation now owns a `std::vector<ProviderConditionalCopyOperationItem>` — a transaction, its journal context and its presentation per item — and runs them in order. One item is a batch of one and goes through the same construction, so every case above still describes it: a second class would have forked the gate, the cancel-checker sanitiser and the construction pipeline, which are precisely the parts that are hard to get right. A batch stays **one** operation with one journal entry and one terminal state; N single-item operations would each carry their own, and the Operation Center would show N of them where the user asked for one.

### The journal decides what "derived from the whole set" can mean

`OperationJournalValidEntryLifecycle` cannot express a `Failed` item and a `Cancelled` item in one entry. So aggregation is not a fold over statuses; it also decides which results exist:

- **The run stops at the first item that does not succeed.** Continuing would let a later cancellation meet an earlier failure and produce a set with no legal state at all — and every item's evidence was checked before execution began, so an item that has just proved the world moved is a reason to stop spending the rest.
- **A cancellation cancels the untouched tail** through a forced commit with an always-true checker, which is what produces a `Cancelled` result. **A failure aborts it instead**, and an aborted transaction has no journal result: a `Failed` entry may legally say nothing about the items behind the failure, which is what "never attempted" should look like.
- **A failure can still appear during the wind-down**, when a forced commit's abort cannot confirm `NotPublished`. The state is then `Failed` and the `Cancelled` results are dropped — a cancelled item is always `NotPublished`, so leaving it out withholds nothing about what is on disk, while dropping the failure would hide a destination that may exist behind an outcome saying none does.
- **A completed batch must account for every item.** An entry reporting a failure or a cancellation may legally omit the items behind it; a completed one may not. An item whose provider answers with a terminal that precedes execution — what a commit on an already-aborted transaction replays — has no journal result to give, so a run that neither failed nor was cancelled and is missing one is `Inconsistent`, which is how the single-item path has always answered the same event.
- **Evidence is `Pending` until every item is resolved**, and `Inconsistent` when no item produced a result at all. The Pool reads the accessor once and latches the first non-`Pending` answer, so a partial snapshot would be recorded durably and a permanently-`Pending` one would retain the operation forever. Zero results is the cold-abort case the application boundary already reads as its integration blocker.
- **The numbering is checked at construction.** Indices that do not strictly increase are refused before anything is copied, because a `Finalize` the journal rejects is not a visible error but a slot latched into contract violation.

### Verification

Ten new `OperationsUT` cases cover: a three-item batch running in order with one result per item and per-item bytes; a mid-batch failure leaving the untouched item unrecorded and never committed; a cancelled item cancelling the ones behind it; a wind-down failure outranking the cancellations it uncovered; construction refusing an empty batch, duplicate or decreasing indices, a missing transaction and an unshowable item; a cold stop cancelling every item and a dropped cold batch aborting every one of them with no run to report; a stop accepted mid-run cancelling exactly the unstarted items while the commit in flight keeps its outcome; a stop accepted just ahead of an item cancelling that item instead of committing it; an exception between items settling every item as cancelled rather than stranding the evidence; and a batch refusing to call itself completed while an item has no result to give. `[provider-conditional-copy]` passes **25/25, 726 assertions** in Debug on four seeds and the same under **Release ASAN** and **Release UBSAN**; full `OperationsUT` **255/255, 6,312 assertions**; `OperationsIT` **98 passed, 2 skipped, 973/973**; full `WinCommanderUT` **890/890, 12,150** unchanged.

### Stopping, pausing and progress

A stop arriving while the worker owns the sequence is **accepted exactly while items remain unstarted** — the worker cancels those when it observes it — and refused once the last item is committing, which is the single-item rule unchanged. Acceptance and observation are **one decision under one lock**: the stop is accepted on the strength of "this item has not started", so if the worker read that answer somewhere other than where the start is published, a stop could be accepted for an item committed a moment later and the caller would be told it had been cancelled. That leaves the fixed rule testable but the race itself unreachable from outside — after the fix there is no window to drive.

**Whoever claims the sequence owes every item a terminal.** Nothing else can settle it afterwards: a stop finds the sequence already claimed, and the destructor only acts on an untouched job. So an exception thrown between items — the pause wait or the current-path publication are the only ones that can — winds the batch down rather than escaping, and winds it down as cancelled: the items that committed stay committed, and a run that stopped short of the rest is not a completed one. Left unhandled it would strand unresolved slots, and an unresolved slot is evidence that never arrives — the Pool holds the operation forever waiting for it, with the journal entry unfinalized. The stop path never terminates transactions the worker owns: doing so would run provider commits on the caller's thread under `Job`'s state mutex and could enter a transaction the worker is already inside. A stop that arrives before the worker exists still resolves every transaction inline on the caller's thread, as it did for one item; for a batch that is N aborts, each of them free because nothing has been committed yet. `BlockIfPaused` now runs between items, the only point where a pause can mean anything, since a commit cannot be interrupted — with the consequence every other multi-item job here already carries: a paused job blocks its worker, so a paused operation destroyed without being stopped or resumed would wait in its destructor. Statistics estimate both timelines and keep Items preferred: a conditional copy publishes atomically, so a byte fraction would sit still and then jump, while the byte total still says how much the batch weighs. Each item publishes its own current-item path, which is what a progress line follows.

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
