# `file.open` Command Registry slice

> Status: M1 production routing implemented and focused integration verified
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 10, 11, 26, 29, 39 and 44
> Execution tracker: `Docs/Development-Plan.md`, R3 in `Docs/refactor_plan.md`

## Command contract

`file.open` is the stable, non-destructive Command Registry definition for opening an item with its default external handler. Its compatibility metadata is selector `OnOpenNatively:`, shortcut action `menu.file.open`, and tag `11020`. The descriptor declares `requires_operation_plan == false`: external opening hands existing items to another application and does not mutate filesystem content.

The synchronous `CommandContext` carries the invocation source, a live `PanelController` target, and a borrowed item snapshot. Pane-driven menu, selector, Enter, and shortcut adapters materialize the exact current selection or focused entry for that call. The context menu validates and executes the exact `m_Items` snapshot captured when that menu was built, preserving clicked-item and multi-selection identity instead of resampling the pane.

## Availability

The definition fails closed with a structured, localized `DisabledReason`:

- `context.paneTargetRequired` when the live pane target is absent;
- `selection.empty` when the item snapshot is empty;
- `provider.unavailable` when an item or its provider is unavailable;
- `selection.parentEntryUnsupported` for a dot-dot entry;
- `selection.sameProviderRequired` when a batch spans provider instances;
- `selection.regularFilesRequired` when a batch contains a directory or special item;
- `provider.remoteItemTypeUnsupported` for a single remote directory or special item;
- `provider.readUnsupported` when a regular file's provider does not declare `Read` for its directory.

One readable regular file is eligible on a native, archive, or remote provider. A batch is eligible only when every item is a regular file on the exact same provider instance and every source directory resolves `Read`. A single native directory or special item remains eligible through the established native workspace handoff path.

## Production routing and execution boundary

The dispatcher routes these surfaces through the same Registry definition:

- the File menu `OnOpenNatively:` item and its Shift-Return shortcut;
- the ordinary-file fallback of Enter, including its Return shortcut;
- responder-compatible programmatic selector calls;
- context-menu Open with its captured item snapshot.

Enter remains a router. Directory and archive entries use folder navigation, executable items use terminal execution, and ordinary files use `file.open`. The explicit Open command can hand a single native directory or special item to the default workspace even though normal Enter keeps directory navigation semantics.

The application-owned Registry injects the shared production `FileOpener`. After availability succeeds, the executor synchronously calls `SubmitOpenItemsWithDefaultHandler` with the accepted target and exact items. `ExecutionStatus::Executed` therefore means that the request was submitted to `FileOpener`; completion by Launch Services or a remote opener is outside this command boundary. A rejected handoff raises the typed `FileOpenExecutionError` and is contained by the Objective-C++ dispatcher.

The legacy `OnOpenNatively:` entry was removed from `PanelActionsMap`, and context-menu Open no longer owns a separate action instance. Menu validation, responder execution, shortcuts, and context-menu presentation now converge on one availability and execution path.

## Navigation and adjacent-command boundaries

`file.open` rejects dot-dot before execution. The retained Enter/dot-dot navigation path now returns immediately after `GoToEnclosingFolder`, so one activation submits exactly one enclosing-folder request. Uniform listings navigate to the provider parent; the established non-uniform path retains its Back fallback.

Folder/archive navigation remains an Enter and navigation-command concern. Open With and Always Open With retain their dynamic application-choice delegate and use the same shared `FileOpener`, while their future canonical `file.openWith` Registry definition remains separate work.

## Verification boundary

- `FileOpenCommand`: 8 cases / 102 assertions;
- `FileOpenCommand` plus `CommandRegistry` and `LegacyShortcutBindingAdapter`: 23 / 201;
- incremental arm64 Debug `WinCommanderUT` build: passed;
- focused production menu/Enter/context-menu/Shift-Return integration: 1 / 66;
- complete production Registry fixture: 3 / 122;
- explicitly instrumented Release ASAN for the core command and production route: 9 / 168;
- hosted CI: pending.

Combined focused `file.open` evidence is 24 / 267. The core evidence covers stable metadata, every invocation source, exact borrowed payload submission, native and remote regular files, same-provider batches, native directory/special handoff, every typed disabled reason, failed live submission, and missing-executor registration. The production fixture covers main-menu presentation/execution, ordinary-file Enter routing, actual Shift-Return key handling, live execution revalidation, exact context-menu snapshot preservation after pane focus changes, and the directory/executable Enter branches.
