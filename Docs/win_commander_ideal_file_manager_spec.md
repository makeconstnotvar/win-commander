# Win-commander: спецификация идеального файлового менеджера

**Версия:** 1.0  
**Формат:** рабочий Markdown-документ для ИИ-агента  
**Цель:** создать файловый менеджер, который заменяет Finder в обычной работе, превосходит его в прозрачности операций, даёт power-user возможности Commander/Directory Opus/ForkLift-класса и остаётся чистым, понятным и нативным для macOS.

---

## 0. Как агент должен использовать этот документ

Этот документ является главным рабочим брифом для проектирования и реализации Win-commander.

Агент должен использовать его как:

- продуктовую спецификацию;
- архитектурный контракт;
- UX/UI-чеклист;
- Definition of Done;
- карту приоритетов;
- тестовую матрицу;
- источник инвариантов, которые нельзя нарушать.

Если локальный код проекта противоречит этому документу, агент должен:

1. зафиксировать противоречие;
2. объяснить риск;
3. предложить миграционный план;
4. менять код только после выбора наименее рискованной архитектурной траектории.

---

# 1. Видение продукта

Win-commander — это файловый менеджер нового поколения для macOS.

Он объединяет:

- визуальную чистоту и платформенную аккуратность macOS;
- понятность навигации Windows Explorer;
- эффективность dual-pane файловых менеджеров;
- мощность Directory Opus / Total Commander / ForkLift / Path Finder / Commander One / QSpace;
- прозрачность, реактивность и безопасность файловых операций.

Продуктовая формула:

```text
Finder-like native comfort
+ Windows Explorer mental model
+ Commander-grade power
+ explicit operation safety
+ complete visual state awareness
```

Идеальный результат:

> Пользователь всегда понимает, где он находится, что выбрано, что происходит, куда пойдут файлы, какие операции активны, что завершилось, что сломалось и что можно сделать дальше.

---

# 2. Главные продуктовые инварианты

Эти правила являются обязательными для всех функций.

```text
1. Пользователь всегда видит текущий путь.
2. Пользователь всегда видит активную панель.
3. Пользователь всегда видит выбранные элементы.
4. Пользователь всегда видит источник и назначение перед изменяющей операцией.
5. Пользователь всегда видит долгие операции.
6. Пользователь всегда видит ошибки после их возникновения.
7. Пользователь всегда получает визуальный отклик на действие.
8. Пользователь всегда может отменить долгую операцию, если это технически возможно.
9. Пользователь не теряет файлы молча.
10. UI не блокируется файловым вводом-выводом.
11. Все важные действия проходят через Command Registry.
12. Все файловые изменения проходят через Operation Engine.
13. Все providers явно объявляют свои capabilities.
14. Все destructive operations имеют видимый OperationPlan.
15. Все disabled actions объясняют причину недоступности.
16. Все статусы отображаются через единую Visual State System.
17. Все ошибки имеют user-facing объяснение и technical log.
18. Любая функция имеет loading, empty, error и permission states, если они применимы.
19. Базовый интерфейс остаётся чистым, power-user возможности раскрываются прогрессивно.
20. Расширенные функции не обходят core architecture.
```

---

# 3. Главный принцип проектирования

Файловый менеджер умеет очень много, но не показывает всё сразу.

Функции делятся на три слоя:

## 3.1. Default Layer

Чистый интерфейс для обычной работы:

- навигация;
- список файлов;
- path/address bar;
- sidebar;
- basic operations;
- preview/details;
- status bar;
- поиск;
- понятный progress.

## 3.2. Power Layer

Возможности для активной работы с файлами:

- dual-pane;
- operation queue;
- conflict resolver;
- batch rename;
- folder compare;
- folder sync;
- workspaces;
- command palette;
- custom shortcuts;
- developer actions.

## 3.3. Expert Layer

Расширенные возможности:

- remote providers;
- archive browsing;
- cloud providers;
- automation rules;
- plugin/actions API;
- macros;
- scheduled sync;
- advanced metadata.

---

# 4. Scope Discipline

Продуктовая амбиция — уметь всё важное, что умеют лучшие файловые менеджеры. Реализационная стратегия — строить это слоями, не ломая ядро.

## 4.1. Core Product

Core product — это локальный файловый менеджер с выдающейся прозрачностью, безопасностью, навигацией и операциями.

Core обязан включать:

- local filesystem browsing;
- path/address model;
- sidebar;
- file views;
- selection model;
- command system;
- safe file operations;
- Operation Engine;
- Operation Center;
- Visual State System;
- Error Model;
- Permission Model;
- settings foundation.

## 4.2. Layered Expansion

Все расширения строятся поверх core:

```text
Core filesystem UX
→ Safe operations
→ Visual state/reaction system
→ Dual-pane
→ Search
→ Batch tools
→ Compare/sync
→ Archives
→ Remote
→ Cloud
→ Automation/plugins
```

## 4.3. Early Non-goals

До production-качества Phase 1–2 агент не должен реализовывать:

- plugin API;
- automation rules;
- scheduled sync;
- cloud providers;
- image similarity duplicate finder;
- macro recording;
- archive editing;
- advanced scripting;
- S3 provider;
- complex remote sync.

Эти функции допустимы только после стабилизации ядра.

---

# 5. Agent Prioritization Rules

Когда агент выбирает между задачами, приоритет такой:

```text
1. Data safety
2. Correctness
3. UI reactivity
4. Clear visual state
5. Performance
6. Accessibility
7. Common workflows
8. Power-user workflows
9. Customization
10. Advanced integrations
```

Правила реализации:

```text
Every UI action must map to a command.
Every command must declare availability conditions.
Every file mutation must create an OperationPlan.
Every OperationPlan must be visible before destructive execution.
Every long operation must be represented in Operation Center.
Every provider must declare capabilities.
Every disabled action must expose a human-readable reason.
Every important state must be represented in the Visual State System.
Every error must be typed, logged and explainable.
Every advanced feature must reuse core architecture.
```

---

# 6. Антижалобы: какие боли продукт закрывает

| Боль пользователей | Требование Win-commander |
|---|---|
| Finder плохо показывает путь | Всегда видимый путь: breadcrumbs + editable address input + copy path |
| Finder неудобен для копирования между папками | Single-pane, dual-pane и multi-pane modes |
| Поиск непонятно где ищет | Явный search scope |
| Finder search воспринимается ненадёжным | Search modes: name/content/metadata/regex/fuzzy/exact |
| Drag & drop непредсказуем | Operation badge: Copy / Move / Link / Alias / Upload / Download |
| Копирование непрозрачно | Operation Center: queue, progress, pause, retry, conflicts, history |
| Ошибки исчезают | Persistent operation log + visible failed state |
| Контекстные меню неудобны | Главные действия в первом уровне, редкие — сгруппированы |
| Finder слаб для power users | Batch rename, sync, compare, terminal/editor, hotkeys |
| Альтернативы перегружены | Clean default UI + progressive power modes |
| Альтернативы выглядят не-native | macOS typography, spacing, sheets, popovers, sidebar behavior |
| Альтернативы медленные | Async architecture, virtualization, lazy metadata, thumbnail cache |
| Remote/network операции ненадёжны | Transfer engine with retries, resume, checksum, logs, throttling |
| Function keys неудобны на MacBook | Custom shortcuts + command palette + toolbar actions |
| Пользователь не понимает статус файлов | Unified badges, row states, status priority resolver |
| Пользователь не понимает доступность действий | ProviderCapabilities + disabled action reasons |

