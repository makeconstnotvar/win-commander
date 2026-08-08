# Q2-3 HP-1: Hotkey profiles and the conflict detector

> Status: implemented and tested — see §Verification. Model increment: profiles are defined and validated but not yet selectable in the UI, see §Scope.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-3.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §26.2 (P1: Total Commander-like, Finder-like and Windows Explorer-like profiles; shortcut conflict detector).

## Scope

Two things land here: the named profiles §26.2 asks for, and the conflict detector that keeps them — and any user's own overrides — honest.

Not here: the Preferences UI that selects a profile, and applying one at runtime. `ActionsShortcutsManager` already exposes exactly what that needs (`RevertToDefaults` plus `SetShortcutsOverride` per binding), so applying a profile is a short, mechanical step once there is a surface to choose from; the Vim-like profile of §26.2 is deliberately not attempted.

## The conflict detector

`DetectShortcutConflicts` reports every set of actions in the same domain claiming one key equivalent.

**Domains are the whole difficulty.** Shortcut lookup in this codebase is domain-filtered (`ActionTagsFromShortcut(shortcut, in_domain)`), and the action tables carry three domains — `menu`, `panel`, `viewer`. A `menu.file.copy` and a `panel.copy` bound to ⌘C are not in conflict; they are how the application is meant to work. A detector that flagged them would produce a wall of false positives on the existing tables, and a detector nobody trusts is worse than not having one. So the domain is part of the grouping key, and cross-domain sharing is silent.

Unbound actions collide with nothing — any number of actions may have no shortcut, and treating "" as a claimed key would report the entire unbound half of the table as one enormous conflict.

Results are ordered by first appearance and grouped explicitly rather than through a hash map's iteration order, so a report reads in the same order as the table it came from and two runs over one input are byte-identical rather than merely equivalent.

## The profiles

Four, `Default` first. `Default` deliberately carries no bindings: selecting it is how a user returns to the application's own shortcuts, which is `RevertToDefaults` and nothing else.

- **macOS native** — Return renames, ⌘↓ opens, ⌘↑ goes to the enclosing folder, ⌘⌫ trashes, ⌘I gets info.
- **Windows Explorer** — F2 renames, Delete trashes, ⇧Delete deletes permanently, ⌥← / ⌥→ / ⌥↑ move through history and up.
- **Commander** — F5 copies, F6 moves, ⇧F6 renames in place, F7 makes a folder, F8 trashes, ⌘U swaps panes.

### The detector caught a real defect in this slice's own data

The first draft of the Commander profile bound **F5 to both copy and rename**, because Total Commander renames on ⇧F6 rather than on a bare function key and I had written it from memory. The `ships no profile that conflicts with itself` case is what makes that class of mistake impossible to land — it runs the detector over every built-in profile, so the data is checked by the same code the user's own overrides will be. A companion case rejects a profile that binds one action twice, which the conflict detector cannot see (a duplicate action with *different* keys is not a key collision).

Those two cases are the reason this increment is worth having as its own slice rather than being folded into whatever eventually applies the profiles: they are the part that will still be protecting the data after the UI exists.

## Verification

Built and run in this session (Xcode 26.6 toolchain):

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::DetectShortcutConflicts*' --rng-seed 424242`: **6/6 cases, 15/15 assertions** — a same-domain collision reported with both actions; a key shared across domains not reported; unbound and nameless bindings colliding with nothing; three-way grouping with first-appearance order and run-to-run identity; a dotless action name forming its own domain; an empty set.
- Focused `WinCommanderUT 'nc::core::AllShortcutProfiles*' --rng-seed 424242`: **4/4 cases, 18/18 assertions** — Default first and empty, the exact id list, no built-in profile conflicting with itself, no built-in profile binding an action twice, and id resolution rejecting unknown and empty ids.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **666/666 cases, 11,038/11,038 assertions**.
- No ASAN/UBSAN run: pure value code with no allocation ownership, concurrency or `Operations`/`VFS`/`RoutedIO` involvement.

### Known gaps

- **The profiles are not validated against the live action tables.** A binding naming an action that does not exist would be silently ignored by `SetShortcutsOverride`, and nothing here catches that yet — the check needs the real table, which `WinCommanderUT` does not bootstrap. Worth adding when the profiles become selectable, because a typo in a profile is otherwise invisible.
- **The detector is not yet run over the shipped tables.** Doing so would have caught the ⇧⌘O collision found by hand during Q2-3 CP-2 (`menu.go.documents` already held it). That is a natural follow-up once the tables can be loaded in a test.
- No Preferences UI, no runtime application, and no Vim-like profile.
