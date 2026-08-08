# Q2-9 GT-1: Git status parsing and badge states

> Status: implemented and tested — see §Verification. Model increment: no user-visible surface yet.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-9.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §11 badge vocabulary (`GitModified`, `GitAdded`, `GitDeleted`), Q2-9 "корректный cwd, пути, git badges".

## Scope

Turning `git status` output into per-path badge states, and deciding which states deserve a badge. Not here: running git, watching a repository for changes, or the row rendering.

## Why `--porcelain=v1 -z`

A newline is a legal character in a path. A line-based parser splits such a path into two records and then mis-badges whatever the second half collides with — a silently wrong answer on exactly the files whose names are already unusual. The `-z` form separates records with NUL and, crucially, stops git from quoting and escaping unusual paths, so what arrives is the exact bytes on disk.

Malformed input returns nothing rather than a salvaged prefix. A partial status is **indistinguishable from a clean tree for the paths it omits**, so salvaging would badge a modified file as unmodified — a silent wrong answer instead of a visible failure. Empty input, by contrast, is a clean tree and parses to an empty list.

## Classification

Two judgements carry this:

**A conflict outranks every other reading of the same pair.** Porcelain spells an unresolved merge with `U` on either side, but also as `AA` and `DD` — which read as an ordinary add and delete if taken at face value. A conflict is the one state where doing nothing is wrong, so it is checked first.

**The worktree column decides when the two columns disagree.** A file staged as added but since edited (`AM`) reads as `Modified`, because that is what its row on screen actually shows. Only when the worktree is clean does the index decide.

## The badge rule

`Unmodified` and `Ignored` earn no badge, for the same reason a synced cloud file does not: in a repository most files are unmodified, and ignored paths routinely number in the thousands. Badging either decorates rows that carry no news and buries the ones that do.

This mirrors [`cloud_sync_state_foundation.md`](cloud_sync_state_foundation.md) on purpose — two independent badge sources landing on the same rule is what keeps a file list from becoming uniformly decorated.

## Verification

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::GitStatus*' --rng-seed 424242`: **7/7 cases, 44/44 assertions** — conflicts including `AA`/`DD`; the worktree column winning and the index deciding a clean worktree; untracked and ignored; a full parse with a rename's original path; a path containing a newline kept whole; four malformed shapes refused while empty input parses clean; and the badge rule across all eight states.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **710/710 cases, 11,293/11,293 assertions**.
- No sanitizer run: a pure parser over a `string_view` with no ownership, concurrency or engine involvement.

### Coverage gaps

- **Nothing runs git yet.** Invoking it, locating the repository root, and refreshing on change are the next increment; fixing the parse contract first means the process-running layer has one well-tested boundary to hand bytes to.
- Porcelain v2 is not parsed. v1 carries everything the badge vocabulary needs, and adding v2 without a consumer that wants its extra fields would be speculative.
- Submodule status codes are classified by their XY pair like any other path, which is right for a badge but says nothing about the submodule's own contents.