---

# 7. Архитектурная модель: source of truth

UI является проекцией domain state. UI-компоненты не выполняют файловые операции напрямую.

## 7.1. Authoritative systems

```text
PaneStore
- owns pane path
- owns loading state
- owns selection
- owns view mode
- owns sort/group/filter state

FileSystemProvider
- owns listing
- owns file metadata retrieval
- owns provider-specific capabilities

OperationEngine
- owns every mutating file operation
- owns operation plan
- owns queue
- owns progress
- owns cancellation/retry/finalization

SearchEngine
- owns search lifecycle
- owns search result state
- owns backend limitations

ConnectionManager
- owns remote/network connection state
- owns reconnect/retry state

PermissionManager
- owns permission state
- owns permission recovery actions

CommandRegistry
- owns executable user actions
- owns availability conditions
- owns shortcut bindings
- owns menu/toolbar/palette integration

VisualStateMapper
- converts domain state into UI states
- resolves status priority
- provides row/pane/app visual states

SettingsStore
- owns user preferences
- owns presets
- owns import/export
```

## 7.2. Event flow

Правильный поток:

```text
User action
→ Command
→ Domain intent
→ Validation
→ State update
→ Operation/Search/Provider execution
→ Domain event
→ Store update
→ VisualStateMapper
→ UI render
→ User feedback
```

Для файловых изменений:

```text
User action
→ Command
→ OperationPlan
→ Preflight
→ Conflict detection
→ Permission check
→ OperationEngine queue
→ Execution
→ Progress events
→ Final state
→ Operation log
→ UI feedback
```

---

# 8. Core modules

Агент должен выделить следующие модули:

```text
App Shell
Design System
Command Registry
Shortcut Registry
Menu Registry
Toolbar Registry
Context Menu Registry
File System Abstraction
Provider Capability Layer
Pane State
Navigation State
Selection Model
File View Model
Visual State System
Operation Engine
Operation Center
Conflict Resolver
Search Engine
Preview Engine
Metadata Engine
Permission Manager
Connection Manager
Archive Layer
Remote Connection Layer
Settings/Profile Layer
Persistence Layer
Diagnostics Layer
Accessibility Layer
Telemetry/Local Debug Layer
```

---

# 9. File System Abstraction

Нужен единый интерфейс для разных источников файлов.

## 9.1. Providers

```text
LocalFileProvider
MountedVolumeProvider
RemoteFileProvider
ArchiveFileProvider
SearchResultProvider
TrashProvider
VirtualFolderProvider
CloudProvider
```

## 9.2. FileItem

```text
FileItem
- id
- providerId
- stableId
- path
- parentPath
- displayName
- extension
- type
- kind
- isDirectory
- isFile
- isPackage
- isBundle
- isSymlink
- isAlias
- symlinkTarget
- aliasTarget
- isHidden
- isSystem
- isReadOnly
- isLocked
- isCloudOnly
- isAvailableOffline
- size
- sizeState
- createdAt
- modifiedAt
- lastOpenedAt
- permissions
- owner
- group
- tags
- comments
- metadata
- thumbnailState
- previewState
- gitState
- providerCapabilities
- errorState
```

## 9.3. ProviderCapabilities

Каждый provider обязан объявлять свои возможности.

```text
ProviderCapabilities
- canRead
- canWrite
- canRename
- canMoveWithinProvider
- canMoveAcrossProviders
- canCopyWithinProvider
- canCopyAcrossProviders
- canTrash
- canDeletePermanently
- canRestoreFromTrash
- canCreateFolder
- canCreateFile
- canWatchChanges
- canCalculateChecksum
- canSearchByName
- canSearchContent
- canSearchMetadata
- canPreview
- canGenerateThumbnail
- canResolveSymlink
- canResolveAlias
- canSetTags
- canSetComments
- canSetPermissions
- canSetOwnerGroup
- canResumeTransfer
- canReportProgress
- canEstimateSize
- canDryRun
- canListIncrementally
- canHandlePackages
- canHandleResourceForks
- canReadExtendedAttributes
- canWriteExtendedAttributes
- supportsAtomicRename
- supportsCaseSensitivity
- supportsUnicodeNormalization
```

Команды активируются на основе:

```text
provider capabilities
+ current selection
+ permission state
+ operation state
+ pane mode
+ app mode
```

Disabled command обязан возвращать:

```text
DisabledReason
- code
- userMessage
- technicalMessage
- suggestedAction
```

---

# 10. Command Registry

Все действия должны быть командами.

## 10.1. Command model

```text
Command
- id
- title
- description
- category
- icon
- defaultShortcut
- alternativeShortcuts
- enabledWhen
- visibleWhen
- disabledReason
- handler
- isDestructive
- requiresOperationPlan
- supportsUndo
- analyticsName
```

## 10.2. Required command categories

```text
Navigation
Pane
File
Edit
View
Search
Operation
Archive
Remote
Sync
Developer
Settings
Window
Help
```

## 10.3. Core commands

```text
file.copy
file.cut
file.paste
file.move
file.rename
file.duplicate
file.delete
file.trash
file.restore
file.newFolder
file.newFile
file.open
file.openWith
file.preview
file.getInfo
file.copyPath
file.copyPosixPath
file.copyRelativePath
file.copyFileUrl
file.showPackageContents
file.calculateChecksum
file.editPermissions
file.editTags

view.toggleHiddenFiles
view.togglePreviewPane
view.setListView
view.setIconView
view.setColumnView
view.setGalleryView
view.setDensity
view.customizeColumns

pane.splitVertical
pane.splitHorizontal
pane.closePane
pane.swap
pane.focusLeft
pane.focusRight
pane.copyToOtherSide
pane.moveToOtherSide
pane.linkNavigation

search.open
search.run
search.cancel
search.save
search.clearFilters

operationCenter.open
operation.pause
operation.resume
operation.cancel
operation.retry
operation.showLog

archive.create
archive.extract
archive.browse

sync.compare
sync.runDryRun
sync.execute

terminal.openHere
editor.openHere
developer.copyImportPath
settings.open
commandPalette.open
```

## 10.4. UI integration

Toolbar, context menu, command palette, shortcuts and menu bar use the same command definitions.

Нельзя добавлять отдельную кнопку, которая вызывает бизнес-логику напрямую.

---

# 11. Visual State & Reactivity System

Файловый менеджер обязан качественно показывать всё, что происходит, и реагировать на действия пользователя.

Главный принцип:

> В приложении нет невидимых процессов, молчаливых ошибок, непонятных задержек и действий без визуального ответа.

Любое действие пользователя даёт немедленную обратную связь. Любое длительное действие имеет наблюдаемое состояние. Любая ошибка видима, объяснима и доступна для повторного просмотра.

## 11.1. Категории отображаемых состояний

```text
App State
Window State
Pane State
Folder State
File Item State
Selection State
Operation State
Search State
Remote Connection State
Permission State
Drag & Drop State
Error State
Background Task State
```

---

## 11.2. App State

Глобальное состояние отображается через status bar, toolbar indicator, notification area или Operation Center.

Обязательные состояния:

```text
Ready
Loading
Scanning
Indexing
Searching
Copying
Moving
Deleting
Renaming
Syncing
Connecting
Disconnected
Offline
PermissionRequired
Warning
Error
BackgroundTasksRunning
```

Правила:

- фоновая задача всегда видима;
- ошибка оставляет след в operation log или diagnostics;
- permission request объясняет требуемое разрешение;
- восстановленное состояние обновляет UI без ручного refresh.

