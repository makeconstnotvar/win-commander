# План работ: редизайн Nimble Commander в стиле Windows Explorer

> Статус: черновик для обсуждения. Документ описывает форк Nimble Commander (NC), у которого сохраняется весь движок (VFS, файловые операции, панельная модель данных, встроенный вьювер, терминал, конфиги), но полностью переосмысляется UI/UX по образцу проводника Windows 11.
> Второй документ — [WindowsUI-Redesign-Design.md](WindowsUI-Redesign-Design.md) — описывает сам дизайн (макеты, компоненты, палитра, иконки, хоткеи). Этот файл — про то, **как** до него дойти.

## 0. Как читать этот документ

Все ссылки на файлы даны относительно корня репозитория (`Source/...`). Они получены прямым чтением исходников NC (не по памяти), поэтому на них можно ориентироваться при декомпозиции задач в issue-трекере.

---

## 1. Цель и рамки проекта

**Цель.** Получить macOS-приложение для управления файлами, которое:
- по **функциональности и механике** — это NC (VFS, операции, вьювер, терминал, темы, конфиги, VFS-архивы/FTP/SFTP/WebDAV/xattr/processes);
- по **визуальному языку и раскладке экрана** — узнаваемо похоже на Windows 11 Проводник (File Explorer): командная панель вместо меню-ориентированного тулбара, sidebar с «Быстрым доступом»/«Этим Mac»/«Сетью», breadcrumb-адресная строка, список файлов в духе Details View, статус-бар снизу, реорганизованное контекстное меню.

