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

Первый сквозной сценарий развивается по вертикальным срезам: открыть local folder → опубликовать navigation/refresh state → выбрать item → вычислить command availability → создать structural `OperationPlan` → выполнить bound preflight/review → durably admit → создать operation → показать progress/result → обновить pane state. Navigation/refresh and eleven command routes already use production contracts. `operationCenter.open` получает value snapshot через weak process-owned coordinator и передаёт независимую копию в static Explorer panel без Journal, Pool, executor или observer authority. Operations now has the provider transaction, lossless result mapper, sealed execution product, private reviewed-factory path, exact journal authority, receipt retry/reconcile/release custody, restricted cold hooks, exact durable-outcome delivery, preallocated Pool terminal transition, production-configured Copy orchestrator and one bounded reviewed `CopyAs` consumer. Its process-owned coordinator bridges the exact receipt to that orchestrator and reduces Start/durable terminal outcomes into the model. Native now distinguishes same-volume clone from an unactivated cross-volume helper scope; physical-volume/power-loss evidence and the separately signed cross-volume staging implementation remain next gates.

**Current batch terminal boundary:** `OperationJournal` can atomically finalize an exact canonical vector of terminal results and retain that vector for retry. `[batch-durable-terminal]` passes 6 Debug cases / 251 assertions, with fresh Release ASAN and UBSAN passes at the same count; the final full Debug `OperationsUT` result is 210 / 211 and 5,662 / 5,666 with only the known set-ID host baseline. The only multi-item custody method is test-only and accepts an exact journal-issued Running receipt. Batch reviewed plans remain closed in the production factory, orchestrator, coordinator and `CopyAs`; the application’s scalar result projection applies only to a one-item vector.

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

**Статус 2026-08-02:** partial — conservative resolver, explicit provider declarations и lossless POSIX `FileManagerError` adapter добавлены с unit coverage. `VFSHost` имеет authoritative case/namespace seams, error classification и symlink capability; planning adapter возвращает typed blockers/warnings with exact bindings. A private-constructible reviewed authority retains one consumed exact preflight. Native implements exact-same-host internal-writable-APFS clone-only conditional publication with anchored descriptors, exact supported metadata seals/parity, post-clone verification and ordered `fsync(destination) → fsync(parent) → F_FULLFSYNC(destination)` barriers. Cross-volume staging, physical-volume/power-loss evidence and application/error presentation integration остаются открыты.

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

**Статус 2026-08-07:** partial — standalone `PaneSnapshot`/`PaneStoreAdapter`, pure reducer/producer/coordinator и production `PanelControllerPaneStoreAdapter` проверены на main-queue publication, coalescing, stale generations, ordered lifecycle и reentrancy. Snapshot содержит exact focused item, exact selected identities в display order, hidden-file filter, semantic sort/group state, actual view mode/valid layout slot, Back/Forward availability and runtime current History entry ID. Explorer owns an ordered per-tab controller/Store/History set and observes exactly one active entry through a generation- and `PaneId`-bound token. Q1-5/Q1-6 persist exact location view and session topology. Q1-7a/Q1-7b consume the admitted active snapshot for folder state and exact-mount disconnect classification; Q1-7c adds exact inline-rename intent. Q1-7d seals the local drag source listing/generation/items and target listing/generation/item across one persistent Validate→Receive receiver, while a pure policy owns modifier, path-capability, provider and Native-volume decision facts. Receive rebuilds and compares the complete decision before legacy operation enqueue. Q1-7 functional and closure gates pass. Breadcrumb, footer, View/Sort popovers and Back/Forward/Up/Refresh toolbar continue reading only matching active Store state. Remaining operation projection is incremental.

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
5. Reduce producer events into `PaneSnapshot`; **Done for navigation lifecycle and presentation context.** Sort/group/layout changes rebuild semantic Store state without advancing listing generation.
6. Expose intents that delegate to current APIs: navigate, back/forward, refresh, set selection, set sort/filter, set view mode. **Navigation and exact pane-local view restore paths are connected; remaining generic Store intents are incremental.**
7. Route Explorer status/address/command context reads through snapshot, retaining `PanelController` execution below the adapter. **Address, footer count/selected bytes, hidden-files, sort, group, layout and Back/Forward/Up/Refresh presentation context done; Q1-5 exact per-location view persistence and Q1-6 exact session capture consume the same matching projection.**

**Existing seams:** [`PanelController` intents and generation](../Source/WinCommander/WinCommander/States/FilePanels/PanelController.h), [`PanelData::Model`](../Source/Panel/include/Panel/PanelData.h), [`PanelHistory`](../Source/WinCommander/WinCommander/States/FilePanels/PanelHistory.h), [`PanelView` context notification](../Source/WinCommander/WinCommander/States/FilePanels/PanelView.h), current JSON persistence in [`PanelControllerPersistency.mm`](../Source/WinCommander/WinCommander/States/FilePanels/PanelControllerPersistency.mm).

**Concurrency contract:** accepted requests carry stable per-pane identity and produce ordered main-queue events with exactly one terminal outcome. Navigation and refresh success follow the model/generation commit. Dedicated worker slots are correlation boundaries; queue occupancy only detects unrelated work. Deferred submissions are freshly probed and report resolution exactly once. Admission and accepted-worker feedback use separate validity tokens and a typed `DirectoryChangeResultSource`; competing content intents invalidate applicable feedback and delayed commits. Refresh admits one running plus one latest pending request and validates identity, epoch, source, and generation before publication. `PaneLifecycleProducer::Subscribe` atomically joins live observation with active seed or retained-failure replay/checkpoint. The Store bridge requires the exact post-model projection for `Committed`, suppresses focus while the model has committed but the cursor is not restored, then samples the final live item in the deferred context rebuild. All snapshot commits occur on main queue; stale generations produce no visible commit.

