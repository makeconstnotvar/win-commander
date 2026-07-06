Скрин в целом движется в правильную сторону: есть sidebar, breadcrumb/search, command bar, таблица, inspector справа. Но центральная область действительно сбилась в “dashboard для проекта”, а не в File Explorer. Для Explorer-подобного UX центральная часть должна быть почти скучной: путь уже объясняет, где мы находимся; контент начинается с файлов; статистика живёт в status bar; секции появляются только как результат группировки.

Эталонная анатомия Windows 11 File Explorer: сверху вкладки, слева navigation pane, в центре content pane, опционально справа details pane, снизу status bar; toolbar содержит Back/Forward/Up/Refresh/Search, а app/command bar — действия вроде New, Copy/Move/Delete/Share. В Details view сортировка идёт через заголовки колонок вроде Name и Date modified. ([Microsoft Support][1]) Microsoft отдельно фиксирует команды Cut, Copy, Paste, Rename, Share, Delete, View, Compact view, file extensions, hidden items и item checkboxes как базовые элементы File Explorer. ([Microsoft Support][2]) Для macOS-оболочки логично держаться нативной структуры: leading sidebar, toolbar с логическими группами контролов, split view и inspector-панель справа. ([Apple Developer][3])

## Главная правка текущего макета

Текущий блок:

`Launch Assets → Project files → 42 items · Synced 2 min ago`

лучше убрать из обычной папки. Это выглядит как страница SaaS-приложения. В Win11-подходе обычная папка должна выглядеть так:

```text
[Tabs]
[Back] [Forward] [Up] [Refresh] [Breadcrumb path................] [Search]
[New] [Cut] [Copy] [Paste] [Rename] [Share] [Delete]     [Sort] [View] [...]
────────────────────────────────────────────────────────────────────────────
Name                         Date modified       Type        Size
Today
  Design-system.fig           10:42               Design      18.4 MB
  Q3-budget.xlsx              09:18               Spreadsheet 842 KB
Yesterday
  assets                      Yesterday           Folder      --
  linux-build.zip             Yesterday           Archive     426 MB
────────────────────────────────────────────────────────────────────────────
42 items    3 selected    84.6 MB                         18.2 GB available
```

Если включена группировка по дате — группы `Today / Yesterday / Earlier this week`. Если включена группировка по типу — группы `Folders / Documents / Images / Archives / Source files`. Сейчас у тебя текст говорит `grouped by type`, а строки сгруппированы по дате. Это надо развести.

Колонки по умолчанию: `Name`, `Date modified`, `Type`, `Size`. `Owner` не должен быть дефолтной колонкой для локальной папки; его лучше показывать только в cloud/shared/network-сценариях. `Modified` лучше заменить на `Date modified`, потому что это узнаваемая терминология Explorer.

## Общий принцип продукта

Делай не “Finder с кнопками Windows” и не “Windows в macOS-скине”, а:

**Windows Explorer interaction model + macOS visual grammar.**

То есть поведение, структура и набор функций как у Win11 Explorer; материалы, spacing, toolbar, sidebar, inspector, скругления, blur/vibrancy, SF Symbols-подобная иконография — как у нормального macOS-приложения.

---

# План макетов для Pencil.dev

## 0. Foundation / дизайн-система

### `00 Design Tokens`

Нарисовать отдельный лист с токенами:

| Область            | Что зафиксировать                                                                 |
| ------------------ | --------------------------------------------------------------------------------- |
| Window sizes       | 1280×800, 1440×900, 1512×982, narrow 1024×700                                     |
| Sidebar width      | 220 / 240 / 280 px                                                                |
| Inspector width    | 300 / 320 / 360 px                                                                |
| Toolbar height     | 48–52 px                                                                          |
| Command bar height | 44 px                                                                             |
| Table header       | 32 px                                                                             |
| File row normal    | 38–42 px                                                                          |
| File row compact   | 28–32 px                                                                          |
| Group header       | 28 px                                                                             |
| Status bar         | 24–28 px                                                                          |
| Radius             | 6 px small controls, 10–12 px panels/cards                                        |
| Typography         | window title, toolbar labels, table text, metadata text, sidebar group label      |
| States             | default, hover, selected, focused, disabled, drag target, warning, error, syncing |

### `01 Components`

Собрать компоненты, из которых потом делать все экраны:

| Компонент              | Варианты                                                                      |
| ---------------------- | ----------------------------------------------------------------------------- |
| App window             | light, dark, focused, unfocused                                               |
| Tab strip              | one tab, many tabs, active tab, tab overflow, new tab                         |
| Navigation controls    | Back, Forward, Up, Refresh                                                    |
| Breadcrumb/address bar | breadcrumb mode, editable path mode, long path truncation                     |
| Search field           | empty, focused, typing, searching, result count                               |
| Command button         | icon-only, icon+label, split button, disabled                                 |
| Sidebar item           | normal, selected, hover, expanded, collapsed, pinned, cloud/sync              |
| Sidebar group          | Favorites, Home, This Mac, Cloud, Network, Tags                               |
| Table header           | sorted asc, sorted desc, resized column, hidden column                        |
| File row               | file, folder, app bundle, alias, symlink, archive, image, video, code, hidden |
| File row state         | selected, multi-selected, hover, focused, cut-buffer ghosted, drag source     |
| Group header           | date, type, size, none                                                        |
| Inspector panel        | no selection, one file, folder, multi-selection, drive, network               |
| Context menu           | file, folder, background, sidebar, drive                                      |
| Popover                | Sort, View, New, More, Share                                                  |
| Dialog                 | delete, copy conflict, permissions, progress, properties                      |

---

# Основные экраны

## `FM-01 Home`

Это стартовый экран, аналог Windows File Explorer Home. В Windows 11 Home / Quick access включает pinned folders, recent/favorite cloud/Office-файлы, а известные папки вроде Desktop, Documents, Downloads, Pictures, Music, Videos доступны как pinned folders в Home и navigation pane. ([Microsoft Support][2])

Что нарисовать:

| Зона         | Что показать                                                                                              |
| ------------ | --------------------------------------------------------------------------------------------------------- |
| Sidebar      | Home selected; Favorites/Quick Access; Desktop, Downloads, Documents, Pictures; This Mac; Drives; Network |
| Main content | секции `Pinned`, `Recent`, `Favorites`                                                                    |
| Command bar  | New, Cut/Copy/Paste disabled без выбора, View, Sort, More                                                 |
| Status bar   | total items / sync state                                                                                  |
| Details pane | hidden by default                                                                                         |

Важно: Home может иметь заголовки секций. Обычная папка — нет.

---

## `FM-02 Regular Folder / Details View / Clean`

Главный макет. Именно его надо довести до эталона.

Что показать:

| Зона         | Что показать                                                   |
| ------------ | -------------------------------------------------------------- |
| Tab          | `Launch Assets`                                                |
| Address bar  | `This Mac > Projects > Launch Assets`                          |
| Search       | `Search Launch Assets`                                         |
| Command bar  | New, Cut, Copy, Paste, Rename, Share, Delete, Sort, View, More |
| Sidebar      | текущая папка выделена в дереве                                |
| Main content | таблица без крупного заголовка                                 |
| Columns      | Name, Date modified, Type, Size                                |
| Rows         | 12–20 файлов разных типов                                      |
| Status bar   | `42 items · 18.2 GB available`                                 |
| Details pane | off                                                            |

Это должен быть самый “Windows 11 Explorer, но macOS” экран.

---

## `FM-03 Regular Folder / 3 Selected / Details Pane On`

Это переработка твоего текущего скрина.

Что показать:

| Зона              | Что показать                                                     |
| ----------------- | ---------------------------------------------------------------- |
| Main content      | 3 строки выделены                                                |
| Command bar       | Cut/Copy/Rename/Share/Delete активны                             |
| Details pane      | `3 selected items`, total size, type summary, modified, location |
| Inspector actions | Open, Share, Copy path, More                                     |
| Status bar        | `42 items · 3 selected · 84.6 MB`                                |

Правка твоего текущего inspector: карточка с тремя цветными иконками выглядит красиво, но слишком “productivity dashboard”. Лучше сделать более системно: stack thumbnails/icons, затем plain metadata list. Цветные иконки оставить только как аккуратные file-type badges.

---

## `FM-04 Folder / No Selection / Details Pane On`

Нужен, чтобы понять, что справа показывать, когда ничего не выбрано.

Варианты:

1. Пустой inspector: `Select an item to see details`.
2. Folder summary: название текущей папки, путь, количество items, disk available, sync state.
3. Recent activity скрыта или вынесена в отдельную секцию только для cloud-папок.

Для обычного локального файлового менеджера вариант 1 чище.

---

## `FM-05 Folder / Single File Selected`

Проверочный экран для деталей файла.

Показать:

