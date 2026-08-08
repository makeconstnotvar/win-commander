# Q2-7 CL-1/GL-1/NW-1…NW-3: Cloud sync state, badge rule, Gallery eligibility, network volume state, the mount table and the probe

> Status: implemented and tested — see §Verification. Model increment: no user-visible surface yet.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-7.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §11 (badge vocabulary: `CloudOnly`, `Downloading`, `Uploading`), §23 (Remote / Cloud), item attributes `isCloudOnly` / `isAvailableOffline`. The plan's own wording for Q2-7 is the operative constraint: *badges синхронизации только там, где они уместны*.

## Scope

CL-1 is the classification and the badge rule. **GL-1** then uses it for the one Gallery decision that has consequences.

**NW-1** covers Q2-7's third item, network volume states.

Not here: the Gallery view itself, or the provider adapters that produce these facts.

## The rule the plan actually asks for

`ShouldBadgeCloudSyncState` returns false for `Synced`, and that is the whole point of the slice.

Inside a cloud folder, synced is what everything is *supposed* to be. Badging it decorates every row identically, which conveys no information at all, and it makes the handful of rows that genuinely differ harder to find — the opposite of what a badge is for. So "only where they are appropriate" is implemented as a rule rather than left as a preference each surface interprets: a badge appears only for a state that differs from what the surrounding rows are expected to be.

`NotCloud` likewise gets nothing, for the same reason at a larger scale.

## Classification order

Facts are booleans a provider can actually answer, not a provider-specific enum — every service words its states differently, and that mapping belongs at the adapter rather than in the shared model.

Ordering is by what the user most needs told:

1. **Outside a cloud container, nothing else counts.** A stray `downloading` flag from a confused adapter must not turn an ordinary local file into a cloud one.
2. **A conflict outranks everything.** It is the only state where doing nothing loses data.
3. **A deliberate exclusion outranks transfer flags.** An excluded item is not waiting for anything, and a stale flag must not make it look like sync is still coming.
4. **Transfers outrank placeholder status.** A downloading placeholder is on its way; a stalled one is not. Reporting both as `CloudOnly` would hide exactly the difference someone about to open the file cares about.
5. Download outranks upload when an adapter reports both, because the missing bytes are the more consequential half.

## GL-1: what a Gallery row may show

`ClassifyGalleryItem` answers one question per item — thumbnail, placeholder, plain icon, or folder — and exists for a single judgement:

**A cloud-only media file is not thumbnailable.** Asking for its thumbnail makes the file manager fetch the bytes: potentially gigabytes, potentially on a metered link, *because the user switched view mode*. Switching a view is not consent to a transfer. So such items get a placeholder row and downloading stays an explicit action.

That is why GL-1 lives beside CL-1 rather than in a view layer — it needs `CloudSyncState`, and putting the rule where the state is keeps a future Gallery implementation from having to rediscover it.

Two smaller calls:

- **A folder is never a thumbnail of itself**, whatever it is named. A directory called `holiday.jpg` is still a directory, and the extension check runs after the directory check for exactly that reason.
- **An unrecognized extension degrades to an icon.** The media list is deliberately conservative: a wrong guess renders an empty frame, which reads as a *broken file*, while an icon simply looks correct. Being wrong in the safe direction costs a thumbnail; being wrong in the other direction makes intact files look damaged.

## NW-1: network volume state

The rule this exists for: **an unresponsive network mount must not be touched from the drawing thread.**

A `stat()` under a dead NFS or SMB mount does not fail — it *blocks* until the kernel's own timeout, measured in tens of seconds. Doing that while drawing is exactly how a file manager becomes the beachball, and it is why the state has to be known *before* the access rather than discovered by making it. `MayTouchSynchronously` admits only `Local` and `Responsive`.

Two states that both mean "broken" are kept apart because they need opposite handling:

- **`Unresponsive`** blocks. It must go off the main thread, but it must *not* be refused up front — the server may simply be slow, and refusing would make a recoverable stall look permanent.
- **`Stale`** answers immediately and fails. It should be refused before attempting, because an optimistic attempt fails per item — potentially thousands of times, each with its own error. One refusal naming the volume is more useful than a thousand naming files.

