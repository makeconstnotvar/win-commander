# Q2-1 DP-3: Dual-pane layout persistence

> Status: implemented, built and tested — see §Verification.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-1.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §13.2 (Dual Pane Mode), §17/§31 (session restore).
> Depends on: [`dual_pane_explorer_skeleton_slice.md`](dual_pane_explorer_skeleton_slice.md) (DP-1), [`dual_pane_explorer_commands_slice.md`](dual_pane_explorer_commands_slice.md) (DP-2), [`explorer_session_restore_slice.md`](explorer_session_restore_slice.md) (Q1-6).

## Product surface

An Explorer window that was left in dual-pane mode reopens in dual-pane mode. Both sides come back with their own ordered tabs and exact restorable locations, the side that had focus still has it, and the divider sits where the user left it. A window left in single-pane mode reopens single-pane, exactly as before — DP-1's explicit non-goal ("a restarted window always reopens single-pane") is what this slice closes.

Nothing else about restore changes: window identity and geometry stay with the ordinary Cocoa restoration path, every restored tab still receives a freshly allocated `PanelController`/`PaneId`/`History`/Store adapter/view-settings binding, and the session still stores no process-local identity, credential or view-setting value.

## Schema v2

`ExplorerSessionPersistency` moves to schema v2. The left pane's tabs stay exactly where v1 put them, so the shape is a strict extension:

```text
explorer: null | {
  tabs:    [{ location: <canonical PersistentLocation> | null }, ...],   // left pane (v1)
  active:  <ordered tab index>,                                          // left pane (v1)
  right:   null | { tabs: [...], active: <index> },                      // v2
  focused: "left" | "right",                                             // v2
  divider: null | <left share of the split's usable width>               // v2
}
```

The right pane's **presence is the single authority for dual-pane mode** — there is no separate boolean that could disagree with it, and therefore no way to persist "dual-pane on" with no right side to show. `focused` and `divider` are written as `"left"`/`null` whenever there is no right pane, so a single-pane session cannot carry stale dual-pane state forward.

`SchemaVersion` is 2 and `MinimumReadableSchemaVersion` is 1: a v1 session written by the current release still decodes, as one left pane with no right side. Writes always emit v2. The pre-existing downgrade guard does the rest without change — an older binary sees `schema: 2 > 1`, so `CanReplaceStoredSession` refuses and the newer session bytes survive being read by a build that cannot parse them.

### What degrades and what rejects

The codec already distinguished "malformed element normalizes" from "invalid envelope rejects atomically". v2 places the three new fields on the side of that line their content earns:

| Field | Invalid value | Why |
|---|---|---|
| `right` | **rejects the envelope** | It carries its own ordered tab locations — user data no default can stand in for. Same `MaximumTabs` bound as the left pane. |
| `focused` | degrades to left focus | A presentation hint with an exact safe default. |
| `divider` | degrades to the even split | Same; accepted only as a finite number inside `[MinimumDividerRatio, MaximumDividerRatio]` = `[0.05, 0.95]`, out-of-band values are dropped on both encode and decode. |

## Runtime capture and restore

`captureTabsSession`/`restoreTabsFromSession:` become `capturePanesSession`/`restorePanesFromSession:` on `NCExplorerState`, with the per-side bodies factored into `captureTabsForContent:` / `restoreTabs:intoContent:` over DP-1's `NCExplorerPaneContent`. The single-pane path through those helpers is unchanged.

Capture reads the **Left** side for `left` rather than the focused side. That is a behavior fix this slice carries: pre-DP-3 capture used `[self focusedContent]`, so a dual-pane window with the right side focused would have persisted the *right* side's tabs and restored them into the *left* side on next launch, silently swapping the user's panes. With two sides persisted independently the focused side is recorded explicitly instead.

Restore order is left → dual-pane on → right → focus, and the failure policy is asymmetric on purpose:

- The left side restores first, into the only side that exists at that point (dual-pane is never on before a session is applied), and a failure there still fails the whole session — unchanged Q1-6 behavior, including atomic topology rollback.
- Once the left side has succeeded it is the window's primary content. A right side that cannot be rebuilt — no panel available from the dual-pane seam, or its own extra tab failing to attach — therefore **degrades the window to single-pane** instead of discarding a restore that already succeeded for the side the user is looking at. `setDualPaneEnabled:NO` runs the existing teardown, so no half-built right side survives.

`restoreTabs:intoContent:` keeps routing the focused side's attach through the overridable 2-argument `attachExplorerTabPanel:createPaneStore:` (the DP-1 regression note explains why: test doubles simulating a failed attach hook there). Only the right side, restored while the left side still holds focus, names its content explicitly via the `toContent:` variant.

## Divider ratio

`m_PaneDividerRatio` is the left side's share of the pane split's usable width, retained across dual-pane toggles and window sessions the same way `m_LastInspectorWidth` already is for the inspector. `applyPaneDividerRatio` requests it after `adjustSubviews` when dual-pane turns on; AppKit then constrains that request against the per-side `g_PanelMinimumWidth`, and `splitViewDidResizeSubviews:` records what was actually achieved — so a user drag and a constraint-limited restore both converge on a ratio the layout can really hold.

`measuredPaneDividerRatio` refuses to report below `2 × g_PanelMinimumWidth` of usable width. Below that no divider position satisfies the constraints at all, so AppKit's chosen position is an artifact of the window being too narrow; recording it would let one cramped layout pass silently destroy the ratio the user chose. This mirrors the inspector, which likewise only accepts a width already inside its own usable band. The guard was added in response to a real failure: the first version of this slice recorded unconditionally, and the unit fixture's ~419 pt split (narrower than two 360 pt minimums) round-tripped a requested 0.35 back as 0.66.

## Verification

Built and run in this session (Xcode 26.6 toolchain, same as DP-1/DP-2):

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**, no errors or warnings in the changed files.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED** (production app target).
- Focused `WinCommanderUT 'nc::explorer::ExplorerSessionPersistency*' --rng-seed 424242`: **12/12 cases, 237/237 assertions** (9 pre-existing plus 3 new: dual-pane round-trip; single-pane freedom from dual-pane state in both directions including a stored v1 payload; right-pane rejection alongside focused/divider degradation and the shared tab bound).
- Focused `WinCommanderUT 'NCExplorerState tabs *' --rng-seed 424242`: **21/21 cases, 268/268 assertions** (17 pre-existing plus 4 new: dual-pane restore rebuilding both sides with focus and divider and round-tripping through capture; a single-pane session leaving dual-pane off and carrying no dual-pane state; a right side that cannot be rebuilt degrading to single-pane in two distinct failure modes; swap moving each side's tabs into the other side's captured session).
- Focused `WinCommanderUT '*Explorer*' --rng-seed 424242`: **167/167 cases, 3,680/3,680 assertions**.
- Focused `WinCommanderUT '*PanelController*' --rng-seed 424242`: **121/121 cases, 1,788/1,788 assertions**.
- Focused `WinCommanderUT '*MainWindowController*' --rng-seed 424242`: **4/4 cases, 35/35 assertions** (the call sites this slice retargeted).
- No ASAN/UBSAN run: this slice touches a pure value codec plus AppKit-adjacent `NCExplorerState` presentation code, and calls existing `Operations`/`VFS`/`RoutedIO` entry points unchanged — per `AGENTS.md`'s verification budget, focused filters plus Debug builds are the right tier.

### Known coverage gaps

- The end-to-end restart walkthrough — quit a real dual-pane window and relaunch — is not exercised here, for the same reason DP-1 and DP-2 recorded: no tool in this session drives a running native macOS app's UI. The codec round-trip and the state's capture/restore contract are covered on both sides of the boundary they meet at.
- The divider ratio's *constraint-limited* path (a split wide enough to honor `g_PanelMinimumWidth` on both sides, where AppKit clamps a requested ratio to a different achievable one) is covered by reasoning and the guard above, not by a unit case: the shared fixture's window is too narrow to reach it, and widening it would perturb the geometry every other case in that file asserts against.
