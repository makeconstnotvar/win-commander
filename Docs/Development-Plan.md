# План разработки Win Commander

> Статус: активный roadmap
> Переприоритизирован: 2026-08-06
> Каноническая продуктовая спецификация: [`win_commander_ideal_file_manager_spec.md`](win_commander_ideal_file_manager_spec.md)
> Историческое evidence M0–M3 до переприоритизации: [`Evidence-Archive.md`](Evidence-Archive.md)

## 1. Цель и главный принцип

Цель ближайшего периода — **работающий, приятный файловый менеджер, которым можно пользоваться каждый день**. Не доказанный движок операций, не полное покрытие спецификации, а продукт.

Главный принцип переприоритизации:

> **Пользовательская функция важнее нового движка. Существующий движок важнее нового движка.**

Практически это значит:

1. Каждый срез Очереди 1 обязан добавлять поведение, которое пользователь видит и может выполнить мышью и клавиатурой.
2. Исполнение мутаций в Очереди 1 идёт через **существующие `nc::ops::*` операции** (`Copying`, `Deletion`, `DirectoryCreation`, `Compression`, `BatchRenaming`, `Linkage`, `AttrsChanging`). Новый reviewed-движок в Очередь 1 не расширяется.
3. Новые контракты (Registry, `PaneStore`, `Capabilities`, `FileManagerError`, Visual State) уже построены — их надо **использовать как дешёвый шаблон**, а не углублять.
4. Сложное, внешне заблокированное и требующее физического оборудования — уходит в конец (раздел 6).

## 2. Что фактически готово

| Область | Статус | Что реально работает |
|---|---|---|
| Движки | **done** | `Base`, `Utility`, `Config`, `CUI`, `Panel`, `VFS`, `VFSIcon`, `Operations`, `Viewer`, `Term`, `RoutedIO` разделены, собраны, покрыты тестами. |
| Legacy Commander | **done** | `MainWindowFilePanelState` — полнофункциональный двухпанельный менеджер: вкладки, копирование, перемещение, удаление, архивы, viewer, terminal. Это работающий fallback-продукт. |
| Legacy операции | **done** | `Copying`, `Deletion`, `DirectoryCreation`, `Compression`, `BatchRenaming`, `Linkage`, `AttrsChanging` + диалоги и `AggregateProgressTracker`. Покрывают все P0-мутации, включая cross-volume. |
| Legacy actions | **done** | `States/FilePanels/Actions/*` — ~40 готовых действий (Delete, MakeNew, Compress, Duplicate, BatchRename, CalculateSizes, CopyFilePaths, InsertFromPasteboard, Link, FindFiles, ChangeAttributes…). |
| VFS | **partial** | local, archive, FTP, SFTP, WebDAV, xattr, process. `ProviderCapabilities` даёт conservative path-aware проекцию, typed host-error classification, таймауты 30 с для FTP/SFTP/WebDAV. |
| `PaneStore` | **done для чтения** | Источник правды для navigation, listing, selection, hidden/sort/group/view state, layout slot, Back/Forward availability, current History entry. Reducer и production bridge покрыты тестами. |
| Explorer shell | **partial** | Sidebar, toolbar, breadcrumb + address editor, quick search, command bar, status bar, mounted details/preview inspector, runtime tabs, exact per-location view settings, versioned session restore and Q1-7 folder/row behavior are active. Q1-8 adds exact per-window Pool progress with current file, rate, ETA and lifecycle plus the explicit established conflict sheet. Q1-9 mounts per-tab Search Mode with explicit scope/backend state, progress, cancellation, ordinary result listings and Reveal Original. |
| Command Registry | **complete для Queue 1 roster** | 27 stable IDs имеют production definitions. Q1-1/Q1-2 закрывают mutation roster и его background/overflow composition; Q1-3 добавляет exact `file.getInfo`, `file.preview` и checked `view.togglePreviewPane`. Остальные совместимые legacy-действия сохраняют отдельные явные пути. |
| Visual State / Errors | **partial** | Pure mapper + AppKit adapters, полная taxonomy `FileManagerError` с POSIX-маппингами. Подключены к 27 Registry-командам, mounted Inspector and the Q1-7a/Q1-7b active-folder loading/empty/typed blocking projection, including production exact-volume disconnect classification. |
| Operation Engine (новый) | **partial, заморожен** | `OperationPlan` → preflight → review → journal → Pool → durable terminal. Продакшн-охват: **одно обычное файловое копирование на том же внутреннем APFS-томе**. Cross-volume — `Unsupported`, идёт по legacy-маршруту. |
| Operation Center | **partial** | Статическая панель со снапшотом активных и терминальных операций + Cancel сохраняет bounded value-only контракт. Explorer отдельно показывает live progress exact per-window Pool. Full live Center с history/result/log и sealed pause/resume/retry остаётся Queue 2. |
| Search | **complete для Queue 1 P0** | Per-tab `SearchStore` and controller unify direct folder/disk scan and local Spotlight behind explicit scope, filters, backend status/limitations, progress/cancel, bounded ordinary result listings and exact Reveal Original. Provider-native remote indexing, saved searches and advanced P1/P2 filters remain later increments. |
| Local development identity | **done** | Канонический local-development runtime устанавливается в `~/Applications/WinCommander-Codex.app`, использует bundle ID `com.wincommander.App.CodexDev` и закреплённые вне app bundle certificate fingerprint и exact designated requirement. Legacy `build_unsigned_and_run.sh` делегирует стабильному entry point; build/sign/install сериализован machine-local lock, replacement и rollback проходят exact identity/entitlement gates. Unsigned products используются как compile/test evidence. |
| Tests/CI | **partial** | Локально: Debug `VFSUT` проходит 179 cases / 46 786 assertions, current `OperationsUT` — 216 / 5 726, full Debug `WinCommanderUT` — 580 / 10 147. Q1-9 focused Search passes 62 / 1 010 in Debug, Release ASAN and Release UBSAN; `SearchForFiles` passes 9 / 68, and Debug `WinCommander-Unsigned` builds. Both sanitizer runtimes are linked with no diagnostics. Q1-8 evidence remains recorded in its feature spec. Seeded Docker ASAN 163/89 392. Hosted CI and signed walkthroughs remain pending. |

