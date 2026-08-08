# Q2-7 CL-1: Cloud sync state and when a badge earns its place

> Status: implemented and tested — see §Verification. Model increment: no user-visible surface yet.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-7.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §11 (badge vocabulary: `CloudOnly`, `Downloading`, `Uploading`), §23 (Remote / Cloud), item attributes `isCloudOnly` / `isAvailableOffline`. The plan's own wording for Q2-7 is the operative constraint: *badges синхронизации только там, где они уместны*.

## Scope

The classification and the badge rule. Not here: the Gallery mode, provider adapters that produce these facts, or network-volume states.

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

## Verification

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::CloudSyncState*' --rng-seed 424242`: **7/7 cases, 17/17 assertions** — every other fact ignored outside a container; conflict outranking a maximally-set fact struct; exclusion beating a stale transfer flag; an arriving placeholder distinguished from a stalled one; upload, and download winning over it; a quiet present item; and the badge rule for all seven states.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **703/703 cases, 11,249/11,249 assertions**.
- No sanitizer run: a pure `noexcept` classifier over a value struct.

### Coverage gaps

- **Nothing produces `CloudItemFacts` yet.** The macOS side (`NSURLUbiquitousItemDownloadingStatusKey` and friends for iCloud, and per-provider equivalents) is the next increment; this slice deliberately fixes the vocabulary and the ordering first, so each adapter has one place to map onto rather than inventing its own.
- Gallery mode and network-volume states are untouched.