---

## 11.3. Pane State

Каждая файловая панель имеет собственное состояние.

```text
Idle
LoadingFolder
Refreshing
EmptyFolder
AccessDenied
PathNotFound
VolumeDisconnected
RemoteDisconnected
SearchActive
FilterActive
DropTargetActive
OperationTarget
```

Панель показывает:

- текущий путь;
- состояние загрузки;
- количество файлов;
- размер выбранных файлов;
- активные фильтры;
- search scope;
- режим отображения hidden files;
- состояние подключения для remote/network;
- ошибку доступа.

---

## 11.4. Folder State

Папка может иметь несколько признаков одновременно.

```text
Loaded
PartiallyLoaded
LargeFolder
CalculatingSize
WatchingForChanges
ChangedExternally
Unavailable
ReadOnly
NoPermission
NetworkLocation
RemoteLocation
ArchiveLocation
SearchResults
VirtualFolder
```

Правила:

- большие папки отображаются постепенно;
- подсчёт размеров папок не блокирует UI;
- внешние изменения отражаются через live update или soft refresh indicator;
- недоступный путь показывает причину.

---

## 11.5. File Item State

Каждый файл/папка имеет визуально различимые состояния.

```text
Normal
Selected
Focused
Hovered
Pressed
Renaming
CutPending
CopiedPending
Dragging
DropTarget
Opening
Deleting
Moving
Copying
Syncing
Conflict
Error
Locked
ReadOnly
Hidden
System
Alias
Symlink
Package
Unavailable
CloudOnly
Downloading
Uploading
ThumbnailLoading
PreviewUnavailable
PermissionDenied
GitModified
GitAdded
GitDeleted
GitIgnored
```

Правила:

- `Selected` и `Focused` визуально различаются;
- `CutPending` виден до paste;
- `Dragging` показывает количество объектов;
- `DropTarget` показывает будущую операцию;
- `Conflict` виден до destructive action;
- `Unavailable` отличается от обычного файла;
- `ThumbnailLoading` не ломает layout;
- Git/cloud/permission badges не конфликтуют визуально.

---

## 11.6. Selection State

Выбор файлов является отдельной state-моделью.

```text
NoSelection
SingleSelection
MultiSelection
RangeSelection
MixedTypesSelection
LargeSelection
SelectionWithErrors
SelectionWithReadOnlyItems
SelectionWithRemoteItems
SelectionWithPackages
```

Status bar показывает:

- количество выбранных элементов;
- суммарный размер, если доступен;
- количество папок;
- количество файлов;
- количество недоступных элементов;
- предупреждения, если операция невозможна для части выборки.

---

## 11.7. Operation State

Все файловые операции имеют строгую state machine.

```text
Planned
Queued
Preparing
Running
Paused
WaitingForConflictDecision
WaitingForPermission
WaitingForNetwork
Retrying
Cancelling
Cancelled
Failed
Completed
CompletedWithWarnings
RolledBack
```

Для каждой операции отображается:

- тип операции;
- источник;
- назначение;
- количество файлов;
- общий размер;
- текущий файл;
- прогресс по файлам;
- прогресс по байтам;
- скорость;
- ETA;
- предупреждения;
- ошибки;
- текущий policy;
- кнопки управления.

Правила:

- операция с ошибкой остаётся видимой;
- завершённые операции попадают в history;
- warning означает `CompletedWithWarnings`, а не полноценный success;
- отмена имеет промежуточное состояние `Cancelling`;
- retry является явным действием;
- destructive operation имеет план до запуска.

---

## 11.8. Search State

Поиск отображает процесс, ограничения и результат.

```text
SearchIdle
SearchTyping
SearchPreparing
SearchRunning
SearchPartiallyCompleted
SearchCompleted
SearchCancelled
SearchFailed
NoResults
TooManyResults
IndexUnavailable
PermissionLimitedResults
```

Показывать:

- query;
- scope;
- filters;
- searched locations;
- excluded locations;
- progress, если возможно;
- количество результатов;
- permission limitations;
- backend limitations.

---

## 11.9. Drag & Drop State

Drag & drop обязан быть предсказуемым.

Во время drag показывать:

```text
Dragged item count
Operation type
Source
Potential destination
Allowed / forbidden state
Conflict warning
Permission warning
External app target state
```

Типы операций:

```text
Copy
Move
Link
Alias
Upload
Download
Extract
AddToArchive
Forbidden
```

Правила:

- папка под курсором подсвечивается как drop target;
- между панелями показывается направление операции;
- невозможный drop показывает причину;
- конфликт имён предупреждается до drop;
- modifier keys меняют operation badge в реальном времени.

---

## 11.10. Remote / Network State

```text
Disconnected
Connecting
Authenticating
Connected
Listing
Transferring
Reconnecting
ConnectionLost
AuthenticationFailed
PermissionDenied
ReadOnly
RateLimited
Timeout
```

Показывать:

- статус подключения;
- host/account;
- slow connection warning;
- reconnect attempts;
- transfer speed;
- failed operations;
- last successful connection time.

---

## 11.11. Permission State

```text
PermissionUnknown
PermissionGranted
PermissionMissing
PermissionDenied
PermissionRevoked
RequiresAdmin
RequiresFullDiskAccess
RequiresSecurityScopedAccess
ReadOnlyVolume
```

Правила:

- UI объясняет, какое разрешение требуется;
- action button ведёт к корректному сценарию получения доступа;
- disabled action объясняет причину;
- permission errors логируются в Operation Center.

---

## 11.12. User Action Feedback

Минимальные требования:

- hover state для интерактивных элементов;
- pressed state для кнопок;
- focus ring для keyboard navigation;
- disabled state с объяснением причины;
- optimistic feedback для быстрых операций;
- progress feedback для долгих операций;
- rollback/error feedback при неудаче;
- toast/banner только для событий, которые требуют внимания;
- log entry для важных операций.

Правила задержек:

```text
0–100 ms: визуальный отклик
100–300 ms: локальный loading indicator
300+ ms: явный progress/loading state
1s+ длительная операция попадает в Operation Center
```

---

## 11.13. Empty / Loading / Error States

Каждый экран и каждая панель имеют состояния:

```text
Empty
Loading
PartiallyLoaded
Error
AccessDenied
NoResults
Offline
Unavailable
Unsupported
```

Примеры user-facing сообщений:

```text
Folder is empty
No search results in current scope
Access denied: Full Disk Access is required
Network volume disconnected
Archive format is not supported
Preview is unavailable for this file type
```

---

## 11.14. Visual Priority

Приоритет визуальных сигналов:

```text
1. Destructive danger
2. Blocking error
3. Active operation
4. Permission/network warning
5. Selection/focus
6. Drag/drop target
7. Sync/git/cloud badges
8. Metadata/decorative indicators
```

Правила:

- критичные ошибки имеют максимальный приоритет;
- badges группируются при избытке;
- цвет не является единственным носителем смысла;
- status icon имеет tooltip;
- status text доступен для VoiceOver.

---

## 11.15. Status Composition Rules

Один file item может иметь несколько статусов.

Порядок визуальной композиции:

```text
1. Row interaction state: hover / focus / selected / pressed
2. Blocking state: unavailable / permission denied / conflict / error
3. Operation state: copying / moving / deleting / uploading / downloading
4. Structural state: folder / package / alias / symlink
5. Source state: local / remote / cloud-only / archive
6. Metadata state: git / tags / comments / hidden / system
```

Правила:

