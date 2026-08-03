# План разработки Win Commander

> Статус: активный roadmap
> Актуализирован: 2026-08-03
> Базовая ревизия: `ec7f9f4`
> Каноническая продуктовая спецификация: [`win_commander_ideal_file_manager_spec.md`](win_commander_ideal_file_manager_spec.md)

## 1. Роль roadmap

Продуктовая спецификация задаёт целевое поведение, архитектурные инварианты, Definition of Done, test matrix и release gates. Этот roadmap задаёт порядок реализации, зависимости, текущий статус и доказательства готовности.

Полномочия источников разделены по назначению:

1. `win_commander_ideal_file_manager_spec.md` определяет целевой продукт и критерии качества;
2. `Development-Plan.md` определяет последовательность работ и статус;
3. код и исполняемые тесты определяют подтверждённое текущее поведение;
4. feature-spec и ADR фиксируют границы и решения конкретной задачи.

`WindowsUI-Redesign-Plan.md` и `WindowsUI-Redesign-Design.md` сохраняют исторический контекст. Новые задачи планируются по этому roadmap.

Статусы:

- **done** — acceptance criteria приняты и подтверждены evidence;
- **partial** — есть работающая часть, exit criteria закрыты не полностью;
- **not started** — целевой контракт фазы ещё не реализован.

Приоритеты: **P0** — следующий release gate; **P1** — Beta/1.0 после устойчивого local core; **P2** — post-1.0 expert layer.

## 2. Текущий baseline

| Область | Статус | Фактическое состояние |
|---|---|---|
| Бренд и targets | **done** | Win Commander и bundle identifiers настроены; есть схемы Unsigned, NonMAS и MAS. |
| Базовые библиотеки | **done** | `Base`, `Utility`, `Config`, `CUI`, `Panel`, `VFS`, `VFSIcon`, `Operations`, `Viewer`, `Term` разделены на проекты и test targets. |
| VFS | **partial** | Работают local, archive, FTP, SFTP, WebDAV, xattr и process providers. Conservative `ProviderCapabilities` resolver получает явные declarations от NativeHost, ArcLA, ArcLARaw, FTP, SFTP, WebDAV, XAttrHost и PSHost. `VFSHost` теперь предоставляет authoritative case/namespace identity seams, typed host-error classification и symlink capability; Native и SFTP имеют нужные production specializations. Raw-archive path semantics и selection-sensitive application composition ещё не закрыты. |
| Operations | **partial** | Structural plan/preflight/review, private provider authority, schema-v1 plan codec, schema-v3 journal-owned ID reservations/high-water, tri-state publication and hardened `Job`/`Operation`/`Pool` lifecycle реализованы. Native provider supports one create-only regular-file Copy on the exact same internal/local/writable APFS volume with anchored metadata seals, exclusive clone publication, destination verification and ordered durability. Provider results map losslessly into the journal; a sealed move-only execution product owns commit/stop/destruction authority. The production-configured `CopyOperationOrchestrator` reaches the private reviewed factory, configures restricted cold-operation hooks, custodies the exact run receipt, delivers an owning exact durable outcome, and retains Pool residency until durable finalization or exact reopen/release reconciliation. Pool preallocates terminal-transition authority and separates generic success reporting through `ReleaseWithoutCompletion`. A bounded production `CopyAs` consumer now shows the exact bound plan, revalidates pane/item intent, uses a process-owned `OperationCenterCoordinator`, and submits only explicit path-specific `Supported` Native eligibility. The coordinator publishes `Queued` from its exact receipt, appends only absent terminal history from the same cold reopened storage, registers preallocated exact live Pool/Operation residency before enqueue, reduces Start and durable terminal outcomes through weak callbacks, and exposes revision-checked engine cancellation without raw executor access. Confirmed explicit Copy recovery projects that history separately from custody; Registry/UI presentation, physical-volume evidence, cross-volume staging and non-Copy preflight remain open. |
| Explorer shell | **partial** | `NCExplorerState` встроен в window state machine; работают sidebar, toolbar, breadcrumb/address editor, quick search и command bar. |
| Навигация | **partial** | Sidebar использует favorites, volumes, connections и tags; breadcrumb поддерживает сегменты, ввод пути и `Command-L`. `PanelController`, его lifecycle coordinator и production Store bridge используют один factory-injected `PaneId`; breadcrumb читает immutable snapshot. Navigation и refresh проходят через stable request identity, explicit worker ownership и exactly-one terminal outcome. Store публикует exact focused item только после cursor restoration; refresh сохраняет location generation, публикует новую listing identity и coalesces запросы как running + latest pending. Остальные consumers и live-provider/permission integration остаются в работе. |
| File views | **partial** | Используются list/brief/gallery presentation; доступны layout, sort, grouping и hidden items. `PaneStore` публикует hidden-file hard-filter, semantic sort/group state, actual view mode и valid layout slot даже до загрузки listing. Explorer View/Sort popovers читают matching Store snapshot для hidden, sort, group и layout markers. Pure `VisualStateMapper` задаёт baseline pane/command composition; history и остальные view/item/operation render adapters ещё не подключены. |
| Команды | **partial** | Добавлены pure C++ command contracts, registry и shortcut binding. При одиннадцати stable IDs девять app-owned production definitions маршрутизируют `file.copy`, `file.cut`, initiation `file.rename`, `file.open`, `view.toggleHiddenFiles`, `navigation.back`, `navigation.forward`, `navigation.up` и `navigation.refresh` через Registry и общий `CommandPresentationAdapter`. Explorer подаёт matching Store snapshots в Registry presentation для hidden-files и навигационной toolbar; menu/shortcut execution использует live pane context. Остальные P0-команды ещё используют migration paths. |
| Cut state | **done** | Pasteboard cut token, визуальное состояние и отмена через Escape реализованы и имеют unit coverage. |
| Status/preview | **partial** | Explorer footer через `PanelView` получает count, selection count и selected bytes только из matching `PaneStore` snapshot; отдельные `NCExplorerStatusBarView` и inspector placeholder не используются, полноценного preview/details pane нет. |
| Tabs/multi-pane | **partial** | Legacy file panels поддерживают tabs и dual-pane; Explorer state сейчас однопанельный. |
| Search | **partial** | Есть quick search, Find Files и Spotlight actions; нет общего Search State с явным scope и backend limitations. |
| Tests/CI | **partial** | Recorded local M0 gate прошёл unsigned Debug build и 10/10 aggregate binaries: 897 / 132 011. The current focused CopyAs policy and app-boundary suites pass 10 / 98 and 4 / 70; the latter also passes Release ASAN and UBSAN. Explorer footer Store-state coverage passes 2 / 18. Fresh isolated Debug `OperationsUT` has 195 / 196 passing cases and 5,270 / 5,274 passing assertions; the single set-ID metadata failure is the existing NativeCreateCopy host baseline. Focused Operations evidence: schema-v3 journal 33 / 752, pure OperationCenterModel 4 / 64, receipt-bound `OperationCenterCoordinator` with cold exact-storage history refresh 15 / 284, coordinator/orchestrator/control integration 31 / 1,060, and confirmed recovery-to-history projection 8 / 107 with fresh Release ASAN and UBSAN, Native create-copy 19 / 924, Native outcome mapper 2 / 382, provider journal mapper 4 / 237, product 9 / 188, factory 8 / 225, Job 10 / 608, probes 5 / 178, orchestrator 19 / 849 (production 3 / 138), Pool 17 / 219. ProviderCapabilities and Native conditional Copy pass 16 / 549 and 16 / 328; fresh full Debug `VFSUT` passes 95 / 43 556. Focused Docker FTP/SFTP/WebDAV `WinCommanderIT` app-boundary coverage passes 4 / 178, including a WebDAV endpoint outage, typed `NetworkError`, committed-listing retention and reconnect through the same user Refresh route; a production deferred external-loading Busy case passes 1 / 22. Aggregate ASAN/UBSAN reruns stop at the same pasteboard baseline after BaseUT and ConfigUT; no sanitizer diagnostic preceded it. Seeded Docker-backed ASAN integration passes 163 / 89 392. Hosted CI remains open. |

