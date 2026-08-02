# План поэтапного архитектурного рефакторинга

> Статус: active M1–M3 refactor plan
>
> Основание: [текущий архитектурный аудит](current_architecture_audit.md)
>
> Целевые contracts: [разделы 7, 9–11, 14 и 16 канонической спецификации](win_commander_ideal_file_manager_spec.md#7-архитектурная-модель-source-of-truth)
>
> Execution tracker: [`Development-Plan.md`](Development-Plan.md)

## 1. Цель и стратегия

Цель — перенести authoritative ownership в `CommandRegistry`, `PaneStore`, `ProviderCapabilities`, Visual State/Error и `OperationPlan`/Operation Center, сохранив зрелые engines `Panel`, `VFS` и `Operations`.

Стратегия — branch by abstraction:

```text
characterization
→ pure contract
→ adapter over current owner
→ one Explorer vertical slice
→ all entry points use the adapter
→ ownership moves behind the contract
→ legacy route becomes removable
```

Первый сквозной сценарий развивается по вертикальным срезам: открыть local folder → опубликовать navigation/refresh state → выбрать item → вычислить command availability → создать structural `OperationPlan` → выполнить bound preflight/review → durably admit → создать operation → показать progress/result → обновить pane state. Navigation/refresh and nine command routes already use production contracts. Operations now has the provider transaction, lossless result mapper, sealed execution product, private reviewed-factory path, exact journal authority, receipt retry/reconcile/release custody, restricted cold hooks, exact durable-outcome delivery, preallocated Pool terminal transition, production-configured Copy orchestrator and one bounded reviewed `CopyAs` consumer. Live boundary proof, cross-volume staging and physical-volume fixtures remain later gates.

## 2. Неподвижные архитектурные границы

1. `VFSHost` остаётся provider execution interface; capability layer описывает его возможности и контекст.
2. `Operation`/`Job`/`Pool` остаются execution engine; `OperationPlan` владеет structurally validated intent, а отдельный pure planner выполняет preflight и только затем разрешает factory создать concrete operation.
3. `Panel::data::Model` остаётся listing/sort/filter engine в миграционный период; `PanelController` сначала становится явным producer’ом request lifecycle, затем `PaneStore` проецирует observable state contract и intents.
4. Existing `PanelAction`/`StateAction` objects остаются handlers до перевода каждой команды; selector routing становится compatibility adapter.
5. `nc::Error` остаётся low-level error transport; `FileManagerError` добавляет product semantics и recovery metadata.
6. `Config` и существующие panel encoders остаются persistence mechanism до появления versioned snapshots.
7. Каждый этап завершает один vertical slice, focused tests и unsigned build до расширения scope.

## 3. Целевая зависимость

```text
UI entry point
  → CommandRegistry
      → CommandContext(PaneSnapshot, ProviderCapabilities, PermissionState, OperationState)
      → CommandState(enabled/visible/disabledReason)
      → domain intent
          ├─ PaneStore intent → PanelController/VFS adapter
          └─ OperationPlan → preflight/review → journal admission
                             → OperationFactory/capsule → Operations::Pool
                             → durable item/terminal outcome

PanelController lifecycle producer
  → request-scoped domain events
  → PaneStore
Other domain events
  → PaneStore / OperationCenterModel
  → VisualStateMapper + FileManagerErrorPresenter
  → UI render and feedback
```

Dependencies between new contracts:

```text
ProviderCapabilities ─┐
PaneSnapshot ─────────┼→ CommandRegistry
FileManagerError ─────┘        │
                               ├→ Pane intents
                               └→ OperationPlan → journal → Operation Center

PaneSnapshot + CommandState + OperationState + FileManagerError
                               → VisualStateMapper
```

## 4. Этапы

### R0 — Characterization and contract skeleton

**Purpose:** зафиксировать behavior seams до переноса ownership.

**Статус 2026-08-01:** partial — command types/registry, `PaneSnapshot`, pure `PaneLifecycleProducer`/`PanelControllerLifecycle`, controller-owned `PaneId`, `ProviderCapabilities`, `FileManagerError` и structural `OperationPlan`/`OperationPlanId` foundations добавлены с unit coverage; characterization legacy adapters остаются в работе.

**Changes:**

- добавить characterization tests для `PanelAction::Predicate/Perform` adapters, pane snapshot extraction и pool terminal events;
- определить value types без UI dependencies: `CommandId`, `DisabledReason`, `PaneSnapshot`, `ProviderCapabilities`, `FileManagerError`, `OperationPlanId`;
- закрепить stable ids для первого slice: `file.open`, `file.copy`, `file.cut`, `file.rename`, `view.toggleHiddenFiles`, `operation.cancel`, `operationCenter.open`;
- сохранить legacy selector/action tag в descriptor как migration metadata.

**Placement:**

- app-facing command/error contracts: `Source/WinCommander/WinCommander/Core/Commands/` и `Source/WinCommander/WinCommander/Core/Errors/`;
- provider capability contract: `Source/VFS/include/VFS/ProviderCapabilities.h`;
- pane value types и app-layer read adapter: `Source/WinCommander/WinCommander/Core/Pane/`;
- operation value types and pure preflight: `Source/Operations/include/Operations/{OperationPlan,OperationPlanner}.h`.

**Verification seam:** pure unit tests in `WinCommanderUT`, `VFSUT`, `PanelUT`, `OperationsUT`; existing action maps from [`PanelControllerActions.mm`](../Source/WinCommander/WinCommander/States/FilePanels/PanelControllerActions.mm) and [`StateActions.mm`](../Source/WinCommander/WinCommander/States/FilePanels/StateActions.mm) act as fixtures.

**Exit:** contracts compile without reverse dependencies from engine modules into app UI; current behavior remains byte-for-byte routed through legacy paths.

### R1 — ProviderCapabilities and FileManagerError baseline

**Purpose:** дать commands и preflight единый ответ о применимости и recovery.

**Статус 2026-08-02:** partial — conservative resolver, explicit provider declarations и lossless POSIX `FileManagerError` adapter добавлены с unit coverage. `VFSHost` имеет authoritative case/namespace seams, error classification и symlink capability; planning adapter возвращает typed blockers/warnings with exact bindings. A private-constructible reviewed authority retains one consumed exact preflight. Native implements exact-same-host internal-writable-APFS clone-only conditional publication with anchored descriptors, exact supported metadata seals/parity, post-clone verification and ordered `fsync(destination) → fsync(parent) → F_FULLFSYNC(destination)` barriers. Cross-volume staging, dedicated physical-volume fixtures and application/error presentation integration остаются открыты.

**Changes:**

1. Реализовать `ProviderCapabilitiesResolver` over `VFSHost`:
   - map `HostFeatures`;
   - include `IsWritable`, `IsWritableAtPath`, `IsImmutableFS`, `IsCaseSensitiveAtPath`, observation and thumbnail traits;
   - expose operation-specific support with evidence source: declared, path probe, engine fallback;
   - compose source/destination capabilities for cross-provider copy/move.
2. Реализовать `FileManagerErrorAdapter` from `nc::Error`, validation/preflight outcomes and operation terminal state.
3. Добавить `DisabledReason` factory that carries localizable message key, technical detail and suggested action id.
4. Перевести availability `file.copy`, `file.move`, `file.rename`, `file.trash` на resolver while legacy predicate remains comparison oracle.

**Existing seams:** [`VFS::HostFeatures` and host queries](../Source/VFS/include/VFS/Host.h), [`Base::Error`](../Source/Base/include/Base/Error.h), permission probe in [`PanelController.mm`](../Source/WinCommander/WinCommander/States/FilePanels/PanelController.mm), current writable predicates in [`CopyFile.mm`](../Source/WinCommander/WinCommander/States/FilePanels/Actions/CopyFile.mm) and [`Delete.mm`](../Source/WinCommander/WinCommander/States/FilePanels/Actions/Delete.mm).

**Tests:**

- table-driven capability mapping for native writable, immutable archive and remote host;
- path read-only, unsupported trash, cross-provider copy/move and unavailable observation;
- `nc::Error` mappings for permission, missing path, read-only, unsupported and cancellation;
- preservation of domain/code and localized failure reason.

**Exit:** first-slice command state includes a typed disabled reason; capability results agree with existing predicates for supported scenarios.

### R2 — Observable PaneStore adapter

**Purpose:** создать единый read model и intent boundary без раннего переноса listing engine.

**Статус 2026-08-02:** partial — standalone `PaneSnapshot`/`PaneStoreAdapter`, pure reducer/producer/coordinator и production `PanelControllerPaneStoreAdapter` проверены на main-queue publication, coalescing, stale generations, ordered lifecycle и reentrancy. Snapshot содержит exact focused item, exact selected identities в display order, hidden-file filter, semantic sort/group state, actual view mode/valid layout slot, Back/Forward availability and runtime current History entry ID. Commit suppresses pre-restoration cursor projection, deferred context rebuild publishes final focus/selection/filter/sort/group/layout/history state. Explorer breadcrumb, View/Sort popovers и Back/Forward/Up/Refresh toolbar читают matching Store state; `PaneNavigationAvailability` derives Up/Refresh state without granting execution authority. Remaining consumers, History restore/persistence and live-provider integration are next increments.

**Changes:**

1. Расширить текущий immutable `PaneSnapshot` полями provider id, selection/focus identities, sort/filter/group, view mode, history availability и permission/error state. **Exact focus, selected identities, hidden-file filter, semantic sort/group, view mode/layout, Back/Forward availability and runtime current History identity done. Provider and permission projections remain.**
2. Подключить существующий app-layer `PaneStoreAdapter` к одному `PanelController`. **Done for Explorer read projection.**
3. Keep one pane identity from controller construction through the read projection:
   - inject `PaneId` in the common `PanelController` factory and retain it for the controller lifetime; **Done.**
   - initialize `PanelControllerPaneStoreAdapter` from `controller.paneId`; **Done.**
4. Connect the verified pure `PaneLifecycleProducer` and `PanelControllerLifecycle` before lifecycle projection:
   - make `PanelController` own the producer-backed coordinator for its existing `PaneId`; **Done.**
   - route synchronous/asynchronous `GoToDirWithContext` through explicit request-ID-correlated navigation worker slots; **Done. Async fetch is detached on a shared `SerialQueue`, main callbacks use a weak controller box, intent invalidation cancels the token, and dealloc stops without waiting. Persistency recovery callbacks are weak.**
   - map navigation acceptance/rejection, deferred resolution, cancellation, typed failure and supersession to ordered main-queue events and source-typed feedback; **Done in production path; fake-producer adapter tests and deterministic-VFS production worker E2E cover the orchestration boundary.**
   - publish navigation success only after model/generation commit and preserve one terminal outcome for each accepted request; **Done in production path and verified across the adapter boundary plus production controller worker path.**
   - bring refresh under the same request identity and explicit worker-slot contract; **Done:** every refresh enters through `SubmitRefresh`, publishes only after exact identity/epoch/source validation and same-generation model commit, and emits typed failure/cancellation;
   - coalesce refresh as one running request plus one latest pending request, cancel it on navigation/content intent, and recover asynchronously from a disappeared current directory; **Done in production path.**
   - keep `NCPanelViewContextDidChangeNotification` for committed selection/focus/filter diffing, not loading inference. **Exact focus, selected identities and hidden-file filter projection done; commit-time focus suppression lasts until deferred cursor restoration.**
5. Reduce producer events into `PaneSnapshot`; **Done for navigation lifecycle.** Translate sort/layout callbacks and directory observation into explicit committed state changes.
6. Expose intents that delegate to current APIs: navigate, back/forward, refresh, set selection, set sort/filter, set view mode.
7. Route Explorer status/address/command context reads through snapshot, retaining `PanelController` execution below the adapter. **Address, hidden-files, sort, group, layout and Back/Forward/Up/Refresh presentation context done; status and remaining command contexts pending.**

**Existing seams:** [`PanelController` intents and generation](../Source/WinCommander/WinCommander/States/FilePanels/PanelController.h), [`PanelData::Model`](../Source/Panel/include/Panel/PanelData.h), [`PanelHistory`](../Source/WinCommander/WinCommander/States/FilePanels/PanelHistory.h), [`PanelView` context notification](../Source/WinCommander/WinCommander/States/FilePanels/PanelView.h), current JSON persistence in [`PanelControllerPersistency.mm`](../Source/WinCommander/WinCommander/States/FilePanels/PanelControllerPersistency.mm).

**Concurrency contract:** accepted requests carry stable per-pane identity and produce ordered main-queue events with exactly one terminal outcome. Navigation and refresh success follow the model/generation commit. Dedicated worker slots are correlation boundaries; queue occupancy only detects unrelated work. Deferred submissions are freshly probed and report resolution exactly once. Admission and accepted-worker feedback use separate validity tokens and a typed `DirectoryChangeResultSource`; competing content intents invalidate applicable feedback and delayed commits. Refresh admits one running plus one latest pending request and validates identity, epoch, source, and generation before publication. `PaneLifecycleProducer::Subscribe` atomically joins live observation with active seed or retained-failure replay/checkpoint. The Store bridge requires the exact post-model projection for `Committed`, suppresses focus while the model has committed but the cursor is not restored, then samples the final live item in the deferred context rebuild. All snapshot commits occur on main queue; stale generations produce no visible commit.

**Current tests:** Store passes 18 cases / 220 assertions, reducer 19 / 335, production bridge 28 / 461; dedicated `PanelHistory` passes 5 / 457; combined 70 / 1,473. Coverage includes exact focus/selection/filter/sort/group/view/history projection, runtime current-entry identity, selection payload reuse/reordering, malformed committed selection/count/sort/group/view/history rejection, valid/disabled layout-slot fallback, identity-only navigation transitions and lifecycle recomposition/sequencing. The Model generation contract passes 1 / 42, and Explorer matching-pane presentation/toolbar baseline passes 2 / 266 for absent/foreign state, sort/group/layout markers and all four history pairs. Lifecycle producer remains 24 / 267 and controller coordinator 21 / 211. Broader [`PanelControllerNavigation_UT.mm`](../Source/WinCommander/WinCommander/Tests/PanelControllerNavigation_UT.mm) prefixes pass production navigation 8 / 103 and refresh 16 / 154, covering deterministic VFS lifecycle, non-cooperative teardown, coalescing, recovery and exact publication. Prior full current-tree `WinCommanderUT` snapshot remains 217 / 2,694 in Debug and explicit ASAN/UBSAN.

**Remaining tests:** live local/remote provider integration, permission failure, deferred Busy feedback, temporary-listing production fixture, deterministic allocation fault injection, large-selection timing, remaining UI rendering and grouping/view/history persistence round-trip. Base queue accounting and callback isolation remain covered by `BaseUT`.

**Exit:** `PanelController` satisfies the lifecycle producer contract; one Explorer pane renders path/loading from `PaneSnapshot`, while counts and exact focus are available in the same Store. Selected identities and remaining render consumers stay open; Commander behavior continues through the same controller/model.

### R3 — CommandRegistry and shared command state

**Purpose:** установить stable command identity и единый availability/dispatch для всех UI entry points.

**Статус 2026-08-02:** partial — pure registry/value layer, eleven stable IDs и `LegacyShortcutBindingAdapter` готовы. Девять app-owned production definitions покрывают `file.copy`, `file.cut`, initiation `file.rename`, `file.open`, `view.toggleHiddenFiles`, `navigation.back`, `navigation.forward`, `navigation.up` и `navigation.refresh`. `file.open` объединяет main-menu/`OnOpenNatively:`, ordinary-file fallback of Enter, explicit Shortcut/Shift-Return и context-menu exact-item payloads; Enter сохраняет routing folder/archive navigation и terminal execution. Hidden-files and navigation commands converge across their menu, persisted shortcut and Explorer surfaces; Up/Refresh toolbar presentation is matching-Store-backed and execution rechecks live controller state through the shared admission/availability queue-ownership helper. The secondary Up shortcut retains its menu bounce, and command descriptor aliases preserve Shortcut classification. Rename filesystem execution and History listing restore remain legacy. Параллельно продолжается migration оставшихся P0 commands and `PaneStore` consumers.

**Changes:**

1. Define `CommandDescriptor`, `CommandContext`, `CommandState`, handler and registry lookup by `CommandId`.
2. Add `LegacyPanelActionAdapter` and `LegacyStateActionAdapter`:
   - execute existing `PanelAction`/`StateAction`;
   - compare legacy predicate with new `CommandState` during migration;
   - retain selector only inside adapter.
3. Add shortcut binding adapter over [`ActionsShortcutsManager`](../Source/Utility/include/Utility/ActionsShortcutsManager.h). **Read binding and `file.copy`/`file.cut`/`file.rename`/`file.open`/`view.toggleHiddenFiles`/Back/Forward/Up/Refresh responder routes done; `file.open` owns Shift-Return, while Up maps both enclosing-folder aliases to one stable ID; remaining command migration pending.**
4. Route four first-slice entry points through registry:
   - Explorer toolbar/command bar;
   - main menu responder action;
   - context menu item;
   - keyboard shortcut.
5. Publish the same title/icon/destructive flag, checked state and disabled reason to every surface. **Shared presentation done for `file.copy`, `file.cut`, rename initiation, `file.open` and `view.toggleHiddenFiles`; remaining commands pending.**
6. Move command execution to explicit intents for navigation and clipboard copy/cut; use synchronous pane-target adapters for rename initiation and hidden-files view state. `file.open` hands the accepted exact items synchronously to the shared `FileOpener`; Registry `Executed` records submitted handoff rather than Launch Services or remote-opener completion. `file.cut` records Move intent only. Rename submission still creates `nc::ops::Copying(docopy = false)` through the established controller path; paste and rename mutation boundaries must create, preflight and execute an accepted `OperationPlan`.

**Existing seams:** action type in [`DefaultAction.h`](../Source/WinCommander/WinCommander/States/FilePanels/Actions/DefaultAction.h), maps in [`PanelControllerActions.mm`](../Source/WinCommander/WinCommander/States/FilePanels/PanelControllerActions.mm) and [`StateActions.mm`](../Source/WinCommander/WinCommander/States/FilePanels/StateActions.mm), dispatcher validation in [`PanelControllerActionsDispatcher.mm`](../Source/WinCommander/WinCommander/States/FilePanels/PanelControllerActionsDispatcher.mm), Explorer surface in [`NCExplorerCommandBarView.mm`](../Source/WinCommander/WinCommander/States/Explorer/NCExplorerCommandBarView.mm).

**Current tests:** Back/Forward Registry 14 / 173 and dispatcher/toolbar 2 / 102; Up/Refresh total 32 / 549: core command/availability/identity/alias run 22 / 281, production navigation 4 / 50, production Refresh 3 / 37, Registry surfaces 1 / 27 and Explorer model/toolbar 2 / 154. The explicit-Up non-cooperative teardown case passes 1 / 12; broader production navigation/refresh prefixes pass 8 / 103 and 16 / 154. `file.open` passes 24 / 267: `FileOpenCommand` 8 / 102, Registry/shortcut integration 15 / 99 and focused production routing 1 / 66; the complete production Registry fixture passes 3 / 122, and explicit Release ASAN passes the core command plus production route at 9 / 168. `view.toggleHiddenFiles` baseline 7 / 70; combined prior command/registry/mapper/presentation 28 / 372; `file.copy` 6 / 58; `file.cut` 8 / 73; `file.rename` initiation 5 / 64; rename editor regression 2 / 23; cut pasteboard marker 1 / 47; failed marker 1 / 11; invalid UTF-8 atomicity 1 / 6; command-to-pasteboard failure integration 1 / 7. Incremental arm64 Debug `WinCommanderUT` build passed on 2026-08-02. The prior full current-tree `WinCommanderUT` snapshot remains 217 / 2,694 in Debug and explicit ASAN/UBSAN; hosted CI remains pending.

**Remaining tests:** AppKit responder focus integration, persistence migration, disabled-reason/Visual State presentation and equivalent all-entry-point coverage for remaining P0 commands; hosted CI remains pending.

**Exit:** `file.copy`, `file.cut`, `file.rename` initiation, `file.open`, `view.toggleHiddenFiles`, `navigation.back`, `navigation.forward`, explicit `navigation.up` and forced-user `navigation.refresh` have one definition and validation result across their production surfaces. `file.open` accepts one readable regular item, or a batch of regular items from the exact same readable provider instance; native directory/special-item handoff is supported, while dot-dot and remote directory/special-item contexts fail closed. `OnOpenNatively:` has one Registry owner, ordinary-file Enter and Shift-Return converge on it, and context menu supplies its exact captured items. Enter continues to route folder/archive navigation and terminal execution. Rename reports editor-start failure as `FileRenameInitiationError`; hidden-files state uses semantic On/Off presentation and reports a failed pane setter as `ViewToggleHiddenFilesError`; navigation commands expose localized boundary reasons and retain live execution guards. Up accepts uniform child/provider-parent hierarchy; non-uniform dot-dot/Enter retains compatibility Back behavior, and dot-dot submits exactly one enclosing-folder request. Refresh `Accepted`/`Deferred` means lifecycle submission, not listing commit; automatic soft refresh remains outside Registry. Continue with remaining P0 commands and Store consumers. Legacy rename mutation and History restore stay internal bridges until their later planning/lifecycle slices.

### R4 — Visual State and Error presentation

**Purpose:** превратить domain state в детерминированную UI projection.

**Статус 2026-08-02:** partial — pure `VisualStateMapper` проецирует `PaneSnapshot`, `FileManagerError` и normalized `CommandState` в toolkit-independent pane/breadcrumb/status/command states. Он различает initial unavailable и loaded empty, сохраняет committed content при refresh, применяет blocking/critical priority, отделяет nonblocking notices и сохраняет command `Off`/`On`/`Mixed`. AppKit adapter отображает checked state; `view.toggleHiddenFiles` является первым production checked consumer. Item/operation composition и остальные P0 consumers остаются следующими increments.

**Changes:**

1. Implement pure `VisualStateMapper` with explicit priority composition for pane loading/error/permission, item selection/focus/cut/operation and app operation state.
2. Feed mapper from `PaneSnapshot`, `CommandState`, operation snapshot and `FileManagerError`.
3. Replace first-slice local flags in breadcrumb, command bar and operation affordance with mapped state.
4. Add a shared error presenter that chooses inline pane state, command disabled explanation, sheet/popover or operation log according to severity and recovery actions.
5. Preserve ephemeral UI state inside views: hover, animation phase, popover visibility and field editor focus.

**Existing seams:** [`NCPanelViewContextDidChangeNotification`](../Source/WinCommander/WinCommander/States/FilePanels/PanelView.h), local Explorer availability in [`NCExplorerCommandBarView.mm`](../Source/WinCommander/WinCommander/States/Explorer/NCExplorerCommandBarView.mm), navigation feedback in [`NCExplorerBreadcrumbControl.mm`](../Source/WinCommander/WinCommander/States/Explorer/NCExplorerBreadcrumbControl.mm), operation observation in [`PoolViewController.mm`](../Source/Operations/source/PoolViewController.mm).

**Current tests:** initial versus empty folder, loading/refreshing, counts/selection, multiple locations, table-driven blocking categories, embedded/request error precedence, recoverable notice, cancellation, generic failure and normalized command state (final Debug 8 cases / 181 assertions). Explorer breadcrumb focused coverage verifies retained address/content, loading activity/editor gating, localized Store error/AX state, cancellation suppression and admission-versus-fetch feedback (1 case / 32 assertions).

**Remaining tests:** live local/remote provider and permission/deferred-Busy integration, operation progress plus item error, remaining presentation localization and manual accessibility evidence derived from state.

**Exit:** first-slice loading, disabled, permission, error, progress and completion have observable mapped states and user-facing recovery actions.

### R5 — OperationPlan and operation factory

**Purpose:** отделить intent/preflight от execution и сделать mutations проверяемыми до queueing.

**Статус 2026-08-02:** partial — structural plan through reviewed token, schema-v1 codec, private provider authority, bounded Native clone publication, lossless provider-result mapper, sealed transaction-backed Job and exact journal receipts are implemented. The production-configured orchestrator reaches the private reviewed factory; the public compatibility path still aborts and fails closed. Restricted cold hooks and owning durable-terminal delivery are implemented; Pool preallocates terminal transition and separates durable non-success with `ReleaseWithoutCompletion`. Receipt custody covers pre-rename retry, same-storage read-only reconciliation and exact Pool release handshake. Bounded production `CopyAs` now composes path-aware eligibility, exact review, stale-intent gates, process recovery, window submission custody and typed outcomes. Remaining gates are live boundary proof, cross-volume staging and physical-volume fixtures.

**Changes:**

1. Add immutable structural `OperationPlan`: plan/provider identities, source paths, destination kind/path, requested conflict policy where operation-valid, created-at timestamp and intrinsic type effects. **The structural intent is done for Copy/Move/Rename/Trash/PermanentDelete; the canonical visible plan remains partial.** The UI-visible `OperationPlan` projection must compose this immutable intent with preflight estimates, affected paths, resolved capabilities, requirements, conflicts, warnings/errors, controls and dry-run evidence required by the canonical specification.
2. Add copy-first pure `OperationPlanner` with injected probes and typed `AcceptedOperationPlan` / `BlockedOperationPlan` outcomes. **Done as a review/factory-readiness boundary:**
   - validate source/destination;
   - resolve provider capabilities;
   - detect same-path/recursive destination;
   - fetch space/estimate when provider supports it;
   - classify conflicts and destructive effects;
   - return an owning deterministic preflight report without embedding VFS ownership, UI, factory or queue effects.
3. Add a production VFS probes adapter for the pure planner. **Done:** immutable bindings own exact hosts; bound preflight retains the exact `Bindings::Ptr`; authoritative `ExactBytes` / `ASCIICaseSensitive` / `ASCIICaseInsensitive` / `Unavailable` identity and typed failures remain fail closed.
4. Add application access composition and explicit reviewed authority. **Provider authority, private execution integration and first app review done.** The move-only private-sealed authority consumes the exact reviewed preflight and Native consumes it into a bounded transaction. Public `ReviewedOperationFactory::Create` remains a deliberate aborting compatibility path; the private orchestrator friend returns the sealed product. Bounded `CopyAs` shows the exact bound summary and mints authority only after approval.
5. Add a versioned lossless `OperationPlanCodec`. **Done for schema v1:** strict deterministic JSON, canonical Base64 for opaque bytes, stable enum tokens, checked epoch-nanosecond timestamps and bounded decode.
6. Add an anchored Native execution capsule. **Bounded provider publication is done:** the Native transaction owns descriptor anchoring, exact internal-APFS volume policy, supported metadata seals/parity, exclusive atomic clone publication, post-clone verification and ordered full-filesystem durability. Isolated `NativeCreateCopy` remains the named-staging characterization capsule with a weaker `FileSystemSyncOnly` promise. Cross-volume bounded staging remains required.
7. Add durable journal admission and item results. **Done for schema v1:** private anchored journal, exact move-only admission and run receipts, atomic item+terminal `Finalize`, tri-state publication validation, poison-on-uncertainty and startup interruption without auto-resume.
8. Add typed Pool finalization and production composition. **Done at engine boundary:** `TryEnqueue` rejects atomically, terminal operations remain `Finalizing` while persistence returns `Retain`, and production `CopyOperationOrchestrator` composes the private reviewed factory/product. Exact custody supports retry, reopen reconciliation and `ReleaseReconciled`; injected construction is test-only.
9. Change `CopyAs::Perform` first for one focused regular Native create-only item. Add an app review summary and a coordinator that supplies the implemented restricted cold hooks and presents the owning exact durable outcome across retry/reconciliation before wiring `intent → plan → preflight → review → journal → private factory → Pool → durable outcome`. Other copy entry points remain legacy until their broader scope is planned.
10. Extend accepted preflight and factory adoption in order: move, trash, permanent delete, rename, create folder; reuse the existing structural plan types for the first four.
11. Move drag/drop operation resolution in [`DragReceiver.mm`](../Source/WinCommander/WinCommander/States/FilePanels/DragReceiver.mm) behind planner so drag badge and execution share the same decision.

**Existing seams:** concrete operation creation in [`CopyFile.mm`](../Source/WinCommander/WinCommander/States/FilePanels/Actions/CopyFile.mm), [`Delete.mm`](../Source/WinCommander/WinCommander/States/FilePanels/Actions/Delete.mm), [`PanelController.mm`](../Source/WinCommander/WinCommander/States/FilePanels/PanelController.mm), operation types under [`Source/Operations/source`](../Source/Operations/source), queue entry in [`MainWindowController.mm`](../Source/WinCommander/WinCommander/States/MainWindowController.mm).

**Current tests:** journal 27 / 592; Native create-copy 19 / 924; Native mapper 2 / 382; provider mapper 4 / 237; product 9 / 188; factory 8 / 225; Job 10 / 608; probes 5 / 178; orchestrator 15 / 758; Pool 17 / 219; ProviderCapabilities 16 / 549; Native conditional Copy 16 / 328; CopyAs policy/gate 6 / 28; recovery coordinator 6 / 67. Full `OperationsUT` passes 170 / 4,748 in Debug and explicit Release ASAN/UBSAN with confirmed runtimes and no diagnostics. The latest full Debug `VFSUT` run passes 95 / 43,566; full Debug `WinCommanderUT` passes 309 / 4,995; recorded M0 remains 897 / 132,011; Docker-backed ASAN remains 163 / 89,392.

**Remaining tests:** typed app review and durable-outcome presentation through the implemented hooks, one production `CopyAs` boundary, bounded cross-volume staging and physical internal/external-volume plus power-loss fixtures.

**Exit:** every first-slice copy enters the pool only from an exact durable admission receipt, publishes its typed item/terminal outcome durably, and leaves no unbounded staging state; direct UI construction of copy operation leaves the migrated paths.

### R6 — OperationCenterModel and persistence

**Purpose:** предоставить durable queue/history/read model поверх per-window pools.

**Статус 2026-08-02:** partial — journal/result/product/private-factory/Pool integration is complete at the engine boundary. Production-configured orchestration owns exact finalization and reconciliation release for one item. App adoption, bounded history, recovery presentation and Operation Center projection remain open.

**Changes:**

1. Add `OperationRecord` projection: plan id, operation id/type, status, item/byte progress, speed/ETA, current item, controls, error/result and timestamps.
2. Use Pool terminal finalizer as persistence authority: map and durably finalize the typed terminal result before returning `Release`; return `Retain` and retry on uncertainty. `NotifyAboutAddition/Removal` and lifecycle observers feed projection only.
3. Keep per-window pool as executor and aggregate records at app level, using [`AggregateProgressTracker`](../Source/Operations/source/AggregateProgressTracker.h) only as progress compatibility source.
4. Persist terminal history and interrupted markers through a versioned store. **Journal/Pool foundation done:** schema-v1 exact receipts, atomic terminal results, restart interruption, retained finalization retry and storage-bound run-receipt reconciliation. Production outcome linkage, bounded history and exported-diagnostics redaction remain.
5. Build Explorer Operation Center UI from `OperationCenterModel`; adapt the existing brief pool view as a compact surface during migration.
6. Route pause/resume/cancel/retry commands through `CommandRegistry` using operation id.

**Tests:** pending→running→completed, pause/resume, cancel, failure, terminal capture, pool removal, multiple windows, persistence/restart classification, bounded retention and redaction.

**Exit:** first-slice copy remains visible after completion, supports applicable controls through commands and restores terminal history after restart.

### R7 — Consolidation and expansion

**Purpose:** завершить ownership transfer and broaden contracts.

**Order:**

1. migrate remaining P0 Explorer commands;
2. route menu/context/palette/keyboard construction entirely from registry descriptors;
3. make PaneStore the write boundary for Commander panes and tabs;
4. extend OperationPlan to all mutation entry points, batch rename and sync dry-run;
5. project SearchEngine, ConnectionManager and PermissionManager state into stores;
6. remove compatibility adapters after repository-wide call-site search and aggregate regression suite.

Removal evidence for each legacy path includes zero UI call sites, passing characterization tests against the new owner, clean build and updated roadmap/changelog.

## 5. Change-set discipline

Каждый change set содержит один contract increment и один proven consumer:

| Change set | Contract | Consumer | Required proof |
|---|---|---|---|
| 1 | capabilities + error adapter | `file.copy` availability | VFS/adapter unit tests |
| 2 | PaneSnapshot + committed-state adapter | Explorer address | Panel/WinCommander tests + UI smoke |
| 2a | pure `PaneLifecycleProducer` | pure `PanelControllerLifecycle` coordination seam | producer/coordinator ordering tests; completed |
| 2b | shared controller/store `PaneId`, controller-owned coordinator, navigation and refresh worker/content-token boundaries | production navigation/refresh lifecycle events | coordinator and Store seams plus deterministic-VFS production worker E2E covered; live providers/permission/deferred Busy remain |
| 2c | PaneStore lifecycle reducer | Navigation/refresh lifecycle and error state rendered by Explorer breadcrumb | reducer + adapter + breadcrumb integration tests plus production worker E2E; remaining consumers/live-provider evidence pending |
| 2d | exact focused-item identity in PaneState | production `PanelControllerPaneStoreAdapter` | completed: commit/cursor timing fail-closed; included in current combined Store/reducer/bridge 65 / 1,016 |
| 2e | hidden-file filter state in PaneState | Explorer View popover command presentation | completed: unloaded/loaded projection, filter-only revision, lifecycle recomposition, matching-pane fail-closed model; execution remains live |
| 2f | exact selected identities in PaneState | production `PanelControllerPaneStoreAdapter` | completed: immutable shared payload, deterministic display order, selection-generation cache, O(1) cursor-path reuse, strict reducer invariant and retained lifetime; combined 65 / 1,016 |
| 2g | unified History navigation state + runtime current-entry identity | `PaneSnapshot` production bridge | completed: identity-only middle transitions, branch/trim stability, O(1) reducer invariants and lifecycle composition; History/Store/reducer/bridge 70 / 1,473 |
| 3 | registry + legacy handler | Explorer `file.copy` and `file.cut` across production entry points | command, presentation and pasteboard integration tests |
| 3a | registry + pane-target rename initiation adapter | Explorer `file.rename` editor initiation | completed: focused 5 / 64 plus editor regression 2 / 23; pre-adapter Debug/ASAN/UBSAN snapshot 213 / 2,638 each; execution remains legacy |
| 3b | registry + pane view-state adapter + semantic checked state | `view.toggleHiddenFiles` in View menu/shortcut/Explorer popover | completed: Debug scheme build; combined focused 28 / 372 |
| 3c | `navigation.back`/`navigation.forward` Registry definitions + Store/live contexts | Go menu/shortcut and Explorer toolbar | completed: core 14 / 173, dispatcher/toolbar 2 / 102; legacy History restore lifecycle remains open |
| 3d | `navigation.up`/`navigation.refresh` Registry definitions + derived Store/live availability | Go/View menu, shortcuts and Explorer toolbar | completed locally: uniform child/provider-parent Up, forced user Refresh, soft-refresh separation, shared queue-ownership guard, descriptor-aware secondary aliases, preserved menu bounce and nonblocking detached navigation teardown; total 32 / 549: core 22 / 281, navigation 4 / 50, Refresh 3 / 37, Registry surfaces 1 / 27, Explorer 2 / 154; hosted CI pending |
| 3e | `file.open` Registry definition + exact-item/provider context | main menu/`OnOpenNatively:`, ordinary-file Enter fallback, Shift-Return and context menu | production route implemented: shared `FileOpener` handoff, Enter router boundary, one Registry owner and exactly-one dot-dot enclosing request; 24 / 267 total: core 23 / 201 (`FileOpenCommand` 8 / 102 + Registry/shortcut 15 / 99) and production 1 / 66; full Registry fixture 3 / 122; hosted CI pending |
| 4 | visual mapper | loading/error/disabled states | state matrix + accessibility smoke |
| 4a | structural `OperationPlan` | Operations contract, no production consumer | completed: plan 8 / 113 |
| 4b | pure Copy `OperationPlanner` Accepted/Blocked | review/factory-readiness contract, no production consumer | completed: Debug planner 13 / 228 |
| 4c | production VFS/application probes + bound preflight | production evidence boundary | completed: adapter 5 / 178; access checker 4 / 56; ProviderCapabilities 16 / 549 |
| 4d | explicit review + compatibility `LegacyOperationFactory` | foundation-only concrete operation construction | completed: legacy factory evidence retained |
| 4e | schema-v1 `OperationPlanCodec` | durable structural plan representation | completed: codec 12 / 151 |
| 5a | typed conditional-authority gate + Native clone transaction + anchored staged capsule | fail-closed bounded publication | provider authority and internal-APFS metadata/durability complete: Native transaction 16 / 328, ProviderCapabilities 16 / 549, Native create-copy 19 / 924, Native mapper 2 / 382 |
| 5b | schema-v1 `OperationJournal` + exact admission/run/finalize authority | persistent lifecycle/item-result foundation with tri-state publication | completed: journal 27 / 592; public ID-addressed mutation removed |
| 5c | typed Pool finalizer + production-configured `CopyOperationOrchestrator` | engine-only single-item composition | completed: Pool 17 / 219, orchestrator 15 / 758 with restricted hooks, exact durable outcome, preallocated terminal transition and retry/reconcile/release custody |
| 5d | provider mapper + sealed product + private reviewed factory | lossless execution integration | completed: mapper 4 / 237, product 9 / 188, factory 8 / 225, Job 10 / 608 |
| 5e | app review/presentation + one Copy consumer | `CopyAs` mutation boundary | completed: path-aware selection, exact review, process recovery, window submission gate, item/durable-outcome presentation; focused app tests 12 / 95, live zero-enqueue/UI proof remains open |
| 6 | OperationCenterModel | copy progress/history | lifecycle/persistence tests + UI smoke |

Статус этапа меняется после acceptance evidence в [`Development-Plan.md`](Development-Plan.md), а завершённое user/developer-visible изменение фиксируется в [`../changelog.md`](../changelog.md).

## 6. Verification ladder

### Per contract

```bash
Scripts/run_all_unit_tests.sh Debug
```

Во время итерации affected console test executable запускается из Xcode; перед завершением change set выполняется aggregate script. OperationPlan execution дополнительно проходит `OperationsIT` with documented fixtures.

### Per vertical slice

```bash
Scripts/verify_m0.sh
```

Current M3 `OperationsUT` evidence passes 170 / 4,748 in Debug and explicit Release ASAN/UBSAN with confirmed runtimes and no diagnostics. The latest full Debug `VFSUT` run passes 95 / 43,566; full Debug `WinCommanderUT` passes 309 / 4,995; recorded M0 remains 897 / 132,011 and Docker-backed ASAN integration remains 163 / 89,392. Hosted CI publication and required-check enforcement remain separate work.

Manual smoke records exact source/destination providers and covers mouse, menu, context menu, keyboard, loading, disabled reason, progress, cancel and terminal result.

### Before adapter removal

```bash
Scripts/run_all_unit_tests.sh Debug
Scripts/run_all_integration_tests.sh
git diff --check
```

Adapter removal also requires repository search confirming that migrated UI entry points use command ids and mutation paths create operations through `OperationPlan`.

## 7. Stop conditions and rollback boundaries

| Signal | Action | Rollback boundary |
|---|---|---|
| New command availability disagrees with legacy predicate | keep legacy execution active, record fixture, fix capability/context model | registry adapter toggle for that command id |
| Pane snapshot loses selection/history during reload | retain controller as read/write owner, fix generation and identity mapping | view continues reading controller directly for affected field |
| Planner changes concrete operation semantics | keep accepted plan as diagnostics-only and execute legacy construction | operation factory boundary |
| Operation Center misses terminal event | capture completion at pool callback before enabling persistence | compact legacy pool view remains active |
| Aggregate tests expose cross-mode regression | limit consumer to Explorer and preserve Commander routing | UI entry-point adapter |

Rollback happens per command or projection; engine modules and stored user data remain compatible throughout the migration.

## 8. Completion criteria

The refactor sequence is complete when:

- every P0 action resolves through one command id and returns one `CommandState` on all surfaces;
- each pane publishes one observable snapshot and accepts navigation/selection/view intents through `PaneStore`;
- provider-dependent decisions derive from `ProviderCapabilities` with a typed `DisabledReason`;
- critical UI states derive from one visual mapper and typed errors retain recovery plus technical context;
- each mutating P0 command produces an `OperationPlan` before execution;
- Operation Center owns queue/history projection across windows and restart;
- current `Panel`, `VFS`, `Operations` and `Config` engines remain covered by focused and aggregate tests.