**Не цель:**
- пиксель-в-пиксель клон Windows (юридически рискованно и бессмысленно на другой ОС — см. [раздел 9](#9-риски-и-открытые-вопросы));
- переписывание движка (VFS/Operations/Viewer/Term) — он остаётся практически нетронутым, см. [раздел 2](#2-что-не-трогаем-движок);
- отказ от «фишек» NC (двухпанельный режим, VFS-архивы, batch rename, quick search, вкладки) — они остаются, но получают новую визуальную упаковку и, где уместно, новые точки входа в духе Explorer.

**Лицензия.** NC распространяется под GPLv3 (`LICENSE.md`). Форк обязан оставаться под GPLv3, сохранять copyright-уведомления и явно указывать происхождение (README/NOTICE: «основано на Nimble Commander, © Michael Kazakov»). Это не блокирует коммерциализацию или отдельный бренд — GPLv3 такому не мешает, но требует открытости исходников производного продукта.

**Бренд.** Оставлять имя «Nimble Commander» для продукта с другим UX нежелательно (путаница с апстримом, конфликт бандл-ID `info.filesmanager.Files`). Выбор нового имени/иконки/бандл-ID — задача Фазы 0, в этом документе не фиксируется.

---

## 2. Что не трогаем (движок)

Ниже — слои, которые в ходе редизайна не должны меняться по существу. Это заявлено не декларативно, а подтверждено чтением кода: каждый из этих слоёв уже отделён от UI структурно (протоколами/callback-интерфейсами), а не только «по договорённости».

| Слой | Модуль/файлы | Почему не трогаем |
|---|---|---|
| Виртуальная файловая система | `Source/VFS`, `Source/VFSIcon` | Общий интерфейс доступа к локальной ФС, архивам, FTP/SFTP/WebDAV, xattr, списку процессов; иконки/thumbnails. Чистый C++, без AppKit. |
| Базовые утилиты | `Source/Base`, `Source/Utility` | Хэши, пути, потоки (`SerialQueue`), форматирование дат/размеров, системные хелперы (FSEvents, буфер обмена). |
| Admin Mode | `Source/RoutedIO` | Privileged-helper и IPC-клиент для операций с правами администратора. |
| Хранилище настроек | `Source/Config` (`ConfigImpl.cpp`, `FileOverwritesStorage.cpp`) | JSON через RapidJSON, файлы в `~/Library/Application Support/<Имя приложения>/Config`. Меняется только *имя папки* при ребрендинге, не формат. |
| Модель данных панели | `Source/Panel/include/Panel/PanelData.h`, `PanelDataSortMode.h` | Листинг, сортировка, выделение — общий для всех view-mode реализаций через `NCPanelViewPresentationProtocol`. |
| Движок Quick Search | `Source/Panel/include/Panel/QuickSearch.h`, `Source/Panel/source/QuickSearch.mm` | Фильтрация по вводу — чистая логика поверх `PanelDataFilter`, не знает об AppKit. |
| Движок файловых операций | `Source/Operations/source/Job.h/.cpp` + каждый `*Job.h/.cpp` (`CopyingJob`, `DeletionJob`, `AttrsChangingJob`, `BatchRenamingJob`, `CompressionJob`, `DirectoryCreationJob`, `LinkageJob`) | Доказано grep’ом по всем Job-файлам — ноль ссылок на Cocoa/NSWindow/NSView. Job общается наружу только через `std::function`-колбэки с безопасными дефолтами. Также используется в `_IT`-тестах без единого диалога. |
| Движок вьювера | `Source/Viewer` — `DataBackend`, `TextModeFrame`/`TextModeWorkingSet`, `HexModeFrame`/`HexModeLayout`/`HexModeProcessing`, `Highlighting/*` | Буфер файла, текстовый/hex-layout, подсветка синтаксиса. |
| Движок терминала | `Source/Term` — `Screen`/`ScreenBuffer`, `ParserImpl` (VT100), `InterpreterImpl`, `ShellTask`, `InputTranslatorImpl` | Полностью независим от UI, читает палитру только через абстракцию `nc::term::Settings`. |
| Архитектура тем | `Source/NimbleCommander/NimbleCommander/Core/Theming/Theme.h/.mm`, `ThemesManager.*`, `SystemThemeDetector.h` | Механизм (типизированный снэпшот темы + менеджер переключения + авто light/dark) остаётся; меняется только **содержимое** JSON-тем (см. Design-документ). |
| State-machine окна | `Source/NimbleCommander/NimbleCommander/States/MainWindowStateProtocol.h`, `MainWindowController.h/.mm` | Протокол `NCMainWindowState` (`windowStateContentView`/`windowStateToolbar` + lifecycle-хуки) и стек push/pop — это уже готовая точка расширения для переключения панели/вьювера/терминала. Redesign использует её, не переизобретает. |

**Практическое следствие:** весь существующий набор unit/integration-тестов (`_UT`/`_IT`, см. `Docs/Building.md`) должен продолжать проходить на протяжении всего проекта. Если какой-то PR ломает тест из `Source/Operations/tests`, `Source/VFS/tests` и т.д. — это сигнал, что редизайн случайно залез в движок.

---

## 3. Что меняем (UI-слой)

Здесь — обратная сторона: компоненты, которые *являются* UI и подлежат переработке. Для каждого указано, что именно происходит: **reskin** (визуальный рестайл в имеющемся месте), **rebuild** (логика остаётся, но виджет пересобирается заново под новую раскладку), **new** (компонента сегодня нет вообще).

| Текущий компонент | Файлы | Судьба | Комментарий |
|---|---|---|---|
| Тулбар панелей | `States/FilePanels/MainWindowFilePanelsStateToolbarDelegate.h/.mm` | **rebuild** | Сегодня это `NSToolbarDelegate` с кнопками Go-To и операциями. Становится Explorer-style «командной панелью»: New / Sort / View всегда видимы, Cut/Copy/Paste/Rename/Share/Delete — контекстно при выделении. |
| Заголовок панели (путь + сортировка + quick search) | `States/FilePanels/PanelViewHeader.h/.mm`, `PanelViewHeaderTheme.h` | **rebuild, разделить на 3** | Сейчас 3 разные функции живут в одном классе. Разбивается на: (а) breadcrumb-адресную строку, (б) quick-search как отдельный оверлей, (в) сортировку — переезжает в командную панель/заголовки колонок. |
| Футер панели (статистика) | `States/FilePanels/PanelViewFooter.h/.mm`, `PanelViewFooterTheme.h`, `PanelViewFooterVolumeInfoFetcher.h` | **reskin** | Уже получает готовые `VFSListingItem`/`ItemVolatileData`/Statistics — чисто рендер-таргет. Меняется только визуал под статус-бар Explorer (количество/размер выделения слева, переключатель видов справа). |
| List View (детальный режим) | `States/FilePanels/List/PanelListView.h`, `PanelListViewTableHeaderView.h`, `PanelListViewTableHeaderCell.swift` | **reskin, становится view-mode по умолчанию** | Уже `NSTableView`-based с сортируемыми заголовками колонок — ближайший аналог Details View. Минимальные структурные правки, максимум — визуальные. |
| Brief View (Short/Medium) | `States/FilePanels/Brief/PanelBriefView.h` + layout-engines | **reskin** | `NSCollectionView`-based грид — переиспользуется под Icons (large/medium/small) режимы Explorer. |
| Gallery View | `States/FilePanels/Gallery/PanelGalleryView.h` | **reskin** | Переиспользуется под Content/Gallery-подобный режим. |
| Панель вкладок | `Source/Panel/include/Panel/UI/PanelTabBarView.h`, `States/FilePanels/Views/FilePanelsTabbedHolder.h` | **reskin** | Уже чистый view-слой (без ссылок на PanelData/VFS), управляется темой через протокол. Требуется только визуальный рестайл под браузероподобные вкладки Explorer. |
| Контекстное меню | `States/FilePanels/ContextMenu.h/.mm` | **rebuild содержимого** | Меню строится напрямую из живых `VFSListingItem` + `PanelController` — реструктуризация состава пунктов (см. Design-документ) требует правок именно здесь, не только визуала. |
| Go-To popup / Quick Lists | `States/FilePanels/Actions/ShowGoToPopup.h/.mm`, виджет `Source/CUI/include/CUI/CommandPopover.h` | **переиспользовать как источник данных** | `GoToPopupListActionMediator` уже агрегирует Favorites/History/Connections/Tags/Volumes — это ровно тот набор данных, который нужен новому sidebar. Виджет `NCCommandPopover` — общий, некомпетентен в панелях/VFS. |
| Sidebar / Navigation Pane | — | **new** | Сегодня в NC нет постоянного sidebar — вся навигация через модальные popup’ы. Новый компонент строится на тех же источниках данных, что и Go-To popup (см. выше), плюс `NativeFSManager` (тома → «Этот Mac»), `NetworkConnectionsManager` (сеть), `TagsStorage` (метки). |
| Диалог копирования/перемещения | `Source/Operations/source/Copying/CopyingDialog.h/.mm` | **reskin** | `NSWindowController`, вызывается только из `Copying.mm`; движок (`CopyingJob`) не видит и не должен видеть. |
| Диалог конфликта («уже существует») | `Source/Operations/source/Copying/FileAlreadyExistDialog.h/.mm` | **reskin** | Аналог Windows «Replace or Skip Files»; превью можно строить на готовых thumbnails из `VFSIcon`. |
| Диалог удаления | `Source/Operations/source/Deletion/DeletionDialog.h/.mm` | **reskin** | Переименовать/оформить под метафору «Корзины». |
| Прогресс операций (в окне) | `Source/Operations/source/Pool.h/.mm`, `PoolView.h/.mm`, `PoolViewController.h/.mm`, `BriefOperationView*.h/.mm` | **reskin** | Слой поверх `Statistics`/`AggregateProgressTracker`, независим от конкретных Job. |
| Тулбар вьювера | `States/InternalViewer/MainWindowInternalViewerState.h/.mm` | **new (де-факто)** | `windowStateToolbar` сегодня возвращает неподключенный IBOutlet — тулбара фактически нет ни у встроенного, ни у отдельного окна вьювера (`Viewer/InternalViewerWindowController.xib` тоже без тулбара). |
| Тулбар терминала | `States/Terminal/ShellState.mm` | **new** | `windowStateToolbar` явно возвращает `nil` — сегодня у терминала нет вообще никакого чрома. |
| Футер/поиск вьювера | `Source/Viewer/include/Viewer/ViewerFooter.h`, `ViewerSearchView.h` | **reskin** | Чистый программный AppKit-чром, палитра уже приходит через абстракцию `nc::viewer::Theme` → `ThemeAdaptor.h`. |
| Окно настроек | `Preferences/PreferencesWindow.swift`, `Preferences.mm`, 8 вкладок `PreferencesWindow*Tab.h` | **reskin** | Уже сделано в стиле системных настроек macOS (Swift-контроллер + toolbar-переключатель вкладок) — самая низкорисковая зона, минимум структурных изменений. |
| Диалог атрибутов | `Command > File Attributes` (Ctrl+A) | **rebuild под «Свойства»** | Становится основой вкладки «Разрешения» нового диалога Properties (см. Design-документ §10). |
| Содержимое JSON-тем | данные `ThemesManager`/`ThemePersistence` | **новый контент, старый движок** | Авторские новые темы Light/Dark «Explorer», без переписывания `Theme.h`/`ThemesManager.h`. |
| Иконка приложения, bundle ID, имя | `NimbleCommander.xcodeproj/project.pbxproj`, `Resources/*-Info.plist`, `Resources/Icon.icns` | **new identity** | `info.filesmanager.Files` → новый bundle id; новое имя вместо `PRODUCT_NAME = "Nimble Commander"`. |

---

## 4. Точечные новые «механики» (явно помечено — это не просто скин)

Задача — «не сильно менять существующие механики», но пара мест требует минимального нового состояния/поведения, чтобы ощущение «прямо как в Windows» не расклеилось. Список специально короткий и явный:

1. **Визуальное состояние Cut («вырезано»).** У NC уже есть Copy/Move через pasteboard (`Cmd+C`/`Cmd+V`/`Opt+Cmd+V`), но нет предварительной пометки «вырезанного» элемента (в Explorer вырезанные иконки становятся полупрозрачными до вставки/отмены). Нужен: (а) набор «помечено как cut» на стороне UI-контроллера панели, (б) флаг в presentation-слое (`NCPanelViewPresentationProtocol`-реализациях) для затемнения иконки. Данные не трогаем, только рендер + маленький стейт в контроллере.
2. **Properties → расчёт размера папки в «Общих» сведениях.** Не новая механика по сути (уже есть `Calculate Folder Sizes`, `Shift+Opt+Return`), но диалогу Properties нужно уметь дергать этот расчёт по требованию, а не только через отдельный пункт меню — небольшая интеграционная склейка, не новый движок.
3. **(Опционально/stretch) Один шаг Undo для последней операции.** В Windows Explorer `Ctrl+Z` отменяет последнее файловое действие. У NC такого нет вовсе — это честно новая механика (не просто UI), а не просто скин. Рекомендация: не включать в MVP, вынести в бэклог после того, как визуальный слой стабилизируется (см. §6, Фаза 15, «дорожная карта после релиза»).
4. **Корзина vs Trash.** Решение: **не** изобретать свою «Корзину» — оставить нативный macOS Trash (`Cmd+Backspace`, `DeletionJob`), только переименовать в UI на «Корзина»/«Recycle Bin» и оформить диалог в стиле Explorer. Это чистый reskin, не механика — явно записано, чтобы не соблазниться писать собственный undelete-слой без необходимости.

---

## 5. Стратегия выполнения

1. **Инкрементально, не «большим взрывом».** Каждая фаза должна оставлять приложение в собираемом и используемом состоянии. Механизм `NCMainWindowState` и протокол-ориентированные presentation-классы (`NCPanelViewPresentationProtocol`) это позволяют: новый компонент подключается рядом со старым, старый выключается в последнюю очередь.
2. **Отдельная тема как feature-flag.** Пока новый визуальный слой не готов целиком, разработку стоит вести за новой темой/сборочной схемой, а не удалять старый UI — `ThemesManager` уже поддерживает переключение конфигураций в рантайме.
3. **Git-стратегия.** Форкать с сохранением upstream remote (`git remote add upstream https://github.com/mikekazakov/nimble-commander`), чтобы иметь возможность подтягивать апстрим-фиксы в нетронутых модулях (VFS/Operations/Viewer/Term/Base/Utility) без потери собственной истории UI-слоя. Периодически (например, раз в релизный цикл апстрима) делать `merge`/`cherry-pick` профильных багфиксов из движковых директорий.
4. **AppKit, не полный переход на SwiftUI.** Список файлов (`PanelListView`/`PanelBriefView`/`PanelGalleryView`) должен остаться на AppKit/`NSTableView`/`NSCollectionView` — производительность на больших директориях (десятки/сотни тысяч файлов) via VFS уже отлажена именно на AppKit-рендеринге. Новые самостоятельные поверхности (Sidebar, Command Bar, Properties, Preferences — уже частично на Swift) можно и стоит делать на SwiftUI, поднятым через `NSHostingController`, где это не требует rewrite существующего.
5. **Не копировать проприетарные ассеты Microsoft.** Segoe UI/Segoe UI Variable — лицензируемый шрифт Microsoft, недоступный на macOS из коробки и не подлежащий свободному распространению; Segoe Fluent Icons — тоже собственность Microsoft. Визуальный язык — «в духе», собственные шрифты (системные на macOS) и оригинальный набор иконок (см. Design-документ §12).

---

## 6. Фазы и этапы (roadmap)

Оценки — для одного full-stack macOS/AppKit-разработчика (в духе того, как сам NC поддерживается «одним контрибьютором», согласно `Docs/Help.md`, FAQ), работающего с постоянной вовлечённостью. При частичной занятости (10–20 ч/нед) сроки нужно умножать на 2–3.

| # | Фаза | Что делаем | Ключевые файлы/точки входа | Готовность (demo criteria) | Оценка |
|---|---|---|---|---|---|
| 0 | Discovery и прототип | Выбор имени/бренда/bundle id; кликабельный прототип раскладки (Figma) на реальном контенте; аудит `NSOutlineView`/`NSToolbar`/`NSVisualEffectView` под sidebar/command bar; согласование объёма Фазы 1–14 с этим документом | — | Утверждённый макет + бренд-имя | 2–3 нед |
| 1 | Каркас окна | Новый `NSToolbar` для `MainWindowFilePanelState` вместо `MainWindowFilePanelsStateToolbarDelegate`; базовая раскладка content view (sidebar-слот + панель-слот + статус-бар-слот) | `States/FilePanels/MainWindowFilePanelState.*`, новый `*ToolbarDelegate` | Окно открывается с новым скелетом, старые панели рендерятся внутри | 2 нед |
| 2 | Sidebar / Navigation Pane | Новый `NSOutlineView`-компонент; источники — `FavoriteLocationsStorage`, `NativeFSManager`, `NetworkConnectionsManager`, `TagsStorage` (переиспользовать логику `GoToPopupListActionMediator`) | `States/FilePanels/Actions/ShowGoToPopup.mm` (референс), `Favorites.h` | Клик по разделу sidebar меняет путь активной панели | 3 нед |
| 3 | Breadcrumb Address Bar | Разбор `PanelViewHeader` на breadcrumb + quick-search-оверлей; сегменты пути кликабельны, дропдаун соседей, режим редактирования как текст | `States/FilePanels/PanelViewHeader.*` | Адресная строка полностью заменяет старый заголовок панели | 3 нед |
| 4 | Список файлов (Details/Icons/Content) | Рестайл `PanelListView`/`PanelBriefView`/`PanelGalleryView` под новую палитру и типографику; колонки, группировка (см. §4.2 Design-документа) | `States/FilePanels/List/*`, `Brief/*`, `Gallery/*` | Все три текущих режима визуально соответствуют новому языку | 4 нед |
| 5 | Статус-бар | Рестайл `PanelViewFooter` под левый/правый блок в духе Explorer | `PanelViewFooter.*` | Статус-бар показывает счётчик/размер выделения и переключатель видов | 1 нед |
| 6 | Командная панель | Полная сборка New/Sort/View/Cut/Copy/Paste/Rename/Share/Delete с контекстной сменой состава | `MainWindowFilePanelsStateToolbarDelegate` (замена) | Кнопки триггерят существующие Actions (`Command > ...`) без дублирования логики | 3 нед |
| 7 | Контекстное меню | Реструктуризация `ContextMenu.mm`: ряд иконок Cut/Copy/Rename/Share/Delete, подменю New, группировка редких пунктов | `ContextMenu.h/.mm` | Меню открывается с новым составом, все действия работают как раньше | 2 нед |
| 8 | Диалоги операций | Рестайл `CopyingDialog`, `FileAlreadyExistDialog`, `DeletionDialog`, `PoolView`/`BriefOperationView` | `Source/Operations/source/**/*Dialog.mm`, `PoolView*.mm` | Копирование/удаление визуально в новом языке, поведение не изменилось | 3 нед |
| 9 | Properties | Новый диалог поверх существующего Attributes-движка + Statistics (расчёт размера) | новый контроллер + переиспользование `Command > File Attributes` | Alt+Enter/аналог открывает вкладки «Общие»/«Разрешения» | 2 нед |
| 10 | Вьювер и терминал: чром | Реальный `NSToolbar` через уже существующий протокольный хук (`windowStateToolbar`); рестайл `ViewerFooter`/`ViewerSearchView` | `MainWindowInternalViewerState.mm`, `ShellState.mm`, `Viewer/ViewerFooter.*` | У вьювера и терминала появляется тулбар в общем языке | 3 нед |
| 11 | Настройки | Визуальный рестайл 8 вкладок без структурных изменений | `Preferences/*.h/.mm/.swift` | Все вкладки в новом визуальном языке | 2 нед |
| 12 | Темы | Авторские JSON-темы Light/Dark «Explorer»; поддержка system accent color (`NSColor.controlAccentColor`) | данные `ThemesManager`, новые `.json` | Переключение system light/dark меняет приложение целиком | 2 нед |
| 13 | Иконки и визуальные ассеты | Оригинальный набор моно-линейных иконок (см. Design-документ §12); новая app icon | новые ассеты | Полный набор toolbar/sidebar/file-type иконок в едином языке | 4–6 нед (может идти параллельно, если есть дизайнер) |
| 14 | Windows-раскладка хоткеев | Новый пресет в `ActionsShortcutsManager` (см. Design-документ §13) — **не замена дефолтов**, а выбираемая раскладка в Preferences > Hotkeys | `Preferences/PreferencesWindowHotkeysTab.h` | Пользователь может переключиться на «Windows-style» набор сочетаний | 1 нед |
| 15 | QA, доступность, локализация | Ручной чек-лист паритета с Explorer; VoiceOver-labels для новых компонентов (sidebar, breadcrumb, command bar); прогон существующих `_UT`/`_IT` | всё дерево `tests/` | Зелёный CI, чек-лист закрыт | 2–3 нед |
| 16 | Ребрендинг и дистрибуция | Новый bundle id/иконка/имя; обновление `Info.plist` (MAS и non-MAS), CI (`build.yml`/`nightly.yml` как стартовая точка), README/CONTRIBUTING/NOTICE с указанием происхождения от NC | `*.pbxproj`, `Resources/*-Info.plist`, `.github/workflows/*` | Подписанная сборка ставится и обновляется (Sparkle/App Store — по выбору) | 2 нед |

**Итого:** ориентировочно **41–46 недель** (≈ 9.5–11 месяцев) при полной вовлечённости одного разработчика; фазы 2–11 частично независимы и могут идти параллельно при наличии более чем одного контрибьютора (например, sidebar и командная панель не блокируют друг друга после Фазы 1).

---

## 7. Риски и открытые вопросы

| Риск/вопрос | Обсуждение | Рекомендация |
|---|---|---|
| Сохранять ли двухпанельный режим? | Это ключевая механика NC (`Shift+Cmd+P` переключает dual/single-pane уже сегодня). Explorer — однопанельный. | Не убирать. По умолчанию открываться в одиночном режиме (использовать существующий toggle), но оставить «Split View» как один клик/хоткей для опытных пользователей — специально описано как способ **не терять механику**, только сменить дефолт. |
| Юридический риск копирования иконок/названия | «Windows», «File Explorer», Segoe UI/Segoe Fluent Icons — собственность Microsoft. | Не использовать чужие ассеты и wordmarks; визуальный язык — «в духе», не факсимиле. См. §5.5 и Design-документ §12. |
| AppKit vs SwiftUI | Полный переход рискован для сложных `NSTableView`/`NSCollectionView` списков с VFS-данными. | Гибрид: список файлов — AppKit; новые самостоятельные поверхности (sidebar/command bar/properties) — SwiftUI через `NSHostingController`, где не требует rewrite. |
| Совместимость конфигов при апгрейде | `Source/Config` хранит настройки в JSON под `~/Library/Application Support/<имя>/Config`; смена имени приложения при ребрендинге создаёт новую пустую папку. | Написать миграцию (одноразовое копирование `~/Library/Application Support/Nimble Commander/*` → новую директорию) в Фазе 16, либо явно предупредить пользователя, что это новый продукт без автоимпорта. |
| MAS (App Store) vs прямая дистрибуция | Сегодня поддерживаются оба варианта (`NimbleCommander-MAS-Info.plist`/`NimbleCommander-NonMAS-Info.plist`), с урезанным функционалом в песочнице (нет Admin Mode, терминала, монтирования сетевых шар — см. `Docs/Help.md`, «Version Differences»). | Решить на Фазе 0: если приоритет — «максимально похоже на Explorer», часть функционала (терминал/сеть) всё равно важна → скорее всего прямая дистрибуция первична, MAS — вторично/опционально. |
| Ограниченность ресурсов (потенциально 1 контрибьютор) | NC сам об этом прямо пишет в FAQ. | Приоритизировать Фазы 1–7 (то, что визуально «продаёт» идею) и Фазу 16 (дистрибуция), Фазы 10/13/14 можно сдвигать. |
| Реструктуризация `ContextMenu.mm`/`ShowGoToPopup.mm` | Это не чисто вьюха — логика построения меню/попапов завязана на `PanelController`/VFS напрямую. | Планировать эти фазы (2 и 7) с запасом по времени и тестированием регрессий вручную — автотестов на состав меню нет и вряд ли стоит их писать. |

---

## 8. Тестирование

- **Не ломать существующее.** Полный прогон `_UT`/`_IT` таргетов после каждой фазы, затрагивающей файлы рядом с движком (Operations, Panel data-слой, Viewer/Term backend).
- **Ручной чек-лист паритета** — на основе таблиц соответствия из Design-документа (§15): каждый пункт «элемент Explorer → элемент нового UI» проверяется вручную сценарием (открыть, найти взглядом, воспроизвести действие).
- **UI-снапшот-тесты не заводим** — для AppKit-рендеринга это дорого в поддержке относительно пользы; вместо этого — чек-лист + скриншот-сравнение вручную перед релизом (как это уже делает NC, см. `Docs/ScreenshotsAutomation`).

---

## 9. Открытие исходного кода

- Лицензия остаётся **GPLv3** (наследуется от NC, `LICENSE.md`), с сохранением copyright-заголовков там, где код не переписан с нуля.
- В README/CONTRIBUTING/NOTICE — явное указание, что проект является форком [Nimble Commander](https://github.com/mikekazakov/nimble-commander) (© Michael Kazakov), с описанием, что именно изменено (UI-слой) и что нет (движок).
- Новый репозиторий, новое имя, README/CONTRIBUTING/CODE_OF_CONDUCT — адаптировать под новый бренд, но структуру (`Docs/Building.md`-подобный гайд, `Docs/Help.md`-подобная документация) можно унаследовать как шаблон.
- CI: `.github/workflows/build.yml` и `nightly.yml` — рабочая стартовая точка, адаптировать под новый bundle id/схему.

---

## 10. Открытые решения для Фазы 0 (нужен ответ до старта)

1. Имя продукта, bundle id, домен, репозиторий.
2. MAS-сборка нужна с самого начала или откладывается?
3. Кто, кроме автора, участвует (дизайнер для иконок — Фаза 13 сильно от этого зависит по срокам)?
4. Судьба «оверлапнутого» терминала внутри панели (`FilePanelOverlappedTerminal.h`) — фича специфична для orthodox file manager UX и не имеет аналога в Explorer; оставить как «продвинутую» опцию или спрятать по умолчанию?
