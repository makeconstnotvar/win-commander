# Feature: `view.toggleHiddenFiles` Command Registry slice

> Status: M1 production routing implemented
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 9, 10, 25, 28, 40 and 44
> Execution tracker: `Docs/Development-Plan.md`, R3/R4 in `Docs/refactor_plan.md`

## Scope

`view.toggleHiddenFiles` is the shared View command that changes whether the active pane includes hidden entries. This slice unifies command identity, state, checked presentation, invocation-source tracking, and synchronous mutation across the main menu, its persisted keyboard shortcut, and the Explorer View popover.

The command changes the existing `PanelData` hard-filter value through `PanelController::changeHardFilteringTo`. The established panel filtering, refresh, and `PanelDataOptionsPersistence` paths remain responsible for applying the value and restoring it with the pane state.

## Command contract

- stable id: `view.toggleHiddenFiles`;
- category: View;
- legacy responder selector: `ToggleViewHiddenFiles:`;
- legacy shortcut action: `menu.view.sorting_view_hidden`, tag `13140`;
- synchronous context: invocation source, native sender, live pane target, and an optional current `show_hidden` value;
- descriptor semantics: non-destructive, no `OperationPlan`, no undo claim.

Menu and shortcut state read the current controller value. Explorer presentation uses the matching `PaneSnapshot::state.shows_hidden_files`; before that snapshot arrives, or after a foreign-pane snapshot, the optional value is absent and state evaluation fails closed. Execution always resamples the live controller value, computes the requested value once, and passes it to the application-owned setter together with the borrowed pane target.

## State and disabled reasons

The command is enabled when the context carries both a live pane target and the current hidden-files state. Its `CommandCheckState` is `On` when hidden entries are shown and `Off` when they are filtered out.

Structured disabled states cover:

- missing pane target: `context.paneTargetRequired`;
- unavailable view-state snapshot: `context.hiddenFilesStateRequired`.

The Visual State mapper preserves the checked state while normalizing visibility and availability. The AppKit presentation adapter maps it to `NSMenuItem.state` and `NSButton.state`; localized disabled reasons continue through tooltip and accessibility help.

## Production entry points

- the View menu item through `ToggleViewHiddenFiles:`;
- its persisted `Command-Option-I` shortcut through the same responder action;
- the Explorer View popover through an explicit Toolbar invocation.

All entry points query or execute the same Registry definition. The dispatcher intercepts the selector before the legacy action map. Menu and shortcut presentation sample `PanelData::HardFiltering().show_hidden`; the Explorer state observer instead sends the initial and subsequent matching `PaneSnapshot` values to the command bar. The popover derives its checked icon and availability feedback from that Store-backed value, then executes through the explicit Toolbar route against the live controller.

## Failure boundary

The application setter copies the current hard-filter value, changes only `show_hidden`, applies it through `changeHardFilteringTo`, and reports success only when the controller exposes the requested value afterward. A missing controller or failed application raises `ViewToggleHiddenFilesError` from the command handler.

Disabled execution does not call the setter. C++ exceptions are contained at the Objective-C++ dispatcher boundary, and a non-executed result produces the established audible feedback. The legacy `ToggleSortingShowHidden` object remains a compatibility registration but is no longer the production execution route for this selector.

## Verified coverage

Focused command tests cover descriptor and legacy metadata, missing target/state reasons, On/Off projection, all invocation sources, exactly-once inversion, setter failure, and missing-handler registration. Visual-state and AppKit presentation tests cover checked-state preservation and mapping for visible, hidden, enabled, and disabled command states. Pane projection coverage verifies unloaded/loaded filter state, filter-only revision, and scoped production-bridge rebuild without listing-generation advancement.

The Debug `WinCommanderUT` scheme build succeeded. The command plus seven-stable-ID filter passed 7 cases / 70 assertions; the combined command/registry/mapper/presentation filter passed 28 / 372. Current aggregate evidence is maintained in `Docs/Development-Plan.md`. The hosted M0 run remains a separate publication gate.

## Remaining work

- migrate the other View commands to stable IDs and the same checked-state path;
- provide a generic enabled/checked presentation contract for `NCCommandPopoverItem` instead of command-specific icon projection;
- add command-palette and shortcut-remapping surfaces over the Registry descriptor;
- complete manual keyboard, VoiceOver, and visual evidence for the View menu and Explorer popover.
