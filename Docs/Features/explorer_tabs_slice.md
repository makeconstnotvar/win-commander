# Q1-4 Explorer tabs slice

> Status: production implementation locally verified on 2026-08-07
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 13, 17 and 31
> Execution tracker: `Docs/Development-Plan.md`, Queue 1 item Q1-4

## Product surface

Explorer uses the established `FilePanelsTabbedHolder` and `NCPanelTabBarView` surface around an ordered set of file panels. The tab bar is hidden for one tab and becomes visible when a second tab is added. New Tab, Close Tab, previous/next tab, numbered-tab shortcuts, selected-tab clicks and drag reorder use the existing responder and shortcut-manager routes.

Command-T creates and activates a new tab at the current location. The new `PanelController` receives the current display options and listing through the established controller APIs. It owns a fresh `PaneId`, its own `Panel::History` and its own `PanelControllerPaneStoreAdapter`. Command-W closes the active tab while more than one tab exists; the final tab keeps the ordinary window-close behavior. Closing an active tab selects its right neighbour when present and otherwise its left neighbour. Closing an inactive tab preserves the active identity. Drag reorder changes only the ordered tab collection and retains the active `PaneId`.

## Ownership and active projection

`NCExplorerState` owns one ordered entry per tab. Each entry retains exactly one `PanelController` and one Store adapter, so listing, selection, navigation lifecycle and History remain pane-local. `ExplorerTabsModel` owns the toolkit-independent ordered `PaneId` set and active identity. Its create, activate, append, insert, close and reorder mutations reject zero, duplicate, missing, out-of-range and final-tab operations atomically.

Only the active entry has a Store observation ticket. Every bind invalidates the prior `ExplorerTabObservationToken` before releasing that ticket, then issues a fresh generation bound to the new active `PaneId`. A snapshot reaches presentation only when token generation, token `PaneId`, model-active `PaneId`, snapshot `PaneId`, bound controller identity and entry membership all agree. This retires callbacks from inactive and removed tabs before they can update active chrome.

Tab items carry the exact nonzero `PaneId` and exact panel view. Selection, close and reorder callbacks require both values to resolve to the same owned entry. Cross-holder drops, reused identities with a different controller/view and foreign snapshots leave the model and presentation unchanged.

## Atomic active-tab binding

An active-tab switch retires the previous Store observation and pane-local presentation before publishing the new binding. The toolbar dispatcher and breadcrumb, sidebar, command bar and inspector are rebound to one controller identity. The old panel releases its toolbar busy-indicator override and Quick Search presentation; the new panel receives both. Floating Quick Look closes through the existing Explorer ownership path before the active controller changes. The selected panel view becomes first responder, and selecting the already-active tab restores focus without replacing its observation.

Path changes update the matching tab label for every owned controller. Sidebar selection follows only the active controller. Inspector, Quick Search, Quick Look, command presentation and matching Store snapshots remain active-only.

## Verification

- Focused Debug `WinCommanderUT` Q1-4 filters pass 14 cases / 148 assertions.
- The full affected Debug `WinCommanderUT` binary passes 446 cases / 8,316 assertions.
- The model matrix covers unique identity, activation, insertion, close successor, inactive close, final-tab refusal and active-preserving reorder.
- The observation gate proves rejection of inactive-pane, foreign-snapshot and retired removed-tab callbacks.
- AppKit/state coverage proves final-tab Command-W window routing, exact controller membership, active chrome rebind, busy-indicator and Quick Search transfer, Quick Look close routing, focus restoration and same-active no-rebind behavior.
- Real toolbar and inspector rebind coverage proves immediate stale-state clearing and fresh revision admission for the new pane.
- The ordinary unsigned arm64 Debug `UnitTests` scheme build succeeds, and `git diff --check` passes.

Q1-6 owns session serialization and restart restoration for this runtime tab model. Signed interaction, numbered-shortcut walkthrough, drag animation, VoiceOver and screenshot evidence remain Queue 1 and release gates.