Local development identity evidence 2026-08-04: the installed certificate `66CA97F3581582C97871BCC0DFC11BEAB4C65C83` is pinned in machine-local state, and the prior content-changing rebuild changed CDHash from `367a50c5943b49e619d46b247f5ecb00e670022f` to `cda67423c625a18eb1ede01f21c163a0b358c862` while preserving bundle ID `com.wincommander.App.CodexDev` and the exact designated requirement `designated => identifier "com.wincommander.App.CodexDev" and certificate leaf = H"66ca97f3581582c97871bcc0dfc11beab4c65c83"`. The closing current-tree gate passed two consecutive `Scripts/build_stable_dev_and_run.sh --no-run` rebuilds. A separate contention probe held the fixed machine-local lock for three seconds and invoked the build through a different signing-state override; the build waited, completed, and preserved the same requirement. Both staged-candidate and canonical post-install verification passed. The state directory is `0700`; the certificate pin, requirement pin, build lock and dedicated keychain are `0600`. Negative checks rejected an otherwise valid candidate with an extended designated requirement, an unsigned DerivedData candidate and unsafe state-directory permissions. No WinCommander process was started. Observed continuity of existing FDA/Automation/Accessibility grants remains the explicit M7 runtime smoke.

### 2.1 Что осталось до продукта

Все срезы Очереди 1 (Q1-1…Q1-10) закрыты. Что осталось до полноценного закрытия Очереди 1 по разделу 4.3 — два явных пункта, ни один не является продуктовым разрывом:

1. Первый hosted CI run ещё не выполнен.
2. Обнаружен и вынесен отдельно предсуществующий дефект тестовой инфраструктуры: полный нефильтрованный прогон `WinCommanderUT` без `--filter` падает по `assert(g_Config)` в `AppDelegate.mm`, потому что ни один тест не бутстрапит глобальный `Config` singleton — воспроизводится и на чистом коде до среза Q1-10, не связан с продуктовой функциональностью. Целевые/отфильтрованные прогоны для каждого затронутого файла проходят чисто.

Очередь 2 может начинаться параллельно с закрытием этих двух пунктов.

## 3. Решение о переприоритизации

### 3.1 Что замораживается и почему

**Заморожено: cross-volume staging authority (бывший пункт очереди 2 старого плана).**

Построены `ProtectedRootLedger`, `LeaseStore`, `LeaseLifecycle` (Begin/Commit/Abort в helper-dispatcher, без artifact и namespace mutation), `SourceSnapshotWriter`, `DestinationStageWriter`, `StagingRootAuthority`, `LockedSession`, `StagingSessionRunner`, `StagingPublicationBarrier`, `PublicationPermit`, lifecycle retention/inspection и V1 protocol/codec/client. При этом:

- **publisher отсутствует** и заблокирован: нет descriptor-bound namespace primitive; `VFSUT`-характеризация подтвердила окно подмены same-euid перед `renameatx_np(..., RENAME_EXCL)`;
- нужен подписанный привилегированный helper (Developer ID + hardened runtime + SMJobBless), локальных identity нет;
- нужен фикстур из двух отдельных внутренних APFS-томов, которого нет на машине разработки (`/`, `/private/tmp`, `/Volumes` — один `st_dev`);
- **функционального выигрыша нет**: `nc::ops::Copying` уже копирует между томами сегодня.

Это самая дорогая работа в репозитории с нулевым видимым пользователю результатом. Возобновляется только после закрытия Очереди 1 — либо не возобновляется вовсе, если legacy-маршрут окажется достаточным.

**Заморожено: hardware power-loss evidence.** Требует одноразового физического Mac и оператора, вручную обрывающего питание в двух фазах. Это release-gate 1.0, а не задача разработки. Test-only checkpoint harness уже готов и ждёт оборудования.

**Заморожено: расширение reviewed-движка на batch, Move, Delete и остальных потребителей.** Возобновляется в Очереди 2 после того, как продукт станет пригоден к ежедневному использованию.

### 3.2 Что меняется в процессе работы

| Было | Стало |
|---|---|
| Каждый срез переписывает абзацы evidence-прозы в плане | Срез меняет **одну ячейку статуса** + одну строку в `changelog.md`. Подробное evidence — в feature-spec среза. |
| План накапливает историю | История — в [`Evidence-Archive.md`](Evidence-Archive.md); план описывает только текущее состояние и очередь. |
| Release ASAN + UBSAN на каждом «risk-bearing batch» | ASAN/UBSAN только при изменениях в `Operations`, `VFS`, `RoutedIO`, конкурентности, владении памятью и async-teardown. Для AppKit/Registry/Store-срезов — focused-фильтр + один Debug-билд. |
| Полный aggregate-прогон на закрытии среза | Полный прогон затронутого бинарника на закрытии среза; aggregate — на закрытии очереди. |
| Формальная строгость как цель | Fail-closed остаётся обязательным для мутаций. Доказательная строгость уровня M3 — только для нового движка операций, не для UI-срезов. |

## 4. Очередь 1 — продукт

Цель очереди: **Explorer становится основным режимом и полностью заменяет Commander для повседневных задач.**

Все мутации исполняются через `nc::ops::*`. Queue 1 переиспользует готовые legacy-операции; единственная выявленная дыра, создание пустого файла, закрыта узкой `nc::ops::EmptyFileCreation` без расширения reviewed engine. Все команды используют готовые `Command`/`CommandRegistry`/`CommandState`/`ProviderCapabilities`/`FileManagerError`/`VisualStateMapper` контракты — как шаблон, копированием структуры уже сделанных срезов.

