# Q1-1 New Folder and visible-selection Registry batch

Status: implemented second Q1-1 batch; the full roster remains in progress.

## Contract

The application-owned Registry defines seventeen stable production commands. This batch adds:

| Command | Existing execution path | Mounted entry points |
|---|---|---|
| `file.newFolder` | `nc::ops::DirectoryCreation` | File menu, `Shift-Command-N`, Explorer New popover |
| `pane.selectAll` | `PanelData` selection mutation | Edit menu, `Command-A` with responder-chain precedence |
| `pane.invertSelection` | `PanelData` selection mutation | Edit menu, `Control-Command-A` |

All definitions carry stable legacy selector, shortcut action and tag metadata. Menu, shortcut and Explorer callers submit an explicit invocation source through `NCPanelControllerActionsDispatcher`; Registry state and execution both receive the live pane target. The shared Edit Select All item restores its base presentation when the menu closes, so a focused field editor keeps standard AppKit responder precedence.

## New Folder admission

State requires a live pane and window operation queue, a committed uniform listing, a stable provider/path, path-aware write access and `can_create_folder`. Execution captures the exact window, provider, path and listing, finds a valid collision-free provisional name, then repeats loading, provider, path, listing, writable and capability checks before it constructs and enqueues the established `nc::ops::DirectoryCreation` operation. Any stale, read-only, unsupported or unavailable state returns a typed localized Registry rejection and reaches no operation queue.

The Explorer New popover uses the same Registry state as the File menu and shortcut. A disabled New Folder row stays visible with its localized tooltip and has no target or action. The adjacent legacy New File action remains unchanged.

## Visible selection

Select All and Invert Selection operate on `PanelData::EntriesBySoftFiltering()`. They exclude the parent entry, clear selection from items outside the current soft-filter projection and work on readable remote or read-only listings because they do not mutate provider data. Loading, missing listings and empty visible projections are disabled with localized reasons. Execution rechecks the exact listing before applying one complete selection vector.

## Remaining New File gap

The current `OnQuickNewFile:` action writes directly through `VFSEasyCreateEmptyFile`; there is no established `nc::ops::*` file-creation operation. Queue 1 permits product-surface migration only over existing operations and freezes expansion of the reviewed engine. Therefore `file.newFile` remains pending until it has an allowed legacy operation boundary; routing the direct VFS mutation through Registry would violate the Queue 1 contract.

## Verification

- `WinCommanderUT 'nc::core::FileMutationCommands *' --rng-seed 424242`: 8 cases / 230 assertions.
- `WinCommanderUT 'nc::core::CommandRegistry *' --rng-seed 424242`: 17 / 189.
- `WinCommanderUT 'CommandPresentationAdapter *' --rng-seed 424242`: 12 / 93.
- `WinCommanderUT 'PanelController navigation Registry New Folder*' --rng-seed 424242`: 1 / 26.
- `WinCommanderUT 'PanelController navigation Registry Select All*' --rng-seed 424242`: 1 / 15, including visible soft-filter projection and parent-entry exclusion.
- `WinCommanderUT 'Explorer presentation geometry New popover*' --rng-seed 424242`: 1 / 13.
- Full Debug `WinCommanderUT --rng-seed 424242`: 359 / 359 cases and 5,996 assertions.
- Unsigned arm64 Debug `WinCommanderUT` and `WinCommander-Unsigned` builds: passed.
