# Feature: Local Explorer browse-and-copy vertical slice

> Status: active target definition; the bounded M3 Native execution chain is implemented, while production Paste remains blocked by application review/presentation contracts and broader Copy scope
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 7, 10, 11, 13.1, 14-18, 35, 39-42
> Execution tracker: `Docs/Development-Plan.md`

## User problem

A local file workflow currently crosses several established controllers and views without a single observable state, command definition, preflight contract, or result model. The first architecture slice must prove that the target contracts can wrap the mature panel, VFS, and Operations engines while keeping the application usable throughout migration.

## Product behavior

The user opens Explorer mode, navigates to a local folder, selects one or more items, copies them, navigates to another local folder, and pastes. The same command definitions serve the command bar, menu, context menu, and keyboard. Before execution, the application creates an `OperationPlan`; during execution, Operation Center exposes progress; after execution, both folder state and the operation log show the final result.

The first slice is restricted to local `NativeHost` source and destination folders and a copy operation. It establishes reusable seams for move, trash, rename, archives, and remote providers in later slices.

## User scenarios

### Happy path

1. Open Explorer mode.
2. Navigate through sidebar, breadcrumb, history, or editable path.
3. Select one or more local files.
4. Invoke `file.copy` from any registered UI entry point.
5. Navigate to a writable local destination.
6. Invoke `file.paste`.
7. Review the generated plan and start the operation.
8. Observe queued/running/completed state in Operation Center.
9. Confirm that the destination listing refreshes and the operation remains available in history.

### Recoverable failure

1. Select a source that becomes unavailable or a destination without write access.
2. Invoke paste.
3. Receive a typed error with affected items, reason, and applicable recovery action.
4. Grant access, choose another destination, retry, or cancel.

### Conflict

1. Paste an item whose name already exists at the destination.
2. Review source and destination metadata in the conflict resolver.
3. Choose Replace, Skip, or Keep Both with an explicit scope.
4. See the decision and per-item result in the operation log.

### Cancellation

1. Start a copy large enough to expose progress.
2. Cancel it from Operation Center.
3. See the final cancelled or completed-with-warnings state and any partial results.

## UI states

### Pane

- loading: previous safe content remains visible with navigation progress;
- ready: current path, items, selection, view, sort, group, and filter are coherent;
- empty: folder is accessible and contains no visible items;
- permission required: requested location and recovery action are explicit;
- error: current location and last usable state remain understandable;
- refreshing: listing refresh is distinct from first load.

### Selection and commands

- no selection;
- focused item;
- single selection;
- multiple selection;
- cut-marked selection;
- disabled command with a human-readable reason;
- provider-limited command with an alternative when one exists.

### Operation

- planned;
- validating/preparing;
- queued;
- running with bytes, files, speed, and ETA when available;
- waiting for conflict or permission;
- cancelling/cancelled;
- failed or completed with warnings;
- completed.

## Commands

The slice introduces canonical definitions for:

| Command | Availability | Handler bridge |
|---|---|---|
| `file.copy` | local selection contains copyable items | existing copy-to-pasteboard action |
| `file.cut` | local selection contains movable items | existing cut-token action |
| `file.paste` | clipboard has supported items and destination can accept them | reviewed plan → durable journal admission → production orchestrator/private factory → `Pool` |
| `operationCenter.open` | obtains a current value snapshot through the weak coordinator and opens a bounded static copied panel | Registry passes the copied records to a borrowed synchronous Explorer presenter; the panel shows active plus terminal/interrupted type/state, `OperationId`/`PlanId` and timestamps, and its Cancel re-enters the Registry |
| `operation.cancel` | an active snapshot record supplies immutable `{OperationId, expected revision, can-cancel}` context | `OperationCenterCoordinator` revalidates the exact value context through its sealed control port |
| `operation.retry` | final error is retryable | recreate execution from persisted plan/result |
| navigation commands | derived from pane history/location state | existing panel action dispatcher |

Every entry point resolves `CommandState` from the same `CommandContext`. Existing selectors remain adapter details during migration.

`Operation::Stop()` and `Job::Stop()` remain engine lifecycle primitives. The production `operation.cancel` adapter accepts only published `{OperationId, expected revision, can-cancel}` and calls the control port; it never receives the corresponding Pool/operation reference. `operationCenter.open` obtains a weak-coordinator snapshot and its panel holds only copied value records. `More` remains an active-only compact menu; panel reopen is the only refresh. A live observer, progress, results/log, pause/resume and retry remain future presentation work.

## Data model

### PaneStore slice

```text
PaneStore
- paneId
- location(providerId, path)
- navigationHistory
- loadingState
- listingSnapshot
- selection
- focusedItem
- viewConfiguration
- visibleError
```

`PanelController` and `PanelData` remain the current engine. Context notifications are sufficient for committed read projection only. Before `loadingState` and `visibleError` are projected, `PanelController` exposes stable request identity and ordered main-queue started/terminal outcomes; `PaneStore` reduces those events instead of inferring lifecycle from queue occupancy or view notifications. Mutations enter through commands; UI code does not write store fields directly.

### Operation slice

The structural `OperationPlan` owns stable source identities, destination semantics, requested conflict policy, and intrinsic effects. Bound preflight composes estimates, conflicts, permission/space evidence, `NativeHost` capabilities, warnings, blockers, and confirmation requirements into the review model. After explicit review, `CopyOperationOrchestrator` durably admits the plan and privately consumes `ReviewedOperationFactory` into the bounded transaction-owning execution product. The public factory compatibility surface remains fail closed so direct callers cannot bypass journal and queue custody.