`Unresponsive` also outranks a rejected export: if the server is not answering, "the export is gone" is a conclusion that cannot have been reached, and reading it as `Stale` would send the optimistic fast-failing path into a mount that actually blocks.

## Verification

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::NetworkVolumeState*' --rng-seed 424242`: **6/6 cases, 19/19 assertions** — a local volume never read as a network state whatever flags it carries; unresponsive outranking a rejected export; the blocking mount and the fast-failing one distinguished and given opposite handling; unmounted reported before anything else; the drawing thread kept away from every state that can block; and a healthy mount.
- Focused `WinCommanderUT 'nc::core::GalleryEligibility*' --rng-seed 424242`: **6/6 cases, 24/24 assertions** — case-insensitive extension matching; unknown, empty and over-long extensions degrading to an icon; a directory named like an image still a folder; a cloud-only media file held to a placeholder while all six other cloud states thumbnail; a cloud-only non-media file still an icon.
- Focused `WinCommanderUT 'nc::core::CloudSyncState*' --rng-seed 424242`: **7/7 cases, 17/17 assertions** — every other fact ignored outside a container; conflict outranking a maximally-set fact struct; exclusion beating a stale transfer flag; an arriving placeholder distinguished from a stalled one; upload, and download winning over it; a quiet present item; and the badge rule for all seven states.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **727/727 cases, 11,350/11,350 assertions**.
- No sanitizer run: a pure `noexcept` classifier over a value struct.

### Coverage gaps

- **Nothing produces `CloudItemFacts` yet.** The macOS side (`NSURLUbiquitousItemDownloadingStatusKey` and friends for iCloud, and per-provider equivalents) is the next increment; this slice deliberately fixes the vocabulary and the ordering first, so each adapter has one place to map onto rather than inventing its own.
- The Gallery view itself is untouched; GL-1 fixes the eligibility rule, not the rendering.
- **Nothing produces `NetworkVolumeFacts` yet.** Reading the mount table and running a non-blocking probe are the next increment; NW-1 fixes what the answers mean.

---

# NW-2: where the network-volume facts come from

NW-1 decided what a network volume's state means and, crucially, that an unresponsive one **must not be touched from the drawing thread**. Nothing produced those facts, so the rule had no input.

`MountTable` is that producer: it reads the mount table and places a path on a volume, without touching the path or the volume.

## The read must not wait

`getmntinfo` is asked **not** to refresh each filesystem's statistics. Requesting fresh statistics makes the call wait on every mounted filesystem in turn, and a network mount whose server has gone away will not answer — so the very call meant to *discover* unresponsive volumes would hang on one. That is the whole failure this model exists to prevent, arriving through the back door.

## Network-ness comes from the mount flag, not a list of names

Matching `smbfs`, `nfs`, `afpfs` and friends by name would go stale the moment a new network filesystem appears — and the failure is silent and in the worst direction: an unrecognised network volume would be reported as local and then probed synchronously while drawing. The kernel's own "not local" flag does not go stale.

## Containment is by path component, never by string prefix

`/Volumes/data` does not contain `/Volumes/database`. A prefix match would attribute one volume's state to another — reporting a live local disk as an unresponsive network mount, or, worse, the reverse. The innermost containing mount point wins, so a volume mounted inside another owns the paths beneath it, and a mount point belongs to its own volume rather than to its parent.

## A path that cannot be placed is reported as such

This one changed shape while being tested. The first version returned facts saying "not a network mount, not mounted" — and `ClassifyNetworkVolume` reads that as **`Local`**, which means *safe to touch on the drawing thread*, for exactly the path we could not account for. The test caught it immediately.

The fix was not to revise NW-1's precedence, which is deliberate and pinned by its own test. Neither available answer is honest here: `Local` invites the synchronous touch, and `Unmounted` refuses operations up front — which would refuse **everything** on a machine where the mount table could not be read at all. So the absence is reported, and the caller, which knows which of those risks applies to it, decides.

