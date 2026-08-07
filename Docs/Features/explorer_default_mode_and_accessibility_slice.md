# Explorer Default Mode, Large-Folder Interactivity and Accessibility Closure

> Status: Queue 1 Q1-10 complete
>
> Execution tracker: `Docs/Development-Plan.md`, Queue 1 item Q1-10

## User-visible contract

Explorer opens by default for every fresh window: the very first window of a run restores a saved Commander session when one exists, and every other window-creation path (`Cmd+N`, manual restoration without a prior window, system window restoration with nothing decodable) ends in Explorer. Commander remains one `toggleExplorerMode:` away and is still the app's permanent base window state, so nothing about switching back or falling through to it changed.

Folders with 10,000 and 100,000 items stay interactive: the shared navigation pipeline Explorer and Commander both drive keeps the main thread's heartbeat responsive while a 100k listing loads and sorts in the background, and cancelling navigation into a large folder before it commits no longer produces a duplicate, confusing completion notification.

The inline rename field (F2) and the List view's column header row now carry explicit accessibility identifiers and labels, matching the coverage already present across the rest of Explorer's mounted surfaces (sidebar, breadcrumb, toolbar, command bar, quick search, search mode, operation progress, pane states, inspector, tabs, conflict dialog).

## Default-mode startup policy

`nc::bootstrap::PlanDefaultExplorerStartup` (`Source/WinCommander/WinCommander/Bootstrap/AppDelegate+MainWindowCreation.h`) is a pure function of window-creation kind, whether a prior window's state can be copied, and whether session restore is configured on. `ShouldEnsureDefaultExplorer` reduces that plan plus an optional stored-session-restore result to a single boolean consumed by `AppDelegate+MainWindowCreation.mm:allocateMainWindowInContext:`. System window restoration is the sole path that defers entirely to `NCMainWindowController.restoreStateWithCoder:`, which itself falls back to `ensureExplorerMode` when nothing Cocoa-encoded or config-stored can be decoded. Every other creation path resolves through the pure policy function above before a window is shown.

This logic and its four `NCMainWindowController default Explorer` test cases in `Source/WinCommander/WinCommander/Tests/ExplorerTabsState_UT.mm` (startup policy truth table, idempotent `ensureExplorerMode`/`toggleExplorerMode:` entry, external open/reveal routing through the visible pane, and terminal cwd reuse across the first and subsequent Explorer-hosted executions) were already present on `main` going into this slice; this closure verifies them, not authors them.

## Large-folder interactivity fix

`Source/WinCommander/WinCommander/States/FilePanels/PanelController.mm:finishNavigationRequest` reports a request's cancellation to its legacy `LoadingResultCallback` from three call sites. Two of them called `PanelControllerLifecycle::Cancel(...)` and then invoked the callback unconditionally, without checking whether that specific `Cancel()` call was the one that actually published the cancellation (`PaneLifecycleProducer::FinishResult::Published`) or a stale duplicate against a request an earlier, unrelated cancellation (e.g. `CancelBackgroundOperations`) had already terminated and already reported through the modern `PaneLifecycleCancelled` event. `PaneLifecycleProducer::Finish` documents that "delayed and duplicate completions are suppressed," but the caller-side callback dispatch did not honor that for these two branches — a caller subscribed to both channels for the same navigation received one cancellation reported twice.

Fixed by checking the `FinishResult` each `Cancel()` call returns and only invoking the legacy callback when it is `Published`, matching the pattern already used for the adjacent `Fail()` branches in the same function. This is exactly the interactivity case a user causes by cancelling navigation into a huge folder before it finishes preparing.

## Accessibility gap closure

