# Q1-1 Paste, Trash and Permanent Delete Registry batch

Status: implemented first Q1-1 batch; the full roster remains in progress.

## Contract

At closure of this first batch, the application-owned Registry defined fourteen stable commands. This batch added:

| Command | Existing execution path | Primary entry points |
|---|---|---|
| `file.paste` | `nc::ops::Copying` through `PasteFromPasteboard` | Edit menu, `Command-V`, Explorer command bar |
| `file.trash` | `nc::ops::Deletion(Trash)` | Command menu, `Command-Backspace`, Explorer Delete button, exact-item context menu |
| `file.delete` | reviewed `nc::ops::Deletion(Permanent)` | Command menu, `Shift-F8`, exact-item context menu |

All three definitions expose localized unavailable reasons through `CommandState`. The shared Edit Paste item preserves AppKit field-editor precedence and restores its base presentation when the menu closes. `OnDeleteCommand:` continues to own the legacy F8 chooser, whose combined Trash/Permanent intent is distinct from the two canonical commands in this batch. Background Paste and command-bar overflow composition belong to Q1-2.

## Live admission

Paste requires a live pane, window operation queue, stable uniform destination, path-aware writable capability, readable complete clipboard file list and an unclaimed move token. The execution port captures destination provider and path, resolves every clipboard source, then repeats the destination and token checks before enqueue. A failed submission returns a typed reason; a Cut claim is released on every pre-enqueue exit.

Trash and Permanent Delete evaluate every exact item through `ProviderCapabilitiesResolver`. Read-only paths, parent entries, missing providers and unsupported provider semantics receive distinct state. Execution accepts the same current panel listing generation; Native items also retain their inode identity at submission. Permanent Delete presents the established review sheet with an explicit permanent result and repeats listing, inode, target and capability checks after confirmation. A stale confirmation displays the localized stale-selection message and creates no operation.

The canonical Trash submission starts as `DeletionType::Trash`. The established operation keeps its interactive error recovery: any later permanent recovery requires a separate explicit choice in the error dialog.

## Presentation

- Explorer Paste and Delete buttons use the same Registry definitions as menu and shortcut routes.
- The exact-item context menu uses Registry state and execution for Trash and Permanent Delete.
- Disabled Trash remains visible with tooltip and accessibility help; Permanent Delete becomes the ordinary context action when Trash capability is unavailable.
- `CommandPresentationAdapter` owns tooltip, accessibility help, hidden and enabled projection for the new surfaces.

## Verification

- `WinCommanderUT 'nc::core::FileMutationCommands *' --rng-seed 424242`: 5 cases / 119 assertions.
- `WinCommanderUT 'nc::core::CommandRegistry *' --rng-seed 424242`: 17 / 186.
- `WinCommanderUT 'CommandPresentationAdapter *' --rng-seed 424242`: 12 / 88.
- `WinCommanderUT 'PanelController navigation Registry Paste*' --rng-seed 424242`: 1 passed, 1 pasteboard-server fixture skipped / 39 assertions.
- `WinCommanderUT 'Explorer presentation geometry compact Operations menu*' --rng-seed 424242`: 4 / 70, covering the command-bar construction fixture after the roster expansion.
- Full Debug `WinCommanderUT --rng-seed 424242`: 348 / 353 cases and 5,746 / 5,750 assertions; the four failures are the existing headless `NSPasteboard` host baselines, and the new pasteboard-server-dependent case is an explicit skip.
- `Scripts/verify_m0.sh`: the unsigned app and aggregate `UnitTests` schemes built successfully; `BaseUT` passed 78 / 70,566 and `ConfigUT` passed 38 / 76 before the script stopped at the same four `WinCommanderUT` pasteboard baselines (exit 42), with no new failure.