| Элемент     | Значение                                       |
| ----------- | ---------------------------------------------- |
| Preview     | thumbnail для image/video/pdf или generic icon |
| Name        | `hero-preview.png`                             |
| Actions     | Open, Quick Look, Share, Copy path, More       |
| Metadata    | Kind/Type, Size, Created, Modified, Location   |
| Permissions | Read/Write/Execute compact list                |
| Tags        | если поддерживаешь macOS tags                  |
| Activity    | только для cloud/sync location                 |

---

## `FM-06 Folder / Folder Selected`

Отдельно нарисовать папку, потому что у неё другой набор действий.

Показать:

| Элемент     | Значение                                               |
| ----------- | ------------------------------------------------------ |
| Main action | Open                                                   |
| Secondary   | Open in New Tab, Open in New Window, Copy path         |
| Metadata    | item count, size calculation state, location, modified |
| Permissions | compact                                                |
| Optional    | pin to Favorites / Quick access                        |

---

## `FM-07 Grouped by Date`

Windows-like grouped table.

Показать группы:

```text
Today
Yesterday
Earlier this week
Last week
```

Функции:

| Функция           | UI                              |
| ----------------- | ------------------------------- |
| Collapse group    | chevron у group header          |
| Select group      | checkbox/hover action           |
| Sort inside group | через column header             |
| Group menu        | Sort → Group by → Date modified |

---

## `FM-08 Grouped by Type`

Исправляет конфликт текущего макета.

Показать группы:

```text
Folders
Documents
Images
Archives
Source files
Other
```

Здесь строка в шапке может говорить `grouped by type`.

---

## `FM-09 Compact View`

Windows Explorer имеет Compact view как отдельную настройку плотности. ([Microsoft Support][2])

Показать:

| Параметр         |   Normal |  Compact |
| ---------------- | -------: | -------: |
| Row height       |    40 px |    30 px |
| Icon             | 20–24 px | 16–18 px |
| Vertical padding |   больше |   меньше |
| Status bar       |   тот же |          |

Это важно для пользователей Windows: многие привыкли к более плотной таблице.

---

## `FM-10 Icon Grid View`

Показать режим large/medium icons.

Функции:

| Функция           | UI                             |
| ----------------- | ------------------------------ |
| Folder thumbnails | folder icon + preview stack    |
| Images            | реальные thumbnails            |
| Videos            | thumbnail + duration badge     |
| Selection         | rounded selection background   |
| Multi-select      | checkboxes или selection rings |
| Sort/group        | остаются в command bar         |
| Status            | items / selected / size        |

---

## `FM-11 Tiles / Content View`

Windows Explorer поддерживает разные layout modes, включая Details, Tiles и Content. ([Microsoft Support][1])

Показать два варианта:

1. **Tiles** — крупная иконка, имя, тип/размер рядом.
2. **Content** — строка с preview, названием и дополнительной metadata.

Этот режим полезен для медиа, документов, архивов.

---

## `FM-12 Gallery`

Windows File Explorer имеет Gallery для просмотра фото из PC/phone/cloud storage. ([Microsoft][4])

На macOS можно сделать аналог:

| Зона         | Что показать                                        |
| ------------ | --------------------------------------------------- |
| Sidebar      | Gallery selected                                    |
| Main content | masonry/grid фотографий                             |
| Top filters  | All, This Mac, External Drive, Cloud                |
| Inspector    | выбранное изображение: EXIF, размер, дата, location |
| Command bar  | Import, Share, View, Sort                           |

Не делай Gallery первым MVP, но макет нужен, если целишься в функциональную полноту Windows 11 Explorer.

---

## `FM-13 Search / Typing`

Search box в File Explorer находится справа в toolbar, а поиск запускается из текущей папки/библиотеки. ([Microsoft Support][1])

Показать состояния:

| State        | Что видно                               |
| ------------ | --------------------------------------- |
| Empty        | `Search Launch Assets`                  |
| Focused      | cursor, suggestions dropdown            |
| Typing       | `hero`                                  |
| Searching    | progress spinner in content area        |
| Results      | `Search results in Launch Assets`       |
| No results   | empty state                             |
| Search scope | `Current folder` / `This Mac` / `Cloud` |

---

## `FM-14 Search Results / Filters`

Показать экран результатов.

Функции:

| Фильтр        | UI                                      |
| ------------- | --------------------------------------- |
| Kind          | Document, Image, Video, Folder, Archive |
| Date modified | Today, This week, This month, Custom    |
| Size          | Small, Medium, Large                    |
| Location      | Current folder, This Mac, Cloud         |
| Contents      | filename only / include file contents   |