Subsequent focused evidence adds the pure `OperationCenterModel` foundation at 4 / 64, schema-v3 `OperationJournal` allocation/migration at 33 / 752, and receipt-bound `OperationCenterCoordinator` hydration/admission/cold exact-storage refresh at 15 / 284. Coordinator/orchestrator/control integration passes 31 / 1,060, including `Queued` before Pool addition, Start reduction, durable terminal reduction, sealed pre-enqueue residency, stale-revision rejection, exact cancellation, reentrant cancellation rejection, a failed handoff without Pool side effects and cold refresh rejection while stage/commit/liveness is present; the same subset passes Release ASAN and UBSAN. The explicit confirmed recovery-to-history bridge passes 8 / 107 in Debug, Release ASAN and Release UBSAN. Fresh isolated Debug `OperationsUT` is 195 / 196 and 5,270 / 5,274, with the same NativeCreateCopy set-ID metadata host baseline.

Current-tree M0 2026-08-01: `Scripts/verify_m0.sh` собрал unsigned Debug application и выполнил все десять seeded aggregate unit-test targets — 897 cases / 132 011 assertions в recorded run. Its historical `OperationsUT` snapshot passed 170 / 4 748 in Debug and explicitly instrumented Release ASAN/UBSAN; the current coordinator/control subset separately passes fresh Release ASAN and UBSAN at 31 / 1,060. Seeded Docker-backed integration ASAN passed 163 / 89 392; the first hosted CI run remains open evidence.

Целевые контракты вводятся адаптерами поверх зрелых модулей:

| Контракт спецификации | Основа в коде |
|---|---|
| `FileSystemProvider` + `ProviderCapabilities` | `Source/VFS`, `VFSHost`, `VFSOperationPlanningBindings`, `VFSOperationPlanningProbes` и `Core/VFSOperationPlanningAccessChecker` |
| `PaneStore` | `Core/Pane/PaneSnapshot`, `PaneStoreAdapter`; `PanelController`, `PanelData`, panel persistency |
| `Command Registry` | `PanelControllerActionsDispatcher`, menu actions, `ActionsShortcutsManager` |
| `Operation Engine` | structural `OperationPlan`, codec, copy-first `OperationPlanner`, reviewed bound preflight, provider conditional transaction, result mapper, sealed execution product, durable `OperationJournal`, private reviewed factory path, production-configured `CopyOperationOrchestrator`, `Operation`, `Job`, `Pool` in `Source/Operations` |
| Visual State System | panel presentation, notifications, operation statistics |
| Error Model | `Core/Errors/FileManagerError`, `FileManagerErrorAdapter`; `nc::Error`, VFS errors, operation dialogs |

Технический принцип: ввести контракт и адаптер, перевести на них один сквозной сценарий, подтвердить seam тестами, затем мигрировать остальные сценарии.

## 3. Карта milestones

| ID | Milestone | Spec phase | Priority | Status | Depends on | Gate |
|---|---|---:|---:|---|---|---|
| M0 | Baseline и архитектурные границы | 0 | P0 | **partial** | — | — |
| M1 | Source of Truth и Command foundation | 1 | P0 | **partial** | M0 | Alpha |
| M2 | Local Explorer vertical slice | 1 | P0 | **partial** | M1 | Alpha |
| M3 | Безопасный Operation lifecycle | 2 | P0 | **partial** | M1, M2 | Alpha |
| M4 | Search и ежедневные workflow | 2 | P0 | **partial** | M1–M3 | Beta |
| M5 | Power-user workflow | 3 | P1 | **partial** | M1–M4 | Beta |
| M6 | Archives и remote workflow | 4 | P1 | **partial** | M3–M5 | 1.0 |
| M7 | Надёжность и выпуск 1.0 | cross-cutting | P0 | **partial** | M1–M6 | 1.0 |
| M8 | Expert layer | 5 | P2 | **not started** | M7 | post-1.0 |

