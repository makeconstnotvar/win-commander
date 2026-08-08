# Q2-3 CP-1: Command palette matching and ranking

> Status: implemented and tested — see §Verification. Foundation increment: no user-visible surface yet, see §Scope.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-3.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §26.1 (P0: command palette), §12.2 (modern command palette), § command roster (`commandPalette.open`).

## Scope

`FilterCommandPalette` ranks a command roster against a query. It is the whole of the palette's behaviour that can be wrong in a way a user notices, and none of it needs a window, so it lands first and alone.

No surface yet: nothing calls it in production. Remaining Q2-3 increments:

- **CP-2** — the palette itself: a sheet over Explorer that builds its roster from the `CommandRegistry`, filters through this model on each keystroke, and executes the chosen command through the Registry.
- **HP-1** — hotkey profiles (§26.2 P1: Finder-like, Windows Explorer-like, Total Commander-like), including the shortcut-conflict detector.

## Why ranking is the hard part

A palette is judged almost entirely on whether the thing you meant is the first row. Two properties matter more than raw match quality:

**A title match must always beat a subtitle match.** Subtitles carry menu paths and synonyms, so a common word like "view" appears in many of them. A palette that floats a coincidental subtitle hit above the command whose *name* the user typed is worse than no palette. The subtitle penalty is therefore larger than the entire title score range, which makes the ordering structural rather than a matter of tuning.

**Equal-scoring rows must not reorder between keystrokes.** If two rows tie and their order is left to an unstable sort, the highlighted row can silently become a different command between one character and the next — and the user commits with Return without re-reading. Ties break on roster position as part of the comparator, never left to the algorithm.

Ranking otherwise runs: verbatim at the title's start, verbatim at a word boundary, verbatim anywhere, then subsequence — rewarded for landing consecutively and on word boundaries, so typing the initials of a multi-word command ("ooc" → *Open Operation Center*) finds it, which is how palettes are actually used. Two titles that both contain the query are separated by how much of the title the query accounts for, so `Copy` outranks `Copy Item Path to the System Clipboard` for `copy`.

Subsequence matching is greedy left-to-right rather than an optimal alignment: it is predictable, explicable in one sentence to whoever reads the ranking next, and cheap enough to run on every keystroke over the whole roster.

## Decisions worth naming

**Case folding is ASCII-only.** The same call `CompareFolders` makes and for the same reason: a half-correct Unicode folding is worse than a predictable one. Non-ASCII bytes still match exactly, so a localized title remains findable by typing it — just not by typing it in a different case. Named in the header rather than left to be discovered.

**Disabled entries are shown by default.** A command that exists but cannot run right now is information the user wants — finding it and seeing it greyed out answers "where is that command?", whereas hiding it answers nothing and looks like the command does not exist. `exclude_disabled` is available for a caller that disagrees.

**Offsets are returned, not just scores**, so a row can highlight exactly the characters that matched. Without that the ranking is unexplainable to the person looking at it.

**An empty query lists the roster in its own order** rather than imposing one. Ordering an unqueried palette meaningfully (recent, frequent, contextual) is the roster's job, and CP-2 owns it.

## Verification

Built and run in this session (Xcode 26.6 toolchain):

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED** (the slice adds files to the Xcode project).
- Focused `WinCommanderUT 'nc::core::FilterCommandPalette*' --rng-seed 424242`: **11/11 cases, 47/47 assertions** — empty query lists the roster unchanged; verbatim beats subsequence; start beats word boundary beats anywhere; the tighter of two containing titles wins; a subtitle hit never overtakes a title hit even when the subtitle hit is exact and the title hit is scattered; case-insensitivity and both offset shapes; word-boundary initials; unreachable and wrong-order queries return nothing; equal-scoring rows hold one order across three different queries; result bound, zero bound and the disabled filter; empty roster and empty entry text.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **648/648 cases, 10,964/10,964 assertions**.
- No ASAN/UBSAN run: a pure value function with no allocation ownership, concurrency or `Operations`/`VFS`/`RoutedIO` involvement.

`plutil -lint` accepts the project file and both targets build — the project-metadata check `AGENTS.md` asks for when project metadata changes.

One build-time correction worth recording: the first draft memoised the test roster in a function-local `static const`, which this project rejects under `-Werror=exit-time-destructors`. Returning by value is the house-compatible form.