Для macOS хорошо добавить Quick Look preview справа.

---

## `FM-15 Tabs`

Windows 11 File Explorer поддерживает tabs; Microsoft указывает Ctrl+T для новой вкладки. ([Microsoft][4])

Нарисовать:

| Variant             | Что проверить                                 |
| ------------------- | --------------------------------------------- |
| One tab             | обычное состояние                             |
| 3 tabs              | активная/неактивные                           |
| Many tabs           | overflow/scroll                               |
| Drag tab            | reorder                                       |
| New tab             | plus button                                   |
| Close tab           | hover close                                   |
| Dirty/operation tab | progress badge, если в папке идёт копирование |

В macOS лучше сделать вкладки частью titlebar, а не отдельной “браузерной” полосой на весь экран.

---

## `FM-16 Sidebar Variants`

Sidebar должен быть очень близок к Windows navigation pane по структуре, но выглядеть как macOS sidebar.

Нарисовать:

```text
Home
Gallery

Favorites
  Desktop
  Downloads
  Documents
  Projects

This Mac
  Macintosh HD
  External SSD
  Applications
  Users

Cloud
  iCloud Drive
  Design Vault

Network
  SMB Share
  NAS
```

Функции:

| Функция                | UI                              |
| ---------------------- | ------------------------------- |
| Collapse group         | chevron                         |
| Pin/unpin              | context menu                    |
| Drag reorder favorites | insertion line                  |
| Eject drive            | trailing eject icon             |
| Sync state             | compact badge                   |
| Offline/error          | badge + tooltip                 |
| Tree mode              | expandable folders под This Mac |

---

## `FM-17 View Popover`

Нарисовать popover по кнопке `View`.

Содержимое:

```text
Layout
  Extra large icons
  Large icons
  Medium icons
  Small icons
  List
  Details
  Tiles
  Content
  Gallery

Density
  Comfortable
  Compact

Show
  Navigation pane
  Details pane
  Preview pane
  Item check boxes
  File name extensions
  Hidden items
```

Это почти прямой функциональный слой Windows Explorer, но оформленный как macOS popover.

---

## `FM-18 Sort / Group Popover`

Содержимое:

```text
Sort by
  Name
  Date modified
  Type
  Size
  Date created

Direction
  Ascending
  Descending

Group by
  None
  Name
  Date modified
  Type
  Size
```

Проверить, что после выбора `Group by Type` центральная таблица реально показывает группы по типу.

---

## `FM-19 More Menu`

Содержимое:

```text
Open in New Window
Open in New Tab
Copy as Path
Open in Terminal
Properties / Get Info
Options
```

Для macOS можно назвать `Get Info`, но если делаешь Windows-like UX, допустимо `Properties`. Я бы выбрал гибрид: в UI `Get Info`, в shortcut/help можно указать `Properties`.

---

## `FM-20 Context Menu / File`

Windows 11 использует streamlined context menu, где common actions вроде Cut, Copy, Paste, Rename, Share, Delete находятся сверху. ([Microsoft Support][2])

Нарисовать:

```text
[Cut] [Copy] [Rename] [Share] [Delete]

Open
Open With >
Quick Look
Compress
Copy as Path
Show in Enclosing Folder
Tags >
Get Info
More Options
```

На macOS обязательно добавить Quick Look. Это не Windows, но это ожидаемое поведение Mac.

---

## `FM-21 Context Menu / Folder Background`

Показать правый клик по пустому месту:

```text
New Folder
New File >
Paste
Select All
Sort By >
Group By >
View >
Show View Options
Open in Terminal
Get Info
```

---

## `FM-22 New Menu`

Показать split button `New`.

Содержимое:

```text
Folder
Text Document
Markdown File
Shortcut / Alias
From Template >
```

Тут надо решить продуктово: Windows делает `New > ...`, macOS обычно беднее в создании файлов из Finder. Для твоего файлового менеджера создание пустого файла — полезная power-user функция.

---

## `FM-23 Copy / Move Progress`

Файловый менеджер без хорошего copy UI будет выглядеть игрушкой.

Нарисовать:

| State               | Что показать                                |
| ------------------- | ------------------------------------------- |
| Copying             | floating progress window или inline popover |
| Multiple operations | список операций                             |
| Pause/resume        | pause button                                |
| Speed               | MB/s                                        |
| ETA                 | remaining time                              |
| Current file        | имя файла                                   |
| Completed           | success state                               |
| Failed              | retry / skip / details                      |