## Provider capabilities

`NativeHost` exposes the initial capability snapshot:

- list/read metadata;
- create folder/file;
- copy source;
- accept copy destination;
- rename/move/trash where supported by the mounted volume;
- read/write permission state;
- available-space query;
- file watching and refresh support.

Capability resolution includes the concrete location and volume. Command availability is derived from the snapshot and current selection, rather than host type checks inside views.

## Operation lifecycle

```text
file.paste command
-> capture source and destination context
-> create OperationPlan
-> bind providers and preflight paths, capabilities, permissions, conflicts, and available space
-> present and explicitly review the composed plan/decisions
-> durably admit the exact plan to OperationJournal
-> submit reviewed authority through the production CopyOperationOrchestrator
-> privately construct the Native conditional transaction and typed execution product
-> enqueue the admitted operation in Pool
-> translate progress into operation state
-> persist mapped item results and terminal state before Pool removal
-> reconcile post-rename journal uncertainty and release the exact reconciled Pool operation
-> refresh affected PaneStore instances
-> announce completion and recovery actions
```

The engine adapter owns composition of `OperationJournal`, `CopyOperationRunReceiptCustodian`, the production `CopyOperationOrchestrator`, `Operation`, `Job`, and `Pool`. The provider result mapper, conditional transaction, typed execution product, private reviewed factory, restricted cold hooks, owning exact durable outcome, preallocated Pool terminal transition, `ReleaseWithoutCompletion`, journal/Pool orchestration, read-only reconciliation, and exact reconciled Pool release are implemented for one create-only regular Native file on the same internal writable APFS volume.

Bounded `CopyAs::Perform` now supplies an app-owned exact review step, lifecycle/item-status hooks, owning durable-terminal UI dispatch and process-owned retry/reconciliation. It uses journal publication, sync, recovery, reconcile and Pool-release evidence because generic `Operation::Completed` also represents non-cancellation provider failures. Clipboard Paste remains later because it adds clipboard freshness, batches, destination changes, cut-token semantics and broader provider/volume scope. Views consume state and commands only.

## Error states

The slice must produce typed errors for:

- permission denied at source or destination;
- missing source or destination;
- read-only destination;
- insufficient space;
- name conflict requiring a decision;
- source or volume disappearing during execution;
- file changing during execution;
- user cancellation;
- partial completion;
- unexpected engine/provider failure.

Each error carries a user message, technical context, affected items, operation id, provider id, severity, retryability, and suggested actions. Credentials and file contents are excluded from diagnostics.

## Edge cases

- Unicode and decomposed filenames;
- case-sensitive and case-insensitive destinations;
- symlink, alias, package, hidden file, and app bundle;
- duplicate names inside one multi-item operation;
- long paths and deep trees;
- thousands of small files and one large file;
- destination nested inside a selected source folder;
- clipboard replaced after Copy or Cut;
- application or volume interruption during execution;
- destination listing changing between plan and execution.

## Accessibility

- All command entry points expose the same localized title and disabled reason.
- Pane loading, permission, error, selection, and operation transitions are announced.
- Operation Center is fully keyboard navigable and reports progress without color-only meaning.
- Conflict choices expose source/destination metadata and scope to assistive technologies.
- Focus returns to a deterministic pane/item after dialogs and operation completion.

## Acceptance criteria

- The happy path runs through `CommandRegistry`, `PaneStore`, `ProviderCapabilities`, bound preflight, explicit review, durable journal admission, the production orchestrator/private factory, `Pool`, typed durable result, and visual state mapping.
- Toolbar, menu, context menu, and keyboard expose identical availability and execution for `file.copy` and `file.paste`.
- UI remains responsive while loading 10,000/100,000-item folders and during copy.
- Permission, conflict, insufficient-space, cancellation, and partial-failure states are visible and recoverable where applicable.
- Destination refresh and persisted operation history reflect the final item-level result.
- Existing Commander mode and direct engine tests remain green throughout adapter migration.

## Tests

### Unit

- command availability and disabled reasons;
- `PanelController` lifecycle producer acceptance, commit ordering and exactly-one terminal outcomes;
- PaneStore snapshot/reducer transitions;
- NativeHost capability mapping;
- OperationPlan validation and immutable source/destination capture;
- error mapping and recovery actions;
- visual-state priority composition.
- `operationCenter.open` snapshot-copy isolation, static-panel content and Registry-gated cancellation; focused evidence is core 17 / 183, compact `More` 4 / 70 and static panel 3 / 104.

### Integration

- command entry point -> bound plan/review -> durable admission -> production orchestrator/private factory -> `Pool` -> durable final result;
- copy into writable destination;
- conflict policies and apply-to-scope;
- permission denial/recovery;
- cancel and partial completion;
- source/destination volume removal;
- restart/history recovery when persistence lands.

### Performance

- open and interact with generated 10,000- and 100,000-item local folders;
- measure time to first visible rows, main-thread stalls, peak memory, and full metadata completion;
- copy many small files and one large file while sampling UI responsiveness.

### Manual

- mouse and keyboard paths across toolbar, menu, context menu, and shortcuts;
- VoiceOver walkthrough for navigation, selection, preflight, progress, conflict, error, and completion;
- light/dark visual evidence for loading, empty, permission, error, running, and completed states.
