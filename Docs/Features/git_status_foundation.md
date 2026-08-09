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

---

# GT-2: which repository a directory belongs to

GT-1 could read a status. Nothing could say *whose* status to read.

`FindGitRepositoryRoot` walks up from a directory to the root of the repository containing it. The walk itself is three lines; the refusals are the content.

## `.git` is not always a directory

A linked worktree and a submodule both have a `.git` **file** pointing elsewhere. Accepting only a directory would report both as not repositories at all — and a submodule is exactly the case where getting it wrong is worst, because the enclosing repository *is* found instead, and its status knows nothing about the submodule's files.

Nearest wins for the same reason: a path inside a submodule belongs to the submodule.

## The walk stops at a filesystem boundary

This is what git itself does by default, and the reason survives restating: a volume mounted inside a checkout is not part of that checkout. Letting it inherit the repository would badge files git has never heard of, using a status that will never mention them.

The mirror case matters too — a repository that begins exactly *at* a mount point is still found, because the walk starts there and has no boundary to cross.

## An unreadable directory ends the walk

Not "skip it and keep going". Continuing past a directory we could not check means crossing a boundary we failed to look for, which is the thing the boundary rule exists to prevent.

## A relative path is refused, not resolved

The caller's "here" is a panel's directory. Resolving a relative path against the process working directory would answer confidently about a completely different place — the same failure mode as `ResolveLocalWorkingDirectory` in TL-1, where a path that *looks* absolute and local turns out not to be.

A dangling `.git` symlink is refused as well: it is not a usable marker, and accepting it would point every later git call at a repository that is not there.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::FindGitRepositoryRoot*'`: **9/9 cases, 25 assertions** — the walk up and its absence; nearest-wins for submodules; the filesystem boundary in both directions; an unreadable directory ending the walk rather than being stepped over; relative, empty and dotted paths refused, and an empty probe not guessing; path normalization; and two cases against the **real** filesystem — a `.git` file accepted, and a dangling `.git` symlink rejected.
- Full `WinCommanderUT --rng-seed 424242`: **774/774 cases, 11,598 assertions**.
- The probe is injected, so the walk's decisions are tested without needing mounts or permissions the test cannot arrange.

### Coverage gap at GT-2

**Nothing runs git yet** — closed by GT-3 below.

---

# GT-3: asking git, on terms a badge refresh can live with

GT-2 finds the root, GT-1 parses the output. Between them sat launching a process, and the launch is where the decisions are.

## Never through a shell

The argument vector is explicit. A repository path may contain spaces, quotes, semicolons and newlines, and a command line would let all of them be re-read as syntax. A test creates a repository literally named `a dir; rm -rf $x 'quoted' & odd` and reads it successfully.

**That test caught the flaw in its own helper first.** The fixture builder was pasting the directory into a shell command with single quotes, so the quoted directory name broke it — the helper failed on exactly the input the code under test handles. It now spawns with an argument vector too.

## Every `GIT_*` variable is removed from the child's environment

A stray `GIT_DIR` or `GIT_WORK_TREE` inherited from whoever launched the application would silently point git at a different repository, and every badge would then describe someone else's working tree. A test sets both to a decoy repository and asserts the answer is still about the real one, with none of the decoy's files in it.

## `--no-optional-locks`

A badge refresh must not take the index lock. Refreshing the index behind the user's back is how a background redraw ends up fighting the git command they typed themselves.

## stdin and stderr go to `/dev/null`

git must never be able to ask a question. A credential prompt or an editor launch would block a refresh forever, on a thread the user is waiting on.

## Both budgets report rather than apply

A timeout stops the child and says so; an over-budget repository is refused rather than truncated. Truncating would be the silent wrong answer GT-1 exists to refuse: a partial status is indistinguishable from a clean tree for the paths it omits.

## What the frame-size check caught