**Current tests:** Store passes 18 cases / 220 assertions, reducer 19 / 335, production bridge 28 / 461; dedicated `PanelHistory` passes 5 / 457; combined 70 / 1,473. Coverage includes exact focus/selection/filter/sort/group/view/history projection, runtime current-entry identity, selection payload reuse/reordering, malformed committed selection/count/sort/group/view/history rejection, valid/disabled layout-slot fallback, identity-only navigation transitions and lifecycle recomposition/sequencing. The Model generation contract passes 1 / 42, and Explorer matching-pane presentation/toolbar baseline passes 2 / 266 for absent/foreign state, sort/group/layout markers and all four history pairs. Q1-4 ordered tab/model/gate/AppKit coverage passes 14 / 148. Q1-5 standalone persistence passes 8 / 580 and its complete focused binding/sort/layout/controller/Explorer boundary passes 26 / 823. Q1-6 codec passes 8 / 116, tab/session state 7 cases, adjacent Inspector 10 / 158 and no-password restore 2 / 14. Current full Debug `WinCommanderUT` passes 507 / 9,030; combined Q1-7 passes 19 / 179. Full `PanelUT` records 54 / 1,349 total with 53 / 1,348 passed plus one expected host-UI failure / one assertion and exit 0; Debug `WinCommander-Unsigned` builds successfully. Lifecycle producer remains 24 / 267 and controller coordinator 21 / 211. Broader [`PanelControllerNavigation_UT.mm`](../Source/WinCommander/WinCommander/Tests/PanelControllerNavigation_UT.mm) prefixes pass production navigation 8 / 103 and refresh 16 / 154, covering deterministic VFS lifecycle, non-cooperative teardown, coalescing, recovery and exact publication. The same production file adds real temporary-directory `NativeHost` navigation plus forced refresh at 1 / 30 and `EACCES` permission failure with exact typed context at 1 / 22. Docker-backed [`PanelControllerRemoteNavigation_IT.mm`](../Source/WinCommander/WinCommander/Tests/PanelControllerRemoteNavigation_IT.mm) adds FTP/SFTP/WebDAV navigation, shadow-host mutation, forced refresh and endpoint outage/reconnect (6 / 244): FTP/WebDAV prove the correlated soft-refresh successor after user supersession under latest-wins, SFTP proves direct user-refresh commit without provider observation/cache, FTP and SFTP retain their provider-domain `Unavailable` classification, and WebDAV retains `POSIX/ECONNREFUSED` or `ECONNRESET`, all with retained listing followed by the fresh reconnect successor.

**Recorded remote-transport update:** The later remote-slice Debug `VFSUT` run passed 95 / 43,563 and `WinCommanderUT` 321 / 5,206; `IntegrationTests` built, the SFTP blackhole VFSIT passed 1 / 2, and focused Docker remote navigation passed 9 / 376. FTP listing, WebDAV blocking-control and SFTP TCP/session paths use bounded 30-second production deadlines and isolated 500 ms seams. Endpoint pause/unpause preserves raw FTP `operation_timeout`, SFTP `timeout` and WebDAV `POSIX/ETIMEDOUT`, maps each to `TimeoutError`, retains listing/generation and proves same-controller reconnect. Negative SFTP `readdir` fails closed; unavailable/timeout connections do not re-enter its pool. Current full Debug evidence is `VFSUT` 127 / 130 and 44,616 / 44,619 with three host-environment baselines, and `WinCommanderUT` 333 / 5,376.

**Remaining tests:** Deferred Busy feedback beyond the proven external-loading admission case, FDA/security-scoped and other permission classes, temporary-listing production fixture, deterministic allocation fault injection, large-selection timing, remaining UI rendering, History persistence and signed restart walkthrough. Base queue accounting and callback isolation remain covered by `BaseUT`.

**Exit:** `PanelController` satisfies the lifecycle producer contract; each Explorer runtime tab retains an independent controller/Store/History, and only the exact active tab renders path/loading/counts/focus and persists complete view settings through its matching snapshot. Commander behavior continues through the same controller/model. Q1-6 now restores the ordered runtime set from canonical locations while minting fresh process-local identities.

### R3 — CommandRegistry and shared command state

**Purpose:** установить stable command identity и единый availability/dispatch для всех UI entry points.

**Статус 2026-08-07:** complete for the Queue 1 roster — pure Registry/value layer, twenty-seven stable IDs, twenty-seven app-owned production definitions and `LegacyShortcutBindingAdapter` are active. Q1-1/Q1-2 complete the mutation roster and background/overflow composition. Q1-3 adds exact read-only `file.getInfo`, `file.preview` and checked `view.togglePreviewPane` over the mounted Explorer inspector; mutating File Attributes remains a separate compatibility action.

**Changes:**

1. Define `CommandDescriptor`, `CommandContext`, `CommandState`, handler and registry lookup by `CommandId`.
2. Add `LegacyPanelActionAdapter` and `LegacyStateActionAdapter`:
   - execute existing `PanelAction`/`StateAction`;
   - compare legacy predicate with new `CommandState` during migration;
   - retain selector only inside adapter.
3. Add shortcut binding adapter over [`ActionsShortcutsManager`](../Source/Utility/include/Utility/ActionsShortcutsManager.h). **All twenty-seven current command definitions, including Get Info, Quick Look and Details Pane, use stable IDs and persisted-action aliases on their mounted routes.**
4. Route four first-slice entry points through registry:
   - Explorer toolbar/command bar;
   - main menu responder action;
   - context menu item;
   - keyboard shortcut.
5. Publish the same title/icon/destructive flag, checked state and disabled reason to every surface. **Shared presentation covers the full Queue 1 roster on menu, persisted shortcut, exact context and Explorer surfaces; hidden-files and details-pane toggles retain checked state.**
6. Move command execution to explicit typed intents and live ports. Queue 1 mutations use the established `nc::ops::*` operations; Paste, Trash, Permanent Delete, New Folder, Duplicate, Archive Create and Batch Rename apply typed fail-closed admission before enqueue. Calculate Sizes uses the existing background calculation queue with exact listing/generation commit guards; Copy Path returns typed pasteboard submission, while Select All and Invert Selection use one complete visible-projection vector. Reviewed-engine expansion resumes in Queue 2.

**Existing seams:** action type in [`DefaultAction.h`](../Source/WinCommander/WinCommander/States/FilePanels/Actions/DefaultAction.h), maps in [`PanelControllerActions.mm`](../Source/WinCommander/WinCommander/States/FilePanels/PanelControllerActions.mm) and [`StateActions.mm`](../Source/WinCommander/WinCommander/States/FilePanels/StateActions.mm), dispatcher validation in [`PanelControllerActionsDispatcher.mm`](../Source/WinCommander/WinCommander/States/FilePanels/PanelControllerActionsDispatcher.mm), Explorer surface in [`NCExplorerCommandBarView.mm`](../Source/WinCommander/WinCommander/States/Explorer/NCExplorerCommandBarView.mm).