## 4. Milestones и exit criteria

### M0 — Baseline и архитектурные границы

**Работы:** оформить architecture audit, gap matrix, risks и refactor boundaries; закрепить владельцев app/pane/folder/selection/search/operation state; сопоставить actions, VFS capabilities и errors с целевыми контрактами; добавить CI для unsigned build и unit tests; описать local vertical slice.

**Acceptance:**

- у каждого authoritative system из раздела 7 спецификации указаны модуль, API boundary и владелец состояния;
- каждое P0-требование в gap matrix связано с milestone;
- unsigned build и aggregate unit tests запускаются одной командой локально и в CI;
- vertical slice описан feature-spec по разделу 39 спецификации;
- есть baseline для папок на 10 000 и 100 000 элементов.

**Evidence:** ADR/architecture docs, CI run, performance report и feature-spec.

**Текущее evidence (2026-08-02):**

- [`current_architecture_audit.md`](current_architecture_audit.md) — runtime topology, state owners, adapters и verification seams;
- [`M0-Acceptance-Evidence.md`](M0-Acceptance-Evidence.md) — requirement-by-requirement аудит: четыре локальных критерия закрыты, hosted execution остаётся единственным M0 closure gate;
- [`refactor_plan.md`](refactor_plan.md) — последовательность R0–R7 и boundaries миграции;
- [`feature_gap_matrix.md`](feature_gap_matrix.md) и [`implementation_risks.md`](implementation_risks.md) — полное P0 coverage и risk gates;
- [`Features/local_explorer_vertical_slice.md`](Features/local_explorer_vertical_slice.md) — первый local browse-and-copy slice;
- [`Performance/large_folder_baseline_2026-08-01.md`](Performance/large_folder_baseline_2026-08-01.md) — model load/natural-sort baseline 10k/100k; UI first-render/main-thread/memory evidence остаётся в M2/M7;
- `Scripts/verify_m0.sh` — current-tree local M0 с unsigned build и 10/10 unit targets: 897 cases / 132 011 assertions в финальном recorded seeded run. Entry point требует Xcode 26.5, а aggregate runner fail closed проверяет присутствие всех десяти baseline products до сборки. `.github/workflows/m0-verification.yml` использует тот же entry point; первый hosted run ожидается.
- `Base::SerialQueue` сохраняет корректный `Length` при failure построения callable, task exception и observer exception; cleanup выполняет decrement до освобождения context, а штатная dispatch policy логирует и поглощает callback exceptions. Focused coverage входит в локально пройденный `BaseUT`: 78 cases / 70,566 assertions.

### M1 — Source of Truth и Command foundation

**Работы:** ввести `Command`, `CommandContext`, `CommandState`, `CommandRegistry`; адаптировать shortcut storage; построить observable `PaneStore`; добавить `ProviderCapabilities`, базовый `FileManagerError` и Visual State composition; перевести P0-команды Explorer на registry через существующий dispatcher. Pure pane lifecycle producer/reducer и app-layer coordinator готовы; `PanelController` владеет coordinator для своего `PaneId`, navigation и refresh подключены к production lifecycle, Store bridge редуцирует их events и публикует exact focus/selection identities, hidden-file filter, semantic sort/group state, view mode/layout slot, Back/Forward availability и runtime current History entry identity. Explorer breadcrumb рендерит activity/error projection, View/Sort popovers и toolbar получают presentation state из matching snapshot. Production Registry routes готовы для `file.copy`, `file.cut`, initiation `file.rename`, `file.open`, `view.toggleHiddenFiles`, `navigation.back`, `navigation.forward`, explicit `navigation.up` и forced-user `navigation.refresh`. Следующие M1 increments переводят оставшиеся P0-команды и contexts; M3 now has one bounded `CopyAs` mutation consumer, while physical-volume, cross-volume and broader-consumer gates remain separate.

**Acceptance:**

- одно действие имеет один command id и одинаковую validation во всех UI entry points;
- disabled command возвращает локализуемую причину;
- toolbar, menu, context menu и shortcut исполняют P0-команды через registry;
- `PaneStore` является источником navigation, listing, selection и view state;
- каждый принятый navigation/refresh/load request имеет stable per-pane identity и ровно один main-queue terminal outcome; success публикуется после model commit;
- capability-dependent actions корректны для local, archive и remote hosts;
- registry, stores и adapters покрыты unit tests.

**Evidence:** unit tests, integration test одной команды через все entry points, ADR mapping старого dispatcher.

**Текущее evidence (2026-08-02):**

