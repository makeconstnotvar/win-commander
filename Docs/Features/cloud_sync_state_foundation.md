# Q2-7 CL-1/GL-1/NW-1: Cloud sync state, badge rule, Gallery eligibility and network volume state

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