A local volume is also reported as answering by construction, whatever probe result is passed alongside it: carrying a stale probe into a local volume would report a working disk as unresponsive.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::MountTable*'`: **8/8 cases, 58 assertions** — the `/Volumes/data` versus `/Volumes/database` collision in both directions; innermost-wins including a mount point belonging to its own volume; the root-volume fallback and a path that cannot be placed; normalization; a local volume answering by construction despite a contrary probe; a network volume classifying as responsive, unresponsive and stale from the probe it is given, with the unresponsive one asserted unsafe to touch; the unplaceable path reported rather than invented; and one case against the **real** mount table, finding the root volume and placing `/` on it.
- Full `WinCommanderUT --rng-seed 424242`: **789/789 cases, 11,698 assertions**.

### Coverage gap at NW-2

**The probe itself does not exist** — closed by NW-3 below.

---

# NW-3: asking a volume whether it is still there

NW-2 supplies everything the mount table knows. Whether the server still *answers* is the one fact that cannot be had without touching it — and touching it is the thing this whole model exists to keep off the drawing thread.

## The wait has a deadline, because the call cannot

There is no timeout on `statfs`. Under a dead network mount the kernel holds it for tens of seconds, and nothing can interrupt it. The only way to have a deadline at all is to stop *waiting* for it: the call runs on a thread of its own and is abandoned when the budget is spent.

The abandoned thread owns everything it touches — a heap-allocated block shared with it — so its eventual return writes somewhere valid and unread, whatever else has been destroyed by then.

**A late answer is not an answer.** Once the budget is spent the volume is reported unresponsive, and a reply arriving afterwards does not retract that. Something that takes half a minute to respond is precisely what must not go on the drawing thread, whatever it eventually says.

## Silence and refusal are different answers

A server that replies "no such export" has answered — the mount is `Stale`, which fails fast and can be refused up front. A server that says nothing is `Unresponsive`, which blocks. The probe distinguishes them because NW-1 treats them differently, and collapsing them would send the optimistic fast-failing path into a mount that hangs.

## The answer is remembered, and it expires

Probing to draw a row would be the synchronous touch being avoided, so `Known` never blocks and never probes — that is the call the drawing thread may make. But a remembered answer expires: a `Responsive` from a minute ago would hide a mount that has died since, and hiding it is how the synchronous touch happens anyway. An aged-out answer is withheld rather than repeated, which turns into "needs a refresh" instead of a stale reassurance.

**The prober runs outside the lock.** It is the part that can take tens of seconds, and holding the lock across it would block every drawing-thread read for exactly that long — reintroducing the stall this class exists to prevent, one indirection further away. A test holds a probe inside the prober and asserts reads still answer while it is stuck.

A slower probe that started earlier does not overwrite a fresher answer: it is older news whatever order the two land in.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::NetworkVolumeProbe*'`: **7/7 cases, 42 assertions** — an answer served from memory without asking again, and asking what is known never probing; an aged-out answer withheld and then refreshed; mount points kept apart and one forgotten; an out-of-order refresh not overwriting fresher news; **reads answering while a probe is stuck inside the prober**; the real prober against a path that answers, against a budget nothing can meet, and against a path that is not a mount point — checking that refusal and silence come out different; and the result feeding through to `Unresponsive`, asserted unsafe to touch.
- The same suite plus `RemoteConnectionRegistry` under **TSan**: **15/15 cases, 93 assertions** — this slice detaches a thread and shares state with it.
- Full `WinCommanderUT --rng-seed 424242`: **796/796 cases, 11,740 assertions**.
- The TSan build again needed the frame-size warning relaxed for two unrelated tests; see the note in `git_status_foundation.md`.

### Coverage gap

**No scheduler and no caller.** Something has to notice which volumes a listing touches, refresh the stale ones off the drawing thread, and redraw when an answer changes. The cloud-facts adapter and the Gallery view also remain.