- `Source/VFS/include/VFS/ProviderCapabilities.h` и resolver дают conservative path-aware projection для NativeHost, ArcLA, ArcLARaw, FTP, SFTP, WebDAV, XAttrHost и PSHost. `VFSHost` классифицирует POSIX failures, SFTP дополняет provider-domain classification; authoritative case semantics, semantic namespace identity и symlink creation/read capability поступают в preflight fail closed. Native exposes a tri-state path-specific conditional-Copy support probe using the same volume policy as execution. Move-only reviewed authority privately retains the consumed exact preflight and issues immutable Copy claims once. Native conditional transaction owns source/destination-parent descriptors, admits exact same internal/local/writable APFS volume, seals supported metadata, publishes through one `fclonefileat(..., CLONE_ACL)`, verifies the destination and executes ordered durability barriers; other scopes remain `Unsupported`. Current focused ProviderCapabilities passes 16 / 549, Native conditional Copy 16 / 328, combined Debug selection 32 / 877; the latest full Debug `VFSUT` run passes 95 / 43 566, and seeded Docker-backed VFS/VFSIcon ASAN integration remains 58 / 87 975;
- `Source/WinCommander/WinCommander/Core/Commands/` содержит pure C++ command contracts, одиннадцать stable IDs, registry и read-only legacy shortcut binding adapter с explicit ambiguity. Девять IDs имеют app-owned production definitions. Focused Back/Forward Registry run прошёл 14 / 173, Up/Refresh core command/availability/identity/alias run — 22 / 281, а `file.open` core/Registry/shortcut run — 23 / 201;
- [`Features/file_copy_command_registry_slice.md`](Features/file_copy_command_registry_slice.md) фиксирует первый production command slice: `file.copy` имеет один typed synchronous context и один pasteboard writer для command bar, menu, shortcut и context menu; native-only eligibility исключает partial mixed-provider writes. Focused suite прошёл 6 cases / 58 assertions. `file.cut` является вторым app-owned Registry/Visual State consumer; writer failure превращается в typed `FileCutWriteError`, и registry не сообщает `Executed`. Focused `file.cut` suite прошёл 8 / 73. Production surfaces используют общий `CommandPresentationAdapter`; focused presentation suite прошёл 10 / 69;
- [`Features/file_rename_command_registry_slice.md`](Features/file_rename_command_registry_slice.md) фиксирует третий app-owned command slice: stable `file.rename` принимает ровно один non-dotdot item, проверяет path-aware `ProviderCapabilities::can_rename` и live borrowed pane target, затем синхронно re-resolves item, фокусирует его и открывает inline editor. Menu/persisted shortcut, Explorer command bar, context menu и direct field-editor mouse entry сходятся в одной Registry definition; false editor start превращается в `FileRenameInitiationError`. Focused suite прошёл 5 cases / 64 assertions. Commit нового имени остаётся на `PanelController::requestQuickRenamingOfItem` → `nc::ops::Copying(docopy = false)` до миграции через `OperationPlan`;
- [`Features/view_toggle_hidden_files_command_registry_slice.md`](Features/view_toggle_hidden_files_command_registry_slice.md) фиксирует четвёртый app-owned command slice: stable `view.toggleHiddenFiles` проецирует `On`/`Off`, локализованные missing-target/state reasons и exactly-once setter в View menu, persisted shortcut и Explorer View popover. Selector execution больше не вызывает legacy action напрямую. Popover presentation получает `shows_hidden_files` из matching `PaneSnapshot`; отсутствие Store state и foreign pane identity fail closed, а execution остаётся live. Debug `WinCommanderUT` scheme build прошёл; объединённый focused command/registry/mapper/presentation run — 28 cases / 372 assertions;
- [`Features/operation_plan_foundation.md`](Features/operation_plan_foundation.md), [`Features/copy_preflight_planner_foundation.md`](Features/copy_preflight_planner_foundation.md), [`Features/vfs_operation_planning_probes_foundation.md`](Features/vfs_operation_planning_probes_foundation.md), [`Features/reviewed_copy_factory_foundation.md`](Features/reviewed_copy_factory_foundation.md), [`Features/provider_conditional_copy_execution_product.md`](Features/provider_conditional_copy_execution_product.md), [`Features/operation_plan_codec_foundation.md`](Features/operation_plan_codec_foundation.md), [`Features/native_create_copy_execution_foundation.md`](Features/native_create_copy_execution_foundation.md), [`Features/operation_journal_foundation.md`](Features/operation_journal_foundation.md), [`Features/operation_center_model_and_control_projection.md`](Features/operation_center_model_and_control_projection.md), [`Features/copy_operation_submission_hooks.md`](Features/copy_operation_submission_hooks.md) и [`Features/reviewed_copy_as_production_consumer.md`](Features/reviewed_copy_as_production_consumer.md) фиксируют M3 chain. Exact bindings reach a provider-owned internal-APFS clone transaction; the provider result maps losslessly into a transaction-backed Job and exact journal terminal evidence. `CopyOperationRunReceiptCustodian` owns retry/reconcile authority, including `ReleaseReconciled` Pool handoff. The production-configured orchestrator calls the private reviewed factory, installs restricted cold hooks and delivers the exact durable outcome; its injected seam is test-only. The bounded `CopyAs` consumer adds path-aware eligibility, exact user review, stale-intent gates, process recovery, window submission custody and typed UI outcomes. The process-owned coordinator preallocates a model draft, binds `Queued` to the exact journal receipt, appends only absent terminal history from the same cold reopened storage, registers sealed live residency before enqueue, and reduces Start plus durable terminal outcomes through weak callbacks. Confirmed explicit recovery projects that history separately from custody. Debug evidence: factory 8 / 225, probes 5 / 178, Native create-copy 19 / 924, Native mapper 2 / 382, provider mapper 4 / 237, product 9 / 188, schema-v3 journal 33 / 752, pure OperationCenterModel 4 / 64, coordinator 15 / 284, coordinator/orchestrator/control integration 31 / 1,060, recovery-to-history projection 8 / 107 in Debug, Release ASAN and Release UBSAN, Pool 17 / 219, ProviderCapabilities 16 / 549, Native conditional Copy 16 / 328, CopyAs policy/gate 6 / 28 and recovery coordinator 6 / 67; fresh isolated Debug `OperationsUT` is 195 / 196 and 5,270 / 5,274 with the existing set-ID metadata host baseline;
- [`Features/pane_store_explorer_read_projection.md`](Features/pane_store_explorer_read_projection.md) фиксирует production read/lifecycle slice; `PanelControllerPaneStoreAdapter` проецирует exact focus, immutable shared selected identities в display order, hidden/filter/sort/group/view state, Back/Forward availability and runtime current History entry ID. `SelectionProjectionGeneration()` ограничивает O(N) materialization изменениями membership/order/listing, cursor-only projection переиспользует payload за O(1), а reducer проверяет new payload на exact listing/range/count/uniqueness и использует trusted-identity fast path. Sort/group/view mapper сохраняет legacy ordering semantics, derives effective grouping and separates configured slot identity from actual presentation/fallback; malformed enum/combination fail closed за O(1). Presentation/History-only changes продвигают `revision`, но не `listing_generation`; lifecycle overlays сохраняют updated committed state. Explorer presentation принимает только matching snapshot. Focused Store suite прошёл 18 / 220, reducer 19 / 335, production bridge 28 / 461; combined 65 / 1 016. Panel generation contract прошёл 1 / 42, Explorer matching-pane presentation/toolbar baseline — 2 / 266. Grouping persistence остаётся M2 gap;
- [`Features/pane_history_availability_projection.md`](Features/pane_history_availability_projection.md) и [`Features/pane_history_current_entry_identity.md`](Features/pane_history_current_entry_identity.md) фиксируют runtime History projection: unified navigation-state callback exactly-once сообщает availability или current-ID changes, bridge публикует их для unloaded/loaded and lifecycle overlays, а Explorer принимает availability только из matching snapshot. Current Store/reducer/bridge прошли 65 / 1 016; dedicated History — 5 / 457; combined 70 / 1 473. Matching-pane model/toolbar baseline прошёл 2 / 266. Lifecycle-correlated restore и persistence остаются отдельной работой;
- [`Features/navigation_history_command_registry_slice.md`](Features/navigation_history_command_registry_slice.md) фиксирует Registry route для stable `navigation.back`/`navigation.forward`: menu, persisted shortcut, Explorer toolbar and programmatic selectors share typed state, localized reasons and one live execution port. Store-backed toolbar presentation и live menu state проходят одну definition; legacy action-map registrations удалены. Core Registry evidence — 14 / 173; dispatcher/toolbar evidence — 2 / 102; arm64 Debug build прошёл. `Executed` пока означает cursor move + submission в legacy `ListingPromiseLoader`, а не lifecycle listing commit;
- [`Features/pane_navigation_up_refresh_command_registry.md`](Features/pane_navigation_up_refresh_command_registry.md) фиксирует Registry route для explicit `navigation.up` и forced-user `navigation.refresh`. Matching-`PaneId` Store snapshots определяют Explorer toolbar presentation, а menu/shortcut/selector execution повторно читает live controller availability через общий с admission queue-ownership helper. Up submits только uniform child/provider-parent navigation; non-uniform dot-dot/Enter сохраняет compatibility Back fallback как отдельную legacy navigation boundary. Secondary Up shortcut сохраняет menu bounce, а command-aware descriptor aliases классифицируют bounced invocation как Shortcut. Registry Refresh выставляет `initiated_by_user` и `F_ForceRefresh`, тогда как automatic soft refresh остаётся вне Registry. Lifecycle `Accepted`/`Deferred` означает submitted request, а не committed listing. Итог slice — 32 / 549: core 22 / 281, production navigation 4 / 50, production Refresh 3 / 37, Registry surfaces 1 / 27, Explorer model/toolbar 2 / 154. Explicit-Up non-cooperative teardown прошёл 1 / 12; broader production navigation/refresh prefixes — 8 / 103 и 16 / 154. Additional live-local `NativeHost` navigation plus forced refresh passes 1 / 30 against a real temporary directory. Hosted CI для slice не запускался;
- [`Features/file_open_command_registry_slice.md`](Features/file_open_command_registry_slice.md) фиксирует stable `file.open` как девятую app-owned production Registry definition при одиннадцати stable IDs. `OnOpenNatively:` и main-menu Open, ordinary-file fallback of Enter, explicit Shortcut/Shift-Return и context-menu exact-item payload используют один typed context и одну availability policy. Один regular file допускается на provider с `Read`; batch состоит только из regular files одного exact provider instance и требует `Read` для всех items; native directory/special item допускает handoff текущему workspace path, а dot-dot и remote directory/special item fail closed с typed локализуемой причиной. Enter остаётся router для folder/archive navigation и terminal executable. Executor синхронно передаёт exact items в общий `FileOpener`; `Executed` означает submitted handoff, а не подтверждённое открытие Launch Services или remote opener. Legacy duplicate `OnOpenNatively:` удалён из action map. Dot-dot route теперь возвращается после единственного enclosing-folder request. Итог focused slice — 24 / 267: `FileOpenCommand` 8 / 102, Registry/shortcut 15 / 99 и production menu/Enter/context-menu/Shift-Return route 1 / 66; полный production Registry fixture прошёл 3 / 122. Explicit Release ASAN для core command и production route прошёл 9 / 168. Hosted CI не запускался;
- `Source/WinCommander/WinCommander/Core/Errors/` содержит полную taxonomy и conservative lossless POSIX mappings для permission/path/read-only/unsupported/cancel/conflict/space/busy/timeout/network/authentication при flow-owned recovery policy; focused suite прошёл 6 cases / 519 assertions;
- [`Features/visual_state_mapper_baseline.md`](Features/visual_state_mapper_baseline.md) и `Core/VisualState` фиксируют pure pane/command projection с explicit priorities для unavailable/loading/refreshing/empty/error/permission/disabled; mapper также сохраняет semantic `Off`/`On`/`Mixed`, а AppKit adapter проецирует его в menu/button state. `file.copy`, `file.cut`, initiation `file.rename`, `file.open`, `view.toggleHiddenFiles`, `navigation.back`, `navigation.forward`, `navigation.up` и `navigation.refresh` имеют production command renderers, а Explorer breadcrumb отображает reduced navigation/refresh activity/error state, retained content и accessibility message. Up/Refresh toolbar state выводится только из matching pane projection и fail closed для missing/foreign state. Prior combined command evidence — 28 / 372; Back/Forward core/UI — 16 / 275; `file.open` focused production route прошёл 1 / 66. Остальные pane/item/operation render adapters и P0 consumers остаются следующими increments;
- `Source/WinCommander/WinCommander/Core/Pane/PaneLifecycleProducer.{h,cpp}` содержит pure main-queue sequencer со stable pane/request identity, explicit acceptance/rejection, owned typed admission diagnostics, ordered terminal events, supersession, shutdown, reentrant FIFO delivery и transactional finish. Linearizable `Subscribe` объединяет live observation с active seed или retained-failure replay/checkpoint. Финальный Debug suite прошёл 24 cases / 267 assertions;
- `Source/WinCommander/WinCommander/Core/Pane/PanelControllerLifecycle.{h,cpp}` содержит pure app-layer coordinator: fresh admission probe, navigation/refresh supersession matrix, deferred reentrant admission с exactly-once resolution feedback, request-aware scheduler, transactional post-model-commit publication, stale-commit suppression и retryable callback-safe shutdown. Admission-probe exception даёт `Rejected(Unavailable)` с owned `admission_error`, scheduler/commit exception даёт `Failed`. Финальный Debug suite прошёл 21 cases / 211 assertions;
- Common factory выдаёт каждому `PanelController` immutable-for-lifetime `PaneId`, Store bridge использует ту же identity, а controller владеет `PanelControllerLifecycle`. `GoToDirWithContext` проводит navigation через explicit `NavigationWorkerSlot`; asynchronous work захватывает shared loading `SerialQueue`, performs detached provider fetch and returns to main through weak controller callbacks. Dealloc stops the queue without `Wait`, so a non-cooperative provider cannot retain or block the controller. `refreshPanelDiscardingCaches` допускает refresh только через `SubmitRefresh`, захватывает immutable source listing/location/provider snapshot и использует один running worker плюс один latest pending intent. Uniform listing загружается через provider, temporary listing обновляется через `ProduceUpdatedTemporaryPanelListing`; prepared model reload предшествует transactional commit. Exact request/content/source/generation/host/path gates подавляют stale completion, а terminal `Committed`, typed `Failed` или `Cancelled` публикуется ровно один раз. После invalid-location failure запускается ordinary asynchronous navigation recovery к доступному ancestor или native home. Broader production navigation/refresh prefixes прошли 8 / 103 и 16 / 154; real temporary-directory `NativeHost` navigation plus forced refresh passes 1 / 30, a `0000` Native directory proves `EACCES` → typed permission failure at 1 / 22, and Docker FTP/SFTP/WebDAV navigation, shadow-host mutation, WebDAV endpoint outage and reconnect passes 4 / 178. FTP and WebDAV intentionally supersede a successful user refresh with a correlated soft-refresh successor under the existing latest-wins contract; SFTP commits the direct user refresh without provider observation/cache. The WebDAV outage emits `Failed(POSIX/ECONNREFUSED, NetworkError)` while retaining the exact committed listing and location generation; a restarted endpoint reconnects through the next user Refresh. A Started observer's external loading task causes the deferred successor navigation to resolve once as `Rejected(Busy)` without fetch or model commit (1 / 22).
- Общий content-intent epoch отменяет running/pending refresh и late navigation/recovery/legacy commits; navigation invalidation also cancels the worker callback/fetch token. Navigation supersedes refresh; ESC отменяет active refresh до появления delayed spinner. Loading/reload queues retain their detached worker resources, while both navigation and refresh cross the main callback boundary through weak controller boxes. Teardown remains main-thread nonblocking when a provider ignores cancellation. Persistency fallback/recovery callbacks also capture the panel weakly;
- Focused rename editor regression прошёл 2 cases / 23 assertions;
- Текущий Debug `WinCommanderUT` прошёл 217 cases / 2 694 assertions; application access checker — 4 / 56. Explicitly instrumented current-tree Release ASAN и UBSAN `WinCommanderUT` прошли 217 / 2 694 без diagnostics;
- Explorer breadcrumb проецирует `PaneState::visible_error` через `VisualStateMapper` в localized persistent error label и accessibility value/help, сохраняет committed address/content во время lifecycle phases, блокирует address editor при loading и не показывает cancellation/informational outcomes как blocking errors.