- blocking states перекрывают decorative states;
- operation states остаются видимыми во время активной операции;
- selection/focus не скрывают ошибки;
- hover не скрывает drag/drop indicators;
- цвет всегда дублируется иконкой, текстом, формой или tooltip.

---

## 11.16. Visual State Architecture

UI не хранит случайные локальные boolean-флаги для критичных состояний.

Нужны единые модели:

```text
AppState
PaneState
FolderState
FileItemState
SelectionState
OperationState
SearchState
ConnectionState
PermissionState
DragDropState
```

Правильный подход:

```text
domain event
→ state update
→ visual state mapping
→ UI render
→ user feedback
```

Примеры domain events:

```text
FolderLoadingStarted
FolderLoadingCompleted
FolderLoadingFailed
SelectionChanged
DragStarted
DropTargetChanged
OperationQueued
OperationStarted
OperationProgressChanged
OperationPaused
OperationFailed
OperationCompleted
SearchStarted
SearchProgressChanged
SearchCompleted
PermissionRequired
RemoteDisconnected
ExternalFolderChanged
```

---

# 12. Design System

Дизайн-система нужна, чтобы все экраны выглядели как один продукт.

## 12.1. Shared primitives

```text
Color tokens
Typography scale
Spacing scale
Icon sizes
Row heights
Density modes
Focus ring
Selection style
Hover style
Pressed style
Disabled style
Badges
Status icons
Progress indicators
Skeleton loaders
Empty states
Error states
Warning banners
Popovers
Sheets
Sidebars
Tables
Toolbars
Command palette
Context menus
Status bar
```

Все экраны используют эти primitives.

## 12.2. Platform UX Principle

Приложение ощущается нативным на macOS и сохраняет ясность Windows Explorer-style navigation.

Использовать:

- macOS titlebar;
- macOS sidebar behavior;
- macOS typography;
- sheets;
- popovers;
- Quick Look;
- Finder-like platform conventions;
- Explorer-like editable address bar;
- Commander-like dual-pane operations;
- modern command palette;
- progressive disclosure.

Интерфейс является синтезом, а не копией одного продукта.

## 12.3. Density modes

```text
Comfortable
Default
Compact
Commander Dense
```

Default mode остаётся чистым. Dense mode включается пользователем.

## 12.4. Badge system

Badge categories:

```text
Error
Warning
Operation
Permission
Remote
Cloud
Git
Alias/Symlink
Package
Hidden/System
Metadata
```

Badge имеет:

```text
icon
tooltip
accessibilityLabel
priority
optional text
```

---

# 13. Основные UI-режимы

## 13.1. Simple Explorer Mode

Режим по умолчанию.

Назначение: заменить Finder в обычной работе.

Состав:

- sidebar слева;
- toolbar сверху;
- breadcrumbs / editable address bar;
- file view;
- optional preview/details pane справа;
- status bar снизу;
- operation indicator;
- search field;
- command bar.

Обязательные действия:

- Back;
- Forward;
- Up;
- Refresh;
- New folder;
- New file;
- Copy;
- Cut;
- Paste;
- Rename;
- Delete / Move to Trash;
- Share;
- Tags;
- View mode;
- Sort / Group;
- Search;
- Show hidden files;
- Open terminal here;
- Copy path.

Acceptance criteria:

- путь виден всегда;
- status bar показывает количество элементов;
- выбранный размер виден;
- операции проходят через Operation Engine;
- список файлов не блокирует UI;
- пользователь получает visual feedback на каждое действие.

---

## 13.2. Dual Pane Mode

Ключевой режим продукта.

Назначение: копирование, перенос, сравнение и синхронизация между двумя локациями.

Состав:

- левая панель;
- правая панель;
- отдельный path/address bar для каждой панели;
- явная активная панель;
- source/destination indicator;
- кнопки Swap, Compare, Sync;
- Copy to other side;
- Move to other side;
- общий Operation Center;
- optional shared preview/details pane.

Требования:

- drag & drop между панелями;
- keyboard-first операции;
- linked navigation;
- mirrored folders;
- independent tabs per pane;
- conflict resolution через общий resolver;
- направление операции видно до запуска.

Acceptance criteria:

- пользователь понимает, откуда и куда копируются файлы;
- активная панель видна;
- operation badge показывает тип операции;
- copy/move/delete не bypass-ят Operation Engine.

---

## 13.3. Multi Pane / Workspace Mode

Назначение: сценарии с 3–6 рабочими локациями.

Примеры:

- Downloads → Projects → Assets;
- Local project → Build folder → Remote server;
- Camera import → Sorted photos → Backup drive;
- Game assets → Godot project → Export folder.

Требования:

- grid layout: 2x2, 1+2, 1+3;
- saved workspaces;
- workspace name;
- per-pane title/path;
- clear active pane;
- drag target highlight;
- restore after restart;
- per-workspace sidebar/favorites.

Acceptance criteria:

- каждая панель читаема;
- нет визуального хаоса;
- action target всегда виден;
- workspace восстанавливается корректно.

---

## 13.4. Search Mode

Поиск является полноценным режимом, а не маленьким полем.

Состав:

- search query;
- explicit scope;
- filters;
- backend status;
- result list;
- preview/details pane;
- save search;
- diagnostics for incomplete search.

Фильтры:

- name contains;
- exact filename;
- extension;
- file type;
- size range;
- created/modified/opened date;
- tags;
- hidden/system/package files;
- content contains;
- regex;
- fuzzy mode;
- duplicate candidates;
- checksum match.

Search scope:

- current folder only;
- current folder + subfolders;
- selected folders;
- whole Mac;
- mounted volumes;
- network volumes;
- remote connection;
- archives;
- developer project.

Acceptance criteria:

- пользователь видит, где идёт поиск;
- ограничения поиска видны;
- результаты можно использовать как обычную выборку;
- можно reveal original;
- можно отменить поиск.

---

## 13.5. Operation Center

Operation Center — центральный экран прозрачности операций.

Состав:

- active operations;
- queued operations;
- completed operations;
- failed operations;
- paused operations;
- operation log;
- conflict resolver;
- retry controls;
- filters by status/type/date;
- persistent history.

Для каждой операции показывать:

- operation type;
- source;
- destination;
- file count;
- total size;
- current file;
- speed;
- ETA;
- errors;
- retry count;
- chosen conflict policy;
- controls.

Операции:

- copy;
- move;
- delete;
- trash;
- restore;
- rename;
- batch rename;
- archive;
- extract;
- sync;
- upload/download;
- checksum verify.

Acceptance criteria:

- операция не исчезает из поля зрения;
- ошибка не теряется;
- частичный успех виден;
- после restart доступна история;
- пользователь может retry/cancel/pause/resume, где возможно.

---

## 13.6. Conflict Resolver

Конфликты решаются явно.

Сценарии:

- файл уже существует;
- папка уже существует;
- файл отличается размером;
- файл отличается датой;
- файл отличается checksum;
- нет прав;
- файл занят;
- destination read-only;
- network disconnect;
- insufficient space;
- case-insensitive conflict;
- Unicode normalization conflict.

Действия:

- Replace;
- Skip;
- Keep both;
- Rename new;
- Rename existing;
- Merge folders;
- Compare;
- Apply to all;
- Apply to same extension;
- Apply to same folder;
- Save as rule.

Acceptance criteria:

- пользователь не принимает решение вслепую;
- destructive action визуально выделен;
- preview для source и destination доступен, если возможно;
- правило можно применить пакетно;
- итог решения попадает в OperationPlan/OperationLog.