**Current tests:** Q1-3 command contracts pass 17 / 288, exact production dispatcher routing 1 / 46, Inspector/AppKit 8 / 111, model 6 / 97 and Explorer geometry 25 / 1,045. Full Debug `WinCommanderUT` records 420 / 425 cases and 7,435 / 7,439 assertions with four known headless pasteboard host baselines and one expected skip. Focused Release ASAN and UBSAN Inspector/model/preview routing passes 20 / 345 in each runtime; the unsigned Debug application builds.

**Remaining tests:** signed interactive Commander/Explorer mouse, keyboard and VoiceOver smoke; shortcut persistence migration for future IDs; hosted CI.

**Exit:** twenty-seven stable commands have one app-owned definition and shared typed state on their current production surfaces. Q1-1 and Q1-2 are complete, and Q1-3 closes the read-only Get Info/Preview/Details visibility boundary. Existing navigation, Open, Hidden Files and Operation Center compatibility boundaries remain as recorded.

### R4 — Visual State and Error presentation

**Purpose:** превратить domain state в детерминированную UI projection.

**Статус 2026-08-07:** partial — pure `VisualStateMapper` owns pane/breadcrumb/status/command projection and Q1-7 completes folder/row interaction state. Q1-8 adds the separate pure `ExplorerOperationProgressModel` over copied exact per-window Pool values, an accessible AppKit progress adapter and explicit accessible conflict actions with preserved legacy response semantics. Terminal item/result/error composition belongs to the future full Operation Center.

**Changes:**

1. Implement pure `VisualStateMapper` with explicit priority composition for pane loading/error/permission, item selection/focus/cut/operation and app operation state.
2. Feed mapper from `PaneSnapshot`, `CommandState`, operation snapshot and `FileManagerError`.
3. Replace first-slice local flags in breadcrumb, command bar and operation affordance with mapped state.
4. Add a shared error presenter that chooses inline pane state, command disabled explanation, sheet/popover or operation log according to severity and recovery actions.
5. Preserve ephemeral UI state inside views: hover, animation phase, popover visibility and field editor focus.

**Existing seams:** [`NCPanelViewContextDidChangeNotification`](../Source/WinCommander/WinCommander/States/FilePanels/PanelView.h), local Explorer availability in [`NCExplorerCommandBarView.mm`](../Source/WinCommander/WinCommander/States/Explorer/NCExplorerCommandBarView.mm), navigation feedback in [`NCExplorerBreadcrumbControl.mm`](../Source/WinCommander/WinCommander/States/Explorer/NCExplorerBreadcrumbControl.mm), operation observation in [`PoolViewController.mm`](../Source/Operations/source/PoolViewController.mm).

**Current tests:** Q1-7 evidence remains in [`explorer_folder_visual_states_slice.md`](Features/explorer_folder_visual_states_slice.md). Q1-8 progress model/view/controller passes 11 / 107 in Debug, Release ASAN and Release UBSAN; both sanitizer executables link their expected clang runtime and report no diagnostics. Conflict UI passes 3 / 41, current-item publication passes 2 / 19 in Debug, Release ASAN and Release UBSAN, full Debug `OperationsUT` passes 216 / 5,726, full Debug `WinCommanderUT` passes 518 / 9,137 and Debug `WinCommander-Unsigned` builds successfully.

**Remaining tests:** terminal item/result/error composition, remaining presentation localization, physical external-drive UI walkthrough and signed manual accessibility evidence derived from state. Production deferred-Busy admission passes 1 / 22: external loading queued from `Started` cancels the initial request and rejects the nested deferred navigation exactly once before provider fetch.

**Exit:** first-slice loading, disabled, permission, error, progress and completion have observable mapped states and user-facing recovery actions.

### R5 — OperationPlan and operation factory

**Purpose:** отделить intent/preflight от execution и сделать mutations проверяемыми до queueing.

**Статус 2026-08-03:** partial — structural plan through reviewed token, schema-v1 codec, private provider authority, bounded Native clone publication, lossless provider-result mapper, sealed transaction-backed Job and exact journal receipts are implemented. The production-configured orchestrator reaches the private reviewed factory; the public compatibility path still aborts and fails closed. Restricted cold hooks and owning durable-terminal delivery are implemented; Pool preallocates terminal transition and separates durable non-success with `ReleaseWithoutCompletion`. Receipt custody covers pre-rename retry, same-storage read-only reconciliation and exact Pool release handshake. Bounded production `CopyAs` composes path-aware eligibility, exact review, stale-intent gates, process recovery, window submission custody and typed outcomes; its production seam proves zero enqueue, exact review projection and durable dispatch before Pool removal. The planner additionally has a narrow one-file same-provider Move preflight, but generic review rejects Move before it can mint factory authority and legacy Move execution is unchanged. The descriptor-anchored physical fixture includes a hidden test-only power-loss checkpoint harness; dedicated real-volume and physical power-loss evidence remain pending. Cross-volume reviewed Copy remains fail closed until the isolated helper authority of ADR 0002 exists.

**Changes:**

1. Add immutable structural `OperationPlan`: plan/provider identities, source paths, destination kind/path, requested conflict policy where operation-valid, created-at timestamp and intrinsic type effects. **The structural intent is done for Copy/Move/Rename/Trash/PermanentDelete; the canonical visible plan remains partial.** The UI-visible `OperationPlan` projection must compose this immutable intent with preflight estimates, affected paths, resolved capabilities, requirements, conflicts, warnings/errors, controls and dry-run evidence required by the canonical specification.
2. Add copy-first pure `OperationPlanner` with injected probes and typed `AcceptedOperationPlan` / `BlockedOperationPlan` outcomes. **Copy is done as a review/factory-readiness boundary; the independent Move slice is intent-only:**
   - validate source/destination;
   - resolve provider capabilities;
   - detect same-path/recursive destination;
   - fetch space/estimate when provider supports it;
   - classify conflicts and destructive effects;
   - return an owning deterministic preflight report without embedding VFS ownership, UI, factory or queue effects.
   - Move accepts only one regular file to an absent exact-item destination on the same provider with `Ask/ThisItem`, exact path identity and `can_rename` plus `Rename` access on both parent namespaces; folders, symlinks, batches, cross-provider and replacement shapes are blocked. Its report has `RuntimeRevalidationRequired` and does not use Copy read/create/estimate/space claims.
