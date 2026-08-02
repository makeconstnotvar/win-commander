# Pane history current-entry identity

> Status: M1 runtime state slice implemented and locally verified on 2026-08-02

## Contract

`Panel::History` owns a nonzero monotonic `EntryId` for every unique entry created during one History lifetime. IDs are runtime identities: recording state exposes the newest entry, playback state exposes the indexed entry, duplicate/current `Put` retains the existing ID, a branch creates a fresh ID after the new entry can be appended, and bounded trimming preserves the IDs of retained entries. Removed IDs are never reused.

`History::NavigationState` combines `NavigationAvailability` with `current_entry_id`. Its single synchronous advisory callback fires exactly once for any effective navigation-state change. This includes the first unique `Put` and moves between middle entries where both availability booleans remain true but current identity changes. Callback registration, duplicate/current `Put`, invalid or repeated `RewindAt`, and VFS-manager configuration are silent. Observer exceptions are contained after the mutation commits.

The playback branch path preserves strong mutation ordering: it constructs and appends the fresh entry before discarding forward entries or switching to recording. An allocation failure therefore leaves the prior cursor and deque intact.

## PaneStore projection

`PaneState::current_history_entry_id` is projected together with Back/Forward availability before the unloaded-state early return. A History-only change advances snapshot `revision`, preserves `listing_generation`, and composes through Loading, Refreshing, Failed, and cancellation overlays.

The reducer rejects identity `0` and rejects any enabled Back/Forward direction without a current identity. It accepts an absent identity with both directions disabled for a fresh History and accepts a present identity with both directions disabled for a single entry. Validation is constant-time.

The identity is not a location, provider, listing, or persistence key. Per-pane history persistence and migration of `ListingPromiseLoader` to request-correlated lifecycle outcomes remain separate work.

## Verification

- `PanelHistory navigation state *`: 5 cases / 457 assertions;
- Store: 18 / 220;
- reducer: 19 / 335;
- production bridge: 28 / 461;
- combined History/Store/reducer/bridge: 70 / 1,473;
- incremental arm64 Debug `WinCommanderUT` build, project-file lint and `git diff --check`: passed.

Coverage includes first/duplicate puts, Back/Forward, identity-only middle transitions, branch truncation, repeated/invalid rewind, observer exception containment, 128-entry trimming, unloaded/loaded projection, lifecycle preservation, cancellation, stable listing generation, zero-ID rejection, and availability-without-identity rejection.
