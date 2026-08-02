# Feature: `file.rename` Command Registry initiation slice

> Status: M1 production initiation routing implemented; rename mutation planning remains on the established Operations path
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 10, 13, 14, 28, 29, 39, 40 and 44
> Execution tracker: `Docs/Development-Plan.md`, R3 in `Docs/refactor_plan.md`

## Scope

`file.rename` is the shared command that starts the active pane's inline filename editor. This slice unifies eligibility, presentation, invocation-source tracking, and editor initiation. It ends when the editor is successfully opened.

Submitting the edited name still uses `PanelController::requestQuickRenamingOfItem`, which validates the filename and enqueues the established `nc::ops::Copying` move operation with `docopy = false`. The complete filesystem mutation has not yet been migrated to the canonical `OperationPlan`, conflict policy, undo, and operation-lifecycle contracts.

## Command contract

- stable id: `file.rename`;
- category: File;
- legacy responder selector: `OnRenameFileInPlace:`;
- legacy shortcut action: `menu.command.rename_in_place`, tag `15141`;
- synchronous context: one borrowed `ListingItem`, invocation source, native sender, and live pane target;
- descriptor semantics for this initiation boundary: non-destructive, no `OperationPlan`, no undo claim.

The application registers one handler. The handler receives the captured item and pane target, resolves the item again in the pane's current model, focuses its current sort position, verifies the focused listing/index identity, and then opens the pane-owned editor. This synchronous re-resolution prevents a stale captured row position from initiating rename on a different item.

## State and disabled reasons

The command is enabled only when the context contains exactly one item, that item is a regular file-list entry, its provider is live and exposes `ProviderCapabilities::can_rename` at the item directory, and the context carries a live pane target.

Structured disabled states cover:

- empty context: `selection.empty`;
- multiple items: `selection.singleItemRequired`;
- parent-directory entry: `selection.parentEntryUnsupported`;
- missing provider: `provider.unavailable`;
- provider without rename support: `provider.renameUnsupported`;
- missing pane target: `context.paneTargetRequired`.

The shared presentation adapter applies Registry state and localized disabled reasons to command-bar and menu controls. State-evaluation failures resolve to a visible generic disabled state at the Objective-C++ boundary.

## Production entry points

- main menu through `OnRenameFileInPlace:`;
- the persisted legacy rename shortcut through the same responder action;
- Explorer command bar Rename button;
- file context menu Rename item, using its captured clicked-item-or-selection payload;
- direct field-editor mouse gesture from list, brief, and gallery views, classified as a programmatic invocation.

All entry points query or execute the same Registry definition. Pane-driven surfaces snapshot the focused item; the context menu preserves its explicit item payload. Existing AppKit responder routing remains the integration boundary for the main menu and shortcut.

## Failure boundary

The initiation handler returns success only after it has re-resolved and focused the captured item and requested the inline editor. A missing pane/view, an item absent from the current model, an identity mismatch, or an editor that cannot be initiated returns `false`; the command converts that result to `FileRenameInitiationError`. The panel dispatcher logs the invocation source and presents the typed failure through the established exception UI.

Disabled execution does not invoke the handler. C++ exceptions are contained at the Objective-C++ dispatcher boundary.

## Verified coverage

Focused tests cover descriptor and legacy metadata, all six invocation-source values, exactly-once initiation, structured disabled reasons, typed initiation failure, and missing-handler registration: 5 cases / 64 assertions. Production adapters cover shared menu validation, command-bar presentation, explicit context-menu payloads, direct field-editor routing, and a boolean editor-start boundary. The focused rename editor regression suite passed 2 / 23.

At completion of this command slice, the structural `OperationPlan` foundation passed 8 cases / 109 assertions and full `OperationsUT` passed 32 / 389 in Debug, Release ASAN, and Release UBSAN. Committed rename still uses `nc::ops::Copying(docopy = false)`; current aggregate evidence is maintained in `Docs/Development-Plan.md`.

The completion snapshot for this command slice was 213 `WinCommanderUT` cases / 2,638 assertions in Debug, Release ASAN, and Release UBSAN, with confirmed sanitizer instrumentation. Its local M0 snapshot passed 10/10 aggregate binaries, 775 cases / 128,572 assertions. Current aggregate evidence is maintained in `Docs/Development-Plan.md`; hosted CI and Docker-backed integration remain separate gates.

## Remaining work

- adopt the structural `OperationPlan` for committed rename, then add provider capabilities, accepted/preflight validation, conflict decisions, and rollback metadata at the appropriate planning layers;
- route filesystem execution, progress, cancellation, typed operation errors, and refresh effects through the canonical Operation Engine lifecycle;
- add undo where the provider and operation semantics can guarantee it;
- converge single-item and batch rename on shared planning and conflict contracts while preserving batch preview requirements;
- complete command-palette, shortcut-remapping, accessibility, telemetry, and manual UI evidence required by the ideal specification.