### M2 — Local Explorer vertical slice

**Работы:** count, selection count и selected size в используемом Explorer footer уже переведены на `PaneStore`; подключить preview/details и tabs; завершить toolbar overflow и context menu через registry; реализовать remaining loading/empty/error/permission states; стабилизировать list/details/icons/content и density; восстановить window, tabs, locations и view settings; измерить большие папки.

**Acceptance:**

- local navigation, history, breadcrumb, views и tabs образуют цельный workflow;
- path приходит из `PaneStore`; count, selection count и selected size в Explorer footer уже приходят из matching snapshot;
- preview/details поддерживает loading и error states;
- create, copy, move, rename и trash доступны мышью и клавиатурой через registry;
- папки на 10 000 и 100 000 элементов сохраняют интерактивный UI;
- session state восстанавливается после restart;
- keyboard focus и accessibility labels проходят VoiceOver walkthrough.

**Evidence:** store/persistence/UI tests, performance report, light/dark screenshots основных состояний.

### M3 — Безопасный Operation lifecycle

**Работы:** plan/preflight/review, provider authority, codec, exact journal receipts, tri-state publication, clone-only Native transaction, metadata parity and ordered durability are implemented. The lossless provider mapper and sealed transaction-backed Job join commit results to exact journal evidence. `CopyOperationRunReceiptCustodian` owns the bounded post-Running slot, exact retry, storage-bound read-only reconciliation and explicit Pool release handshake. The production-configured `CopyOperationOrchestrator` reaches the private reviewed factory, validates and installs restricted hooks while cold, and delivers an owning exact durable outcome before release. Pool preallocates the accepted terminal transition; `ReleaseWithoutCompletion` preserves removal and pending-work progress for durable non-success without publishing generic completion. The first bounded production `CopyAs` consumer now performs path-aware eligibility, exact bound-plan review, stale-intent suppression, durable submission, typed outcome presentation and explicit recovery. The physical-volume fixture is implemented but its physical/power-loss evidence and cross-volume staging remain open.

