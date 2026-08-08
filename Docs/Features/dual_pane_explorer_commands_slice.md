# Q2-1 DP-2: Cross-pane copy/move/swap commands

> Status: implemented, built and tested.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-1.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §13.2 (active/opposite panel, Copy to other side, Move to other side, Swap).
> Depends on: [`dual_pane_explorer_skeleton_slice.md`](dual_pane_explorer_skeleton_slice.md) (DP-1).

## Product surface

With Explorer's dual-pane layout on, F5 copies the focused side's selection to the opposite side's current directory, F6 moves it there, and ⌘U swaps which panel occupies each side (tabs, history and PaneId travel with it). All three reuse the existing `NCOpsCopyingDialog`/`nc::ops::Copying` machinery — same destination-picker dialog and progress/conflict handling Q1-8 already built, just with the destination pre-filled from the opposite side instead of a clipboard target. Two more spec-listed shortcuts needed no new code at all: ⇧F7 (new folder in the opposite panel) and ⌃F6 (rename in place) already work for Explorer dual-pane today, as a side effect of DP-1's real `NCPanelControllerHostingState` conformance — see below.

Out of scope for DP-2, per the plan: Compare and Sync (§28.2's toolbar also lists these; they are Q2-2's "Folder compare and one-way sync"), ⇧F5/⇧F6 ("copy as"/"move as", which prompt for a renamed destination before proceeding), and the visual dual-pane command-bar row (Copy→/←Copy/Move→/←Move/Swap/Operation Center buttons) — F5/F6/⌘U are fully usable today by keyboard, and the toolbar affordance is a follow-up rather than a blocker for this slice's functional value.

## Why these aren't Registry commands

`NCExplorerState` is not `MainWindowFilePanelState`. Legacy `CopyTo`/`MoveTo`/`SwapPanels` (`States/FilePanels/Actions/CopyFile.mm`, `SyncPanels.mm`) are `StateAction`s that read `_target.activePanelController`/`.oppositePanelController`/call `_target swapPanels` — concrete `MainWindowFilePanelState` members `NCExplorerState` does not have and was never meant to grow (its own doc comment: "a single-pane... alternative to the dual-pane `MainWindowFilePanelState`... it never replaces it"). Porting F5/F6/⌘U therefore means re-deriving "active"/"opposite"/"swap" from `NCExplorerState`'s own dual-pane primitives (`m_FocusedSide`, `m_Sides[Left/Right]`), not reusing the legacy classes.

Given that, adding `CommandId`s/`CommandRegistry` entries for these three would be new ceremony without a driving need: there is no existing precedent for a mode-gated Registry shortcut (checked - `grep` across `Core/Commands/` for anything Explorer/dual-pane-aware is empty), the legacy actions they mirror were never Registry commands either, and the one precedent that *is* Registry-routed and F-key-bound (`file.rename` / Ctrl+F6, via `FileRenameCommand.cpp`'s `legacy` metadata) works through the **per-panel** `PanelControllerActionsDispatcher`, not a per-window-state one — a different, unrelated mechanism from what F5/F6/⌘U need. `OnFileCopyCommand:`/`OnFileRenameMoveCommand:`/`OnSwapPanels:` are plain `IBAction`s on `NCExplorerState`, matching DP-1's own `onSwitchDualSinglePaneMode:` precedent exactly.

## Wiring: no menu/xib/shortcut-table changes

F5/⇧F5/F6/⇧F6/⌃F6/F7/⇧F7/⌘U already exist in `Bootstrap/Actions.h`'s two tables and `MainMenu.xib`, bound (for legacy Commander) to `OnFileCopyCommand:`/`OnFileRenameMoveCommand:`/`OnSwapPanels:` via `StateActionsDispatcher`'s selector→`StateAction` map (`States/FilePanels/StateActions.mm`). AppKit resolves a `target=nil` (First Responder) menu item's action by walking the responder chain from the key window's first responder outward; when Explorer is the active window state, `NCExplorerState` sits in that chain above the focused `PanelView` and below the window, so implementing the *same selector names* directly on `NCExplorerState` intercepts the key there — Explorer's implementation answers before the chain ever reaches `MainWindowFilePanelState`/`StateActionsDispatcher` (which isn't part of the chain while Explorer, not Commander, is the visible state). This is the exact mechanism DP-1 already used for `onSwitchDualSinglePaneMode:`; no `.xib`, `Actions.h`, or shortcut-table edit was needed for DP-2 either.

