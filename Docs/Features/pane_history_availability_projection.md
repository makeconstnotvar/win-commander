# Feature: Pane history availability projection

> Status: M1 read/presentation slice implemented
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 7 and 17
> Execution tracker: `Docs/Development-Plan.md`, R2 in `Docs/refactor_plan.md`

## Contract

`Panel::History` remains the sole owner of its bounded deque and recording/playback cursor. The follow-up current-entry slice evolves this into `NavigationState`: Back/Forward availability plus a nonzero runtime entry identity and one synchronous advisory callback. `Put`, `MoveBack`, `MoveForth`, and `RewindAt` notify exactly once when either availability or current identity changes. Callback registration, duplicate/current `Put`, invalid/repeated `RewindAt`, and VFS-manager configuration are silent. Callback exceptions are contained after the History mutation has committed.

`PanelController` installs a weak-view callback that posts the existing pane-scoped context notification on the main queue. `PanelControllerPaneStoreAdapter` reads the pair into `PaneState::history_availability` before the unloaded-state early return. Availability-only rebuilds advance snapshot `revision`, preserve `listing_generation`, and compose with current Loading/Refreshing/Failed lifecycle overlays.

## Navigation sequencing

Back/Forward changes the History cursor before the asynchronous listing restore starts. Its callback therefore schedules an immediate Store rebuild even if loading later fails or is cancelled. The current restore path then submits `ListingPromiseLoader::Load`; a later model/path notification may publish the loaded listing and record the visit, but this legacy task has no `PaneRequestId` or exactly-one lifecycle terminal outcome. History-only revisions never advance `listing_generation`.

## Explorer presentation

`NCExplorerToolbarDelegate` starts Back/Forward disabled and accepts navigation availability only through a matching-pane `PanePresentationModel`. It supplies that Store state to the `navigation.back` and `navigation.forward` Registry definitions and renders the result with `CommandPresentationAdapter`. A foreign snapshot supplies unavailable state, disables both buttons, exposes the shared reason, and is not forwarded to the breadcrumb. Execution passes through the responder-compatible selectors into Registry; the injected executor delegates to the existing actions and re-checks live `History`, so cached presentation state never becomes execution authority.

## Related and deferred scope

The implemented follow-ups define [`navigation.back`/`navigation.forward` Registry routing](navigation_history_command_registry_slice.md) and the [runtime current-entry identity](pane_history_current_entry_identity.md). Lifecycle-correlated restore and persisted per-pane history remain separate lifecycle/persistence contracts. `PanelControllerPersistency` continues to restore location/sort/layout, and closed-panel recovery retains only the most recent location.

## Verification

- current `PanelHistory navigation state *`: 5 cases / 457 assertions;
- Store: 18 / 220; reducer: 19 / 335; production bridge: 28 / 461;
- combined History/Store/reducer/bridge: 70 / 1,473;
- matching-pane Explorer presentation and toolbar: 2 / 266;
- incremental Debug `WinCommanderUT` build and `git diff --check`: passed.