---

## `FM-24 Copy Conflict Dialog`

Критически важный экран.

Показать варианты:

```text
Replace the file in destination
Skip this file
Compare info
Keep both
Apply to all conflicts
```

Показать две карточки: source file и destination file, с размером, датой изменения, preview.

---

## `FM-25 Delete / Trash Dialog`

На macOS лучше основное действие формулировать как `Move to Trash`.

Варианты:

| State                   | UI                                  |
| ----------------------- | ----------------------------------- |
| One file                | filename + icon                     |
| Multiple files          | `Move 3 items to Trash?`            |
| Permanent delete        | warning style                       |
| Permission denied       | explanation + action                |
| External/network volume | warning about non-restorable delete |

---

## `FM-26 Properties / Get Info`

Это отдельное окно или inspector sheet.

Показать вкладки/секции:

```text
General
  Name
  Kind
  Size
  Where
  Created
  Modified

Permissions
  Owner
  Group
  Read / Write / Execute

Open with
Tags
Preview
```

Для папки показать calculating size state.

---

## `FM-27 Preview Pane`

Не смешивать с Details pane.

| Details pane                   | Preview pane                  |
| ------------------------------ | ----------------------------- |
| metadata, actions, permissions | большой предпросмотр          |
| узкий inspector                | шире                          |
| полезен для любых файлов       | особенно PDF/image/video/text |

Нарисовать правую панель с preview для PNG, PDF, Markdown/text и video.

---

## `FM-28 Drives / This Mac`

Экран аналогичный `This PC`, но в macOS-терминах.

Показать:

| Элемент        | UI                 |
| -------------- | ------------------ |
| Internal drive | name, capacity bar |
| External SSD   | capacity + eject   |
| Network share  | connected/offline  |
| Cloud storage  | sync status        |
| Applications   | folder tile        |
| Users          | folder tile        |

Можно назвать раздел `This Mac`, а не `This PC`. Так ты сохраняешь Windows-модель, но не делаешь абсурдный текст на macOS.

---

## `FM-29 Network Share`

Показать SMB/NAS сценарии:

| State                | Что показать      |
| -------------------- | ----------------- |
| Connected            | folder list       |
| Credentials required | auth sheet        |
| Slow loading         | skeleton/progress |
| Offline              | reconnect         |
| Permission denied    | error row         |
| Eject/disconnect     | sidebar action    |

---

## `FM-30 Cloud / Sync States`

Если в продукте будет iCloud/Dropbox/OneDrive-подобная интеграция, нужны состояния:

```text
Available offline
Online-only
Syncing
Conflict
Error
Shared
Locked
```

Но не перегружай локальные папки cloud-метаданными. В текущем скрине `Synced 2 min ago` в обычной папке делает UI похожим на cloud dashboard.

---

## `FM-31 Empty / Loading / Error States`

Нарисовать:

| State                  | Текст                                           |
| ---------------------- | ----------------------------------------------- |
| Empty folder           | `This folder is empty`                          |
| Loading                | skeleton rows                                   |
| Permission denied      | `You don’t have permission to open this folder` |
| Path missing           | `Location not found`                            |
| External drive removed | `Drive disconnected`                            |
| Search empty           | `No results for “hero”`                         |

---

## `FM-32 Inline Rename`

Показать:

| State               | UI                      |
| ------------------- | ----------------------- |
| Rename file         | текстовое поле в строке |
| Extension preserved | выделено имя без `.png` |
| Invalid character   | inline error            |
| Duplicate name      | warning                 |
| Commit/cancel       | Enter/Esc               |

Для Windows-пользователя F2 rename обязателен.

---

## `FM-33 Drag & Drop`

Нарисовать:

| Scenario                | UI                          |
| ----------------------- | --------------------------- |
| Drag file inside folder | ghost preview               |
| Drag to sidebar folder  | sidebar target highlight    |
| Drag to tab             | tab hover opens after delay |
| Drag external file in   | copy badge                  |
| Drag within same volume | move badge                  |
| Drag with modifier      | copy/move/link indicator    |

---

## `FM-34 Keyboard Shortcuts / Settings`

Так как продукт Windows-like, настройки хоткеев важны.

Нарисовать settings screen:

```text
Keyboard profile
  macOS native
  Windows Explorer compatible
  Commander mode

Selection
  Space opens Quick Look
  Enter opens file
  F2 rename
  Delete moves to Trash
  Ctrl+C/Ctrl+V enabled
  Cmd+C/Cmd+V enabled
```