- `Source/Panel/source/PanelViewFieldEditor.mm`: the inline rename `NSTextView` now carries `accessibilityIdentifier = "wincommander.panel.renameField"` and a localized `accessibilityLabel` (via the Panel module's own `nc::panel::NSLocalizedString`, since this module resolves that name to its own resource bundle rather than the standard Foundation macro).
- `Source/WinCommander/WinCommander/States/FilePanels/List/PanelListViewTableHeaderView.mm`: the List view's column header row now carries `accessibilityIdentifier = "wincommander.panel.list.header"` and a localized `accessibilityLabel`.
- `NCExplorerStatusBarView` and `NCExplorerInspectorPlaceholderView` (`Source/WinCommander/WinCommander/States/Explorer/`) were confirmed genuinely unreferenced dead code — both already noted as superseded in `Docs/Evidence-Archive.md` and `Docs/Development-Plan.md` (the real status bar comes from `PanelViewFooter` off a `PaneStore` snapshot; Q1-3's mounted inspector replaced the placeholder) — and removed, along with their `project.pbxproj` entries.

A signed, manual VoiceOver walkthrough across sidebar → toolbar → breadcrumb → panel rows → inspector → search mode → progress/conflict remains explicit release-gate evidence: it requires a human enabling VoiceOver on a real signed build, which is out of scope for an automated coding session (toggling a live accessibility feature on the operator's own Mac is a system-settings change outside this session's authority, not something a headless run can substitute for). The AppKit `accessibilityLabel`/`accessibilityIdentifier`/`accessibilityRole` properties verified here are the automatable half of that evidence.

## What remains explicitly open

A dedicated Explorer-hosted (`NCExplorerState`-wrapped) instrumented large-folder scenario — enumeration timing, first visible frame, scroll latency and memory through the real Explorer view hierarchy rather than the bare shared `PanelController` — was attempted and reverted; see `Docs/Performance/explorer_large_folder_interactivity_2026-08-07.md` for what was tried and why. Since Explorer hosts the exact `PanelController`/`PanelView` pair validated here unchanged, this is a documentation/instrumentation gap, not an open correctness question, and first-frame/scroll/memory profiling under a real AppKit render loop stays deferred to release-gate (M7) hardening.

A pre-existing, unrelated defect was found while closing this slice: the full unfiltered `WinCommanderUT` binary aborts (`assert(g_Config)` in `AppDelegate.mm`) because nothing in the test suite bootstraps the app's global `Config` singleton, and one test (`PanelPresentationGeometry_UT.mm`, Brief-item accessibility) reaches a `GlobalConfig()`-dependent static through `PanelBriefView.mm`. Reproduced identically on an unmodified `main` checkout and in complete isolation, independent of this slice's changes. Tracked separately rather than fixed here since it is out of this slice's scope and needs its own investigation into the right bootstrap seam.

## Verification

Confirmed current-tree evidence:

- focused Debug `WinCommanderUT "NCMainWindowController default Explorer*"`: 4 cases / 35 assertions passed;
- focused Debug `WinCommanderUT "PanelController*"` across four seeds (`424242`, `1`, `999`, `7`): 94 cases / 1,466 assertions passed on each, with zero failures — before the fix, the same filter and seed reproduced 2 deterministic failures (`callback->Calls().empty()`), reproduced on unmodified `main`;
- focused Debug `WinCommanderUT "PanelListViewTableHeaderView*"`: 1 case / 2 assertions passed;
- full Debug `PanelUT`: 56 cases / 1,374 assertions passed (includes the new `PanelViewFieldEditor` accessibility case);
- Debug `WinCommanderUT` and `PanelUT` test-target builds passed.

The full, unfiltered Debug `WinCommanderUT` binary does not currently complete a run — see "What remains explicitly open" above. Comparison against an unmodified `main` checkout confirmed the abort happens at the identical test position with the identical assertion regardless of this slice's changes, ruling out a regression introduced here; the filtered suites above cover every file this slice touched.

Signed, manual VoiceOver walkthrough evidence and the release-gate large-folder end-to-end instrumentation remain explicit release/M7 evidence, consistent with how this plan already treats other human- or hardware-only evidence.