3. Add a production VFS probes adapter for the pure planner. **Done:** immutable bindings own exact hosts; bound preflight retains the exact `Bindings::Ptr`; authoritative `ExactBytes` / `ASCIICaseSensitive` / `ASCIICaseInsensitive` / `Unavailable` identity and typed failures remain fail closed.
4. Add application access composition and explicit reviewed authority. **Provider authority, private execution integration and first app review are Copy-only.** The move-only private-sealed authority consumes the exact reviewed preflight and Native consumes it into a bounded transaction. `ReviewedVFSOperationPreflight::Review` rejects an accepted Move with `UnsupportedPlanType`, so that intent report cannot reach factory, journal or `Pool`. Public `ReviewedOperationFactory::Create` remains a deliberate aborting compatibility path; the private orchestrator friend returns the sealed product. Bounded `CopyAs` shows the exact bound summary and mints authority only after approval.
5. Add a versioned lossless `OperationPlanCodec`. **Done for schema v1:** strict deterministic JSON, canonical Base64 for opaque bytes, stable enum tokens, checked epoch-nanosecond timestamps and bounded decode.
6. Add an anchored Native execution capsule. **Bounded provider publication is done:** the Native transaction owns descriptor anchoring, exact internal-APFS volume policy, supported metadata seals/parity, exclusive atomic clone publication, post-clone verification and ordered full-filesystem durability. Isolated `NativeCreateCopy` remains the named-staging characterization capsule with a weaker `FileSystemSyncOnly` promise. Cross-volume bounded staging remains required.
7. Add durable journal admission and item results. **Done for schema v1:** private anchored journal, exact move-only admission and run receipts, atomic item+terminal `Finalize`, tri-state publication validation, poison-on-uncertainty and startup interruption without auto-resume.
8. Add typed Pool finalization and production composition. **Done at engine boundary:** `TryEnqueue` rejects atomically, terminal operations remain `Finalizing` while persistence returns `Retain`, and production `CopyOperationOrchestrator` composes the private reviewed factory/product. Exact custody supports retry, reopen reconciliation and `ReleaseReconciled`; injected construction is test-only.
9. Change `CopyAs::Perform` first for one focused regular Native create-only item. Add an app review summary and a coordinator that supplies the implemented restricted cold hooks and presents the owning exact durable outcome across retry/reconciliation before wiring `intent → plan → preflight → review → journal → private factory → Pool → durable outcome`. Other copy entry points remain legacy until their broader scope is planned.
10. Extend accepted preflight and factory adoption in order: the narrow Move preflight is implemented; Move review/factory/execution, then trash, permanent delete, rename and create folder remain. Reuse the existing structural plan types for the first four.
11. Move drag/drop operation resolution in [`DragReceiver.mm`](../Source/WinCommander/WinCommander/States/FilePanels/DragReceiver.mm) behind planner so drag badge and execution share the same decision. **Done for the Queue 1 local-panel boundary:** pure `DragDropPolicy` owns Copy/Move/Link selection from normalized modifiers, path-scoped capabilities and exact provider/Native-volume identities; source/target listing-generation-item seals plus identical receive-time re-evaluation gate established `Copying`/`Linkage` enqueue. The reviewed engine remains unchanged.

**Existing seams:** concrete operation creation in [`CopyFile.mm`](../Source/WinCommander/WinCommander/States/FilePanels/Actions/CopyFile.mm), [`Delete.mm`](../Source/WinCommander/WinCommander/States/FilePanels/Actions/Delete.mm), [`PanelController.mm`](../Source/WinCommander/WinCommander/States/FilePanels/PanelController.mm), operation types under [`Source/Operations/source`](../Source/Operations/source), queue entry in [`MainWindowController.mm`](../Source/WinCommander/WinCommander/States/MainWindowController.mm).

**Current tests:** the durable OperationCenterModel/coordinator evidence remains recorded in its feature specs. Q1-8 progress passes 11 / 107 in Debug, Release ASAN and Release UBSAN, current-item publication passes 2 / 19 in all three configurations, conflict UI passes 3 / 41, full Debug `OperationsUT` passes 216 / 5,726 and full Debug `WinCommanderUT` passes 518 / 9,137. Both WinCommander sanitizer executables link their expected runtime and report no diagnostics. Full Center result/error/log/history and sealed controls remain future work.

**Current physical-checkpoint seam:** `ConditionalCopyIO` presents test-only `BeforePublish` and `AfterPublishBeforeFullFSync` checkpoints. The hidden `OperationsIT` case selects one through explicit environment, carries a custom `NativeHost` through the exact review bindings, writes and `fsync`s a manifest before optional operator blocking, and retains its descriptor-anchored workspace only after that step. The manifest binds run ID, phase, timestamp, journal and parent identities, source/destination identities and component names. A manifest failure reaches `Clone` or `F_FULLFSYNC` as the phase allows. The release protocol begins after reboot with descriptor-bound read-only journal capture before `OperationJournal::Open`, then destination inspection and an `Interrupted` no-auto-resume record.

**Cross-volume foundation:** `ProviderConditionalCopyPathSupport` distinguishes `SameVolumeClone` and `CrossVolumeStaged`. Native routes staged eligibility only through an injected descriptor/seal-only authority, while no installed production transport preserves the legacy route and an unavailable authority fails closed before review selection. `RoutedIO/CrossVolumeStagingProtocol`, its private XPC codec and injectable VFS client carry only exact V1 scalar claims, two duplicated descriptors, a correlation-bound opaque lease and the conservative completion matrix; their focused evidence is protocol 5 / 44, codec 4 / 74 and client 4 / 108. `CrossVolumeStagingHelperV1` validates close-on-exec/read-only exact source/destination-parent descriptor seals into move-only `ValidatedBegin`, and its lease store issues one owner-bound terminal claim; the original validator filter passes 4 / 208 and the final PID-bound lease filter passes 7 / 363. The helper-private ledger persists bounded primary reservations and append-only companions; its final Debug filter passes 16 / 388. `SourceSnapshotWriter` validates the source root/device and returns a sealed read-only continuation after positional snapshot copy. The destination-stage, PID/locking, runner, durable lifecycle retention/inspection and private publication-barrier closures are recorded in the next paragraphs. The dispatcher has no store integration, transport or namespace-mutation authority. NonMAS owns the V1 dependency and privileged-helper requirements; Unsigned and MAS retain separate scope. The target requires Developer ID plus hardened runtime, while signed artifact/SMJobBless proof awaits an available identity. Physical evidence remains a separate layer.