---

## 13.7. Preview / Details Mode

Правая панель закрывает сценарии Finder Preview, Windows Details Pane и developer metadata.

Показывать:

- Quick Look preview;
- file icon/thumbnail;
- filename;
- extension;
- size;
- type;
- created;
- modified;
- last opened;
- path;
- tags;
- comments;
- permissions;
- owner/group;
- checksum;
- image/video dimensions;
- audio/video duration;
- EXIF;
- archive contents summary;
- git status;
- package/bundle info;
- symlink/alias target.

Действия:

- copy path;
- copy filename;
- copy relative path;
- copy POSIX path;
- copy file URL;
- reveal original;
- open with;
- edit tags;
- edit permissions;
- calculate checksum;
- show package contents.

---

# 14. File Operation Lifecycle

Все изменяющие операции проходят через единый lifecycle.

```text
1. Intent
2. OperationPlan creation
3. Validation
4. Preflight
5. Conflict detection
6. Permission check
7. Space check
8. Provider capability check
9. Queue
10. Execute
11. Report progress
12. Handle pause/cancel/retry
13. Finalize
14. Persist log
15. Notify UI
16. Offer undo/recovery when possible
```

## 14.1. OperationPlan

```text
OperationPlan
- id
- type
- sourceItems
- destination
- estimatedFiles
- estimatedBytes
- affectedPaths
- destructiveActions
- conflicts
- permissionRequirements
- spaceRequirements
- providerCapabilities
- canUndo
- canPause
- canResume
- canRetry
- warnings
- errors
- dryRunResult
- createdAt
```

OperationPlan показывает:

- что будет сделано;
- с какими файлами;
- куда;
- какие конфликты найдены;
- какие операции destructive;
- какие права нужны;
- хватит ли места;
- какие provider limitations действуют;
- что можно отменить;
- что можно восстановить.

## 14.2. FileOperation

```text
FileOperation
- id
- planId
- type
- sourceItems
- destination
- status
- progress
- currentItem
- bytesTotal
- bytesDone
- filesTotal
- filesDone
- speed
- eta
- conflictPolicy
- errorPolicy
- retryPolicy
- cancellationState
- createdAt
- startedAt
- finishedAt
- log
```

Статусы:

```text
planned
queued
preparing
running
paused
waitingForConflictDecision
waitingForPermission
waitingForNetwork
retrying
cancelling
cancelled
failed
completed
completedWithWarnings
rolledBack
```

## 14.3. Operation Engine requirements

Operation Engine обязан быть:

- cancellable;
- resumable where possible;
- retryable;
- asynchronous;
- non-blocking for UI;
- log-driven;
- dry-run capable;
- conflict-aware;
- permission-aware;
- provider-capability-aware;
- persistent for history;
- conservative for destructive actions.

---

# 15. Destructive Operation Safety

Destructive operations:

```text
permanent delete
overwrite
folder merge with replacement
sync with deletion
permission changes
owner/group changes
batch rename
archive overwrite
remote delete
empty trash
```

Требования:

- visible OperationPlan before execution;
- irreversible effects highlighted;
- explicit confirmation for permanent destructive actions;
- Trash preferred over permanent delete where possible;
- undo offered where technically possible;
- all destructive actions logged;
- partial failure shown explicitly;
- sync deletion always previewed in dry-run;
- remote destructive operations require extra clarity.

---

# 16. Error Model

Ошибки являются типизированными объектами, а не случайными строками.

## 16.1. FileManagerError

```text
FileManagerError
- code
- category
- severity
- userMessage
- technicalMessage
- affectedItems
- operationId
- providerId
- recoverable
- retryable
- requiresUserAction
- suggestedActions
- originalError
- timestamp
```

## 16.2. Error categories

```text
PermissionError
PathNotFoundError
VolumeUnavailableError
NetworkError
ConflictError
ValidationError
ProviderUnsupportedError
InsufficientSpaceError
FileBusyError
ReadOnlyError
ChecksumMismatchError
OperationCancelledError
PartialFailureError
TimeoutError
AuthenticationError
RateLimitError
UnknownError
```

## 16.3. Severity

```text
Info
Warning
RecoverableError
BlockingError
DestructiveRisk
FatalError
```

## 16.4. Error UI rules

- userMessage понятен без технических знаний;
- technicalMessage доступен в details/log;
- retryable error показывает Retry;
- recoverable error показывает recovery actions;
- permission error показывает permission flow;
- partial failure показывает список затронутых файлов;
- unknown error логируется с техническим контекстом.

---

# 17. Navigation

## 17.1. P0

- Back;
- Forward;
- Up;
- Refresh;
- Home;
- Desktop;
- Documents;
- Downloads;
- Applications;
- Recents;
- Favorites;
- Drives;
- Network;
- iCloud;
- Go to path;
- editable address bar;
- breadcrumbs;
- path autocomplete;
- tabs;
- restore session.

## 17.2. P1

- tab groups;
- saved workspaces;
- navigation history per pane;
- linked pane navigation;
- recent folders;
- pinned folders;
- smart folders.

## 17.3. P2

- per-project workspace;
- automatic workspace restore by connected drive;
- workspace templates.

---

# 18. File Views

## 18.1. P0

- List;
- Icons;
- Columns;
- Gallery;
- Details;
- Preview pane;
- show/hide hidden files;
- sort;
- group;
- column customization;
- folder-specific view settings.

## 18.2. P1

- density modes;
- custom columns;
- metadata columns;
- git columns;
- media columns;
- folder size calculation;
- folder size cache.

## 18.3. P2

- saved view profiles;
- per-folder rules;
- custom thumbnail providers.

---

# 19. Search Architecture

Search поддерживает разные backend-стратегии.

## 19.1. Backends

```text
Filename scan
Metadata scan
Spotlight-backed search
Content search
Provider-native remote search
Archive search
Cached index
```

## 19.2. Search limitations UI

Search UI показывает backend limitations.

Примеры:

```text
Content search is unavailable for this remote provider.
Results may be incomplete because Full Disk Access is missing.
Searching by name only.
Index unavailable, using direct scan.
Remote provider does not support metadata search.
Archive search is limited to filenames.
```

## 19.3. Search features

P0:

- search by filename;
- search by extension;
- search current folder;
- search subfolders;
- search whole Mac;
- exact match;
- contains match;
- filter by date;
- filter by size;
- filter by type.

P1:

- content search;
- metadata search;
- tags search;
- hidden/system files;
- regex;
- fuzzy search;
- saved searches;
- search result actions;
- search in mounted volumes.

P2:

- duplicate finder;
- checksum search;
- similarity search for images;
- saved search folders;
- search index diagnostics.

---

# 20. Large Folder Rendering Contract

Для больших папок:

- initial UI appears immediately;
- entries load progressively;
- metadata loads lazily;
- thumbnails load lazily and cancellably;
- folder size calculation is background or opt-in;
- scrolling remains responsive;
- selection works while metadata loads;
- sorting shows progress when expensive;
- refresh/loading can be cancelled where applicable;
- UI never waits for all thumbnails or folder sizes before rendering.

Target behavior:

```text
10 000 items: first visible rows quickly, progressive metadata
100 000 items: virtualized list, cancellable metadata, no UI freeze
Network folder: listing progress + timeout/retry state
Archive folder: progressive parsing if possible
```

---

# 21. Preview / Metadata

## 21.1. P0

- Quick Look integration;
- image preview;
- PDF preview;
- text preview;
- video/audio preview;
- basic metadata;
- path;
- size;
- dates;
- tags.

