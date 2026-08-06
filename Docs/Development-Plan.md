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
| Explorer shell | **partial** | Sidebar, toolbar, breadcrumb + address editor, quick search, command bar, status bar (count/selection/bytes из Store). Нет вкладок, нет preview/details, inspector — заглушка, Quick Look возвращает `nil`. |
| Command Registry | **partial** | 11 stable IDs с production-определениями: `file.copy`, `file.cut`, `file.rename` (инициация), `file.open`, `view.toggleHiddenFiles`, `navigation.back/forward/up/refresh`, `operation.cancel`, `operationCenter.open`. Остальные действия — только legacy-пути. |
| Visual State / Errors | **partial** | Pure mapper + AppKit adapter, полная taxonomy `FileManagerError` с POSIX-маппингами. Подключены к тем же 11 командам. |
| Operation Engine (новый) | **partial, заморожен** | `OperationPlan` → preflight → review → journal → Pool → durable terminal. Продакшн-охват: **одно обычное файловое копирование на том же внутреннем APFS-томе**. Cross-volume — `Unsupported`, идёт по legacy-маршруту. |
| Operation Center | **partial** | Статическая панель со снапшотом активных и терминальных операций + Cancel. Нет прогресса, pause/resume/retry, наблюдателя. |
| Search | **partial** | Есть quick search, Find Files, Spotlight. Нет `SearchStore`, scope, progress, ограничений backend. |
| Tests/CI | **partial** | Локально: `VFSUT` 162/165 и 46 598/46 601, `OperationsUT` 210/211 и 5 662/5 666, `WinCommanderUT` 334/338 и 5 407/5 411 (известные host-baseline провалы). Seeded Docker ASAN 163/89 392. Hosted CI ещё не запускался. |

### 2.1 Что осталось до продукта

Отсутствует не движок, а **поверхность Explorer**:

- нет команд Paste, Delete/Trash, New Folder/New File, Select All, Compress/Extract, Duplicate, Copy Path, Properties, Batch Rename, Calculate Sizes в Registry;
- нет details/preview-панели (`P0-VIEW-02` = **missing**);
- нет вкладок в Explorer;
- нет контекстного меню через Registry;
- нет полного набора view modes и их persistence;
- нет восстановления сессии;
- нет Search Mode со scope и ограничениями;
- нет прогресса и conflict-диалога копирования в Explorer-оболочке.

## 3. Решение о переприоритизации

### 3.1 Что замораживается и почему

**Заморожено: cross-volume staging authority (бывший пункт очереди 2 старого плана).**

Построены `ProtectedRootLedger`, `LeaseStore`, `SourceSnapshotWriter`, `DestinationStageWriter`, `StagingRootAuthority`, `LockedSession`, `StagingSessionRunner`, `StagingPublicationBarrier`, `PublicationPermit`, lifecycle retention/inspection и V1 protocol/codec/client. При этом:

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

Все мутации исполняются существующими `nc::ops::*`. Все команды используют готовые `Command`/`CommandRegistry`/`CommandState`/`ProviderCapabilities`/`FileManagerError`/`VisualStateMapper` контракты — как шаблон, копированием структуры уже сделанных срезов.

