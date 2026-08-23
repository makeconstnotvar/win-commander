# Explorer interaction contract

> Статус: точечные Explorer interaction fixes реализованы; полная end-to-end матрица остаётся отдельным accessibility/UI gate.
>
> Этот документ задаёт проверяемое поведение. Само наличие пункта здесь не подтверждает его реализацию.

## Цель

Explorer сохраняет один предсказуемый язык взаимодействия для указателя, клавиатуры и accessibility API. Визуальные состояния отражают фактическое состояние команды или объекта, а все команды проходят через существующие Registry, Store и controller-контракты.

Проектный размер обычной интерактивной цели — не менее `28 × 28 pt`; `20 × 20 pt` является нижней границей для macOS. Видимый символ занимает оптическую область `13–16 pt`, а кликабельной остаётся вся кнопка или строка. Адресная строка и поиск используют поверхность высотой `34 pt` из `win-commander.pen` с нативным контролом высотой `28 pt` внутри. Основной шрифт контролов — системный `13 pt`.

## Инженерный baseline

Источники ниже определяют направление проектирования и тестов:

- Apple HIG: [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility), [Focus and selection](https://developer.apple.com/design/human-interface-guidelines/focus-and-selection/), [Toolbars](https://developer.apple.com/design/human-interface-guidelines/toolbars), [Sidebars](https://developer.apple.com/design/human-interface-guidelines/sidebars), [Disclosure controls](https://developer.apple.com/design/human-interface-guidelines/disclosure-controls), [Keyboards](https://developer.apple.com/design/human-interface-guidelines/keyboards), [Buttons](https://developer.apple.com/design/human-interface-guidelines/buttons) и [Pointing devices](https://developer.apple.com/design/human-interface-guidelines/pointing-devices);
- AppKit: [`NSOutlineView`](https://developer.apple.com/documentation/appkit/nsoutlineview), [`NSControl.isEnabled`](https://developer.apple.com/documentation/appkit/nscontrol/isenabled) и [`XCUIElement`](https://developer.apple.com/documentation/xcuiautomation/xcuielement);
- W3C: [WCAG 2.2](https://www.w3.org/TR/WCAG22/), [APG Tree View](https://www.w3.org/WAI/ARIA/apg/patterns/treeview/) и [Keyboard Interface](https://www.w3.org/WAI/ARIA/apg/practices/keyboard-interface/).

Apple HIG и WCAG используются как инженерный baseline. WCAG описывает web content, а APG — web-компоненты; их применение к AppKit не является заявлением о формальной сертификации native-приложения. Нативные роли, состояния и действия выражаются через AppKit accessibility API.

## Общая модель состояний

- `hover` даёт нейтральную подсветку и не меняет selection, focus, disclosure или навигацию;
- `pressed` подтверждает принятый ввод до выполнения действия;
- `focused` использует системное focus-оформление: ring для полей, полнострочную подсветку для списков;
- активная selection использует accent-подсветку; при уходе focus или потере key-window selection сохраняется как системная inactive-подсветка;
- `disabled` публикуется через `isEnabled = false`, не принимает pointer/keyboard action и содержит accessibility help с причиной;
- asynchronous loading, progress, success, cancellation и failure имеют отдельные текстовые и accessibility-состояния; delayed result принимается только для совпадающего pane, request и data generation.

## Матрица интерактивности

| Поверхность | Click / pointer | Hover / pressed | Keyboard и focus | Selection / disabled / AX |
|---|---|---|---|---|
| Toolbar navigation: Back, Forward, Up, Refresh, Commander Mode | Primary click выполняет ровно одну Registry-команду; secondary click не подменяет действие | Системная button-подсветка; tooltip называет действие | Full Keyboard Access достигает кнопку; `Space`/`Return` выполняет её; зарегистрированный shortcut ведёт в ту же команду | `enabled` следует exact matching pane snapshot; disabled не dispatch-ит действие; доступны label, help и enabled state |
| Address / Search | Клик в breadcrumb активирует сегмент; клик по полю переводит его в editing; search открывает Search Mode | Поверхность и поле различимы без увеличения glyph; hover не коммитит путь и не запускает поиск | `Cmd-L` фокусирует address, `Return` коммитит, `Escape` отменяет edit; `Cmd-F` фокусирует Find Files; `Tab` продолжает системный focus order | Address публикует текущий путь и editable state; search — label, placeholder и value; invalid/busy state выражен текстом и AX, а commit повторно проверяет live controller |
| Sidebar section | Клик по всей строке заголовка раскрывает или сворачивает только свою секцию | Нейтральный hover; chevron показывает collapsed/expanded | `Space`/`Return` переключает секцию; `Right` раскрывает, `Left` сворачивает; `Up`/`Down` идут по видимым строкам | Заголовок не является location selection; expanded state доступен AX; после collapse focus возвращается в заголовок, если был внутри скрытого subtree |
| Sidebar location | Primary click устанавливает единственную selection и отправляет одну navigation request; secondary click открывает контекст места | Hover слабее selection и не выполняет навигацию | `Up`/`Down` перемещает single selection как в native source list; `Return` повторяет default action; type-ahead ищет видимую строку | Только одна location selected; inactive selection остаётся видимой; label, role, selected, enabled и help доступны AX; stale navigation result не меняет новый выбор |
| File row / list | Click выбирает строку, `Cmd-click` меняет один элемент, `Shift-click` расширяет диапазон; double-click открывает; secondary click сохраняет exact-item context | Hover не меняет selection; drag state отделён от hover и pressed | Стрелки двигают focus/cursor, `Return` открывает, `Space` вызывает Preview; `Cmd-A` сохраняет приоритет AppKit text editor и иначе выбирает видимые строки | Multi-selection отделена от единственного focus/cursor; dot-dot и скрытая projection следуют существующим правилам; active/inactive selection различимы; AX сообщает row identity, selected и focused |
| Command buttons / popovers | Кнопка вызывает Registry action или открывает один связанный popover; click outside закрывает transient surface | Нативные hover, pressed и disabled states; icon-only action имеет tooltip | `Space`/`Return` открывает; стрелки перемещают menu focus; `Return` выполняет; `Escape` закрывает и возвращает focus в anchor | Disabled action не открывает popover и не выполняется shortcut-ом; menu item сообщает title, enabled/check state и reason; execution повторно проверяет live context |
| Tabs | Click выбирает вкладку; close и new-tab имеют отдельные цели | Hover показывает доступную tab-action, не меняя active tab | Системный focus order и зарегистрированные tab shortcuts выбирают тот же tab identity; focus возвращается в соответствующий pane | Одна active tab; inactive tabs сохраняют title/state без selection styling; AX сообщает tab role, title и selected state; session restore использует стабильную identity |
| Status bar | Статический текст не перехватывает click; отдельная action оформляется кнопкой | Информационный hover отсутствует либо показывает системный tooltip | Статический статус исключён из tab order; action доступна Full Keyboard Access | AX объединяет счётчик, свободное место и status message в осмысленные значения; обновление относится к exact pane snapshot |
| Inspector | Выбор файла обновляет read-only данные; собственные действия используют отдельные controls | Hover применяется только к интерактивным controls | Read-only labels не входят в tab order; controls следуют системному порядку | Inspector принимает только matching snapshot/focus identity; empty, multi-selection и unavailable имеют явный текст и AX-состояние |
| Operation progress | Cancel действует на exact active operation; открытие центра использует отдельную Registry-команду | Progress не имитирует кнопку; Cancel имеет стандартные состояния | Full Keyboard Access достигает Cancel/Open; shortcut проходит тот же Registry route | Progress публикует phase, current item, rate, ETA и terminal state; Cancel enabled только при наличии точной cancel authority; stale record не управляет другой операцией |
| Search Mode | Start, Cancel, Reveal Original и Close имеют отдельные цели; result row следует file-list interaction | Hover не меняет query, scope или result selection | `Cmd-F` фокусирует query; `Tab` проходит controls; `Return` запускает допустимый запрос; `Escape` закрывает transient mode или отменяет editing согласно текущему focus | Query, scope, backend, phase, counts, limitations и progress доступны текстом и AX; Cancel/Reveal enabled только для matching run/result identity; per-tab state сохраняется отдельно |

## Реализованный срез

- Sidebar работает как AppKit source list: системный размер строк следует настройке macOS, glyph использует 20 pt, заголовок — системный 13 pt semibold, а клик по всей строке секции меняет её expanded state.
- Sidebar допускает одну location selection. Обычная и inactive selection перерисовываются поверх непрозрачного semantic background, поэтому снятая selection и свернувшиеся строки не остаются в backing store.
- File List сохраняет поддерживаемую продуктом semantic multi-selection, но hover остаётся отдельным transient state. Row reuse, mouse exit и деактивация очищают hover; focused, semantic-selected, hover и ordinary fills разрешаются в непрозрачный цвет.
- Unified toolbar использует navigation hit frames `32 × 30 pt` и 15 pt SF Symbols. Address item имеет высоту `34 pt`; location, editor и search — `28 pt`; search width — `230 pt`.
- Address editor закрывается при фактическом уходе first responder, восстанавливает breadcrumb и сохраняет новый responder. `Escape` отменяет editing и возвращает focus в panel view.
- `View ▸ Show Hidden Files` использует `⇧⌘.`. Explorer state принимает menu/shortcut action при focus в address, sidebar или другом chrome и маршрутизирует её в dispatcher активной панели с live Registry validation.

## Test contract

### Unit и AppKit geometry/state

- каждый интерактивный frame соответствует проектной цели `28 pt` и нижней границе `20 pt`; symbol frame проверяется отдельно от hit frame;
- toolbar, address/search, sidebar rows и command buttons имеют согласованные высоты, baseline, center и system control size;
- hover не меняет model state; pressed и disabled принимаются из native control state;
- active/inactive selection, focus, expanded, enabled, checked, busy и error строятся из явного snapshot/store state;
- disabled pointer, keyboard и shortcut paths завершаются без dispatch; accepted execution повторно проверяет live controller;
- collapse, tab switch, pane rebind и delayed callback сохраняют точную identity и отбрасывают stale payload.

### XCUITest

- элементы находятся по стабильным accessibility identifier; assertions проверяют `exists`, `isHittable`, `frame`, `isEnabled`, `isSelected`, focus, label и value;
- ожидания используют изменение свойства или состояния, а не фиксированный sleep;
- для каждой action проверяются `click`, keyboard equivalent и, где применимо, shortcut; для строк — hover, click, modifier-click, double-click и secondary click;
- disclosure открывается и закрывается pointer-ом и клавиатурой; меняется только его subtree, а focus после collapse остаётся предсказуемым;
- active selection становится inactive при переводе focus и снова active при возврате без потери selected identity;
- popover получает focus, выполняет enabled item, отклоняет disabled item и по `Escape` возвращает focus в anchor;
- Light, Dark, Increased Contrast, key/inactive window и Full Keyboard Access проходят один и тот же semantic сценарий.

### Accessibility и визуальная проверка

- Accessibility Inspector подтверждает role, label, help, value, selected, expanded, enabled и focus order;
- VoiceOver walkthrough проверяет toolbar, address/search, sidebar disclosure/location, file selection, popover, progress и Search Mode;
- screenshot/pixel comparison подтверждает scale, alignment и состояния, а semantic assertions остаются источником истины для поведения;
- contrast проверяется по фактическим semantic colors для Light, Dark и Increased Contrast.

## Границы evidence

Unit-тест подтверждает только контракт pure/state или AppKit-объектов, который он непосредственно создаёт. XCUITest подтверждает только запущенные сценарии, конфигурацию appearance и accessibility tree конкретной сборки. Screenshot подтверждает видимую геометрию и не доказывает keyboard, AX, stale-state fencing или execution authority. Ручной smoke подтверждает перечисленные действия и не расширяется на соседние поверхности.

Заявление о полном Apple UI, WCAG, VoiceOver, Full Keyboard Access, remote-provider или release-соответствии появляется только после отдельного прогона соответствующей матрицы с сохранённой командой, конфигурацией, результатом и средой. `.pen` остаётся визуальным проектным источником, а production behavior и безопасность определяют код, Registry/Store/controller-контракты и executable tests.

## Evidence

- Debug `WinCommanderUT` scheme build — passed.
- Debug `WinCommander-Unsigned` application build — passed.
- `WinCommanderUT 'Explorer presentation geometry *' --rng-seed 424242` — 38 cases / 1,418 assertions passed, including field-editor responder routing for the hidden-files command.
- `WinCommanderUT 'NCExplorerState tabs *' --rng-seed 424242` — 25 cases / 306 assertions passed.
- `WinCommanderUT 'nc::core::ToggleHiddenFilesCommand *' --rng-seed 424242` — 6 cases / 62 assertions passed.
- Full Debug `WinCommanderUT --rng-seed 424242` — 925 cases / 12,507 assertions passed.
- `Scripts/build_stable_dev_and_run.sh` built, signed, installed and launched the canonical local runtime at `~/Applications/WinCommander-Codex.app`; bundle ID `com.wincommander.App.CodexDev`, signing certificate `66CA97F3581582C97871BCC0DFC11BEAB4C65C83` and the pinned designated requirement were verified.
- Signed-runtime smoke confirmed that a click on the whole `Favorites` header collapses only its subtree without retained row pixels; address editing returns to breadcrumb presentation after a file-row click while preserving the clicked responder; `⇧⌘.` toggles hidden entries while the address field editor owns first responder; the file list and sidebar each publish the expected selection semantics.
- The runtime was returned to its incoming user state after the smoke: hidden entries visible, `Favorites` expanded, address editor closed and `.aspnet` selected.
- Automated native XCUITest, VoiceOver walkthrough, Full Keyboard Access, Increased Contrast and measured contrast remain explicit gates; component tests and screenshots do not substitute for them.
