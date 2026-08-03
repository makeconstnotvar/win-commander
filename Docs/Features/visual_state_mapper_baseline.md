# Feature: Visual State mapper baseline

> Status: M1 pure projection implemented; production rendering connected for `file.copy`, `file.cut`, initiation `file.rename`, `file.open`, `view.toggleHiddenFiles`, Back/Forward/Up/Refresh and Explorer navigation/refresh lifecycle/error state, remaining pane/item/operation consumers in progress
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 11 and 39
> Execution tracker: `Docs/Development-Plan.md`, R4 in `Docs/refactor_plan.md`

## User problem

Loading, empty, error, permission, selection, and disabled-command states need one deterministic composition policy. A shared semantic projection lets every surface render the same priority and message while views retain only transient interaction state.

## Inputs and outputs

`VisualStateMapper::MapPane` receives an immutable `PaneSnapshot` and an optional caller-supplied navigation error. It returns toolkit-independent pane, breadcrumb, and status projections. Production navigation and refresh emit request-scoped lifecycle events that `PaneLifecycleReducer` projects into `PaneSnapshot::state`, including load phase and `visible_error`; the Explorer breadcrumb consumes that projection. Refresh retains committed content, exact commit replaces listing identity at the same location generation, and `PaneStoreAdapter` advances `listing_generation`. Remaining pane/item/operation renderers remain open.

`VisualStateMapper::MapCommand` receives normalized `CommandState` from `CommandRegistry::QueryState` and returns visible/enabled state plus an optional user-facing disabled message.

## Pane composition

- initial `PaneLoadPhase::Empty` maps to unavailable;
- loaded zero-item listing maps to empty folder;
- loading hides committed content and exposes activity;
- refreshing retains committed content, path, and counts while exposing activity;
- uniform location is editable in ready, empty, and refreshing states;
- a non-uniform committed listing maps to multiple locations;
- blocking permission, path, provider, unsupported, and generic errors take precedence over activity/content;
- warning and recoverable errors preserve content as a nonblocking notice;
- fatal and destructive-risk errors receive critical priority.

User-facing messages carry localization keys, safe fallbacks, and suggested command ids. Technical diagnostics stay in `FileManagerError`.

## Command composition

Hidden commands remain absent. Enabled commands omit stale disabled detail. Disabled commands carry the user-facing reason normalized by the registry; the AppKit adapter localizes it and applies the generic safe fallback when the reason or localization key is empty, invalid, or missing. Technical diagnostics stay outside the projection and presentation path.

## Production boundary

The mapper remains a pure foundation. `CommandPresentationAdapter` renders `file.copy`, `file.cut`, and initiation `file.rename` state on their production Explorer command bar, shared menu, and file context menu surfaces, including localized disabled help, safe fallback, and stale-state cleanup. Rename state requires exactly one non-dotdot item, path-aware `ProviderCapabilities::can_rename`, and a live borrowed pane target. Menu/persisted shortcut, Explorer command bar, context menu, and direct field-editor mouse entry execute the same Registry definition; the handler synchronously re-resolves and focuses the item, validates listing/index identity, and reports success only when `PanelView::startFieldEditorRenaming` returns true. A false start becomes typed `FileRenameInitiationError`. The committed rename still runs through `PanelController::requestQuickRenamingOfItem` and `nc::ops::Copying(docopy = false)` until the mutation is migrated to `OperationPlan`.

`file.open` uses the same normalized command projection on `OnOpenNatively:`/menu, ordinary-file Enter fallback, Shift-Return/explicit Shortcut, and context-menu surfaces. Its state is built synchronously from the exact live item payload, exact provider identity, and `ProviderCapabilities`. One regular file requires `Read`; a batch requires only regular files from the same readable provider instance; one native directory/special item can be handed to the native workspace. Dot-dot and remote directory/special-item payloads map to structured disabled presentation. Folder/archive Enter remains the navigation router, including the legacy non-uniform dot-dot Back path. Execution reports `Executed` when the payload is synchronously submitted to `FileOpener`; downstream open completion remains outside the command result.

Production `PaneStore` publishes committed empty/loaded state plus reduced navigation/refresh overlays. `PanelController` shares one factory-injected `PaneId` with its Store bridge and controller-owned `PanelControllerLifecycle`; navigation and refresh map correlated worker ownership, cancellation/failure, and post-model-commit success into ordered lifecycle events. Async navigation performs detached fetch on a shared queue and posts only weak-controller main callbacks; intent invalidation cancels its token and dealloc stops without waiting. Persistency recovery callbacks are weak. Refresh physically coalesces one running worker plus latest pending, preserves location generation on commit and publishes typed invalid-location failure before ordinary navigation recovery. The Explorer breadcrumb maps this snapshot: committed path/content survives loading, refreshing and failure; typed error/notice text is localized and exposed through AX. Docker `WinCommanderIT` proves this boundary across FTP/SFTP/WebDAV at 4 / 178: distinct controller and shadow hosts verify initial navigation, a fresh listing after forced user refresh, provider soft-successor completion for FTP/WebDAV, direct forced-refresh completion for SFTP, and a WebDAV endpoint outage/reconnect that exposes `NetworkError` while retaining committed content before the fresh successor. Footer, item rows, remaining P0 commands and operation surfaces become render adapters as their inputs are connected.