**Acceptance:**

- destructive action показывает точный plan до запуска;
- долгие операции видны и поддерживают применимые pause/resume/cancel;
- conflict policy имеет явный scope;
- partial success, cancel и failure сохраняют item-level result и recovery action;
- restart восстанавливает историю и классифицирует interrupted operations;
- P0-сценарии из test matrix 42.2 автоматизированы.

**Текущее evidence:** schema-v3 journal 33 / 752, pure OperationCenterModel 4 / 64, receipt-bound coordinator including cold exact-storage history refresh 15 / 284, coordinator/orchestrator/control integration 31 / 1,060, explicit recovery-to-history projection 8 / 107, Native create-copy 19 / 924, Native mapper 2 / 382, provider journal mapper 4 / 237, product 9 / 188, factory 8 / 225, Job 10 / 608, probes 5 / 178, orchestrator 19 / 849 with production subset 3 / 138, Pool 17 / 219, ProviderCapabilities 16 / 549 and Native conditional Copy 16 / 328. The production `ReviewedCopyAsApplicationBoundary` proves blocked, stale, unpersisted and cancelled zero-enqueue paths, exact plan projection, and owning durable failure dispatch before Pool removal with generic completion suppressed: 4 / 70; the combined policy/app suite is 10 / 98. That app-boundary filter also passes explicitly instrumented Release ASAN and UBSAN without diagnostics. Fresh isolated Debug `OperationsUT` is 195 / 196 and 5,270 / 5,274, with one unrelated NativeCreateCopy set-ID metadata host baseline. The coordinator/orchestrator/control and recovery-projection filters also pass Release ASAN and UBSAN without diagnostics. Recorded M0 remains 897 / 132 011; seeded Docker-backed integration ASAN passes 163 / 89 392.

