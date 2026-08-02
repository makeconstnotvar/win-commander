# Back/Forward Command Registry slice

> Status: implemented and locally verified on 2026-08-02

## Contract

`navigation.back` and `navigation.forward` are stable command identifiers in the application-owned `CommandRegistry`. Both definitions use a synchronous borrowed `CommandContext`: a live pane target plus the current `can_go_back` and `can_go_forward` projection. Each direction returns its own localized pane-unavailable, state-unavailable, or history-boundary `DisabledReason`.

The main Go menu and its persisted `Command-[` / `Command-]` bindings query live `Panel::History` through `NCPanelControllerActionsDispatcher`. Explorer toolbar presentation supplies the matching `PaneSnapshot::history_availability` pair to the same Registry definitions and renders the resulting state through `CommandPresentationAdapter`. Missing and foreign pane snapshots pass an unavailable state, keep both buttons disabled, expose localized tooltip/accessibility help, and do not update the breadcrumb.

`OnGoBack:` and `OnGoForward:` remain responder-compatible selectors, but the dispatcher intercepts them before the legacy action map. Toolbar, menu, shortcut, and programmatic selector execution therefore converge on `CommandRegistry::Execute`. The production executor delegates to the existing `actions::GoBack` or `actions::GoForward`: it checks the live History immediately before moving, moves the cursor once, and submits the existing listing restore once. The cached Store projection is presentation input, never execution authority.

## Execution boundary

For this slice, `ExecutionStatus::Executed` means that the live History cursor moved and `ListingPromiseLoader::Load` submitted the legacy asynchronous restore task. It does not mean that a listing committed. That loader still uses `commitCancelableLoadingTask`, contains restore failures, and does not issue the `PaneRequestId` plus exactly-one terminal lifecycle outcome used by the newer navigation/refresh path. Migrating history restore to that lifecycle remains separate work.

Runtime current-entry identity and persisted per-pane history are separate state/persistence contracts. The existing shortcut action names and tags remain the persistence keys; the Registry metadata maps them to the two stable IDs without changing user bindings.

## Verification

- `CommandRegistry *` plus `NavigationHistoryCommand *`: 14 cases / 173 assertions;
- new navigation definitions plus stable-ID checks: 7 / 123;
- selector/menu/shortcut/toolbar dispatcher routing: 1 / 29;
- matching/foreign Explorer toolbar presentation and real Back/Forward clicks: 1 / 73;
- incremental arm64 Debug `WinCommanderUT` build: passed;
- project-file lint, localization JSON validation, and `git diff --check`: passed.

The focused tests cover metadata, every availability pair, structured missing-context reasons, every invocation-source enum, exactly-once port execution, failed live guard, menu presentation, Store-backed toolbar presentation, foreign/missing snapshots, tooltip/accessibility help, and real toolbar click routing.
