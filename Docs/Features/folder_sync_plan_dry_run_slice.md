# Q2-2 CS-3: One-way sync plan and dry run

> Status: implemented and tested — see §Verification. Model increment: no user-visible surface yet, see §Scope.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-2.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §24.2 (P1: one-way sync, dry-run, delete handling, conflict policy), §45 (dry-run mandatory before destructive sync; deletion separate and highlighted), §15 (sync deletion always previewed in dry-run).
> Depends on: [`folder_comparison_foundation.md`](folder_comparison_foundation.md) (CS-1).

## Scope

`PlanOneWaySync` turns a `FolderComparison` plus a direction into the exact set of actions a sync would perform. Building the plan touches no filesystem, so **the plan value is itself the dry run** the spec requires — there is no separate "simulate" mode that could drift from the real one, because execution (CS-4) is specified to consume this plan rather than re-derive its own.

No surface yet: nothing calls `PlanOneWaySync` in production. CS-4 adds the preview UI and submits the plan through the established `nc::ops::*` operations.

## Contract

Every comparison entry produces exactly one action, so a preview can never omit a name the comparison saw. Nothing is silently resolved: a pair this level cannot decide becomes a `Skip` carrying its reason, never a guess.

| Comparison entry | Action (source → destination) |
|---|---|
| source-only | `Create` |
| destination-only | `Delete` when deletion was requested, otherwise `Skip(DeletionNotRequested)` |
| changed | `Overwrite` when overwriting was requested, otherwise `Skip(OverwriteNotRequested)` |
| same, file | `Skip(Identical)` |
| same, directory | `Skip(DirectoryContentsNotCompared)` |
| conflict | `Skip(TypeConflict)` |

Direction is applied by relabelling each entry's two sides as source and destination, so `RightToLeft` is the same code path with the roles exchanged — including the indices, which then address the opposite listing. That is why "source-only" and "destination-only" are derived per direction rather than stored on the comparison.

### Decisions worth naming

**A `Same` directory is not `Identical`.** CS-1 compares a directory pair by presence only and states as a consumer obligation that `Same` on a directory says nothing about its contents. Folding that into `Identical` here would be exactly the bug that note warns about — a sync reporting "nothing to do" for a directory whose contents differ. It gets its own reason instead, so a preview can say plainly that the contents were not compared and CS-4 knows recursion is still owed.

**Deletion is opt-in and separately exposed.** `delete_extraneous` defaults to false: removing entries the source does not have is the one genuinely destructive part of a one-way sync, and §15/§45 require it be previewed and chosen rather than assumed. `HasDeletions()` is the gate a destructive-confirmation step reads, and `Deletions()` returns just those actions so the preview can present them as their own highlighted group rather than buried in a list.

**An overwrite that would replace a *newer* destination copy is flagged.** `overwrites_newer_destination` does not change what the sync does — one-way means one-way — but it is the case the user most needs told about, because they are about to lose the more recent of the two files. `Summarize()` counts it separately so a summary line can lead with it.

**Conflicts are skipped, never resolved.** A directory facing a file under one name has no safe direction at all, so it reaches the user as its own status rather than being folded into the copyable set.

## Verification

Built and run in this session (Xcode 26.6 toolchain):

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED** (the slice adds files to the Xcode project).
- Focused `WinCommanderUT 'nc::core::PlanOneWaySync*' --rng-seed 424242`: **7/7 cases, 60/60 assertions** — one action per compared name; each status mapped in both directions with indices following the relabelling; deletion opt-in, its summary counter and its separate `Deletions()` list; the newer-destination flag in both directions; disabled overwrite; an inert plan reported as empty; an empty comparison.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **633/633 cases, 10,889/10,889 assertions**.
- No ASAN/UBSAN run: a pure value model with no allocation ownership, concurrency or `Operations`/`VFS`/`RoutedIO` involvement. CS-4, which submits real mutations, is where that budget changes.

`plutil -lint` accepts the project file, and both the library and test targets build — the project-metadata check `AGENTS.md` asks for when project metadata changes.