**Current destination-stage slice:** `SourceSnapshotWriter` now admits only when its protected root is on `request.source.device`. `DestinationStageWriter` consumes only that sealed Commit continuation and admits only a destination root on `request.destination_parent.device` that differs from the source device. It revalidates the read-only snapshot, original source/destination-parent descriptors, root binding and exact active reservation after each unlocked cancellation probe; it writes a canonical private `O_EXCL|O_NOFOLLOW` `0600` one-link stage, synchronizes `artifact → root → F_FULLFSYNC`, append-seals V1 and returns only a move-only helper-private continuation with read-only descriptors. The final consolidated filter passes 9 cases with one expected no-root device-fixture skip / 385 assertions in Debug, Release ASAN and Release UBSAN. That no-root run does not prove cleanup, physical media or the two-internal-APFS gate. The helper target builds in Debug; the 152 / 45,372 full Debug `VFSUT` aggregate is pre-runner evidence.

**Current protected-root PID/locking closure:** `ProtectedRootLedger::Open` creates its own exact `0700` root-directory descriptor from the borrowed FD and retains a nonblocking exclusive advisory `flock` directly on it. The process-local registry rejects a duplicate local owner and makes close-and-registry-erase indivisible with fork. Its `pthread_atfork` child handler closes inherited locked root descriptors; PID ownership makes inherited ledger calls fail closed and returns `ForkedProcess` for acquisition against a parent-owned registry. Move construction, assignment and destruction leave foreign-PID ledgers inert without taking inherited C++ mutexes or closing a child-reused descriptor number. The same PID boundary spans `ValidatedBegin`, `LeaseStore`/`TerminalLease` and both sealed continuations, with source/destination writers rejecting inherited input and foreign ledgers before callbacks, a mutex or reservation. Debug ledger evidence is 16 / 388; fresh Release ASAN and UBSAN pass without diagnostics at ledger 16 / 388, `LeaseStore` 7 / 363, `ValidatedBegin` reuse 1 / 16 and `TerminalLease` reuse 1 / 58. The shared VFS `TestDir` only conflicts between independent concurrent `VFSUT` processes; the recorded verification is sequential. This serializes only participating helpers. It does not prevent a noncooperating same-UID namespace actor, bind a stage to publication, or expose a publication/cleanup operation. `StagingPublicationBarrier` now performs the reviewed root acquisition/revalidation; the later publisher must consume its permit.

**Pre-runner standalone root-admission slice:** `StagingRootAuthority` consumes only a current-process Commit `TerminalLease`, anchors two borrowed root FDs, exact-validates root owner, `0700` mode and the source/destination-parent device bindings, rejects a same-device pair and opens the ledger pair in canonical `(device,inode)` order. Its opaque move-only PID-bound `LockedSession` retains the terminal lease and both locks; a failed second lock releases the first temporary ledger. It exposes no FD, path, writer, publisher, cleanup or recovery authority. Before `StagingSessionRunner`, it was deliberately detached from `SourceSnapshotWriter → DestinationStageWriter` because both paths consume the terminal lease; the runner now supplies only that private composition, while a publisher still needs its own final revalidation/cancellation barrier. The pre-runner Debug roots coverage passes 3 plus one expected missing-root skip / 111, full Debug `VFSUT` 152 / 45,372 and the Debug helper target builds; Release ASAN and UBSAN each pass roots 3 plus one skip / 111, ledger 16 / 388, `LeaseStore` 7 / 363 and source snapshot/stage 9 plus one skip / 385 without diagnostics. The opt-in fixture requires `WINCOMMANDER_VFS_DESTINATION_STAGE_ROOT` and retains `.wc-staging-roots-ut.*` for manual inspection/removal only. The workspace, `/private/tmp` and `/Volumes` share one `st_dev`, and `/Volumes` has no mounted child, so that pre-runner cross-device evidence remained externally blocked.

**Locked-session runner, before the barrier slice:** helper-private `StagingSessionRunner` consumes `LockedSession` by value, rejects foreign or moved input before either cancellation callback, retains both root-ledger locks through `SourceSnapshotWriter::Create` and then `DestinationStageWriter::Create`, and preserves the exact `Session`, `SourceSnapshot` or `DestinationStage` failure phase. Session destruction releases both locks on every return. Success returns only the existing move-only sealed read-only stage; it does not bind that stage to a namespace operation or grant path, publisher, cleanup or recovery authority. Its pre-barrier Debug roots coverage passed 3 cases plus 2 expected missing-environment skips / 111 assertions, with Debug `VFSUT` 149 / 152 and 45,404 / 45,407 including exactly the three known host baselines.

**Historical private publication barrier:** `StagingPublicationBarrier::Prepare` consumes one current-process sealed destination stage, reacquires and retains its exact destination protected-root `flock`, exact-revalidates the Commit terminal claim, source, destination parent, source snapshot, stage, primary and sealed records, canonical artifact identity and destination absence both before and after its only unlocked cancellation callback, then returns only an opaque move-only permit. It fails closed for forked/moved/stale/cancelled/busy-root/destination-present input and has no pathname or descriptor surface. It creates, renames, links, unlinks and opens no user destination entry; any later namespace authority is subject to the publisher design blocker below, while cleanup/recovery is still separate. Its focused Debug roots coverage recorded 3 passed cases plus 3 expected missing-environment skips / 111 assertions. Its full Debug `VFSUT` run recorded 149 / 152 cases and 45,368 / 45,371 assertions with exactly the isolated `Application marker`, `FetchUsers` and `FetchGroups` host baselines; the Debug helper built. Fresh Release ASAN and UBSAN `VFSUT` builds linked their runtimes and each recorded roots 3 plus 3 expected skips / 111, ledger 16 / 388, `LeaseStore` 7 / 363 and source snapshot/stage 9 plus one skip / 385 without diagnostics. The later lifecycle aggregate is 161 / 164 and 46,560 / 46,563 with the same host baselines. The real stage-to-barrier fixture requires `WINCOMMANDER_VFS_DESTINATION_STAGE_ROOT`, `F_FULLFSYNC` and a distinct device; it is skipped because `/`, `/private/tmp` and `/Volumes` resolve to `disk3`, `/Volumes/Macintosh HD` links to `/`, and no distinct mounted child is present. Therefore no runtime two-device `Prepare` or physical-volume proof exists.

