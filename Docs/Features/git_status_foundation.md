# Q2-9 GT-1/TL-1: Git status badges, and the working directory a local tool may be given

> Status: implemented and tested — see §Verification. Model increment: no user-visible surface yet.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-9.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §11 badge vocabulary (`GitModified`, `GitAdded`, `GitDeleted`), Q2-9 "корректный cwd, пути, git badges".

## Scope

GT-1 turns `git status` output into per-path badge states and decides which states deserve a badge. **TL-1** answers the other half of Q2-9's "корректный cwd, пути": which directory a local shell or editor may be started in.

Not here: running git, watching a repository for changes, the row rendering, or launching the tools themselves.

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

## TL-1: the working directory a local tool may be given

`ResolveLocalWorkingDirectory` fails closed, and the reason is specific rather than defensive habit.

A path inside an archive or on a remote host **looks like an ordinary absolute path**. Handed to a local shell it resolves against the real filesystem, so `/Users/me/backup.zip/etc` silently becomes whatever happens to exist at that name locally — or nothing. "Open terminal here" would then land somewhere the user was not looking. A refusal the user can see is strictly better than a tool that opens the wrong directory convincingly.

So the provider check runs first: it is the failure that would otherwise resolve *silently* rather than visibly. Non-uniform listings (search results and the like) are refused too — there is no single "here" to open — and so is an absent or relative location.

A trailing slash is stripped except on the root, because tools accept both but some echo the path back, where a doubled separator reads as a bug.

## Verification

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::ResolveLocalWorkingDirectory*' --rng-seed 424242`: **5/5 cases, 14/14 assertions** — an archive path refused with the provider reason taking precedence over non-uniformity; a non-uniform listing refused; absent and relative locations refused; a clean path handed over; trailing slashes normalized with the root preserved.
- Focused `WinCommanderUT 'nc::core::GitStatus*' --rng-seed 424242`: **7/7 cases, 44/44 assertions** — conflicts including `AA`/`DD`; the worktree column winning and the index deciding a clean worktree; untracked and ignored; a full parse with a rename's original path; a path containing a newline kept whole; four malformed shapes refused while empty input parses clean; and the badge rule across all eight states.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **721/721 cases, 11,331/11,331 assertions**.
- No sanitizer run: a pure parser over a `string_view` with no ownership, concurrency or engine involvement.

### Coverage gaps

- **Nothing runs git yet.** Invoking it, locating the repository root, and refreshing on change are the next increment; fixing the parse contract first means the process-running layer has one well-tested boundary to hand bytes to.
- Porcelain v2 is not parsed. v1 carries everything the badge vocabulary needs, and adding v2 without a consumer that wants its extra fields would be speculative.
- Submodule status codes are classified by their XY pair like any other path, which is right for a badge but says nothing about the submodule's own contents.
- **Nothing launches a terminal or editor yet.** TL-1 decides which directory is legitimate to hand over; the launching, and the "reveal in editor" paths, are later increments.