The read buffer started as a 64 KB `std::array` on the stack and broke the project's frame-size limit. The limit was right — this runs on whatever thread a listing refresh happens to use — and the buffer moved to the heap.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::ReadGitStatus*'`: **7/7 cases, 42 assertions**, all against a **real git and real repositories** built in a temporary directory — a modified file and an untracked one read from a subdirectory with the root reported; a filename containing a newline surviving end to end, which is the whole reason GT-1 insists on the NUL-separated form; a decoy `GIT_DIR` ignored; a repository path full of shell syntax; a non-repository refused without launching git at all; an over-budget repository reported and then read fine with a budget that fits; and a timeout.
- Focused `'nc::core::ReadGitStatus*'` plus `'nc::core::FindGitRepositoryRoot*'` again under **ASAN+UBSAN**: **16/16 cases, 67 assertions** — this slice spawns a child, hands it descriptors and reads a pipe, which is where that budget applies.
- Full `WinCommanderUT --rng-seed 424242`: **781/781 cases, 11,640 assertions**.

### The sanitizers were blocked by two unrelated tests — now fixed

`WinCommanderUT` could not be built under ASAN or TSan at all. Two test cases sat just under the 32,768-byte frame limit and crossed it once instrumented — `Theme_UT.mm` at 34,688 under ASAN, `PanelPresentationGeometry_UT.mm` at up to 49,184. It is a `-Werror` error, so the whole scheme failed to build.

Both are now split across deliberately non-inlined helpers. No assertion was removed and no content changed; each helper simply gets its own frame. The limit was not raised — it caught a real oversized buffer in this slice's own production code, which is the argument for keeping it.

Both schemes now build with no flag overrides, and the ASAN figures quoted above were re-obtained that way.

### Coverage gap

**No caller yet.** A panel does not refresh badges, nothing watches for changes to re-read a status, and porcelain v2 is unparsed. Rendering the badges is the next increment; so is deciding when a refresh is worth running at all.

---

# TL-2: what gets launched, and what does not

TL-1 decided which directory a local tool may be pointed at, and why it must fail closed. TL-2 turns that into a launch.

## The location is checked first, before the application is even looked at

That ordering is the decision, not an implementation detail. A missing editor is a visible, obvious failure the user can fix in a settings pane. A path inside an archive or on a remote host resolves *silently* against the real filesystem and opens the wrong place. When both are wrong at once, the refusal that would otherwise be silent is the one that has to be reached.

## An application identifier and arguments, never a command line

A directory or filename may contain spaces, quotes, semicolons and newlines. Composing a command line would let all of them be re-read as syntax — the same reason the git integration spawns with an argument vector. A test passes a filename containing a semicolon, an `rm -rf`, a quote and a newline, and checks it arrives intact.

## A terminal is opened *at* a directory; an editor is opened *on* files

The selection is dropped for a terminal. Otherwise "open terminal here" would mean something different depending on what happened to be selected — and "here" is the one thing the command promises.

## A selection that no longer belongs to this directory is dropped

A selection can outlive the listing it came from, and handing an editor a path from somewhere else is how a stale selection edits the wrong file. Containment is by path component: `/Users/me/project-other` is not inside `/Users/me/project`, however alike the two look as strings — the same rule the mount table needs, and the same class of bug as `/Volumes/data` versus `/Volumes/database`.

## Unconfigured and unusable are different refusals

Nothing chosen yet is a settings problem; a blank value is a broken one. Both refuse, but a surface reporting them the same way would send the user to the wrong place to fix it.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::PrepareLocalToolLaunch*'`: **8/8 cases, 25 assertions** — a terminal at the directory with the selection dropped; an editor given its files; the location refused ahead of the application, asserted with and without one configured; a non-uniform listing refused; unconfigured told apart from blank; a selection outside the directory dropped including the `project` / `project-other` lookalike; a filename full of shell syntax arriving intact; and the root, where the trailing separator must not make every path read as outside it.
- Full `WinCommanderUT --rng-seed 424242`: **804/804 cases, 11,765 assertions**.
- No sanitizer run: a pure value transformation, with no allocation ownership, concurrency, or process launching of its own.

### Coverage gap at TL-2

**Nothing performs the launch** — closed by TL-3 below.

---

# TL-3: performing the launch, and one rule instead of two

## The launch is not waited for

An application can take many seconds to start. Blocking for it would freeze the window that asked — the same stall this codebase refuses to accept from an unresponsive network mount, arriving from a different direction. The first draft of this code did exactly that, with a semaphore, and it was wrong for precisely that reason. The completion arrives when Launch Services answers.

What *is* synchronous is resolving the application, because it needs no launch to discover. "You have not got that editor" is reported as itself and immediately, rather than as a generic failure some seconds later — the two send the user to different places.

An application named by path is checked for existence before being handed over, so a stale path is reported as a missing application rather than as a launch failure.

## The directory is what gets opened when nothing is selected

Which is what makes "open here" land in the folder the user is looking at rather than merely starting the application.

## The built-in terminal now asks the same question as everything else

`ShowTerminal` had its own copy of the rule — uniform listing, native filesystem — written inline. It was correct, but it was a second copy, and it did not strip the trailing separator that `currentDirectoryPath` carries, so the shell was handed `/Users/me/project/` and echoed the doubled separator back.