The pure `OperationCenterModel` foundation adds 4 / 64: opaque ID contract, value-only records, stale revision, finalization ordering and immutable snapshots; production ID allocation is no longer public on the model. Schema-v3 journal allocation/migration adds 33 / 752: journal-owned reservations, durable high-water persistence, exact receipt propagation, strict decode and deterministic v1/v2 migration, including a post-rename uncertainty reopen. Coordinator admission/hydration/refresh adds 15 / 284: it preallocates an invisible model draft before journal admission, binds `Queued` to the exact receipt, compensates a foreign-draft publication failure, imports terminal/interrupted history without retaining the Journal, and after exact-storage reopen appends only absent terminal history while model, journal, drafts and residencies are cold. The coordinator/orchestrator/control subset is 31 / 1,060 and proves private pre-enqueue exact residency, `Queued` before Pool addition, Start/durable-terminal reduction, stale-revision rejection, exact cancellation, reentrant cancellation rejection, zero Pool side effects for a failed handoff and cold-refresh serialization. Fresh isolated Debug `OperationsUT` is 195 / 196 and 5,270 / 5,274; the sole failure remains the same host-specific set-ID metadata baseline.

**Оставшееся evidence:** bounded cross-volume staging; physical internal/external-volume matrix; power-loss durability implementation plus checkpoint harness and hardware matrix; bounded retry/defer policy for recovery history projection, Registry/UI projection of `operation.cancel`, pause/resume/retry and persistent Operation Center projection; non-Copy preflight; hosted required CI. Docker-backed integration baseline does not include the production volume fixtures.

### M4 — Search и ежедневные workflow

**Работы:** ввести `SearchStore`, scope и backend descriptor; объединить Find Files, Spotlight и provider search в Search Mode; показать progress, cancel и limitations; завершить Properties и async folder size; добавить copy path variants; перевести batch rename на `OperationPlan` с preview.

**Acceptance:**

- поиск всегда показывает scope, backend, progress и ограничения;
- результаты сохраняют понятный navigation context;
- no-results, cancelled, backend-unavailable и permission-limited states различимы;
- Properties показывает metadata/permissions, folder size считается асинхронно;
- batch rename показывает итоговые имена и конфликты до исполнения;
- требования local search test matrix 42.4 покрыты.

**Evidence:** search/state tests, batch rename preflight tests, accessibility walkthrough трёх поверхностей.

### M5 — Power-user workflow

**Работы:** подключить Dual Pane к независимым `PaneStore`; добавить workspaces и multi-pane persistence; реализовать folder compare, one-way sync и dry-run; интегрировать terminal/editor/git badges; добавить command palette и hotkey profiles; закрыть keyboard-only workflow.

**Acceptance:**

- panes сохраняют независимые location, selection, history и view state;
- workspace восстанавливает layout, tabs и active focus;
- compare выдаёт полный набор состояний раздела 45 спецификации;
- destructive sync запускается после dry-run с deletion preview;
- terminal/editor получают корректный cwd и paths;
- Beta gate 41.2 закрыт полностью.

**Evidence:** multi-pane state tests, generated compare/sync fixtures, workspace persistence tests, keyboard/VoiceOver checklist.

### M6 — Archives и remote workflow

**Работы:** завершить capabilities для archive/FTP/SFTP/WebDAV; подключить archive create/extract к Operation Center; построить Remote Connection Manager; хранить credentials и host verification в Keychain; добавить queue/retry/reconnect; интегрировать системные SMB/NFS mounts; показать latency/offline/read-only states.

**Acceptance:**

- archive browse/create/extract имеют plan, progress и typed result;
- remote transfers управляются через Operation Center;
- interruption сохраняет состояние и предлагает retry/reconnect;
- credentials не попадают в diagnostics;
- provider limitations и применимые alternatives видны пользователю;
- применимые archive/remote тесты 42.5–42.6 закрыты.

**Evidence:** `VFSUT`/`VFSIT`/`OperationsIT`, security review credential paths, reconnect fault-injection report.

### M7 — Надёжность и выпуск 1.0

**Работы:** сделать build/tests/static analysis обязательными CI checks; закрыть crash recovery и atomic persistence; провести performance pass; завершить accessibility, localization и visual states; проверить permissions, external volumes и network transitions; подготовить выбранные release variants; актуализировать Help/Building/Release/changelog.

**Acceptance:**

- Alpha, Beta и 1.0 gates 41.1–41.3 имеют ссылки на evidence;
- unit suite стабильно проходит в CI, integration suite разделён на deterministic и environment subsets;
- performance budgets раздела 35 воспроизводимо измеряются;
- diagnostics важных failures исключает credentials и содержимое файлов;
- release build проходит install, first-run, update и rollback smoke tests;
- P0 test matrix закрыта без silent failures и main-thread stalls.

**Evidence:** release checklist, CI runs, performance/accessibility reports, signed artifacts и notarization receipts.

### M8 — Expert layer

