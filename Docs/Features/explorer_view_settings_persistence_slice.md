# Explorer view settings and per-location persistence

> Status: Queue 1 Q1-5 production implementation and current-tree closure evidence complete
>
> Execution tracker: `Docs/Development-Plan.md`, Queue 1 item Q1-5

## User-visible contract

Explorer keeps the selected presentation for each exact folder. Its five configured presets — Small Icons, Details, Medium Icons, Large Icons and Content — continue to use the established Brief, List and Gallery panel renderers. The persisted value contains the selected configured slot plus the concrete presentation derived from it, including Brief density and icon scale, Details column order and widths, and Gallery icon scale and text-line count.

Sort key, direction, collation, directory ordering flags and the effective grouping state travel with the same folder-specific value. Returning to a folder restores that complete combination. Selecting the current configured layout again intentionally returns the pane to that preset, providing the reset path for a folder-specific customization.

## Exact binding and restore

`NCExplorerState` samples view settings only from a complete Loaded or Refreshing uniform `PaneSnapshot` whose `PaneId`, live controller, host instance, directory path and location generation match the active tab. `ExplorerViewSettingsBindingPolicy` combines those fields with Store revision and a monotonic full-layout observation sequence. A location transition loads once; a settled value is stored only after a complete changed sample.

Restore applies a pane-local concrete layout through `PanelController`, then restores the strictly projected legacy sort mode and grouping. The binding fences the snapshot which initiated a restore and waits for a later complete layout sample before accepting or conservatively persisting the result. Delayed revisions, foreign panes, incomplete locations and intermediate restore notifications have no persistence authority.

An exact location without a record adopts its current valid presentation as the initial local value. This also isolates later column resizing from the shared Explorer preset. Global layout changes retain an applicable pane-local override and refresh its user-facing preset name; a disabled or type-changed slot returns the controller to its valid configured fallback. Commander panels keep their established shared-layout editing behavior.

## Persistence contract

The application owns one `ExplorerViewSettingsPersistence` over `StateConfig()` and `PanelDataPersistency`. Schema v1 lives at `filePanel.explorer.viewSettingsByLocation_v1` and stores at most 512 records in deterministic MRU order.

Each record carries the complete `PanelDataPersistency::LocationToJSON` value. Its footprint is a lookup accelerator, and acceptance always compares the full canonical location. The codec validates every Brief, List and Gallery scalar, list-column identity and bounds, semantic sort combination and grouping relationship. A stale footprint is repaired only after the complete location matches. Malformed roots, schema mismatches, duplicate canonical locations and semantically invalid records fail closed without rewriting their bytes.

Layout titles remain owned by the configured Explorer presets and are recovered from the live slot. Persistent state contains presentation semantics rather than display names. Q1-6 restores window, ordered tabs and locations; each restored location then resolves its Q1-5 value through this single owner.

## Verification

The confirmed results are:

- `WinCommanderUT 'nc::explorer::ExplorerViewSettingsPersistence*'`: 8 cases / 580 assertions passed.
- Q1-5 focused persistence/binding/sort/layout/controller/Explorer integration rerun: 26 cases / 823 assertions passed.
- full Debug `WinCommanderUT`: 459 cases / 8,401 assertions passed.
- Debug `UnitTests` scheme build and incremental `WinCommander-Unsigned` application build succeeded.

Current production and focused test coverage additionally includes:

- exact Brief, List and Gallery round-trips, reordered Details columns, full-location collision protection, MRU touch/eviction, stale-footprint repair and malformed-schema preservation;
- strict two-way mapping of all supported legacy sort modes and rejection of incomplete, contradictory and unknown semantic states;
- atomic pane-local layout application, same-slot reset, column-resize isolation, configured-layout rename/type/disable handling and Commander shared-layout preservation;
- exact location/revision/observation fencing, restore settlement and divergence, and active Explorer integration over the process-owned store.

The focused set exercises the complete Q1-5 ownership boundary; the full affected binary and unsigned application build provide the shared `PanelController`, Store adapter, AppKit integration and link closure gates.