**Current durable staged-lifecycle retention/inspection:** `StagingSessionRunner` invokes the private `StagingPublicationLifecycle::RecordStaged` before releasing its two root ledgers. The writer accepts only the current-process Commit stage, exact-validates request, roots, snapshot/stage descriptors and sealed artifacts, then writes immutable primary plus sealed lifecycle companions on both roots in canonical order. The pair binds header/correlation, both root identities, snapshot/stage IDs and seals, source/destination-parent seals and destination component; partial state stays retained. `Inspect` reacquires roots canonically and classifies only `ExactPending`, `Absent`, `Incomplete`, `Mismatched` or `Malformed`. It never calls `ProtectedRootLedger::Reconcile()`, exposes neither descriptor nor pathname, and grants no cleanup, recovery-execution, publication or selection authority. `ExactPending` requires both exact lifecycle pairs and sealed artifacts on distinct devices. Focused Debug roots/lifecycle coverage passes 15 cases plus 3 fixture skips / 1,295 assertions; full Debug `VFSUT` records 161 / 164 and 46,560 / 46,563 with only the `Application marker`, `FetchUsers` and `FetchGroups` host baselines; and the Debug helper target builds. Fresh Release ASAN and UBSAN `VFSUT` builds link their runtimes and sequentially pass roots/lifecycle 15 plus 3 fixture skips / 1,295, ledger 16 / 388, `LeaseStore` 7 / 363 and source snapshot 9 plus one fixture skip / 385 without diagnostics. A real stage → restart → `Inspect(ExactPending)` fixture remains externally blocked without two distinct mounted internal writable APFS roots.

**Publisher design blocker:** a named stage plus `renameatx_np(..., RENAME_EXCL)` cannot prove that the exact staged inode is bound to the destination name; helper-private `0700` roots, `O_NOFOLLOW`, descriptor rechecks and `flock` do not remove the same-UID substitution window. This environment has no real euid-0 signed root-isolation proof. Only a `VFSUT`-only characterization may demonstrate that window. No `StagingPublisher` API, helper authority or production selection may be introduced until a descriptor-bound namespace primitive exists, or an explicitly revised signed-root trust model has root-acquisition and transport proof. `CrossVolumeStaged` remains `Unsupported`.

**VFSUT named-stage characterization closed:** the deterministic test-owned same-euid rebind proves a `RENAME_EXCL` destination can receive the replacement inode rather than the earlier validated stage inode; its focused Debug result is 1 / 48, and the current sequential full Debug `VFSUT` result is 162 / 165 and 46,598 / 46,601 with only the three known host baselines. It does not grant a publisher or relax the design blocker.

**Remaining tests:** physical internal/external-volume execution, actual power interruption/reboot with the checkpoint manifest, pre-open journal capture and destination inspection, descriptor-bound namespace publication or a separately proven revised signed-root trust model, helper-owned cleanup/recovery execution, and broader mutation-consumer adoption.

**Exit:** every first-slice copy enters the pool only from an exact durable admission receipt, publishes its typed item/terminal outcome durably, and leaves no unbounded staging state; direct UI construction of copy operation leaves the migrated paths.

### R6 — OperationCenterModel and persistence

**Purpose:** предоставить durable queue/history/read model поверх per-window pools.

**Статус 2026-08-07:** partial — the durable model/coordinator foundation and bounded value-only Cancel consumers remain active. Q1-8 adds a separate live projection of the exact per-window Pool into Explorer, including copied current item, lifecycle, bytes/items, rate, ETA and additional-operation count. This strip carries presentation values only. The full Operation Center remains open for persistent result/error/log/history, filters and sealed pause/resume/retry.

**Changes:**

1. **Foundation done:** define opaque `OperationId` distinct from immutable `OperationPlanId`, revisioned immutable value-only `OperationRecord`, control availability and fail-closed lifecycle transitions; schema-v3 journal owns production ID reservation and persists its high-water. A process-owned coordinator preallocates the model draft before journal reservation, binds `Queued` only to the exact durable receipt, imports terminal/interrupted history after `Open`, and can append only absent terminal history from the exact reopened storage while it has no active records, drafts or residencies. Confirmed explicit Copy recovery invokes that projection synchronously after its own mutex and any required Pool release. An exact busy projection mints only a one-use storage-identity continuation; the explicit UI refresh calls only fresh-journal `RefreshColdHistory` and cannot repeat custody recovery. It retains no Journal, and holds the exact Pool/Operation only from private pre-enqueue handoff to durable terminal retirement. The private receipt-aware Orchestrator path verifies the full reviewed plan before construction/Pool admission; weak callbacks reduce Start and exact durable terminal evidence into the same record. **Next:** compose the completed per-window progress semantics with persistent typed result/error/log/history without granting raw executor authority.
2. Use Pool terminal finalizer as persistence authority: map and durably finalize the typed terminal result before returning `Release`; return `Retain` and retry on uncertainty. `NotifyAboutAddition/Removal` and lifecycle observers feed projection only.
3. Keep per-window pool as executor and aggregate records at app level, using [`AggregateProgressTracker`](../Source/Operations/source/AggregateProgressTracker.h) only as progress compatibility source.
4. Persist terminal history and interrupted markers through a versioned store. **Journal/Pool foundation done:** schema-v3 journal-owned operation IDs and receipts with v1/v2 migration and durable high-water, atomic terminal results, restart interruption, retained finalization retry, storage-bound run-receipt reconciliation and bounded CopyAs durable-outcome linkage. Bounded history and exported-diagnostics redaction remain.
5. **Bounded Explorer consumers done:** `More` reads one immutable active-only `OperationCenterModel` snapshot and renders value-only type/state/ID lines with a Cancel child item. Separately, `operationCenter.open` obtains a fresh copied all-record snapshot and opens a static panel with type/state/IDs/timestamps and Registry-gated Cancel. Q1-8 adds the live per-window Pool progress strip through its own copied model. Build the full Explorer Operation Center by composing these values with persistent history/result/error/log detail and aggregate policy; preserve the existing brief Pool view as legacy Commander compatibility during migration.
6. **Cancel route done:** revision-checked engine cancellation goes through `CommandRegistry` with `OperationId` and the record's expected revision; presentation and Registry handlers never receive a raw `Operation *`. The compact menu uses this same path and surfaces a typed rejection instead of claiming success. Add pause/resume/retry only with their own sealed engine authority.

**Tests:** pending→running→completed, pause/resume, cancel, failure, terminal capture, pool removal, multiple windows, persistence/restart classification, bounded retention and redaction; missing/reused id, stale revision, pending/running/paused/finalizing/terminal controls, duplicate/reentrant control, teardown and durable terminal capture before removal.

**Exit:** first-slice copy remains visible after completion, supports applicable controls through commands and restores terminal history after restart.

### R7 — Consolidation and expansion

**Purpose:** завершить ownership transfer and broaden contracts.

**Order:**

