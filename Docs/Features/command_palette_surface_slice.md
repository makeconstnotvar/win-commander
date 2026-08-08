# Q2-3 CP-2: The command palette

> Status: implemented, built and tested — see §Verification.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-3.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §26.1 (P0: command palette), §12.2 (modern command palette), §11 (toolbar, context menu, command palette, shortcuts and menu bar use the same command definitions).
> Depends on: [`command_palette_filter_foundation.md`](command_palette_filter_foundation.md) (CP-1).

## Product surface

`View ▸ Command Palette…` (⌥⌘K) opens a keyboard-first overlay over Explorer: a query field above a ranked list of every command the registry currently offers. Typing narrows and re-ranks on each keystroke, ↑/↓ moves, Return runs, Escape dismisses. A double click is the pointer equivalent of Return, so the palette is keyboard-*first*, not keyboard-only.

Rows show the command's title and its category, and both are searchable — CP-1 ranks a category hit strictly below any title hit, so searching "file" surfaces the File-category commands without displacing a command actually named for what you typed.

The roster comes straight from the `CommandRegistry`, which is what §11 asks for: the palette is another projection of the same command definitions the menu bar, toolbar and context menus use, not a parallel list that can drift from them.

## The safety property: one context, queried and executed

The palette builds **one** `CommandContext` (`source = Palette`, `native_target` = the focused `PanelController`, `items` = the current selection) and uses it both to query each command's state when building the roster and to execute the chosen one.

That matters because a row is offered precisely because the registry reported it enabled *for that context*. Re-deriving a context between listing and running would let a command be offered under one set of facts and executed under another — the registry would still refuse anything genuinely invalid, but the palette would be making a promise it did not hold.

Two consequences are deliberate:

- **A command the registry reports as not visible never enters the roster.** Visibility is how the registry says a command does not apply here at all — as distinct from `enabled`, which says it applies but cannot run right now. A palette that offered the former would route around the registry's own answer. `BuildCommandPaletteRoster` enforces this, and a unit case checks the hidden command cannot even be found by querying its name.
- **A disabled command is listed but cannot be committed.** Finding a command greyed out answers "where is that, and why can't I use it?"; hiding it answers nothing. Committing a disabled row does nothing at all — closing the palette silently would look like the command had run.

The selection is re-read at execution time rather than captured when the palette opened. The palette takes the keyboard but not the world, and running against a stale selection would act on files the user is no longer pointing at.

## View boundaries

`NCExplorerCommandPaletteView` owns no command semantics whatsoever. It renders the roster it is handed, ranks it through `FilterCommandPalette`, and reports a chosen id back through its delegate. What the palette *can* run is therefore decided entirely by the roster its owner builds — the view cannot widen it.

Two behaviours worth naming:

- **Dismissal precedes the choice.** The delegate is told to dismiss before it is told what was chosen, so a command never begins running underneath a palette that is still on screen and still holding first responder.
- **Arrow keys clamp rather than wrap.** A held arrow settles at an end instead of cycling back around past it, so the selection cannot quietly travel to the opposite end of the list while the key is down.

Arrow keys and Return are intercepted while the query field holds focus (`control:textView:doCommandBySelector:`); the field would otherwise consume them as text editing and the list could never be driven from the keyboard.

## Verification

Built and run in this session (Xcode 26.6 toolchain):

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED** (compiles the edited `MainMenu.xib` and `Actions.h`).
- Focused `WinCommanderUT 'NCExplorerCommandPaletteView*' --rng-seed 424242`: **7/7 cases, 33/33 assertions** — roster listed with the first row preselected; a query narrowing and moving the selection to the new best match; arrow movement clamping at both ends; commit reporting exactly once with dismissal first; a disabled row and an empty result set both refusing to commit; a new roster resetting a stale query; an empty roster offering nothing.
- Focused `WinCommanderUT 'nc::core::BuildCommandPaletteRoster*' --rng-seed 424242`: **1/1 case, 8/8 assertions** — hidden, id-less and title-less commands excluded; a disabled command retained and marked; a hidden command unreachable even by querying its name.
- Focused `WinCommanderUT 'nc::core::FilterCommandPalette*' --rng-seed 424242`: **11/11 cases, 47/47 assertions** (unchanged from CP-1).
- Focused `WinCommanderUT '*ActionsShortcuts*' --rng-seed 424242`: **10/10 cases, 81/81 assertions** — the suite covering the action-name/tag tables this slice extended.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **656/656 cases, 11,005/11,005 assertions**.
- No ASAN/UBSAN run: AppKit-adjacent presentation over a pure model, calling existing registry entry points unchanged.

Shortcut selection is recorded because it was not free: ⇧⌘O (the conventional palette binding here) is already `menu.go.documents`, and ⇧⌘P is already `menu.view.switch_dual_single_mode`. ⌥⌘K was verified unused across both tables before being taken.

### Known coverage gaps

- The palette's roster against the **real** registry is not exercised: `WinCommanderUT` does not bootstrap the application's command composition (the same `g_Config`-class gap `Development-Plan.md` §2.1 records). The view is tested against an injected roster, and `BuildCommandPaletteRoster` is tested against injected sources; what is untested is the projection of the live 27-command registry through them.
- No local UI smoke — no tool in this session drives a running native macOS app. For this slice that covers the overlay's appearance, its placement over the pane, and focus handoff back to the panel on dismissal.
