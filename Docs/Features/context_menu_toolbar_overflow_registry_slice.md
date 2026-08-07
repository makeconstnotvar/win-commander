# Q1-2 Context Menu and Toolbar Overflow Registry slice

> Status: production presentation implemented and locally verified on 2026-08-07
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 10, 11, 28 and 29
> Execution tracker: `Docs/Development-Plan.md`, Queue 1 item Q1-2

## Scope

This slice composes the pane-background context menu and the Explorer More menu from the application-owned Registry. Both surfaces expose the same six commands:

| Command | Background context menu handler | Explorer More canonical selector |
|---|---|---|
| `file.paste` | `OnBackgroundPaste:` | `paste:` |
| `file.newFolder` | `OnBackgroundNewFolder:` | `OnQuickNewFolder:` |
| `pane.selectAll` | `OnBackgroundSelectAll:` | `selectAll:` |
| `pane.invertSelection` | `OnBackgroundInvertSelection:` | `OnMenuInvertSelection:` |
| `view.toggleHiddenFiles` | `OnBackgroundToggleHiddenFiles:` | `ToggleViewHiddenFiles:` |
| `navigation.refresh` | `OnBackgroundRefresh:` | `OnRefreshPanel:` |

The background handlers are private `NCPanelContextMenu` presentation adapters. Explorer More stores each canonical responder selector in `NSMenuItem.representedObject`; `performMoreMenuAction:` resolves that identity and calls the corresponding typed dispatcher method.

The Explorer New popover uses the Registry presentation for `file.newFolder` and `file.newFile`, the shared `commands.file.*.title` localization keys, canonical selectors `OnQuickNewFolder:` and `OnQuickNewFile:`, and Toolbar execution.

## Background and exact-item payload contract

`PanelView::panelItem:menuForForEvent:` reports a background invocation with item index `-1`. `PanelController::panelView:requestsContextMenuForItemNo:` converts that request into an empty `std::vector<VFSListingItem>` and passes it to the context-menu provider. The provider selects `initForBackgroundOfPanel:` for the empty vector, so `NCPanelContextMenu::items` remains an empty span throughout the background-menu lifetime.

An item invocation preserves the established exact payload contract:

- an unselected clicked entry contributes exactly that entry;
- a selected clicked entry contributes the current selected entries in sorted order;
- the captured vector is moved into `initWithItems:ofPanel:withFileOpener:withUTIDB:` and remains the item-menu command context;
- the parent entry retains its established `nil` menu result.

This separation gives pane-scoped commands a context that is independent of cursor focus while retaining exact item identities for item-scoped state and execution.

## Registry state and execution sources

The background menu queries all six states with `CommandInvocationSource::ContextMenu`. `validateMenuItem:` repeats the same source-qualified query when AppKit validates an open menu. Each accepted action executes through the matching dispatcher method with `CommandInvocationSource::ContextMenu` and the originating menu item as sender.

Explorer More queries all six states with `CommandInvocationSource::Toolbar`. `performMoreMenuAction:` executes the matched Registry command with the same `Toolbar` source and the originating menu item as sender. Selector identity remains responder-compatible metadata; Registry dispatch remains the execution authority.

`view.toggleHiddenFiles` carries the Registry check state onto both menus. The checked presentation therefore follows the same live hidden-files state used by the command definition.

## Disabled and accessibility presentation

`CommandPresentationAdapter` projects every Registry state onto its `NSMenuItem`. A visible disabled command remains in the roster with:

- `enabled == false`;
- an absent target and action;
- the localized disabled reason in `toolTip`;
- the same reason in `accessibilityHelp`;
- the Registry-provided check state where applicable.

An enabled command receives the surface target and action only after the state projection succeeds. Hidden Registry presentation remains omitted from the constructed menu. The New popover follows the same target/action admission and localized tooltip behavior for its creation commands.

## Surface boundary

The background context menu owns pane-scoped actions that have meaningful state without an exact item payload. The item context menu continues to own exact-item actions and their captured item identities. Explorer More provides the matching pane roster alongside its existing payload and operation-center sections. The New popover remains the compact creation surface for New Folder and New File.

Command definitions, live admission, provider capabilities and mutation execution remain owned by their command slices. This Q1-2 slice supplies presentation composition, invocation-source fidelity and selector-compatible routing over those existing authorities.

Later evidence gates cover signed stable-development runtime interaction, mouse placement, keyboard traversal and VoiceOver behavior. The local evidence for this slice covers AppKit presentation objects, Registry source propagation, exact selector identity, checked state, disabled accessibility projection and an unsigned Debug build.

## Verification

- unsigned arm64 Debug `UnitTests` scheme build with `CODE_SIGNING_ALLOWED=NO`: succeeded;
- `WinCommanderUT '*background context menu projects*' --rng-seed 424242`: 1 case / 75 assertions;
- `WinCommanderUT '*Explorer More exposes*' --rng-seed 424242`: 1 / 72;
- `WinCommanderUT '*New popover projects*' --rng-seed 424242`: 1 / 26;
- existing `WinCommanderUT '*file.open routes menu Enter context menu*' --rng-seed 424242`: 1 / 66, covering the adjacent exact-item context-menu route;
- full Debug `WinCommanderUT --rng-seed 424242`: 391 cases / 6,899 assertions;
- scoped `git diff --check`: passed.