Это сразу снимет конфликт между Windows UX и macOS привычками.

---

## `FM-35 Dual Pane / Commander Mode`

Так как корень у тебя Nimble Commander, можно сохранить power-mode, но не делать его главным экраном.

Нарисовать отдельный режим:

| Зона               | Что показать                    |
| ------------------ | ------------------------------- |
| Left pane          | folder A                        |
| Right pane         | folder B                        |
| Shared command bar | copy/move/sync/compare          |
| Tabs per pane      | optional                        |
| Inspector          | hidden или collapsible          |
| Shortcut hint      | F5 copy, F6 move, F7 new folder |

Это не Windows 11 Explorer, поэтому держать как advanced mode.

---

# Что должно быть на главном макете MVP

Минимальный набор, который должен быть виден на одном hero-макете:

```text
Tabs
Back / Forward / Up / Refresh
Breadcrumb address bar
Search current folder
Command bar:
  New, Cut, Copy, Paste, Rename, Share, Delete, Sort, View, More
Sidebar:
  Home, Gallery, Favorites, This Mac, Drives, Network
Main content:
  Details view table
  Name, Date modified, Type, Size
  folders/files/mixed types
  selected row
Status bar:
  item count, selected count, selected size, free disk space
Optional right pane:
  Details inspector
```

Именно этот макет надо довести первым. Всё остальное — производные.

---

# Конкретная переработка твоего текущего скрина

Я бы поменял так:

1. Убрать крупный `Launch Assets` из content area. Название уже есть во вкладке, breadcrumb и search placeholder.
2. Убрать `Project files`. Для обычной папки это лишний H2.
3. `42 items · 3 selected · grouped by type` перенести вниз в status bar.
4. Если хочешь оставить sync status, показать маленький badge в status bar или sidebar, а не под заголовком.
5. Floating buttons над таблицей справа заменить на `Sort / View / More` в command bar. Сейчас они выглядят как локальный toolbar внутри карточки.
6. Таблицу поднять ближе к command bar, уменьшить пустой верхний отступ.
7. Колонки сделать: `Name`, `Date modified`, `Type`, `Size`. `Owner` — optional.
8. Группы привести в соответствие с выбранной группировкой.
9. Details pane сделать менее “карточным”: меньше декоративных блоков, больше системной metadata.
10. Status bar снизу сделать функциональным: слева items/selection, справа disk space и view toggle.

---

# Приоритет рисования

## Сначала

1. `FM-02 Regular Folder / Details View / Clean`
2. `FM-03 Regular Folder / 3 Selected / Details Pane On`
3. `FM-16 Sidebar Variants`
4. `FM-17 View Popover`
5. `FM-18 Sort / Group Popover`
6. `FM-20 Context Menu / File`

## Потом

7. `FM-01 Home`
8. `FM-09 Compact View`
9. `FM-10 Icon Grid View`
10. `FM-13 Search`
11. `FM-23 Copy Progress`
12. `FM-24 Copy Conflict`

## После MVP

13. Gallery
14. Cloud/sync
15. Network
16. Properties/Get Info
17. Commander mode

---

# Критерий готовности дизайна

Хороший результат должен проходить простой тест: если убрать macOS traffic lights и заменить визуальные материалы на Windows, структура должна считываться как File Explorer. Если заменить иконки/цвета обратно на macOS, окно должно выглядеть как аккуратное native Mac-приложение, а не как Electron-dashboard.

Главный экран надо сделать максимально спокойным: sidebar, toolbar, breadcrumb, command bar, таблица, status bar. Чем меньше “страничных” заголовков и декоративных карточек в content area, тем ближе ты попадёшь в ощущение Windows 11 File Explorer.

[1]: https://support.microsoft.com/en-us/accessibility/windows/use-a-screen-reader-to-explore-and-navigate-file-explorer-in-windows "Use a screen reader to explore and navigate File Explorer in Windows | Microsoft Support"
[2]: https://support.microsoft.com/en-us/windows/file-explorer-in-windows-ef370130-1cca-9dc5-e0df-2f7416fe1cb1 "File Explorer in Windows | Microsoft Support"
[3]: https://developer.apple.com/design/human-interface-guidelines/sidebars?utm_source=chatgpt.com "Sidebars | Apple Developer Documentation"
[4]: https://www.microsoft.com/en-gb/windows/tips/file-explorer "File Explorer | Microsoft Windows"