## 21.2. P1

- EXIF;
- media dimensions;
- audio/video duration;
- permissions editor;
- owner/group;
- checksum;
- symlink/alias target;
- package contents summary.

## 21.3. P2

- syntax highlighting;
- markdown preview;
- diff preview;
- archive preview;
- git metadata.

---

# 22. Archives

## 22.1. P0

- ZIP extract;
- ZIP create.

## 22.2. P1

- browse archive as folder;
- 7z;
- tar;
- gz;
- bz2;
- xz;
- rar extract;
- drag files out of archive;
- preview files inside archive.

## 22.3. P2

- edit archive contents;
- password-protected archives;
- split archives;
- archive integrity test.

Archive operations go through Operation Engine.

---

# 23. Remote / Cloud

## 23.1. P0

- SMB through mounted volumes;
- local network browsing;
- external drives;
- mounted volumes.

## 23.2. P1

- SFTP;
- FTP/FTPS;
- WebDAV;
- NFS;
- AFP;
- connection manager;
- bookmarks;
- transfer queue;
- reconnect/retry;
- remote search by name.

## 23.3. P2

- S3-compatible;
- Dropbox;
- Google Drive;
- OneDrive;
- Backblaze B2;
- remote sync;
- remote checksum when supported;
- bandwidth limits.

Remote operations go through Operation Engine and ConnectionManager.

---

# 24. Folder Compare / Sync

## 24.1. P0

- compare two folders by name/size/date;
- show left-only/right-only/changed/same.

## 24.2. P1

- one-way sync;
- two-way sync;
- dry-run;
- delete handling;
- conflict policy;
- exclude patterns;
- include patterns.

## 24.3. P2

- checksum compare;
- scheduled sync;
- remote sync;
- saved sync jobs.

Sync requirements:

- dry-run before execution;
- deletion preview;
- conflict preview;
- direction clearly visible;
- partial failure report;
- undo/recovery where possible;
- operation log.

---

# 25. Developer Tools

## 25.1. P0

- Open Terminal Here;
- Open in Editor;
- Copy Path;
- Copy POSIX Path;
- Copy Relative Path;
- Copy File URL;
- Show Hidden Files;
- Show Package Contents.

## 25.2. P1

- Git status badges;
- Git branch in folder header;
- `.gitignore` aware filtering;
- open in VS Code;
- open in Zed;
- open in JetBrains IDE;
- copy import-like path;
- calculate checksum;
- chmod/chown UI.

## 25.3. P2

- file diff;
- folder diff;
- run custom command;
- scripts/actions;
- project profiles.

---

# 26. Keyboard / Command Palette

## 26.1. P0

- customizable shortcuts;
- command palette;
- keyboard navigation in file list;
- type-to-select;
- rename shortcut configurable;
- delete/trash shortcuts;
- copy/move shortcuts.

## 26.2. P1

- Total Commander-like profile;
- Finder-like profile;
- Windows Explorer-like profile;
- Vim-like optional profile;
- shortcut conflict detector.

## 26.3. P2

- macro recording;
- command chains;
- custom actions.

---

# 27. Sidebar

Sidebar sections:

```text
Home
Favorites
Workspaces
Drives
Cloud
Network
Tags
Recent
Developer
```

Rules:

- sections can be hidden;
- folders can be pinned;
- order is customizable;
- mounted/unmounted state is visible;
- connection errors are visible near source;
- sidebar item has tooltip/accessibility label;
- sidebar supports drag-to-pin where appropriate.

---

# 28. Toolbar

## 28.1. Simple Mode toolbar

```text
[Back] [Forward] [Up] [Refresh] [Breadcrumbs / Path Input] [Search]
[New] [Cut] [Copy] [Paste] [Rename] [Delete] [Share] [View] [Sort] [More]
```

## 28.2. Dual Pane toolbar

```text
[Back] [Forward] [Up] [Refresh] [Left Path] [Right Path] [Search]
[Copy →] [← Copy] [Move →] [← Move] [Compare] [Sync] [Swap] [Operation Center]
```

Toolbar actions come from Command Registry.

---

# 29. Context Menu

Первый уровень:

```text
Open
Open With
Preview
Rename
Copy
Cut
Paste
Duplicate
Move to Trash
Get Info
Copy Path
Tags
Share
More Actions
```

`More Actions`:

```text
Checksum
Permissions
Archive
Batch Rename
Sync/Compare
Developer
Automation
Advanced Metadata
```

Context menu is not the only entry point for important actions.

---

# 30. Settings / Customization

Обязательные разделы:

```text
General
Appearance
Sidebar
File views
Operations
Search
Hotkeys
Context menu
Remote
Developer tools
Advanced
```

Mechanics:

- search inside settings;
- presets;
- reset section;
- export/import settings;
- shortcut conflict detection;
- safe defaults;
- advanced settings grouped.

Presets:

```text
Finder-like
Windows Explorer-like
Commander-like
Developer
Minimal
Power User
```

---

# 31. Persistence / Crash Recovery

Persist:

- open windows;
- tabs;
- pane paths;
- view modes;
- safe selection state;
- sidebar customizations;
- workspaces;
- operation history;
- failed operation logs;
- remote bookmarks;
- search presets;
- settings;
- hotkeys;
- column layouts;
- recent paths.

For active/interrupted operations:

- persist OperationPlan;
- persist completed steps where possible;
- after restart show interrupted operations;
- destructive operations are not silently resumed;
- user can inspect and retry interrupted operations;
- partial completion is visible.

---

# 32. macOS Platform Requirements

Продукт обязан корректно работать с особенностями macOS:

```text
APFS
case-sensitive volumes
case-insensitive volumes
resource forks
extended attributes
Finder tags
aliases
symlinks
app bundles
packages
security-scoped bookmarks
Full Disk Access
sandbox constraints
external drives
network volumes
iCloud placeholders
Quick Look
Spotlight integration where useful
file coordination
FSEvents
Trash semantics
Unicode normalization
```

macOS-specific rules:

- app bundle отображается как приложение, но может открываться как package;
- alias и symlink различаются;
- tags сохраняются совместимо с Finder;
- Trash semantics соответствуют платформе;
- Full Disk Access объясняется пользователю;
- external drive disconnect не ломает UI state;
- Unicode normalization учитывается в conflict detection.

---

# 33. Security / Permissions

Requirements:

- корректно работать с macOS privacy permissions;
- объяснять пользователю, зачем нужен доступ;
- поддерживать scoped access where required;
- не терять granted permissions;
- безопасно хранить credentials для remote connections;
- использовать Keychain для паролей;
- не логировать секреты;
- не логировать содержимое приватных файлов;
- показывать понятные ошибки доступа;
- destructive remote actions требуют ясного подтверждения.

---

# 34. Accessibility

Requirements:

- full keyboard navigation;
- VoiceOver labels;
- visible focus;
- high contrast compatibility;
- scalable text where practical;
- reduced motion support;
- no color-only status indication;
- all icons have text alternatives;
- operation progress readable by assistive technologies;
- error states announced correctly;
- command palette navigable by keyboard.

---

# 35. Performance Requirements

Target behavior:

- UI thread does not block on file IO;
- folder with 10 000 items starts rendering before full metadata load;
- folder with 100 000 items uses virtualization;
- thumbnail generation is lazy;
- folder size calculation is cancellable;
- preview loads asynchronously;
- search shows progressive results;
- network disconnect preserves app state;
- Operation Center preserves failed logs after restart;
- thumbnail cache has memory limit;
- large operations have concurrency limits;
- file watching is debounced;
- expensive metadata can be disabled per view.