## Accessibility

The production `file.copy`, `file.cut`, `file.rename`, `file.open`, `view.toggleHiddenFiles`, `navigation.back`, `navigation.forward`, `navigation.up`, and `navigation.refresh` buttons and menu items derive tooltip and `accessibilityHelp` from the same localized command projection on applicable surfaces. `file.open` projects its reason from synchronous exact live items/provider/capabilities. Back/Forward feed matching Store History availability to the Registry for Explorer toolbar presentation and use live History for menu/shortcut state and execution. Up/Refresh feed a matching-`PaneId` availability projection derived from pane load/content/hierarchy facts; missing or foreign state fails closed, while execution re-samples the live controller through the same queue-ownership helper used by admission. Up distinguishes Busy, top and unavailable hierarchy. Refresh distinguishes Busy and missing committed content. Shared-menu cleanup restores localized base titles and clears transient help. Exact key-equivalent classification plus Registry descriptor aliases preserve Shortcut source for secondary bindings that bounce through a primary menu item. The Explorer breadcrumb error label publishes the same safe localized text as visible value, tooltip, accessibility value and accessibility help. Suggested-action rendering and manual VoiceOver evidence are still pending; item and operation accessibility remain future render integrations.

## Acceptance criteria

- initial and loaded-empty states remain distinct;
- refresh preserves committed content and counts;
- blocking errors dominate activity and expose safe user messages;
- recoverable notices preserve usable content;
- pane and command mappings contain no toolkit dependencies or technical diagnostics;
- the `file.copy`, `file.cut`, and `file.rename` AppKit adapters clear stale disabled detail for enabled or hidden command states and never expose raw localization keys or technical diagnostics;
- `file.open` maps one regular file, same-provider regular-file batches and native directory/special handoff from exact live context, while dot-dot and remote directory/special payloads expose structured disabled state;
- `file.open` `Executed` means synchronous submission to `FileOpener`;
- priority and category rules are table-driven and covered by unit tests.

## Tests

- unavailable versus loaded empty folder;
- loading and refreshing;
- item and selection counts;
- uniform and multiple-location breadcrumbs;
- blocking category and critical severity matrix;
- recoverable notice and cancellation;
- failed state without a typed error;
- visible, hidden, enabled, disabled, and registry-normalized command state.
- focused AppKit command presentation: 10 test cases / 69 assertions for localization/fallback, tooltip/help parity, stale cleanup, localized base title, and key-equivalent matching; `file.rename` reuses the same adapter in menu, command bar, and context menu;
- focused Registry commands: `file.copy` 6 cases / 58 assertions; `file.cut` 8 / 73; [`file.rename`](file_rename_command_registry_slice.md) 5 / 64; Back/Forward core 14 / 173 and dispatcher/toolbar presentation 2 / 102, including typed disabled reasons, all invocation sources and exactly-once toolbar clicks;
- `file.open` evidence: core definition/Registry/legacy shortcut 23 / 201, focused production route 1 / 66, combined 24 / 267; the full Registry fixture passes 3 / 122;
- focused Up/Refresh command, availability, shortcut, dispatcher/controller and toolbar coverage includes matching/foreign pane presentation, localized disabled reasons, uniform-only explicit Up, forced-user/soft-refresh separation, shared live/admission queue ownership, descriptor-aware aliases, preserved menu bounce, detached navigation lifetime and exactly-once submission; total 32 / 549: core 22 / 281, production navigation 4 / 50, production Refresh 3 / 37, Registry surfaces 1 / 27, Explorer model/toolbar 2 / 154;
- focused rename editor regression: 2 cases / 23 assertions;
- final Debug Visual State mapper: 8 cases / 181 assertions; focused breadcrumb lifecycle/error/AX/source-discrimination coverage: 1 case / 32 assertions.
- current Debug lifecycle reducer: 19 cases / 335 assertions; Store 18 / 220; production bridge 28 / 461; dedicated History 5 / 457;
- broader production navigation and refresh prefixes: 8 cases / 103 assertions and 16 / 154, covering exact terminal publication, detached weak-callback lifetime, non-cooperative teardown, running+latest coalescing, recovery and same-generation listing advancement;
- completion snapshot for the structural `OperationPlan` foundation: 8 / 109; full `OperationsUT` 32 / 389 in Debug, Release ASAN, and Release UBSAN;
- completion snapshot for Debug, Release ASAN, and Release UBSAN `WinCommanderUT`: 213 / 2,638 in each configuration; sanitizer instrumentation was confirmed. Current aggregate evidence is maintained in `Docs/Development-Plan.md`;
- completion snapshot for local `Scripts/verify_m0.sh`: exit 0, unsigned Debug application built and all 10 aggregate unit-test binaries passed, 775 cases / 128,572 assertions. Current aggregate evidence is maintained in `Docs/Development-Plan.md`; remaining pane/item/operation renderers stay open.

Hosted CI has not run for the `file.open` slice. The first hosted M0 workflow run remains separate release-gate evidence.
