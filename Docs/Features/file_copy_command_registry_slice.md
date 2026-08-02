# Feature: `file.copy` Command Registry slice

> Status: M1 production routing and command presentation implemented for `file.copy`; wider P0 migration remains
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 10, 26, 28, 29 and 39
> Execution tracker: `Docs/Development-Plan.md`, R3 in `Docs/refactor_plan.md`

## User problem

Copying file references to the clipboard must behave identically from the Explorer command bar, the main Edit menu, its keyboard shortcut, and the file context menu. Selection validation and execution belong to one command definition so an entry point cannot drift to a different rule or payload.

## Product behavior

`file.copy` writes selected local filesystem entries, or the focused local entry when the selection is empty, through the established system pasteboard writer. The context menu uses its clicked-item-or-selection payload. Each accepted invocation writes once.

This command represents clipboard Copy (`Command-C`). The destination-oriented Copy operation remains a separate operation command and continues through the existing Operations engine.

## Entry points

- Explorer command bar Copy button;
- main Edit menu `copy:` responder action;
- the shortcut assigned to the existing `menu.edit.copy` persistence key;
- file context menu Copy item.

AppKit keeps normal responder-chain precedence. A field editor handles text copy before the panel dispatcher becomes eligible.

## Command contract

- stable id: `file.copy`;
- legacy selector: `copy:`;
- legacy shortcut action: `menu.edit.copy`;
- legacy shortcut tag: `12000`;
- typed synchronous context: borrowed `span<ListingItem const>` plus invocation source;
- disabled reason key: `commands.file.copy.disabled.selectionEmpty`.
- provider reason key: `commands.file.copy.disabled.nativeItemsRequired`.

The application owns one `CommandRegistry` and injects it into panel dispatchers. The registered handler owns only the pasteboard-writer dependency; panel selection is resolved by the invoking adapter and remains valid for the synchronous call.

## Provider and operation boundaries

The system file-list pasteboard format accepts a context only when every item belongs to the native filesystem. Archive, remote, and mixed contexts return a provider-specific disabled reason as one unit, preserving exact payload semantics. The writer validates every host and UTF-8 path before mutating the pasteboard, so an unrepresentable path cannot replace existing clipboard contents with a partial file list. Clipboard Copy does not start a file mutation. Destination capability checks and `OperationPlan` apply to Copy To, Move, Paste, and drag/drop operation commands.

## Error and feedback behavior

An empty or provider-ineligible item context returns a localized structured disabled reason and does not call the writer. Pasteboard write failure preserves the existing audible feedback. C++ exceptions are contained at the Objective-C++ dispatcher boundary, including pane-selection snapshot construction.

## Accessibility

The Explorer command-bar button and the main Edit/context-menu items render the same localized disabled reason through `toolTip` and `accessibilityHelp`. Empty, invalid, or missing localization keys resolve to `commands.disabled.generic` and then to a safe built-in fallback; raw localization keys and `technical_message` do not reach the UI.

The shared Edit Copy item restores its localized base title, visibility, tooltip, and accessibility help after menu tracking. Empty selection also restores `commands.file.copy.title`, and only a key-down event matching the configured key equivalent is classified as a shortcut, so keyboard-opened menus retain normal presentation. Suggested-action rendering and manual VoiceOver evidence remain pending with the wider P0 accessibility pass.

## Acceptance criteria

- all four entry points execute the same stable command id;
- the context menu uses its explicit item payload;
- empty context is disabled with a localized reason;
- remote/archive/mixed context is disabled before pasteboard execution;
- an unrepresentable native path fails before the pasteboard is changed;
- accepted execution calls the pasteboard writer exactly once;
- the `copy:` responder action preserves text-editing precedence;
- existing shortcut persistence keys and menu titles remain compatible;
- command bar, Edit menu, and context menu use one command-state presentation adapter;
- disabled presentation uses the same localized tooltip and accessibility help without exposing diagnostics or raw keys;
- persistent Edit-menu state is cleared after tracking, empty state restores the localized base title, and shortcut detection matches the configured key equivalent;
- focused command tests, aggregate `WinCommanderUT`, and the unsigned application build pass.

## Tests

- stable descriptor and legacy metadata;
- toolbar, menu, context-menu, and shortcut invocation sources;
- empty selection and disabled execution;
- remote and mixed-provider eligibility;
- atomic rejection of a mixed valid/unrepresentable native payload;
- missing composition writer registration;
- explicit context-menu payload versus pane selection;
- exactly-once writer invocation;
- focused `CommandPresentationAdapter` suite: 10 test cases and 69 assertions covering localization, safe fallback, stale-state cleanup, localized base-title restoration, and key-equivalent classification;
- aggregate build coverage for AppKit adapters and application composition.