---

# 36. Reliability Requirements

Агент должен отдельно тестировать:

```text
Unicode filenames
case-sensitive volumes
case-insensitive volumes
APFS
external drives
network volumes
symlinks
aliases
app bundles
packages
hidden files
system files
permission denied
read-only destination
insufficient space
interrupted copy
duplicate names
long paths
thousands of small files
huge single file
file changes during operation
mounted volume disappearing during operation
application restart during operation
```

---

# 37. Diagnostics

Продукт должен иметь локальную диагностику для разработки и поддержки.

Track internally:

- slow folder loading;
- slow thumbnail generation;
- operation failures;
- permission failures;
- provider errors;
- search backend failures;
- memory pressure;
- UI freezes;
- remote reconnects;
- crash recovery;
- repeated conflict patterns.

Diagnostics rules:

- no secrets;
- no credentials;
- no private file contents;
- file paths may be redacted in exported reports;
- user can export diagnostics intentionally;
- diagnostics help reproduce operation errors.

---

# 38. Implementation Phases

## Phase 0 — Audit and architecture map

Goal: понять текущий проект и не сломать его хаотичным rewrite.

Agent outputs:

```text
docs/current_architecture_audit.md
docs/feature_gap_matrix.md
docs/implementation_risks.md
docs/refactor_plan.md
```

Tasks:

- inspect project structure;
- find UI modules;
- find file operation modules;
- find state management;
- find settings;
- find existing tests;
- map current gaps;
- identify rewrite risks.

Acceptance criteria:

- агент понимает текущую архитектуру;
- есть карта gaps;
- есть безопасный план внедрения core modules.

---

## Phase 1 — Product foundation

Goal: файловый менеджер заменяет Finder в базовой локальной работе.

Build:

- App Shell;
- Design System foundation;
- sidebar;
- toolbar;
- breadcrumbs/address bar;
- file list;
- icon/list/details views;
- basic file operations;
- status bar;
- preview/details pane;
- hidden files toggle;
- tabs;
- settings skeleton;
- command registry;
- shortcut registry;
- PaneStore;
- FileSystemProvider;
- Visual State System baseline.

Acceptance criteria:

- можно открыть локальную папку;
- можно копировать, переносить, переименовывать, удалять;
- путь всегда виден;
- status bar показывает количество файлов и размер выборки;
- UI не блокируется на больших папках;
- loading/empty/error states реализованы;
- все actions идут через Command Registry.

---

## Phase 2 — Anti-Finder core

Goal: закрыть главные жалобы на Finder.

Build:

- Operation Engine;
- OperationPlan;
- Operation Center;
- conflict resolver;
- explicit drag & drop operation badge;
- improved search with explicit scope;
- operation history;
- batch rename;
- folder size async calculation;
- copy path variants;
- provider capabilities;
- error taxonomy.

Acceptance criteria:

- копирование между папками понятнее, чем в Finder;
- все долгие операции видны;
- конфликты решаются явно;
- поиск показывает scope;
- batch rename имеет live preview;
- disabled actions объясняют причину;
- destructive operations показывают plan.

---

## Phase 3 — Power-user workflow

Goal: продукт полезен разработчикам и heavy users.

Build:

- dual-pane mode;
- workspaces;
- multi-pane mode;
- folder compare;
- one-way sync;
- dry-run sync;
- terminal/editor integration;
- git status badges;
- custom columns;
- command palette;
- hotkey profiles.

Acceptance criteria:

- можно сохранить workspace;
- можно сравнить две папки;
- можно выполнить dry-run sync;
- можно работать почти полностью с клавиатуры;
- developer actions доступны без ручного копирования путей;
- sync deletion preview обязателен.

---

## Phase 4 — Archives and remote

Goal: закрыть сценарии ForkLift / Commander One / QSpace.

Build:

- archive browsing;
- zip/7z/tar extract/create;
- SFTP;
- FTP/FTPS;
- WebDAV;
- SMB/NFS improvements;
- remote connection manager;
- transfer queue;
- reconnect/retry;
- remote bookmarks.

Acceptance criteria:

- remote transfer идёт через Operation Center;
- обрыв соединения не уничтожает состояние операции;
- архив можно просматривать как папку;
- credentials хранятся безопасно;
- provider limitations видны пользователю.

---

## Phase 5 — Expert layer

Goal: добавить функции “идеального” продукта без перегруза базового UI.

Build:

- S3-compatible storage;
- cloud drives;
- duplicate finder;
- checksum search;
- automation rules;
- plugin/actions API;
- macro/command chains;
- scheduled sync;
- advanced metadata editor.

Acceptance criteria:

- expert features доступны через отдельные режимы;
- default UI остаётся чистым;
- plugins/actions не ломают core file operations;
- automation не bypass-ит Operation Engine.

---

# 39. Feature Implementation Cycle

Для каждой функции агент делает одинаковый цикл:

```text
1. Feature spec
2. User scenarios
3. UI states
4. Data model changes
5. Commands/actions
6. Provider capability impact
7. OperationPlan impact, if applicable
8. Error model impact
9. Minimal vertical implementation
10. Tests
11. Error states
12. Keyboard workflow
13. Accessibility pass
14. Documentation update
```

Feature spec template:

```md
# Feature: <name>

## User problem

## Product behavior

## User scenarios

## UI states

## Commands

## Data model

## Provider capabilities

## Operation lifecycle

## Error states

## Edge cases

## Accessibility

## Acceptance criteria

## Tests
```

---

# 40. Definition of Done

Функция готова только если:

- есть keyboard path;
- есть mouse path;
- есть empty state;
- есть loading state;
- есть error state;
- есть permission denied state;
- есть disabled state с причиной;
- есть large-folder behavior, если применимо;
- операция не блокирует UI;
- command доступна через Command Registry;
- shortcut можно переназначить, если action пользовательская;
- context menu не является единственным входом;
- состояние восстанавливается после restart, если это нужно;
- добавлены тесты на happy path;
- добавлены тесты минимум на 3 edge cases;
- accessibility labels добавлены;
- visual state соответствует Visual State System;
- ошибки типизированы;
- diagnostics/logging добавлены для важных failures;
- destructive operation имеет OperationPlan;
- provider limitations учтены.

Каждая важная операция отвечает на вопросы:

```text
Что сейчас происходит?
Где это происходит?
С чем это происходит?
Сколько уже сделано?
Что будет дальше?
Можно ли отменить?
Можно ли повторить?
Что пошло не так?
Что пользователь может сделать?
```

---

# 41. Release Gates

## 41.1. Alpha

Requirements:

- local browsing works;
- file list is responsive;
- path is always visible;
- basic operations go through Operation Engine;
- Operation Center shows progress and errors;
- Command Registry exists;
- Visual State System exists for loading, empty, error, selected, focused;
- disabled actions explain why;
- local settings persist.

## 41.2. Beta

Requirements:

- dual-pane works;
- conflict resolver works;
- search scope is explicit;
- batch rename has preview;
- folder compare works;
- settings and hotkeys are stable;
- crash recovery for operation history exists;
- large folders remain responsive;
- accessibility baseline passes.

## 41.3. 1.0

Requirements:

- product can replace Finder for local work;
- no silent operation failures;
- no blocking UI on large folders;
- all destructive operations have visible plans;
- all major operations are logged;
- permission failures are understandable;
- remote/archive features do not compromise local core stability;
- visual state quality is consistent across modes;
- keyboard and mouse workflows are both complete.