`-validateMenuItem:` gates all three: `OnSwapPanels:` requires `m_DualPaneEnabled` and both sides to have a panel; `OnFileCopyCommand:`/`OnFileRenameMoveCommand:` additionally require the opposite side to be uniform, have a non-null writable VFS, and the active side to have a non-empty selection — via one shared predicate, `-canCopyOrMoveFromActive:toOpposite:`, called by both the menu validator and the top of each action method, so an action invoked directly (bypassing menu state, e.g. from a test or a future toolbar button) can never proceed further than the menu item's own enabled/disabled state would allow.

### ⇧F7 and ⌃F6 needed no new code

`MakeNewNamedFolderInOppositePanel` (`Actions/MakeNew.mm`) and the rename-in-place command are dispatched per-*panel* (`PanelControllerActionsDispatcher`, attached to every `PanelController.view` regardless of which state hosts it), and `MakeNewNamedFolderInOppositePanel`'s own opposite-panel resolution (`FindOppositeController`) reads only the generic `NCPanelControllerHostingState` protocol (`bothPanelsAreVisible`, `isLeftController:`/`isRightController:`, `leftPanelController`/`rightPanelController`) — exactly what DP-1 already implemented for real. Both shortcuts work for Explorer's dual-pane today purely as a consequence of that protocol conformance; this slice adds no code for them, and their correctness rides on DP-1's own `isLeftController:`/`isRightController:` test coverage plus this slice's "resolves the opposite panel..." case (same underlying primitives).

## Swap and in-flight observation safety

`NCExplorerPaneContent::SwapStaticState` exchanges every field between the two sides **except** the active-panel observation/snapshot state (`m_Panel`, `m_ObservationGate`, `m_PaneStoreObservation`, `m_ViewSettingsContextObservation`, `m_LatestPaneSnapshot`). `-OnSwapPanels:` calls `DetachActiveObservation()` on both sides first, swaps, cross-assigns each side's new active panel, reorders `m_PaneSplitView`'s two subviews so the visual side matches, then calls `BindActiveObservation()` fresh on both sides with their new panel. This is deliberate: an in-flight `PaneStoreAdapter`/view-settings-context callback captured *which side* it belonged to at bind time; silently swapping the observation fields too would leave such a callback reading whichever panel now occupies its originally-captured side after the swap, rather than the panel it was actually bound to. A fresh `Bind()` call is what the observation gate's own contract uses to retire a prior token, so re-binding after the swap is the correctness boundary, not an optimization.

## Verification

Built and run in this session (same Xcode 26.6 toolchain as DP-1):

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'NCExplorerState tabs *' --rng-seed 424242`: **16/16 cases, 216/216 assertions** (13 from DP-1 plus 3 new: swap round-trips both panels' identity and rebinds focused chrome; opposite-panel resolution is dual-pane-and-ownership-gated; copy/move/swap are disabled no-ops outside dual-pane and copy/move stay disabled — not crashing — against the mock fixture's non-uniform default panel).
- Focused `WinCommanderUT 'NCExplorerInspectorView *' --rng-seed 424242`: **10/10, 132/132** (unchanged from DP-1).
- Focused `WinCommanderUT 'NCMainWindowController default Explorer *' --rng-seed 424242`: **4/4, 35/35** (unchanged from DP-1).
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **150/151 cases, 2316/2317 assertions**; the one failure is the same pre-existing, unrelated `assert(g_Config)`-class defect recorded in `Development-Plan.md` §2.1 (this run hit it in `PanelPresentationGeometry_UT.mm` as `assert(g_CurrentTheme)`), confirmed present before this slice's changes.
- No ASAN/UBSAN: this slice adds only AppKit-adjacent `NCExplorerState` methods that call existing `nc::ops::Copying`/`NCOpsCopyingDialog`/`Pool` entry points unchanged; no `Operations`/`VFS`/`RoutedIO` internals were touched.

### Known coverage gap

The full happy path — F5/F6 actually moving files through a real VFS and a real window's sheet/`Pool` — is not exercised by this session's `WinCommanderUT` cases: the mock `PanelController`/`PanelView` fixture used throughout `ExplorerTabsState_UT.mm` has no real listing (so `-isUniform` is false and the guard returns early before ever touching `NCOpsCopyingDialog`/`nc::ops::Copying`), and `-mainWindowController`/sheet presentation need a real window. This mirrors how legacy `CopyTo`/`MoveTo`'s own file-moving behavior is proven at the `WinCommanderIT` (Docker/real-filesystem) tier, not `WinCommanderUT`. A `WinCommanderIT` case exercising an actual cross-pane copy/move end to end is recommended follow-up work, not done here.

Not run: a local UI smoke via `Scripts/build_stable_dev_and_run.sh` (same reason as DP-1 — no tool in this session drives a running native macOS app's UI).
