# Explorer window and tab session restore

> Status: Queue 1 Q1-6 production implementation and current-tree closure evidence complete
>
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 17 and 31
>
> Execution tracker: `Docs/Development-Plan.md`, Queue 1 item Q1-6

## User-visible contract

An Explorer window returns after restart in Explorer mode with the same ordered tab set, active tab and exact restorable location for each tab. The ordinary Cocoa restoration path remains the owner of window identity and geometry. The session payload records the application mode and panel content needed to rebuild the window, while runtime-only pane identity and navigation history are recreated for the new process.

Every restored Explorer tab receives a freshly allocated `PanelController`, `PaneId`, `Panel::History`, Store adapter and view-settings binding. The session stores no process-local `PaneId`, History entry, credential or view-setting value. Once a location commits through its new controller, the Q1-5 binding remains the single owner that resolves the exact folder's layout, density, columns, sort and grouping.

## Versioned envelope and source selection

`ExplorerSessionPersistency` encodes schema v1 around the existing Commander payload:

```text
{
  schema: 1,
  mode: "commander" | "explorer",
  commander: <legacy panels_v1 state>,
  explorer: null | {
    tabs: [{ location: <canonical PersistentLocation> | null }, ...],
    active: <ordered tab index>
  }
}
```

The Commander payload remains intact inside the envelope and is restored first as the permanent window base. A valid legacy `panels_v1` root is accepted as Commander mode; the next successful save emits schema v1. System restoration selects the first valid source in strict order: Cocoa state, `StateConfig`, then the normal default Commander/Home content. Manual restoration uses `StateConfig` when last-window restoration is enabled and no live window supplies the established option-copy path.

The codec accepts at most 64 Explorer tabs. An empty tab array normalizes to one Home tab, and an invalid active index normalizes to the first tab. It requires exact canonical `PersistentLocation` round-trip, including a structurally safe host stack, before a location can enter the runtime. A malformed individual tab retains its ordered slot as Home. An invalid envelope, mode, Commander base or oversized tab collection is rejected atomically.

Unknown future schemas are rejected for reading. A well-formed future schema already stored in `StateConfig` is also preserved from a current-version write, so a downgrade cannot silently replace newer session bytes.

## Restore, fallback and lifecycle boundaries

`NCExplorerState` builds the complete ordered topology before starting location recovery. Allocation, identity, model, holder or active-selection failure rolls the partial topology back to its initial panel; `MainWindowController` then retains the successfully restored Commander base. No partially attached Explorer session becomes the active window state.

Accepted locations restore asynchronously through the established persistent-location restorer and `PanelController` navigation lifecycle. Remote restoration sets `allow_password_ui = false`; an unavailable connection therefore falls back without opening credential UI. A missing, malformed, unavailable, permission-denied or failed location sends only that tab to Native Home. The other tabs and active index remain intact.

Restorer callbacks retain weak window/panel references and require the exact owned `PaneId`. Controller content-intent generation and the navigation request's current predicate fence both the delayed load and the Home fallback, so later user navigation wins. Session capture likewise accepts only an exact uniform Store snapshot whose pane, controller, host, path, listing and location generation still match. Loading, refreshing and failure overlays may contribute only the retained committed location satisfying that same identity contract.

## Verification

The confirmed current-tree results are:

- `WinCommanderUT 'nc::explorer::ExplorerSessionPersistency*'`: 9 cases / 130 assertions passed.
- Explorer tab/session and adjacent Inspector coverage: 16 cases / 245 assertions passed.
- no-password VFS restoration coverage: 2 cases / 14 assertions passed.
- detached recovery teardown and stale-commit fencing: 2 cases / 8 assertions passed.
- the integrated Q1-6 filter passes 29 cases / 397 assertions in both Release ASAN and Release UBSAN without diagnostics.
- full Debug `WinCommanderUT`: 476 cases / 8,656 assertions passed.
- Debug `UnitTests` scheme and incremental `WinCommander-Unsigned` application builds succeeded.

The focused set covers schema-v1 round-trip, legacy Commander migration, Cocoa/`StateConfig` fallback selection, canonical location admission, per-tab Home normalization, active-index and 64-tab bounds, future-schema preservation, fresh runtime identities, atomic topology rollback, exact capture and asynchronous no-password fallback. Window-geometry, signed restart walkthrough and hostile-provider manual evidence remain release-level gates rather than ownership gaps in Q1-6.