It now goes through `ResolveLocalWorkingDirectory`. An unusable location still leaves the path empty, which starts the shell at the user's home — the same thing it did before, and still better than a directory they were not looking at.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- `WinCommanderUT 'nc::core::ResolveLocalWorkingDirectory*'` and `'nc::core::PrepareLocalToolLaunch*'`: both still pass unchanged — the consolidation moved a caller onto an already-tested rule rather than changing the rule.
- Full `WinCommanderUT --rng-seed 424242`: **812/812 cases, 11,796 assertions**.
- The launcher itself has no unit test: every path through it is a Launch Services call, and a test that passed would have had to start an application on the developer's machine. The decisions that can be tested without one — what to launch, with which documents, and when to refuse — are TL-2's, and they are.

### Coverage gap

**No menu action invokes the external launch**, and nothing yet stores which application fills each role. Both are settings-and-menu work rather than decisions.

---

# GT-4: the badge a folder earns

`git status` reports files, not directories. Without something in between, a folder full of modified files looks exactly like an untouched one, and the only way to find a change is to open every folder in turn — which is the thing badges exist to spare.

## The precedence, and what each rule is for

- **`Conflicted` outranks everything.** An unresolved merge is the one state where doing nothing is wrong, and burying it under a milder summary is how it gets missed.
- **`Untracked` does not propagate as itself.** A directory holding untracked files is not itself untracked — git may well be tracking its other contents — so it reports `Modified`. Reporting `Untracked` would invite "add this whole folder" on something already half-tracked.
- **`Added` and `Deleted` summarise as `Modified` too.** A directory whose contents changed in those ways has changed; claiming the directory itself was added or deleted would simply be false.
- **`Ignored` contributes nothing.** Ignored paths run into the thousands and their contents are ignored as well, so a directory does not become interesting for containing them — the same rule `ShouldBadgeGitFileState` already applies to the paths themselves, now applied to what a folder inherits.
- **A folder with nothing to say gets no badge**, rather than an `Unmodified` one. In a repository most folders are unchanged, and badging them all buries the ones that are not.

## Containment is by path component

`src` must not collect what belongs to `src-vendor`. That would badge a folder for changes it does not contain — the third place in this session where the component-versus-prefix distinction decides whether an answer is about the right thing at all, after the mount table and the tool launcher.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- New `WinCommanderUT`: **8/8 cases, 31 assertions** — a conflict outranking each other state; every other state summarising as `Modified`; nothing for an empty, unmodified or wholly-ignored folder; a change badged at every level above it and the file's own state at the bottom; `src` not collecting `src-vendor`; unmodified and ignored children left unbadged; a conflict carried up through each folder above; and an empty status answered with nothing.
- Full `WinCommanderUT --rng-seed 424242`: **848/848 cases, 11,944 assertions**.

### Coverage gap

**No panel draws them.** Feeding a listing through this, and deciding when a refresh is worth running at all, is what remains — along with porcelain v2.

---

# GT-5: how a badge is drawn, decided once

Two view classes draw panel rows — the list and the brief views — and each would otherwise decide independently what a git state looks like. Two independent inventions of the same badge is how one view ends up showing a conflict as a warning and the other as an ordinary mark.

## It refuses exactly what the badge rule refuses

`PresentGitBadge` returns nothing for precisely the states `ShouldBadgeGitFileState` declines. A drawing site cannot then decide differently by accident: the two would disagree about which rows are decorated, and only one of them would be right. The test asserts that equivalence across every state rather than trusting the two lists to stay aligned.

## Every badged state gets its own symbol, and its own words

Two states sharing a symbol would make the badge say less than the status it came from, which is the only thing it is for. And each carries a phrase for a screen reader, distinct as well as present — a badge that is only a coloured mark conveys nothing to someone who cannot see it, which would make git status a sighted-only feature, and two states read aloud identically are two states such a user cannot tell apart.

## Only one state is emphasised

The conflict, because it is the only one where doing nothing loses work. If everything is emphasised, nothing is.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- New `WinCommanderUT 'nc::core::PresentGitBadge*'`: **4/4 cases, 28 assertions** — presence matching the badge rule state for state; six badged states with six distinct symbols; exactly one emphasised; and six distinct, non-empty spoken phrases.
- Full `WinCommanderUT --rng-seed 424242`: **890/890 cases, 12,150 assertions**.

### Coverage gap

**No row draws it yet.** The symbol, the emphasis and the spoken phrase are decided; placing the mark in the list and brief views — alongside the existing trailing tag display, whose geometry it should follow — is what remains, together with feeding a panel its repository status in the first place.
