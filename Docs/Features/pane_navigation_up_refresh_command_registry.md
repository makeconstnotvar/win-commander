# Up/Refresh Command Registry slice

> Status: M1 production routing implemented and locally verified on 2026-08-02
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 7.2, 10, 11, 17, 25 and 44
> Execution tracker: `Docs/Development-Plan.md`, R2/R3/R4 in `Docs/refactor_plan.md`

## Scope

`navigation.up` and `navigation.refresh` are stable command identifiers in the application-owned `CommandRegistry`. The slice gives the Explorer toolbar, Go/View menus, persisted shortcuts and responder-compatible selectors one typed state and execution boundary for explicit Up and user-requested Refresh.

The Registry now contains eleven stable IDs. The two new definitions use borrowed synchronous context: a live pane target and the applicable `NavigationUpAvailability` or `NavigationRefreshAvailability`. They return localized structured reasons for unavailable pane/state, busy lifecycle, top-level hierarchy, unavailable hierarchy, and missing committed refresh content.

## Availability projection

`MapPaneNavigationAvailability` derives advisory Up/Refresh availability from immutable pane facts:

- active navigation maps both commands to `Busy`;
- an empty pane has no enclosing hierarchy and no committed refresh content;
- committed uniform child directories enable both commands;
- a uniform provider root enables Up when the provider exposes a valid parent junction;
- a top-level root disables Up with `AtTop` while Refresh remains available;
- committed non-uniform listings disable explicit Up with `HierarchyUnavailable` while Refresh remains available;
- retained committed content in Refreshing or Failed state remains refreshable.

Explorer toolbar presentation accepts this projection only from the snapshot whose `PaneId` matches the Explorer pane. Before a matching snapshot arrives, and after a foreign snapshot, Up and Refresh fail closed through the same Registry definitions and expose their localized tooltip/accessibility help. The foreign snapshot is not forwarded to the breadcrumb.

Menu and shortcut presentation sample equivalent facts from the live controller. Every execution path samples the live controller again immediately before Registry execution, so Store state is presentation input rather than execution authority.

The controller uses one queue-ownership helper for lifecycle admission and this live availability sample. A loading or reload queue owned by the correlated lifecycle worker is classified as lifecycle work; unrelated queue occupancy and a stopped reload queue make both commands `Busy`. This keeps menu/shortcut validation and admission aligned without treating the controller's own active worker as foreign work.

## Explicit Up boundary

The production `navigation.up` executor accepts only a current uniform hierarchy. From a child directory it submits asynchronous navigation to the enclosing directory, requests focus on the departed folder, restores the previous view state, and marks the request as user initiated. From a uniform provider root it submits the equivalent request through the provider's parent and junction path.

An explicit Up command is disabled for a non-uniform listing and at the top root. The established `GoToEnclosingFolder` action retains its non-uniform Back fallback for indirect dot-dot and folder Enter navigation; dot-dot submits exactly one enclosing-folder request. Folder/archive Enter remains a navigation router. The ordinary-file Enter fallback is owned by the canonical `file.open` command.

The accepted asynchronous Up request executes a detached provider fetch on the shared loading `SerialQueue`. The worker owns request/queue/token resources and posts main work through a weak controller box. Content-intent invalidation cancels the token; controller deallocation stops the queue and returns without `Wait`. A provider that ignores cancellation may finish later without retaining the controller. Persistency recovery callbacks follow the same weak-controller ownership rule.

## User Refresh boundary

The `navigation.refresh` Registry executor calls `PanelController::submitUserRefresh`. The resulting lifecycle descriptor is user initiated and the fetch includes `VFSFlags::F_ForceRefresh`. Automatic refresh callers continue to use `refreshPanel`; they submit a soft refresh outside the Registry, keep `initiated_by_user == false`, and omit `F_ForceRefresh`.

The controller reports success to the Registry when `PanelControllerLifecycle::SubmitRefresh` returns `Accepted` or `Deferred`. This means the user refresh request was submitted to lifecycle admission. It does not mean that provider fetch, model replacement, or terminal Store commit completed. The lifecycle mints the `PaneRequestId` after admission and remains the authority for exactly-one terminal outcome.

## Production entry points

- Up: `OnGoToUpperDirectory:`, primary `menu.go.enclosing_folder`, secondary persisted alias `panel.go_into_enclosing_folder`, Explorer toolbar;
- Refresh: `OnRefreshPanel:`, `menu.view.refresh`, Explorer toolbar;
- invocation source remains distinct for Menu, Shortcut, Toolbar and Programmatic calls;
- the dispatcher intercepts both selectors before the legacy action map and converges on `CommandRegistry::Execute`.

The panel key sink preserves its established hierarchical menu bounce for the secondary Up shortcut instead of dispatching directly. Command-aware invocation-source classification reads every shortcut action name from the Registry descriptor and compares the current key event, so the bounced primary menu item is still recorded as `Shortcut`. Both Up aliases resolve to `navigation.up`; the menu remains the responder/presentation boundary.

## Adjacent `file.open` boundary

The canonical `file.open` definition owns `OnOpenNatively:`, its menu item, the ordinary-file fallback of Enter, Shift-Return/explicit Shortcut, and context-menu execution with the exact captured item payload. State and disabled presentation are resolved synchronously from those live items, their exact provider identity, and `ProviderCapabilities`; this command uses direct command context rather than a `PaneStore` snapshot.

One regular file is eligible on a provider with `Read`. A multi-item payload is eligible when every item is a regular file from the exact same readable provider instance. A native directory or special item can be handed to the native workspace path. Dot-dot and remote directory/special-item payloads are disabled with structured Registry reasons. Execution synchronously submits the accepted payload to `FileOpener`; Registry `Executed` records that handoff, while Launch Services or remote opener completion remains a separate outcome.

## Verification boundary

- core command/availability/identity/alias run: 22 cases / 281 assertions — command definitions 7 / 185, availability mapper 6 / 25, Registry and eleven stable IDs 8 / 62, secondary shortcut aliases 1 / 9;
- production explicit-Up navigation semantics, shared queue ownership and non-cooperative teardown: 4 / 50, including teardown 1 / 12;
- forced-user versus soft production refresh and shared queue-ownership behavior: 3 / 37;
- Registry selector/menu/shortcut/toolbar surface routing: 1 / 27;
- matching/foreign Explorer model and toolbar presentation/click routing: 2 / 154;
- total focused slice: 32 / 549;
- broader production navigation/refresh regression prefixes: 8 / 103 and 16 / 154; the navigation prefix includes the subsequent dot-dot regression, which is outside the 32 / 549 focused slice;
- production deferred-Busy admission: 1 case / 22 assertions. A `Started` observer queues a gated legacy loading task and submits a nested asynchronous navigation; the first request becomes `Cancelled(InternalAbort)`, the deferred successor resolves once as `Rejected(Busy)` with `EBUSY` admission feedback, and neither provider fetch nor model commit occurs;
- adjacent `file.open` evidence: core 23 / 201, focused production route 1 / 66, combined 24 / 267; the full Registry fixture passes 3 / 122;
- incremental application build, `git diff --check`, project-file lint and localization JSON validation belong to the local verification boundary.

Hosted CI has not run for this slice. The first hosted M0 workflow run remains a separate release-gate evidence item.

## Remaining work

- migrate the remaining folder/archive open-location adapters while preserving Enter as their navigation router;
- migrate remaining P0 commands and command contexts through the Registry;
- persist per-pane History and correlate Back/Forward restore with navigation lifecycle identity in M2;
- complete live-provider, permission and manual keyboard/VoiceOver evidence.
