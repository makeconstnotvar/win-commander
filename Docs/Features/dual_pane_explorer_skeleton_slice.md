# Q2-1 DP-1: Dual-pane Explorer skeleton and focus switching

> Status: implemented, built and focus-tested — see §Verification.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-1.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §13.2 (Dual Pane Mode, active/opposite panel and independent tabs per pane only — this slice does not cover §13.3 Multi Pane/Workspace).

## Product surface

Explorer can now hold two independent panes side by side. `View ▸ Switch to Dual Pane` (the existing legacy `onSwitchDualSinglePaneMode:` menu item, reused via Cocoa's first-responder dispatch rather than a new menu entry) toggles a second, fully independent pane into the content area next to the existing one. Each side keeps its own ordered tabs, navigation history, per-tab `PaneStoreAdapter`, search controller and view-settings binding — exactly the machinery a single Explorer pane already had, now instantiated twice. Tab switches `key` (⇥) between the two sides without touching either side's active tab; clicking into a side, or activating one of its tabs, also focuses it. Shared chrome (sidebar, toolbar, command bar, Details/Preview inspector, Search Mode) stays a single instance and rebinds to whichever side is currently focused — the same `rebindToPanelController:`/`rebindToPaneID:` hook Q1-4 already used for active-tab switching within one pane.

Explicit non-goals for this slice, matching the approved plan: no per-side collapse/expand, no F5/F6/F7 or cross-pane Copy/Move commands (Q2-1 DP-2), and no persistence of the dual-pane layout across restarts (Q2-1 DP-3). Turning dual-pane on or off at runtime is not yet itself persisted; a restarted window always reopens single-pane. *(Both non-goals have since been closed — see [`dual_pane_explorer_commands_slice.md`](dual_pane_explorer_commands_slice.md) and [`dual_pane_explorer_layout_persistence_slice.md`](dual_pane_explorer_layout_persistence_slice.md); this paragraph records DP-1's own scope, not current behavior.)*

## Ownership and the extraction seam

`NCExplorerState` used to own one flat bundle of tab-lifecycle state (`ExplorerTabsModel`, per-tab `ExplorerTabEntry` vector, tabbed holder, pane-state overlay, quick-search overlay, PaneStore/view-settings observation). That bundle is extracted verbatim into a new internal C++ value type, `NCExplorerPaneContent` (defined inside `NCExplorerState.mm`; not a separate translation unit, to avoid an unregistered-source-file risk in the Xcode project), and `NCExplorerState` now owns exactly two of them in a fixed `std::array<NCExplorerPaneContent, 2>` keyed by a `NCExplorerPaneSide { Left, Right }` enum. Left is always the side used while dual-pane is off, so every existing single-pane code path is unchanged in shape — it just now reads through `[self focusedContent]` (which resolves to Left whenever `m_DualPaneEnabled` is false) instead of a bare ivar. `NCExplorerState.panelController` becomes a computed property returning the focused side's active panel rather than a stored ivar.

`NCExplorerPaneContent` instances are never copied, moved, or reallocated — they live at a fixed address for the state's whole lifetime — which is what lets the PaneStore-observation and view-settings-context-notification blocks capture a `__weak NCExplorerState *` and safely re-derive the owning content by side after confirming the owner is still alive, instead of ever capturing a raw `this`.

## Real `NCPanelControllerHostingState` conformance

`leftPanelController`/`rightPanelController`/`isLeftController:`/`isRightController:`/`bothPanelsAreVisible` are no longer single-pane stubs; they report the real per-side state (`rightPanelController` is `nil` and `bothPanelsAreVisible` is `false` whenever dual-pane is off). `splitView` still returns `nil` unconditionally: its only production caller (`ShowQuickLook`'s collapse-restore) is typed to the legacy `FilePanelMainSplitView`, which this slice's plain `NSSplitView`-based dual-pane layout is not, and DP-1 has no per-side collapse to restore anyway. `anyPanelCollapsed` stays `false` for the same reason.

Content-area layout is a nested pair of `NSSplitView`s: the existing `m_ContentSplitView` (pane area ↔ inspector) now hosts either one side's container directly (single-pane) or a new `m_PaneSplitView` (left container ↔ right container) in its first slot. Swapping between the two is a `replaceCurrentPaneAreaViewWith:` helper that always reads `m_ContentSplitView`'s current first subview *before* any other reparenting happens, specifically to avoid corrupting `m_ContentSplitView`'s own two-subview invariant (a view can have only one superview, so reparenting a side's container into `m_PaneSplitView` before the swap would silently evict it from `m_ContentSplitView` first).

## Focus model

`m_FocusedSide` is an explicit ivar, not derived from `NSResponder.firstResponder`, updated at exactly the two points that ever legitimately change it: `bindActivePanel:focus:` (tab activation/closure/creation within a side — becomes the focused side whenever dual-pane is off, `focus:` is requested, or the side was already focused) and the new `focusSide:`/`changeFocusedSide` pair (pure focus movement with no tab-model change, used by the Tab key). `focusSide:` is a no-op when dual-pane is disabled or the requested side is already focused, so Tab is inert outside dual-pane — existing single-pane keyboard behavior is unchanged.

## Testability seams

Two of `setDualPaneEnabled:`'s steps are only meaningful against a real app environment and are therefore behind overridable seams that default to production behavior and mirror the existing `allocateExplorerPanelForSessionRestore`/`initForTestingWithFrame:...` pattern:

- `allocateExplorerPanelForDualPane` / `dualPaneCreatesPaneStore` — allocating the right side's panel and (by default) giving it a real `PanelControllerPaneStoreAdapter`, exactly like the initial Left panel gets in production.
- `dualPaneRightSideTestingContentViewForPanel:` — returns non-nil only in tests, routing the right side through `NCExplorerPaneContent::BuildTestingContainer` (a bare view, no real `FilePanelsTabbedHolder`) instead of `BuildViews`. A real `FilePanelsTabbedHolder` renders through `NCPanelTabBarView`, which asserts a non-null `ThemesManager` current theme; `WinCommanderUT` never bootstraps that global, the same pre-existing gap `Development-Plan.md` §2.1 records for `g_Config`, and it is why the Left side's own testing path (`BuildTestingContainer`, used since Q1-4) has always avoided constructing a real tabbed holder too.

## Verification

Built and run in this session (`sudo xcode-select -s /Applications/Xcode.app`, Xcode 26.6):

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**, no errors or warnings in `NCExplorerState.mm` / `ExplorerTabsState_UT.mm`.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED** (production app target, not just the test target).
- Focused `WinCommanderUT 'NCExplorerState tabs *' --rng-seed 424242`: **13/13 cases, 186/186 assertions**, including the four new dual-pane cases (happy-path toggle create/teardown; closing a side's last tab while dual-pane is active is a disabled no-op that touches neither the window nor the other side; toggling off and back on leaves the Left side's tabs/PaneIds untouched; Tab switches focus between sides only while dual-pane is active and is inert for a panel view owned by neither side).
- Focused `WinCommanderUT 'NCExplorerInspectorView *' --rng-seed 424242`: **10/10 cases, 132/132 assertions** (the other test file that constructs `NCExplorerState` directly).
- Focused `WinCommanderUT 'NCMainWindowController default Explorer *' --rng-seed 424242`: **4/4 cases, 35/35 assertions**.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **148/149 cases, 2301/2302 assertions**; the one failure is the pre-existing, unrelated `assert(g_Config)`-class defect recorded in `Development-Plan.md` §2.1 (this run hit its `PanelPresentationGeometry_UT.mm` sibling, `assert(g_CurrentTheme)` in `ThemesManager`, from a test with no relation to Explorer) — confirmed present before this slice's changes and out of scope for it.
- No ASAN/UBSAN run: this slice only touches AppKit/Registry-adjacent presentation code (`NCExplorerState`, its test file) and calls existing `Operations`/`VFS`/`RoutedIO` entry points unchanged — per `AGENTS.md`'s verification-budget rule, focused filter plus Debug build is the right tier.
- Not run: a local UI smoke via `Scripts/build_stable_dev_and_run.sh` (build/sign/install/launch the real app and exercise the View ▸ Switch to Dual Pane menu item by hand). This session has no tool that drives a running native macOS app's UI (unlike the iOS Simulator control available for iOS work), so this step needs a human or a future session with that capability.

### Regressions found and fixed during this verification pass

The mechanical extraction into `NCExplorerPaneContent` introduced three bugs, all caught by the existing test suite once it could actually run:

1. `restoreTabsFromSession:` called the new `attachExplorerTabPanel:createPaneStore:toContent:` directly instead of the overridable 2-argument `attachExplorerTabPanel:createPaneStore:`, silently bypassing `ExplorerTabsTestState`'s simulated-attach-failure override and breaking the existing "rolls back a partially attached restored topology" case.
2. `NCExplorerPaneContent::AttachTabView` dropped the non-tabbed-holder fallback (adding a newly attached panel's view as a hidden subview of the side's own container) that the original `attachExplorerTabViewForPanel:` had; without it, any tab beyond the first was never added to any view hierarchy in the no-tab-strip (unit test) configuration, so `-hidden` state and `-makeFirstResponder:` assertions on inactive/newly-activated tabs failed.
3. `ExplorerTabsTestToolbar` (test double) had no `-toolbar` property; `-focusAddressFieldShowingToolbarIfNeeded`, reached for the first time by this slice's new Tab-key handling, sent it an unrecognized selector. Fixed on the test side (`-toolbar` returns `nil`), not in production code.
