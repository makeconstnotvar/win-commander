# Q2-2 CS-2: Compare Directories in Explorer's dual pane

> Status: implemented, built and tested — see §Verification.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-2.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §24.1 (P0: compare two folders, show left-only/right-only/changed/same).
> Depends on: [`folder_comparison_foundation.md`](folder_comparison_foundation.md) (CS-1), [`dual_pane_explorer_skeleton_slice.md`](dual_pane_explorer_skeleton_slice.md) (DP-1), [`dual_pane_explorer_commands_slice.md`](dual_pane_explorer_commands_slice.md) (DP-2).

## Product surface

`View ▸ Compare Directories` compares Explorer's two dual-pane sides and leaves each pane with exactly the entries that differ selected. The menu item is disabled unless dual-pane is on and both sides show a uniform listing.

The marking rule is "what would have to travel to the other side": a name only one side has is marked there; a changed pair is marked on whichever side is newer, and on both when no timestamp resolves the direction; a name that is a directory on one side and a file on the other is marked on both; identical entries are left unmarked.

That makes this increment immediately useful rather than merely informational, because it composes with DP-2: **Compare, then F5** copies the marked set to the opposite side — a manual one-way sync available today, with the existing copy dialog, progress and conflict handling. CS-3's dry-run and CS-4's execution replace the manual step with a planned one; they do not replace this surface.

Out of scope here, per the plan's Q2-2 decomposition: the full compare screen of §45 (criteria picker, per-file diff, include/exclude rules), the dry-run summary and deletion preview (CS-3), and sync execution (CS-4).

## Why a new menu item

No existing surface could be reused. Legacy ⌥⌘U `menu.view.sync_panels` / `SyncPanels` sounds related but is *navigation* — it points the opposite panel at the active panel's directory (`Actions/SyncPanels.mm` issues a `DirectoryChangeRequest`), and has nothing to do with comparing contents. So this slice adds one item, following the established pattern exactly:

- `Actions.h` gains `menu.view.compare_directories` → tag `13'290` and an empty default shortcut. Empty is deliberate: the action is meaningful only in Explorer's dual-pane mode, so claiming a global key combination for it would take that combination away from Commander, where the item is permanently disabled.
- `MainMenu.xib` gains the item in the View menu next to Swap/Sync Panels, wired to `OnCompareDirectories:` with `target="-1"` (First Responder).
- `NCExplorerState` implements that selector directly, so AppKit resolves it through the responder chain while Explorer is the visible state — the same mechanism DP-1 used for `onSwitchDualSinglePaneMode:` and DP-2 for F5/F6/⌘U. In Commander mode nothing in the chain implements the selector, so AppKit disables the item on its own.

`-validateMenuItem:` and the action both call one shared predicate, `-canCompareDualPaneDirectories`, for the same reason DP-2 shares `-canCopyOrMoveFromActive:toOpposite:`: an action invoked directly — from a test, or a future toolbar button — must never get further than its own menu item's enabled state would allow.

## Reading the two panes consistently

`-collectCompareSideForPanel:` reduces one pane's visible listing to CS-1's value type, dropping dot-dot (a navigation entry, never a comparison subject) and keeping each item's **sorted position** alongside it. That position is what lets a verdict be marked back onto the exact row it came from, and it is why the marks are computed against collected indices and then expanded onto the pane's full selection vector rather than assumed to line up.

Two correctness points:

**Both listings are revalidated before either selection is applied.** The established single-pane idiom (`Actions/Select.mm`) captures `ListingPtr()`, computes, then rechecks `isDoingBackgroundLoading` and the listing pointer before applying. With two panes there is a second failure mode that idiom does not cover: a refresh landing on one side after its items were collected but before the other's are. Applying then would mark the two panes from inconsistent snapshots — a compare result that never actually existed. So both sides are rechecked together, and the whole operation declines rather than applying a partial result.

**A provider that publishes no modification time disables the date criterion** instead of treating every missing timestamp as the same instant, which would silently report unrelated files as `Same`. `VFSListingItem::HasMTime()` is false for some providers, so the collection records it and the comparison falls back to size alone.

## Verification

Built and run in this session (Xcode 26.6 toolchain):

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED** (the production target is what compiles the edited `MainMenu.xib` and `Actions.h`).
- Focused `WinCommanderUT 'nc::core::CompareFolders*' --rng-seed 424242`: **8/8 cases, 86/86 assertions** (unchanged from CS-1).
- Focused `WinCommanderUT 'nc::core::MarkDifferences*' --rng-seed 424242`: **3/3 cases, 23/23 assertions** — every status projected onto the correct side including both directions of `Changed` and the no-direction case; no row marked outside the stated listing size; identical listings mark nothing.
- Focused `WinCommanderUT 'NCExplorerState tabs *' --rng-seed 424242`: **22/22 cases, 277/277 assertions** (21 from DP-3 plus the new gating case: Compare declines and stays inert both outside dual-pane and with dual-pane on but no uniform listing, whether reached through `-validateMenuItem:` or invoked directly).
- Focused `WinCommanderUT '*ActionsShortcuts*' --rng-seed 424242`: **10/10 cases, 81/81 assertions** — the suite that covers the action-name/tag tables this slice extended.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **626/626 cases, 10,829/10,829 assertions**.
- No ASAN/UBSAN run: this increment adds AppKit-adjacent presentation code over a pure model and the established `setEntriesSelection:` path; no `Operations`/`VFS`/`RoutedIO` internals were touched. CS-4, which submits real mutations, is where that budget changes.

### Known coverage gap

The happy path — a real comparison of two real listings, actually marking rows — is not exercised by `WinCommanderUT`: the mock `PanelController`/`PanelView` fixture in `ExplorerTabsState_UT.mm` has no real listing, so `-isUniform` is false and the shared predicate declines before any listing is read. That is the same tier boundary DP-2 recorded for cross-pane copy/move, and the same recommended follow-up: a `WinCommanderIT` case over a real filesystem. The comparison and marking *rules* themselves are fully covered at the unit tier by CS-1's and this slice's model cases; what is untested here is the AppKit plumbing between them.

Also not run: a local UI smoke through `Scripts/build_stable_dev_and_run.sh`, for the same reason DP-1/DP-2/DP-3 recorded — no tool in this session drives a running native macOS app's UI.