1. migrate remaining P0 Explorer commands;
2. route menu/context/palette/keyboard construction entirely from registry descriptors;
3. make PaneStore the write boundary for Commander panes and tabs;
4. extend OperationPlan to all mutation entry points, batch rename and sync dry-run;
5. extend the completed pane-bound SearchStore projection to provider-native remote/archive backends, and project ConnectionManager and PermissionManager state into stores;
6. remove compatibility adapters after repository-wide call-site search and aggregate regression suite.

Removal evidence for each legacy path includes zero UI call sites, passing characterization tests against the new owner, clean build and updated roadmap/changelog.

## 5. Change-set discipline

Каждый change set содержит один contract increment и один proven consumer:

| Change set | Contract | Consumer | Required proof |
|---|---|---|---|
| 1 | capabilities + error adapter | `file.copy` availability | VFS/adapter unit tests |
| 2 | PaneSnapshot + committed-state adapter | Explorer address | Panel/WinCommander tests + UI smoke |
| 2a | pure `PaneLifecycleProducer` | pure `PanelControllerLifecycle` coordination seam | producer/coordinator ordering tests; completed |
| 2b | shared controller/store `PaneId`, controller-owned coordinator, navigation and refresh worker/content-token boundaries | production navigation/refresh lifecycle events | coordinator and Store seams plus deterministic-VFS production worker E2E covered; external-loading deferred Busy passes 1 / 22, while live providers and permission remain |
| 2c | PaneStore lifecycle reducer | Navigation/refresh lifecycle and error state rendered by Explorer breadcrumb | reducer + adapter + breadcrumb integration tests plus production worker E2E; remaining consumers/live-provider evidence pending |
| 2d | exact focused-item identity in PaneState | production `PanelControllerPaneStoreAdapter` | completed: commit/cursor timing fail-closed; included in current combined Store/reducer/bridge 65 / 1,016 |
| 2e | hidden-file filter state in PaneState | Explorer View popover command presentation | completed: unloaded/loaded projection, filter-only revision, lifecycle recomposition, matching-pane fail-closed model; execution remains live |
| 2f | exact selected identities in PaneState | production `PanelControllerPaneStoreAdapter` | completed: immutable shared payload, deterministic display order, selection-generation cache, O(1) cursor-path reuse, strict reducer invariant and retained lifetime; combined 65 / 1,016 |
| 2g | unified History navigation state + runtime current-entry identity | `PaneSnapshot` production bridge | completed: identity-only middle transitions, branch/trim stability, O(1) reducer invariants and lifecycle composition; History/Store/reducer/bridge 70 / 1,473 |
| 2h | pure `ExplorerTabsModel` + ordered per-tab controller/Store/History ownership | `NCExplorerState` active binding over `FilePanelsTabbedHolder` | completed locally: exact create/activate/close/reorder, generation+`PaneId` observation gate, active chrome/QuickSearch/Quick Look/focus rebinding; focused 14 / 148 and ordinary Debug `UnitTests` build |
| 2i | strict layout/sort inverse + bounded exact-location settings schema | `NCExplorerState` active view binding and pane-local `PanelController` override | completed: schema-v1 deterministic MRU, full-location admission, restore notification fence, concrete Brief/List/Gallery layout, sort/group round-trip and Commander layout isolation; persistence 8 / 580, complete focused boundary 26 / 823, full Debug `WinCommanderUT` 459 / 8,401 plus successful Debug test/app builds |
| 2j | schema-v1 window/session envelope + canonical location/no-password restore | `MainWindowController` and `NCExplorerState` ordered runtime reconstruction | completed: legacy Commander migration, Cocoa → `StateConfig` → default precedence, future-schema preservation, fresh `PaneId` allocation, exact capture, atomic topology rollback and isolated per-tab Home fallback; codec 8 / 116, tab/session state 7 cases, no-password 2 / 14, full Debug `WinCommanderUT` 473 / 8,634 plus successful Debug builds |
| 3 | registry + legacy handler | Explorer `file.copy` and `file.cut` across production entry points | command, presentation and pasteboard integration tests |
| 3a | registry + pane-target rename initiation adapter | Explorer `file.rename` editor initiation | completed: focused 5 / 64 plus editor regression 2 / 23; pre-adapter Debug/ASAN/UBSAN snapshot 213 / 2,638 each; execution remains legacy |
| 3b | registry + pane view-state adapter + semantic checked state | `view.toggleHiddenFiles` in View menu/shortcut/Explorer popover | completed: Debug scheme build; combined focused 28 / 372 |
| 3c | `navigation.back`/`navigation.forward` Registry definitions + Store/live contexts | Go menu/shortcut and Explorer toolbar | completed: core 14 / 173, dispatcher/toolbar 2 / 102; legacy History restore lifecycle remains open |
| 3d | `navigation.up`/`navigation.refresh` Registry definitions + derived Store/live availability | Go/View menu, shortcuts and Explorer toolbar | completed locally: uniform child/provider-parent Up, forced user Refresh, soft-refresh separation, shared queue-ownership guard, descriptor-aware secondary aliases, preserved menu bounce and nonblocking detached navigation teardown; total 32 / 549: core 22 / 281, navigation 4 / 50, Refresh 3 / 37, Registry surfaces 1 / 27, Explorer 2 / 154; hosted CI pending |
| 3e | `file.open` Registry definition + exact-item/provider context | main menu/`OnOpenNatively:`, ordinary-file Enter fallback, Shift-Return and context menu | production route implemented: shared `FileOpener` handoff, Enter router boundary, one Registry owner and exactly-one dot-dot enclosing request; 24 / 267 total: core 23 / 201 (`FileOpenCommand` 8 / 102 + Registry/shortcut 15 / 99) and production 1 / 66; full Registry fixture 3 / 122; hosted CI pending |
| 3f | `file.newFolder`, `pane.selectAll`, `pane.invertSelection` Registry definitions + live pane projection | File/Edit menu, shortcuts and Explorer New popover | completed locally: exact New Folder queue/provider/path/listing recheck before established `DirectoryCreation`; visible non-parent soft-filter selection with Command-A field-editor precedence; mutation 8 / 230, Registry 17 / 189, presentation 12 / 93, selector 1 / 26, selection 1 / 15 and popover 1 / 13; `file.newFile` is closed in 3i, while hosted CI remains pending |
| 3g | `archive.create`, `file.duplicate`, `file.copyPath` Registry definitions + exact listing/provider projection | Command/File menu, shortcuts, exact context and Explorer More | completed locally: Archive Create revalidates exact listing/generation/provider around the existing sheet before `Compression`; Duplicate plans the full collision-free batch before `Copying`; Copy Path returns pasteboard submission; mutation 11 / 302, Registry 17 / 192, selector/context 1 / 39, Explorer More 1 / 38 and Compression 14 / 227 in Debug/ASAN/UBSAN; typed `archive.extract` is closed in 3i, while hosted CI remains pending |
| 3h | `file.calculateSizes`, `file.batchRename` Registry definitions + exact listing/provider projection | Command menu, shortcuts, exact context and Explorer More | completed locally: Calculate Sizes rejects unreadable/stale/busy input and commits only to the captured listing/generation; Batch Rename revalidates exact context and destination plan after its dialog before established `BatchRenaming`; mutation 15 / 442, Registry 17 / 194, selector/context 1 / 61, Explorer More 1 / 62 and the slice-local full `WinCommanderUT` baseline 368 / 6,339 in Debug/ASAN/UBSAN; signed interactive smoke and hosted CI pending |
| 3i | `file.newFile`, `archive.extract`, `file.getInfo`, `file.preview`, `view.togglePreviewPane` plus Q1-2 presentation composition | File/View menus, persisted shortcuts, exact context, Explorer New/View/More and mounted inspector | completed locally: narrow `EmptyFileCreation`, typed sealed archive acquisition and bounded manifest admission, exact read-only inspector metadata, exact Quick Look handoff and checked compare-and-set pane visibility; Q1-3 command 17 / 288, dispatcher 1 / 46, Inspector/AppKit 8 / 111, model 6 / 97, full Debug 420 / 425 and focused ASAN/UBSAN 20 / 345 in each runtime; signed interactive and hosted CI pending |
| 4 | visual mapper | loading/error/disabled states | state matrix + accessibility smoke |
| 4q | copied per-window Pool progress model + owning current-item snapshot | Explorer progress strip and established copy-conflict sheet | completed in Q1-8: progress 11 / 107 in Debug/ASAN/UBSAN, conflict 3 / 41, current-item 2 / 19 in Debug/ASAN/UBSAN, full Debug `OperationsUT` 216 / 5,726, full Debug `WinCommanderUT` 518 / 9,137 and successful Debug app build |
| 4r | pane-bound `SearchStore` + direct/Spotlight backend adapters | per-tab Explorer Search Mode and ordinary result listings | completed in Q1-9: explicit P0 scope/filter/capability/limitation states, run/pane/listing-generation fencing, nonblocking cancellation teardown and exact Reveal Original; focused Search 62 / 1,010 in Debug/ASAN/UBSAN, full Debug `WinCommanderUT` 580 / 10,147, `SearchForFiles` 9 / 68, full Debug `VFSUT` 179 / 46,786 and successful Debug app build; both sanitizer runtimes are linked without diagnostics |
| 4a | structural `OperationPlan` | Operations contract, no production consumer | completed: plan 8 / 113 |
| 4b | pure Copy `OperationPlanner` Accepted/Blocked plus narrow Move intent preflight | review/factory-readiness contract, no production Move consumer | completed: Copy Debug planner 13 / 228; narrow Move planner 5 / 69 and review rejection 1 / 5, with no execution authority |
| 4c | production VFS/application probes + bound preflight | production evidence boundary | completed: Copy adapter 5 / 178 and access checker 4 / 56; Move rename mapping 1 / 4 and application access checker 4 / 65, with no factory/journal/Pool authority |
| 4d | explicit review + compatibility `LegacyOperationFactory` | foundation-only concrete operation construction | completed: legacy factory evidence retained |
| 4e | schema-v1 `OperationPlanCodec` | durable structural plan representation | completed: codec 12 / 151 |
| 5a | typed conditional-authority gate + Native clone transaction + anchored staged capsule | fail-closed bounded publication | provider authority and internal-APFS metadata/durability complete: Native transaction 16 / 328, ProviderCapabilities 16 / 549, Native create-copy 19 / 924, Native mapper 2 / 382 |
| 5b | schema-v3 `OperationJournal` + exact admission/run/finalize authority | persistent lifecycle/item-result foundation with journal-owned durable operation IDs, high-water and tri-state publication | completed: journal 33 / 752; v1/v2 migration and public raw-ID admission removal verified |
| 5c | typed Pool finalizer + production-configured `CopyOperationOrchestrator` | engine-only single-item composition | completed: Pool 17 / 219, orchestrator 19 / 849, coordinator/orchestrator/control integration 31 / 1,060 and confirmed recovery-to-history projection 12 / 197 with a one-shot projection-only retry, restricted hooks, private pre-enqueue residency handoff, exact cancellation/reentrant rejection, exact durable outcome, preallocated terminal transition, receipt-aware no-re-admission, Start/durable-terminal reduction, cold same-storage history refresh and retry/reconcile/release custody |
| 5d | provider mapper + sealed product + private reviewed factory | lossless execution integration | completed: mapper 4 / 237, product 9 / 188, factory 8 / 225, Job 10 / 608 |
| 5e | app review/presentation + one Copy consumer | `CopyAs` mutation boundary | completed: path-aware selection, exact review, process recovery, window submission gate, item/durable-outcome presentation; production app-boundary tests 4 / 70 prove zero enqueue, exact review and durable dispatch before non-success Pool release; combined policy/app evidence 10 / 98 |
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

The historical M3 evidence remains archived in its feature documents. Current Q1-8 closure records full Debug `OperationsUT` at 216 / 5,726 and full Debug `WinCommanderUT` at 518 / 9,137. Progress model/view/controller passes 11 / 107 in Debug, Release ASAN and Release UBSAN; current-item publication passes 2 / 19 in those three configurations; the sanitizer executables link their expected clang runtime and report no diagnostics. Conflict UI passes 3 / 41 and Debug `WinCommander-Unsigned` builds successfully. Recorded M0 remains 897 / 132,011 and Docker-backed ASAN integration remains 163 / 89,392. Hosted CI publication, signed operation walkthrough and required-check enforcement remain separate work.

The historical destination-stage, protected-root PID/locking, root-admission, locked-session runner and private publication-barrier `VFSUT` result superseded older cross-volume counts at that slice: 149 / 152 cases and 45,368 / 45,371 assertions in Debug, with exactly the three known host baselines. The later lifecycle aggregate is 161 / 164 and 46,560 / 46,563 with the same host baselines. The two-device barrier fixture is skipped without a distinct mounted device; publisher design, cleanup and physical-volume gates are not part of either result.

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