| # | Срез | Размер | Статус | Что даёт пользователю | Основа в коде |
|---|---|---|---|---|---|
| **Q1-1** | Полный roster команд Registry | **L** (серия S-срезов) | **complete** — все mutating-команды и read-only `file.getInfo` имеют production Registry definitions и общие menu/shortcut/context/Explorer routes; последний surface/evidence закрыт вместе с [`explorer_details_preview_registry_slice.md`](Features/explorer_details_preview_registry_slice.md) | Paste, Delete/Trash, Permanent Delete, New Folder, New File, Select All, Invert Selection, Compress, Extract, Duplicate, Copy Path, Calculate Sizes, Batch Rename и Properties используют общий Registry-state на своих продуктовых поверхностях | `Actions/{InsertFromPasteboard,Delete,MakeNew,Compress,Duplicate,CopyFilePaths,CalculateSizes,BatchRename,ChangeAttributes}` + `CommandRegistry` |
| **Q1-2** | Контекстное меню и toolbar overflow через Registry | **S** | **complete** — background context и Explorer More используют единый pane-scoped Registry roster; exact-item payload остаётся отдельным контрактом, evidence в [`context_menu_toolbar_overflow_registry_slice.md`](Features/context_menu_toolbar_overflow_registry_slice.md) | Правый клик по файлу, папке и фону даёт тот же набор действий с теми же состояниями | Определения из Q1-1 + `CommandPresentationAdapter` |
| **Q1-3** | Details / Preview pane | **M** | **complete** — mounted right inspector, exact snapshot metadata/permissions, embedded and Space Quick Look, typed lifecycle/error states and three shared Registry commands; evidence in [`explorer_details_preview_registry_slice.md`](Features/explorer_details_preview_registry_slice.md) | Реальный инспектор: превью, метаданные, права, действия; Quick Look по пробелу; loading/error states | `Viewer`, `VFSIcon`, `QLPreviewPanel`; заменяет `NCExplorerInspectorPlaceholderView` |
| **Q1-4** | Вкладки в Explorer | **M** | **complete** — ordered runtime tabs, per-tab controller/Store/History ownership, active-only exact observation and chrome binding, established shortcuts/close/reorder surface; evidence in [`explorer_tabs_slice.md`](Features/explorer_tabs_slice.md) | Cmd+T, закрытие, переключение, numbered shortcuts и drag-reorder; у каждой вкладки свой `PaneId` и своя история | `ExplorerTabsModel`, `PanelController`, `PanelControllerPaneStoreAdapter`, `FilePanelsTabbedHolder` |
| **Q1-5** | View modes, density и persistence | **M** | **complete** — exact folder-specific slot plus concrete Brief/List/Gallery layout, column geometry, sort and grouping round-trip through bounded schema-v1 `StateConfig`; active-tab restore is fenced by pane/location/revision identity, evidence in [`explorer_view_settings_persistence_slice.md`](Features/explorer_view_settings_persistence_slice.md) | Small/Medium/Large Icons, Details and Content preserve density, icon scale, columns, sorting and grouping per location | `ExplorerViewSettingsPersistence`, pure binding policy, pane-local `PanelController` presentation override and strict sort inverse |
| **Q1-6** | Восстановление сессии | **S** | **complete** — schema-v1 window envelope restores mode, ordered tabs, active index and exact canonical locations through Cocoa → `StateConfig` → default precedence; legacy Commander roots migrate on write, each tab receives fresh runtime identity and isolated Home fallback, evidence in [`explorer_session_restore_slice.md`](Features/explorer_session_restore_slice.md) | После перезапуска возвращаются окно, вкладки, локации и view settings | `ExplorerSessionPersistency`, `MainWindowController`, `NCExplorerState`, no-password `PanelDataPersistency` restore |
| **Q1-7** | Полный набор состояний строки и папки | **M** | **complete** — Q1-7a–Q1-7d production implementation and closure gates pass; evidence in [`explorer_folder_visual_states_slice.md`](Features/explorer_folder_visual_states_slice.md). The adjacent conflict presentation is closed by Q1-8. | Inline rename (F2) с валидацией, modifier-reactive drag & drop с Copy/Move/Link badge и count, loading skeleton, empty, permission denied, drive disconnected | `VisualStateMapper`, `NCExplorerPaneStateView`; exact committed Native mount identity; exact inline-rename plan; pure `DragDropPolicy`; persistent Validate→Receive receiver; exact source/target seals and legacy `Copying`/`Linkage` routing |
| **Q1-8** | Прогресс и конфликты копирования в Explorer | **M** | **complete** — exact per-window Pool progress strip, copied current-item publication and explicit accessible conflict actions; evidence in [`explorer_operation_progress_conflicts_slice.md`](Features/explorer_operation_progress_conflicts_slice.md) | Видимый прогресс операции, MB/s, ETA, текущий файл; диалог конфликта Replace/Skip/Keep both/Apply to all | `ExplorerOperationProgressModel`, `ExplorerOperationProgressController`, `NCExplorerOperationProgressView`, `Job::CurrentItemPath`, `FileAlreadyExistDialog` |
| **Q1-9** | Search Mode | **L** | **complete** — per-tab Store/controller, direct folder/disk and local Spotlight backends, explicit P0 filters/limitations/progress/cancel, bounded ordinary result listings and exact Reveal Original; evidence in [`explorer_search_mode_slice.md`](Features/explorer_search_mode_slice.md) | Поиск с явным scope (папка / диск / Spotlight), прогресс, отмена, понятные ограничения backend, результаты как обычный listing с навигацией к элементу | `SearchStore`, `ExplorerSearchController`, direct/Spotlight adapters, `VFS::SearchForFiles` diagnostics |
| **Q1-10** | Explorer как режим по умолчанию + перф и accessibility | **M** | **complete** — default-startup policy verified (4/4 focused cases), large-folder cancellation double-notification bug fixed and covered (`PanelController*` 94/94 across four seeds), rename-field и List column header accessibility gaps закрыты, подтверждённый мёртвый код удалён; evidence in [`explorer_default_mode_and_accessibility_slice.md`](Features/explorer_default_mode_and_accessibility_slice.md) | Explorer открывается по умолчанию; папки 10k/100k остаются интерактивными без дублирующих уведомлений при отмене; rename field и column headers доступны VoiceOver | `MainWindowController` state machine; `Performance/` baseline + [`explorer_large_folder_interactivity_2026-08-07.md`](Performance/explorer_large_folder_interactivity_2026-08-07.md) |

