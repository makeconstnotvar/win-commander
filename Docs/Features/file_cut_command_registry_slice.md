# Feature: `file.cut` Command Registry slice

> Status: M1 production routing and command presentation implemented for `file.cut`; filesystem move planning remains outside this slice
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 7, 9, 10, 11.5, 13.1, 14, 28, 29, 39 and 40
> Execution tracker: `Docs/Development-Plan.md`, R3 in `Docs/refactor_plan.md`

## Scope

`file.cut` stages a selected local file-list snapshot as `CutPending`. It does not rename, move, delete, or otherwise mutate the filesystem, so its descriptor is non-destructive and does not require an `OperationPlan`.

The later Paste-as-Move operation is a separate command boundary. Convergence of that filesystem mutation on `OperationPlan` and the Operation Engine remains follow-up work.

## Command contract and surfaces

- stable id: `file.cut`;
- legacy responder selector: `cut:`;
- synchronous context: borrowed `span<ListingItem const>`, invocation source, and native sender;
- accepted providers: native filesystem items only;
- invocation sources: Explorer command bar, Edit menu, `Command-X`, and file context menu.

Toolbar, menu, shortcut, and context-menu adapters query and execute the same app-owned Registry definition. Toolbar/menu/shortcut contexts use the selected entries or focused entry; the context menu passes its explicit clicked-item-or-selection snapshot. AppKit field editors retain responder-chain precedence for text Cut.

## State and disabled reasons

The command stays visible and is disabled as one whole payload for:

- empty context: `selection.empty` / `commands.file.cut.disabled.selectionEmpty`;
- a parent-directory entry: `selection.parentEntryUnsupported` / `commands.file.cut.disabled.parentEntryUnsupported`;
- archive, remote, mixed, or otherwise non-native items: `provider.nativeItemsRequired` / `commands.file.cut.disabled.nativeItemsRequired`.

The shared presentation adapter applies enabled/visible state and the same localized reason to tooltips and accessibility help. Menu titles remain selection-aware and return to the localized base title after menu tracking. State-evaluation exceptions resolve to a visible generic disabled state and are logged at the Objective-C++ boundary.

## Pasteboard semantics

The writer validates the complete item set, native hosts, and UTF-8 paths before clearing the pasteboard. It then writes one native file-list payload and establishes Move intent only after a marker plus a process-owned snapshot of pasteboard name, generation, nonce, and exact paths are valid.

Paste treats the staged list as Move only while that identity still matches. Clipboard replacement invalidates the token; claim/release/consume coordination prevents concurrent or repeated Move consumption; cancelling Cut removes Move intent while retaining a safe Copy file list. Cut-state notifications drive the existing row dimming and cancellation refresh. Marker or file-list write failure invalidates the process-owned Cut snapshot, so a stale token cannot turn a later Paste into Move.

## Failure boundary

The injected writer returns success only when staging is complete. A false result becomes `FileCutWriteError`; the panel dispatcher catches it, records `file.cut execution failed` with the invocation source, and presents the error. Disabled execution does not call the writer.

Payload validation failures before pasteboard mutation preserve the previous clipboard. A failure after `clearContents` cannot restore arbitrary previous pasteboard contents or lazy/external types through the public `NSPasteboard` API. The implemented guarantee is fail-closed Move intent: the command reports failure, invalidates its Cut token, and any successfully written file list remains Copy-safe. Full rollback would require a separately designed pasteboard transaction/ownership layer.

## Tests and evidence

Focused coverage verifies:

- descriptor metadata and staging-only flags;
- toolbar, menu, shortcut, and context-menu sources;
- explicit context-menu snapshot and exactly-once writer invocation;
- empty, parent-entry, remote, and mixed-provider disabled states;
- typed writer failure and missing-writer registration;
- shared menu/button presentation, localized title restoration, accessibility help, and exact `Command-X` classification;
- invalid UTF-8 rejection before pasteboard mutation;
- marker identity, replacement invalidation, claim/release/consume, cancellation, and fail-closed marker failure.

At completion of this command slice, the structural `OperationPlan` foundation passed 8 cases / 109 assertions and full `OperationsUT` passed 32 / 389 in Debug, Release ASAN, and Release UBSAN. Current aggregate evidence is maintained in `Docs/Development-Plan.md`.

The completion snapshot passed 213 `WinCommanderUT` cases / 2,638 assertions in Debug, Release ASAN, and Release UBSAN with confirmed sanitizer instrumentation. Its local M0 snapshot passed all 10 aggregate binaries with the unsigned Debug application build: 775 cases / 128,572 assertions. Current aggregate evidence is maintained in `Docs/Development-Plan.md`; hosted CI and Docker-backed integration remain separate gates.

## Remaining work

- route the later Paste-as-Move filesystem mutation through `OperationPlan` and the Operation Engine;
- provide full pasteboard rollback only if a transaction/ownership design can preserve arbitrary prior types correctly;
- complete the wider command-palette, shortcut-remapping, accessibility, and manual UI evidence required by the ideal specification.