---

# 42. Test Matrix

## 42.1. Local filesystem

Test:

- ordinary folder;
- empty folder;
- folder with 10 000 files;
- folder with 100 000 files;
- many small files;
- one huge file;
- hidden files;
- app bundle;
- package;
- symlink;
- alias;
- read-only folder;
- no permission folder;
- external drive;
- disconnected external drive.

## 42.2. File operations

Test:

- copy file;
- copy folder;
- move file;
- move folder;
- rename;
- duplicate;
- delete to trash;
- restore;
- permanent delete;
- cancel copy;
- pause/resume;
- conflict replace;
- conflict keep both;
- insufficient space;
- source disappears;
- destination disappears;
- permission revoked during operation;
- app restart after partial operation.

## 42.3. UI states

Test:

- light mode;
- dark mode;
- small window;
- huge window;
- sidebar hidden;
- preview hidden;
- preview visible;
- dual-pane;
- multi-pane;
- keyboard-only navigation;
- drag & drop;
- context menu;
- command palette;
- selected/focused distinction;
- disabled command reason;
- loading/empty/error states.

## 42.4. Search

Test:

- exact filename;
- partial filename;
- extension;
- date filter;
- size filter;
- content search;
- hidden files;
- external drive;
- no results;
- too many results;
- cancelled search;
- permission-limited search;
- backend unavailable;
- remote search limitation.

## 42.5. Remote

Test:

- successful connect;
- wrong password;
- disconnected network;
- interrupted upload;
- interrupted download;
- retry;
- remote rename;
- remote delete;
- remote folder listing;
- large remote folder;
- timeout;
- reconnect;
- read-only remote destination.

## 42.6. Archives

Test:

- create zip;
- extract zip;
- browse archive;
- unsupported archive;
- corrupted archive;
- password-protected archive;
- large archive;
- drag out of archive;
- preview inside archive.

## 42.7. Visual Reactivity

Test:

- action response under 100 ms;
- loading indicator after delay;
- Operation Center entry after long operation;
- drag operation badge updates with modifier keys;
- conflict warning before drop;
- row status composition with multiple statuses;
- error persists in log;
- partial success is visible;
- retry updates state;
- cancel enters `Cancelling` before `Cancelled`.

---

# 43. UI Mockup Checklist

Agent should produce or update mockups for:

```text
Main Window — Simple Mode
Main Window — Dual Pane Mode
Main Window — Multi Pane Workspace
Search Results Screen
Operation Center
Conflict Resolver
Batch Rename
Folder Compare / Sync
Remote Connection Manager
Settings / Customization
Error States
Empty States
Permission Required Flow
Large Folder Loading State
Drag & Drop State
```

Each mockup must include:

- light variant;
- dark variant;
- loading state;
- empty/error state where applicable;
- keyboard focus state;
- status indicators;
- accessibility considerations.

---

# 44. Batch Rename Requirements

Batch Rename screen includes:

- selected files list;
- rename rules;
- live preview;
- old name;
- new name;
- invalid names warnings;
- conflict warnings;
- undo plan;
- apply/cancel;
- operation log after execution.

Rules support:

- find/replace;
- regex;
- prefix/suffix;
- numbering;
- date/time;
- metadata;
- extension transform;
- case transform.

Acceptance criteria:

- before execution user sees all new names;
- conflicts are detected before execution;
- destructive rename requires plan;
- operation can be undone where possible.

---

# 45. Folder Compare / Sync Requirements

Compare screen includes:

- left folder;
- right folder;
- comparison criteria;
- difference status;
- copy direction;
- dry-run summary;
- file-level diff;
- include/exclude rules;
- delete preview;
- conflict policy;
- execution button.

Difference statuses:

```text
Same
LeftOnly
RightOnly
Changed
Conflict
Ignored
PermissionBlocked
```

Acceptance criteria:

- direction is visible;
- deletion is separate and highlighted;
- dry-run is mandatory before destructive sync;
- execution goes through Operation Engine;
- result is logged.

---

# 46. Remote Connection Manager Requirements

Connection Manager includes:

- list of connections;
- connection type;
- host;
- account;
- credentials state;
- mount/open action;
- recent remote paths;
- transfer settings;
- status;
- last successful connection;
- error history.

Types:

```text
SMB
NFS
AFP
SFTP
FTP/FTPS
WebDAV
S3-compatible
Dropbox
Google Drive
OneDrive
```

Credentials:

- stored in Keychain;
- never logged;
- reconnect uses secure storage;
- authentication errors are typed.

---

# 47. Product Quality Criteria

Продукт можно считать сильным, если:

- обычный пользователь видит понятный файловый менеджер, а не cockpit;
- Windows-пользователь понимает address bar, path, operations и search scope;
- macOS-пользователь не чувствует чужеродный интерфейс;
- power user получает dual-pane, hotkeys, batch operations, sync, remote и archives;
- разработчик получает terminal/editor/path/git workflow;
- любая долгая операция видима, управляема и логируется;
- поиск объясняет, где и как он ищет;
- drag & drop всегда показывает действие;
- ошибки не исчезают;
- настройки мощные, но управляемые;
- UI остаётся отзывчивым на больших папках;
- destructive operations имеют plan;
- provider limitations показываются честно;
- visual states одинаково качественные во всех режимах.

---

# 48. Главная продуктовая проверка

Перед релизом каждая большая функция отвечает на вопрос:

> Эта функция делает файловые операции более понятными, безопасными и быстрыми, или просто добавляет ещё одну кнопку?

Функция, которая добавляет действие без улучшения сценария, уходит в command palette, advanced menu или backlog.

---

# 49. Финальный рабочий порядок для агента

Агент должен идти таким маршрутом:

```text
1. Audit current project
2. Establish Design System primitives
3. Establish Source of Truth stores
4. Implement Command Registry
5. Implement ProviderCapabilities
6. Implement Visual State System baseline
7. Implement local FileSystemProvider
8. Implement responsive file list
9. Implement navigation/path/address model
10. Implement basic operations through Operation Engine
11. Implement OperationPlan
12. Implement Operation Center
13. Implement Error Model
14. Implement Permission Model
15. Implement Conflict Resolver
16. Implement Search with explicit scope
17. Implement Dual Pane
18. Implement Batch Rename
19. Implement Folder Compare/Sync
20. Implement Workspaces
21. Implement Developer Tools
22. Implement Archives
23. Implement Remote
24. Implement Cloud
25. Implement Automation/Plugins
```

На каждом шаге агент сверяется с:

- Product Invariants;
- Definition of Done;
- Visual State System;
- Error Model;
- ProviderCapabilities;
- Test Matrix;
- Release Gates.

---

# 50. Минимальный стандарт “10 из 10”

Win-commander получает оценку 10/10 только если выполняет все условия:

```text
User clarity: пользователь всегда понимает состояние.
Data safety: операции не приводят к тихой потере данных.
Responsiveness: UI остаётся живым на тяжёлых сценариях.
Power: dual-pane, search, batch, sync, remote, archive доступны.
Cleanliness: default UI не перегружен.
Native feel: приложение ощущается платформенным для macOS.
Extensibility: новые providers/actions добавляются через architecture contracts.
Recoverability: ошибки, interruption и partial failures видимы и управляемы.
Accessibility: keyboard и assistive workflows полноценны.
Consistency: статусы, ошибки, progress и commands работают одинаково во всех режимах.
```