### 4.1 Порядок исполнения

Q1-1 → Q1-2 → Q1-3 → Q1-4 → Q1-5 → Q1-6 → Q1-7 → Q1-8 → Q1-9 → Q1-10.

Q1-1 идёт первым и даёт максимальный прирост функциональности на единицу затрат: инфраструктура готова, каждая команда — повторение уже сделанного среза, исполнение — вызов готовой legacy-операции. Q1-2 почти бесплатен после Q1-1.

Внутри Q1-1 команды закрываются партиями по 3–4, а не по одной: у них общий шаблон, общий тестовый фикстур и общий presentation-путь.

### 4.2 Definition of Done среза Очереди 1

1. Действие доступно мышью и клавиатурой, с одинаковой валидацией во всех точках входа.
2. Недоступное действие возвращает локализуемую причину.
3. Мутация fail-closed: заблокированный, устаревший, отменённый или неподдерживаемый intent не доходит до исполнения.
4. Есть happy path + минимум три содержательных edge case в focused-тесте.
5. Проверены loading, empty, error, permission и disabled states.
6. Одна строка в `changelog.md` + обновлённая ячейка статуса в этом плане.

### 4.3 Exit criteria Очереди 1

- Explorer закрывает повседневный цикл: навигация, просмотр, создание, копирование, перемещение, переименование, удаление, архивирование, поиск, свойства. **Выполнено** (Q1-1…Q1-9).
- Вкладки, preview, view modes и сессия работают и восстанавливаются. **Выполнено** (Q1-3…Q1-6).
- Длинные операции видимы и отменяемы, конфликты разрешаются явно. **Выполнено** (Q1-8).
- Папки 10 000 и 100 000 элементов остаются интерактивными. **Выполнено** (Q1-10): shared navigation pipeline держит heartbeat отзывчивым и не дублирует уведомления об отмене; Explorer использует тот же `PanelController`/`PanelView` без изменений.
- Аггрегатные unit-тесты проходят локально; первый hosted CI run выполнен. **Частично**: отфильтрованные прогоны для каждого затронутого файла проходят чисто на нескольких seed; полный нефильтрованный `WinCommanderUT` падает по предсуществующему, не связанному с продуктом дефекту тестового бутстрапа (см. раздел 2.1); первый hosted CI run не выполнен.

## 5. Очередь 2 — power-user и полнота

Начинается только после exit criteria Очереди 1.

| # | Срез | Размер | Содержание |
|---|---|---|---|
| **Q2-1** | Dual Pane в Explorer + workspaces | **L** | Две независимые `PaneStore`, общий command bar, F5/F6/F7, сохранение layout |
| **Q2-2** | Folder compare и one-way sync | **L** | left-only / right-only / changed / same, dry-run с preview удалений |
| **Q2-3** | Command palette и hotkey profiles | **M** | Fuzzy-поиск по Registry; профили macOS native / Windows Explorer / Commander |
| **Q2-4** | Live Operation Center | **L** | Наблюдатель, прогресс, pause/resume/retry, персистентная история, логи |
| **Q2-5** | Remote Connection Manager | **L** | Хранение credentials и host verification в Keychain, queue/retry/reconnect, latency/offline/read-only states, системные SMB/NFS mounts |
| **Q2-6** | Архивы через Operation Center | **M** | Create/extract с планом, прогрессом и typed result |
| **Q2-7** | Gallery, cloud sync states, network states | **M** | Отдельный режим просмотра медиа; badges синхронизации только там, где они уместны |
| **Q2-8** | Расширение reviewed-движка | **L** | Batch, Move, Delete и остальные P0-потребители через `OperationPlan` |
| **Q2-9** | Terminal/editor/git-интеграция | **M** | Корректный cwd, пути, git badges |
| **Q2-10** | Release hardening 1.0 | **L** | Обязательные CI checks, crash recovery, atomic persistence, локализация, полный accessibility, подпись и нотаризация |

