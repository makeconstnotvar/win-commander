# Q2-2 CS-4: Synchronize Directories — preview and execution

> Status: implemented, built and tested — see §Verification.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-2.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §24.2 (one-way sync, dry-run, delete handling), §45 (direction visible; deletion separate and highlighted; dry-run mandatory before destructive sync; execution goes through the Operation Engine), §15 (Trash preferred over permanent delete; sync deletion always previewed).
> Depends on: [`folder_sync_plan_dry_run_slice.md`](folder_sync_plan_dry_run_slice.md) (CS-3), [`folder_compare_explorer_surface_slice.md`](folder_compare_explorer_surface_slice.md) (CS-2), [`dual_pane_explorer_skeleton_slice.md`](dual_pane_explorer_skeleton_slice.md) (DP-1).

## Product surface

`View ▸ Synchronize Directories…` compares Explorer's two dual-pane sides, shows the dry run, and — only after the user chooses — makes the opposite side match the focused one. It is disabled unless dual-pane is on, both sides show a uniform listing, and the destination is writable.

Direction is **focused side → opposite side**, stated verbatim in the sheet's title (`Synchronize "<source>" into "<destination>"?`), so the one thing §45 insists be visible is the headline rather than an inference from which pane is highlighted.

The sheet's body is CS-3's plan rendered directly: how many items would be copied, how many replaced, how many left untouched — and, when it applies, the line that matters most: *"N of those are NEWER in the destination and will be lost."* Folders that are already in sync say so and offer nothing to run.

### Deletion is a separate button, not a checkbox

When the destination holds entries the source does not, the sheet lists them (up to ten by name, then a count) and offers a **third, separately labelled button**: `Synchronize and Trash N Item(s)`. The default button never deletes anything.

This is a deliberate reading of §45's "deletion is separate and highlighted". A checkbox can be left armed from a previous run and then swept along by muscle memory on the default button; a distinct button cannot be — choosing destruction and choosing to proceed are the same click, so the destructive path is never the path of least resistance. The offer appears only for a **native** destination, because Trash is what §15 asks for and a Trash only exists there; on any other provider those entries stay skipped rather than silently escalating to a permanent delete, which is a reviewed action this slice does not introduce.

Removal goes to the Trash, never `unlink`.

## The gate between review and execution

The plan the user approves is built from listings sampled when the sheet opened. Between that moment and the click, either pane can refresh, navigate, or be mutated by another operation. Submitting the approved plan blindly would then act on files nobody reviewed — so execution re-establishes the whole chain before anything reaches the `Pool`:

1. The availability predicate is re-evaluated (dual-pane still on, both sides still uniform, destination still writable).
2. Both panes must still hold **the exact `VFSListingPtr` objects** the preview was built from, and neither may be mid-load.
3. Both sides are re-collected and their sorted positions must be identical to the ones captured at compare time.
4. CS-3's `BindSyncPlan` re-reads the name now at every position the plan references and requires each to still be the name the plan decided about.
5. Each bound position is resolved to a live `VFSListingItem`, and any that fails to resolve aborts.

Every one of those five failures abandons the **entire** plan rather than submitting the part that still matches. That is the whole point: a partially applied plan mutates files the user never saw in the preview they approved, which is a worse outcome than doing nothing and making them look again.

`BindSyncPlan` lives in the pure model precisely so this gate is testable without a filesystem — its atomic-failure behaviour is covered by unit cases, not left to be inferred from the AppKit path.

## Execution

Copies (both new and replaced entries) go out as one `nc::ops::Copying` into the destination directory with `ExistBehavior::OverwriteAll`: a sync's purpose is to make the destination match, so raising the interactive conflict sheet per file would contradict the plan the user just approved. Deletions go out as one `nc::ops::Deletion` with `DeletionType::Trash`. Both are enqueued on the window's existing `Pool` — the same path DP-2's cross-pane copy uses — so Q1-8's progress strip, conflict handling, cancellation and the Operation Center apply unchanged, which is what §45's "execution goes through the Operation Engine" asks for. Both observe completion and refresh the two panes.

The two operations touch disjoint names by construction (copies carry source names, deletions carry destination-only names), so running them concurrently on the Pool is safe.

## Verification

Built and run in this session (Xcode 26.6 toolchain):

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED** (the production target compiles the edited `MainMenu.xib` and `Actions.h`).
- Focused `WinCommanderUT 'nc::core::BindSyncPlan*' --rng-seed 424242`: **3/3 cases, 19/19 assertions** — a reviewed plan resolving onto the listings it decided about; atomic failure when a name moved at a copy position, at a delete position, when a listing shrank below a referenced position, and when both emptied; an inert plan binding to no work even against listings that changed entirely.
- Focused `WinCommanderUT 'nc::core::PlanOneWaySync*' --rng-seed 424242`: **7/7 cases, 60/60 assertions** (unchanged from CS-3).
- Focused `WinCommanderUT 'NCExplorerState tabs *' --rng-seed 424242`: **23/23 cases, 286/286 assertions** (22 from CS-2 plus the new gating case: Synchronize declines and enqueues nothing outside dual-pane and with dual-pane on but no uniform writable destination, whether reached through `-validateMenuItem:` or invoked directly, and is never available where Compare is not).
- Focused `WinCommanderUT '*ActionsShortcuts*' --rng-seed 424242`: **10/10 cases, 81/81 assertions**.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **637/637 cases, 10,917/10,917 assertions**.
- No ASAN/UBSAN run. This slice adds AppKit-adjacent state code that calls existing `nc::ops::Copying`/`Deletion`/`Pool` entry points unchanged and a pure value function; no `Operations`, `VFS`, `RoutedIO`, journal, codec, raw-buffer, concurrency or lifetime internals were touched. That is the same tier DP-2 recorded for the same reason, and matches `AGENTS.md`'s budget, which scopes sanitizer runs to changes *in* those subsystems rather than to any caller of them.

### Known coverage gaps

- The happy path — a real sync moving real files — is not exercised by `WinCommanderUT`. The mock `PanelController`/`PanelView` fixture has no real listing, so `-isUniform` is false and the gate declines before any listing is read; this is the same tier boundary DP-2 recorded for cross-pane copy/move, and the recommended follow-up is the same `WinCommanderIT` case, now covering compare/sync as well. The decision logic either side of that boundary — what the plan contains, and whether a changed listing may be submitted — is fully covered at the unit tier by CS-1/CS-3/CS-4 model cases.
- No local UI smoke through `Scripts/build_stable_dev_and_run.sh`, for the reason DP-1/DP-2/DP-3 recorded: no tool in this session drives a running native macOS app's UI. For this slice that gap is worth naming explicitly, because the sheet's wording and button order are the safety surface, and they have been reviewed only as code.
- Recursive sync of a directory that exists on both sides is still not performed: CS-1 compares such a pair by presence only, and CS-3 surfaces that as `DirectoryContentsNotCompared`. The preview counts those under "left untouched". Closing this needs recursive comparison, which §24.3 places in a later increment.
