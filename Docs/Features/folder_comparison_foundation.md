# Q2-2 CS-1: Folder comparison foundation

> Status: implemented and tested — see §Verification. Foundation increment: no user-visible surface yet, see §Scope.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-2.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §24.1 (P0: compare two folders by name/size/date, show left-only/right-only/changed/same), §45 (difference statuses).

## Scope

This increment builds the pure comparison contract Q2-2's product surface and its later sync increments both need, and nothing else. It adds no command, menu item or view — `nc::core::CompareFolders` has no production caller yet. That is deliberate: the comparison rules are where the correctness lives (what counts as *changed*, what a name collision across two volumes means, which side is newer), they are fully testable without a window, and the surface increments below can then be reviewed as presentation rather than as semantics.

Remaining Q2-2 increments, in intended order:

- **CS-2** — the compare surface: run the comparison over Explorer's two dual-pane sides and show the result. §28.2 puts `[Compare]` in the dual-pane command bar; the spec also reserves the `sync.compare` command ID (§ command roster). No existing surface can be reused — legacy ⌥⌘U `menu.view.sync_panels` / `SyncPanels` is *navigation* (point the opposite panel at the active panel's directory), not folder comparison, so this needs its own entry point.
- **CS-3** — one-way sync plan plus mandatory dry-run with deletion preview (§24.2 P1, §45 acceptance criteria).
- **CS-4** — execution through the established `nc::ops::*` operations, with the partial-failure report and operation log §24 requires.

## Contract

`CompareFolders(left, right, options)` takes two listings already reduced to what the comparison judges — name, size, modification time, directory flag — and returns one ordered entry per distinct name, or a typed failure. Keeping the input a reduced value type rather than a VFS listing is what makes the model free of provider, window and lifetime concerns.

Statuses implement §45's set that P0 can actually decide: `Same`, `LeftOnly`, `RightOnly`, `Changed`, `Conflict`. The remaining spec statuses belong to later increments and are deliberately absent rather than stubbed — `Ignored` needs the include/exclude rules of §24.2, and `PermissionBlocked` needs a real provider probe, which a pure model over two listings cannot honestly produce.

Entry ordering is left input order, then right-only names in right input order, and every entry carries the index it came from on each side. A consumer can therefore map a verdict straight back onto the exact rows it passed in — which is what CS-2's marking of differing files in both panes needs.

### Decisions worth naming

**Names are matched byte-exactly.** Two names differing only in case, or in Unicode normalization form (NFC `é` vs NFD `e`+U+0301, which macOS filesystems genuinely both produce), report as `LeftOnly` plus `RightOnly` rather than as one matched pair. This is the conservative direction: a later sync sees two distinct names and cannot silently overwrite one with the other, whereas a wrong pairing would. Correct matching here needs real Unicode case folding plus HFS+/APFS normalization awareness, and a half-correct version would be worse than a visible non-match. Deferred to the increment that has a provider to ask.

**A directory present on both sides is compared by presence only** and reports `Same`. Nothing is claimed about its contents; recursive comparison is a later increment. The header states this as a contract obligation on consumers — reading `Same` on a directory as "nothing to do" without recursing would be a real sync bug, and this is the one place to prevent it.

**A name that is a directory on one side and a file on the other is `Conflict`**, never `Changed`. There is no direction a one-way sync could safely take for such a pair, so it must reach the user as its own status rather than be folded into the copyable set.

**`newer_side` is reported independently of the status.** A pair compared by size alone can be `Same` while the modification times still differ; a one-way sync needs that direction anyway. Computing it always keeps the criteria (what counts as changed) separate from the direction (which side would win).

**Invalid input is rejected atomically rather than guessed at.** Duplicate names within one listing make matching ambiguous; empty names and the `.`/`..` navigation entries are not comparison subjects; a negative tolerance is not a usable criterion. Each returns its own `FolderCompareFailure`.

**The timestamp tolerance exists for coarse filesystems.** exFAT stores two-second resolution, so a byte-identical copy would otherwise report `Changed` forever. The default is 0 — strict — and a caller opts in.

### Timestamp overflow

`NewerSide` takes the difference in unsigned arithmetic. Filesystem timestamps can legitimately sit at the extremes of `int64_t`, where the signed subtraction `left - right` is undefined behaviour and can invert the verdict. This was a real defect in this slice's first draft, caught by the extreme-value case: the unsigned form yields the exact difference across the whole range on two's-complement, and the maximal span (2⁶⁴−1 seconds) correctly exceeds even the largest expressible tolerance rather than wrapping into "same".

## Verification

Built and run in this session (Xcode 26.6 toolchain):

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**, no errors or warnings in the new files.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED** (the slice adds files to the Xcode project, so the application target is built too, not only the test target).
- Focused `WinCommanderUT 'nc::core::CompareFolders*' --rng-seed 424242`: **8/8 cases, 86/86 assertions** — every P0 status in one pass; entry ordering and index round-trip; `newer_side` under each criteria combination and its absence for unmatched/directory/conflict entries; each criterion and the tolerance boundary on both sides; byte-exact matching for case and NFC/NFD; each of the four typed rejections; empty and identical listings; timestamp extremes.
- No ASAN/UBSAN run: this is a pure value model with no allocation ownership, concurrency, raw buffers or `Operations`/`VFS`/`RoutedIO` involvement — per `AGENTS.md`'s verification budget, a focused filter plus a Debug build is the right tier. The increment that submits real mutations (CS-4) is where that budget changes.

The Xcode project gained a `Core/Compare` group plus the model and its test file; `plutil -lint` accepts the project, and both the library and test targets build, which is the project-metadata check `AGENTS.md` asks for when generated sources or project metadata change.
