# Q1-1 Archive Create, Duplicate and Copy Path Registry batch

Status: implemented third Q1-1 batch; the full roster remains in progress.

## Contract

The application-owned Registry defines twenty stable production commands. This batch adds:

| Command | Existing execution path | Mounted entry points |
|---|---|---|
| `archive.create` | `nc::ops::Compression` and `NCOpsCompressDialog` | Command menu, `F9`, exact-item context menu, Explorer More |
| `file.duplicate` | one established `nc::ops::Copying` operation per selected item | File menu, persisted shortcut, exact-item context menu, Explorer More |
| `file.copyPath` | system text pasteboard | Command menu, persisted shortcut, exact-item context menu, Explorer More |

All three definitions carry stable legacy selector, shortcut-action and tag metadata. Menu, shortcut, context and Explorer callers submit an explicit invocation source through `NCPanelControllerActionsDispatcher`; exact context entries remain immutable Registry payloads. Disabled Explorer More items remain visible with localized tooltip and accessibility help and own no target or action.

## Archive Create admission and publication

State requires a live pane and operation queue, a committed uniform listing, exact current-listing items, readable source paths, regular-file/directory/symlink shapes and distinct normalized top-level archive names. The selected destination provider must be writable at the current path and advertise file creation.

Execution captures the source listing and generation, destination provider/path and window before presenting the existing compression sheet. Confirmation repeats those identities and source capabilities, validates the dialog destination against the captured provider, then enqueues `nc::ops::Compression`. A changed pane, listing, provider, destination capability or source set produces a stale alert and zero enqueue.

Primary current-pane compression now has one route from the menu, `F9`, context menu and Explorer More. The opposite-pane `Shift-F9` action remains a distinct legacy command because it carries different destination semantics.

`CompressionJob` treats filename probing as advisory and opens the chosen output with exclusive-create semantics. Provider-specific missing errors are recognized through `Host::ClassifyError`; other stat and create failures reach the existing target-write error handler. A destination created after the probe cannot be overwritten or unlinked by the failed operation. Existing-name behavior preserves the old archive and chooses the next numbered name.

## Duplicate admission

Duplicate requires exact current-listing non-parent items, readable source paths, a live queue and a writable current destination whose path-aware capabilities can create every selected item kind. It computes every collision-free destination name before enqueue, repeats live admission once after planning, then submits the established `nc::ops::Copying` operations. A stale, unsupported, unreadable, read-only or name-exhausted batch submits nothing.

## Copy Path admission

Copy Path accepts non-parent items from the exact current listing after loading completes. Execution repeats that admission, writes the complete configured separator-joined pathname string to the system pasteboard and reports a typed rejection when the pasteboard refuses the write.

## Remaining Archive Extract gap

The existing archive flow browses through `VFSArchiveProxy` and copies selected archive entries with `nc::ops::Copying`; it has no dedicated Extract selector, menu action or shortcut. Proxy acquisition also collapses unsupported format, corrupt archive, password cancellation and provider I/O into one null result. A production `archive.extract` definition therefore needs a separate typed acquisition and destination contract before it can provide truthful shared availability and error presentation.

## Verification

- Debug `WinCommanderUT 'nc::core::FileMutationCommands*' --rng-seed 424242`: 11 cases / 302 assertions.
- Debug `WinCommanderUT 'nc::core::CommandRegistry*' --rng-seed 424242`: 17 / 192.
- Debug production selector and exact-context route: 1 / 39.
- Debug Explorer More disabled/enabled projection and Toolbar execution: 1 / 38.
- Debug `OperationsIT 'Operations::Compression*' --rng-seed 424242`: 14 / 227, including existing-destination and post-probe race preservation.
- Explicitly instrumented Release ASAN and Release UBSAN `OperationsIT` builds linked their sanitizer runtimes; the same Compression filter passed 14 / 227 in each configuration without diagnostics.
- Full Debug `OperationsUT --rng-seed 424242`: 211 / 211 cases and 5,666 assertions.
- Full Debug `WinCommanderUT --rng-seed 424242`: 364 / 364 cases and 6,149 assertions.
- Unsigned arm64 Debug `WinCommanderUT`, `OperationsIT`, `OperationsUT` and `WinCommander-Unsigned` builds: passed.