**Работы:** S3/cloud providers, duplicate/checksum search, automation, scheduled sync, plugin/actions API, command chains и advanced metadata развиваются отдельными feature-spec после M7.

**Acceptance:** функции используют Registry, Capabilities, Operation Engine и Error Model; automation создаёт тот же `OperationPlan`; extension API версионирован и изолирует failures; default layer сохраняет ясную hierarchy.

**Evidence:** feature-spec, threat model, API compatibility и end-to-end tests для каждого extension surface.

## 5. Release slices

- **Alpha:** M0–M3. Local browsing, базовые операции, Registry, Visual State и Operation Center образуют один законченный vertical slice.
- **Beta:** M4–M5 и соответствующий hardening M7. Закрыты search, conflicts, batch rename, dual-pane, compare/sync, settings, shortcuts и accessibility baseline.
- **1.0:** M6–M7. Полный gate спецификации; archive и remote workflows сохраняют стабильность local core.

## 6. Цикл реализации функции

1. Создать или обновить feature-spec по шаблону раздела 39.
2. Зафиксировать affected stores, commands, capabilities, operation lifecycle и errors.
3. Реализовать минимальный vertical slice через существующие engine boundaries.
4. Добавить happy path и минимум три содержательных edge cases.
5. Проверить mouse, keyboard, accessibility, loading, empty, error, permission и disabled states.
6. Сохранить evidence, обновить milestone, документацию и `changelog.md` в одном change set.

Статус **done** присваивается после полного Definition of Done из раздела 40 спецификации.

## 7. Verification commands

Команды запускаются из корня репозитория.

```bash
# M0 gate: project discovery, unsigned Debug build and all unit-test executables
Scripts/verify_m0.sh

# Unit tests alone; configuration can be Debug, Release, ASAN or UBSAN
Scripts/run_all_unit_tests.sh Debug

# Integration suite; requires Docker and nc, xcpretty is optional
Scripts/run_all_integration_tests.sh

# Documentation and patch hygiene
git diff --check
```

Integration environment описан в `Docs/Building.md` и fixtures соответствующих модулей. Seeded Docker-backed ASAN baseline выполнен: Term 18 / 555, Operations 87 / 862, VFS 52 / 87 968, VFSIcon 6 / 7, total 163 / 89 392. Планируемый CI должен запускать deterministic subset; network, credential, provider transaction и external-volume suites выполняются в выделенном окружении.

## 8. Evidence и ближайшая очередь

Для закрытия milestone требуются commit/PR, точные команды и итоги build/test, ссылки на tests, performance measurements затронутых hot paths, visual evidence сложных UI states, accessibility checklist и запись в `changelog.md`.

Ближайшая очередь:

1. Выполнить physical internal/external-volume `OperationsIT` и hardware power-loss evidence. Безопасный opt-in fixture уже добавлен: он требует descriptor-anchored user-owned marker roots, проверяет internal publication и external `UnsupportedExternalMedia`, а при отсутствии roots даёт `SKIP`; текущая сборка `OperationsIT` проходит, tag `[reviewed-copy-as-physical]` без physical inputs даёт 2 skipped. Physical run требует user-owned roots и отдельного оператора. Reviewed Native transaction уже выполняет ordered `fsync(destination)` → `fsync(parent)` → `F_FULLFSYNC(destination)`; hardware gate требует explicit checkpoint harness с phase `AfterPublishBeforeFullFSync` и checkpoint-driven physical power-loss run. Protocol: `Docs/Features/reviewed_copy_as_physical_volume_protocol.md`.
2. Реализовать bounded private staging authority для отдельного cross-volume scope. Текущий Native in-process путь блокирован: между проверкой named stage и `renameatx_np(..., RENAME_EXCL)` same-UID actor может подменить inode, а journal не имеет descriptor-bound cleanup authority. До отдельного signed helper с isolated staging roots, bounded lease/recovery authority и two-internal-APFS evidence cross-volume остаётся на legacy route. ADR: `Docs/ADR/0002-cross-volume-staging-authority.md`.
3. После первого consumer расширять factory на batch и остальные P0 mutation consumers; параллельно переводить оставшиеся P0 command consumers на Registry/Capabilities/Error/Visual State contracts. [`OperationCenterCoordinator`](Features/operation_center_model_and_control_projection.md) is process-owned at the `CopyAs` boundary: it binds `Queued` to the exact schema-v3 journal receipt, registers sealed exact live residency before Pool admission, reduces Start plus durable terminal outcomes through weak callbacks, revalidates `OperationId`/revision for engine-only cancellation and appends only absent terminal history from the same cold reopened storage without exposing raw `Operation *`. Confirmed explicit Copy recovery invokes that cold projection separately from custody evidence; Registry/UI projection of `operation.cancel` remains open.
4. Расширить production lifecycle evidence на remaining remote transport and permission scopes. Focused Docker `WinCommanderIT` coverage now passes 4 / 178 for FTP/SFTP/WebDAV navigation and a shadow-host mutation followed by forced user refresh: FTP and WebDAV prove user `Refresh` `Superseded` → provider soft-successor `Committed`, while SFTP proves direct user `Refresh` → `Committed` without provider observation/cache. A dedicated WebDAV case stops `nc_webdav_alpine`, proves `Failed(POSIX/ECONNREFUSED, NetworkError)` with retained committed listing and generation, restarts the endpoint, then proves a fresh `Committed` successor through the same controller/host user-refresh path. Deferred external-loading Busy также подтверждён production fixture: nested asynchronous navigation resolves once as `Rejected(Busy)` with `EBUSY` admission feedback and no fetch/model commit (1 / 22). Live-local `NativeHost` navigation/forced-refresh proof is 1 / 30 and the owned `0000` directory permission-denied path is 1 / 22. FTP/SFTP transport failure and timeout, FDA/security-scoped and other permission classes remain open. Explorer footer count/selection/bytes уже читает matching Store snapshot; preview/details, tabs и persistence остаются M2 work.
5. Получить первый hosted run `M0 Verification` после публикации и закрепить выполненный Docker-backed ASAN baseline как отдельный required integration profile.