### 5.1 Отложено за пределы Очереди 2

| Пункт | Причина | Условие возврата |
|---|---|---|
| Cross-volume staging authority (подписанный helper) | Заблокирован отсутствующим descriptor-bound namespace primitive; требует Developer ID + SMJobBless + фикстур из двух APFS-томов; legacy `Copying` уже решает задачу | Появление подходящего примитива ядра **и** доказанная недостаточность legacy-маршрута |
| Hardware power-loss evidence | Требует одноразового физического Mac и ручного обрыва питания | Release-gate 1.0, вне цикла разработки |
| Physical internal/external volume matrix | Требует вручную подготовленных user-owned marker roots | Release-gate 1.0 |
| S3/cloud providers, duplicate/checksum search, automation, scheduled sync, plugin API | Expert layer | После 1.0 |

Test-only checkpoint harness и `[reviewed-copy-as-physical]` остаются в дереве как readiness evidence и не требуют поддержки.

## 6. Цикл реализации среза

1. Прочитать релевантный раздел спецификации и строку среза в этом плане.
2. Реализовать минимальный вертикальный срез через существующие движки и контракты.
3. Happy path + три edge case; проверить mouse, keyboard, loading, empty, error, permission, disabled.
4. Focused-фильтр тестов; полный прогон затронутого бинарника на закрытии среза.
5. Одна строка в `changelog.md`, обновлённая ячейка статуса, feature-spec если срез вводит новый контракт.
6. Local-development UI и local permission smoke выполнять через `Scripts/build_stable_dev_and_run.sh`; release/MAS/NonMAS/updater/Admin Mode/helper/distribution evidence получать на фактическом signed variant с его собственным identity contract.
7. Изменения app target, plist, entitlements, signing, packaging или launch tooling закрывать сравнением exact designated requirement до и после двух последовательных `--no-run` rebuild и проверкой `Scripts/verify_stable_dev_identity.sh`.

ASAN/UBSAN запускаются при изменениях в `Operations`, `VFS`, `RoutedIO`, конкурентности, владении памятью или async-teardown. Aggregate `UnitTests` и `verify_m0.sh` — на закрытии очереди, при изменениях build-системы и перед релизом.

## 7. Verification commands

```bash
# Canonical interactive development build with persistent macOS TCC identity
Scripts/build_stable_dev_and_run.sh

# Read-only validation of the installed development identity
Scripts/verify_stable_dev_identity.sh
```

Local runtime compatibility contract: `~/Applications/WinCommander-Codex.app`, `com.wincommander.App.CodexDev`, the machine-local pins at `~/Library/Application Support/WinCommanderCodex/local-signing-identity.sha1` and `local-signing-requirement.txt`, and the exact entitlement profile form one channel identity. The pins survive app removal and make missing keys, ambiguous initial certificate enrollment, ad-hoc signatures, bundle-ID/requirement/entitlement drift, and implicit certificate rotation fail before replacement. Unsigned DerivedData products remain isolated compile/test artifacts and do not constitute app-bound permission evidence.

```bash
Scripts/run_all_unit_tests.sh Debug
```

```bash
Scripts/verify_m0.sh
```

```bash
Scripts/run_all_integration_tests.sh
```

## 8. Соответствие milestones старого плана

| Старый milestone | Судьба |
|---|---|
| M0 Baseline | Закрыт локально; остаётся первый hosted CI run |
| M1 Source of Truth и Commands | `PaneStore` закрыт для чтения; Queue 1 Registry roster и presentation composition закрыты в Q1-1/Q1-2/Q1-3 |
| M2 Local Explorer | Закрыт в Q1-10: default mode, large-folder interactivity и accessibility gap closure подтверждены |
| M3 Operation lifecycle | Bounded Explorer progress/conflict surface закрыта в Q1-8; full live Operation Center и расширение движка → Queue 2; cross-volume и hardware → раздел 5.1 |
| M4 Search | → Q1-9 |
| M5 Power-user | → Q2-1 … Q2-3, Q2-9 |
| M6 Archives и remote | → Q2-5, Q2-6 |
| M7 Release 1.0 | → Q2-10 |
| M8 Expert layer | → раздел 5.1 |