| # | Срез | Размер | Что даёт пользователю | Основа в коде |
|---|---|---|---|---|
| **Q1-1** | Полный roster команд Registry | **L** (серия S-срезов) | Paste, Delete/Trash, Permanent Delete, New Folder, New File, Select All, Invert Selection, Compress, Extract, Duplicate, Copy Path, Calculate Sizes, Batch Rename, Properties — работают одинаково из toolbar, меню, контекстного меню и по хоткею, с локализованной причиной недоступности | `Actions/{InsertFromPasteboard,Delete,MakeNew,Compress,Duplicate,CopyFilePaths,CalculateSizes,BatchRename,ChangeAttributes}` + `CommandRegistry` |
| **Q1-2** | Контекстное меню и toolbar overflow через Registry | **S** | Правый клик по файлу, папке и фону даёт тот же набор действий с теми же состояниями | Определения из Q1-1 + `CommandPresentationAdapter` |
| **Q1-3** | Details / Preview pane | **M** | Реальный инспектор: превью, метаданные, права, действия; Quick Look по пробелу; loading/error states | `Viewer`, `VFSIcon`, `QLPreviewPanel`; заменяет `NCExplorerInspectorPlaceholderView` |
| **Q1-4** | Вкладки в Explorer | **M** | Ctrl/Cmd+T, закрытие, переключение, drag-reorder; у каждой вкладки свой `PaneId` и своя история | `PanelController` + `PaneId` factory; tab-модель из `MainWindowFilePanelState` |
| **Q1-5** | View modes, density и persistence | **M** | Details / List / Icons / Compact, ширина и набор колонок, сортировка и группировка сохраняются per-location | `PaneStore` уже публикует view mode, layout slot, sort/group; нужен write-path и persistence |
| **Q1-6** | Восстановление сессии | **S** | После перезапуска возвращаются окно, вкладки, локации и view settings | Существующая `StateConfig` persistency |
| **Q1-7** | Полный набор состояний строки и папки | **M** | Inline rename (F2) с валидацией, drag & drop с copy/move-бейджами, loading skeleton, empty, permission denied, drive disconnected | `file.rename` commit → `Copying(docopy=false)`; `PanelView` drag-инфраструктура |
| **Q1-8** | Прогресс и конфликты копирования в Explorer | **M** | Видимый прогресс операции, MB/s, ETA, текущий файл; диалог конфликта Replace/Skip/Keep both/Apply to all | `AggregateProgressTracker`, `CopyingDialog`, существующая панель Operation Center |
| **Q1-9** | Search Mode | **L** | Поиск с явным scope (папка / диск / Spotlight), прогресс, отмена, понятные ограничения backend, результаты как обычный listing с навигацией к элементу | `Actions/FindFiles`, quick search, Spotlight; новый `SearchStore` |
| **Q1-10** | Explorer как режим по умолчанию + перф и accessibility | **M** | Explorer открывается по умолчанию; папки 10k/100k остаются интерактивными; VoiceOver-проход по основным поверхностям | `MainWindowController` state machine; baseline из `Performance/` |

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

- Explorer закрывает повседневный цикл: навигация, просмотр, создание, копирование, перемещение, переименование, удаление, архивирование, поиск, свойства.
- Вкладки, preview, view modes и сессия работают и восстанавливаются.
- Длинные операции видимы и отменяемы, конфликты разрешаются явно.
- Папки 10 000 и 100 000 элементов остаются интерактивными.
- Аггрегатные unit-тесты проходят локально; первый hosted CI run выполнен.

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

ASAN/UBSAN запускаются при изменениях в `Operations`, `VFS`, `RoutedIO`, конкурентности, владении памятью или async-teardown. Aggregate `UnitTests` и `verify_m0.sh` — на закрытии очереди, при изменениях build-системы и перед релизом.

## 7. Verification commands

```bash
Scripts/build_unsigned_and_run.sh
```

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
| M0 Baseline | Закрыт локально; остаётся первый hosted CI run → Q1-10 |
| M1 Source of Truth и Commands | `PaneStore` закрыт для чтения; остаток roster команд → Q1-1, Q1-2 |
| M2 Local Explorer | → Q1-3 … Q1-7, Q1-10 |
| M3 Operation lifecycle | Заморожен на текущем bounded-срезе; UI-часть (прогресс, конфликты) → Q1-8; расширение движка → Q2-8; cross-volume и hardware → раздел 5.1 |
| M4 Search | → Q1-9 |
| M5 Power-user | → Q2-1 … Q2-3, Q2-9 |
| M6 Archives и remote | → Q2-5, Q2-6 |
| M7 Release 1.0 | → Q2-10 |
| M8 Expert layer | → раздел 5.1 |
