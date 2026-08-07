# Q1-1 Calculate Sizes and Batch Rename Registry batch

Status: implemented fourth Q1-1 batch; the full roster remains in progress.

## Contract

The application-owned Registry defines twenty-two stable production commands. This batch adds:

| Command | Existing execution path | Mounted entry points |
|---|---|---|
| `file.calculateSizes` | provider directory traversal and `PanelController` calculated-size projection | Command menu, persisted shortcut, exact-item context menu, Explorer More |
| `file.batchRename` | `NCOpsBatchRenamingDialog` and established `nc::ops::BatchRenaming` | Command menu, persisted shortcut, exact-item context menu, Explorer More |

Both definitions carry stable legacy selector, shortcut-action and tag metadata. Menu, shortcut, context and Explorer callers submit an explicit invocation source through `NCPanelControllerActionsDispatcher`; context-menu calls retain their exact captured entries as the Registry payload. Disabled Explorer More items remain visible with a localized tooltip and accessibility help and own no target or action.

The existing `Calculate All Sizes` action remains a separate legacy command. It targets every directory in the current listing rather than the selected or focused entries and therefore is not an alias of `file.calculateSizes`.

## Calculate Sizes admission and commit

Admission requires a live pane with a committed current listing, exact non-parent entries from that listing, at least one directory, readable source capabilities and an idle directory-size queue. Mixed file/directory selections are accepted, but only directories are submitted. Execution repeats admission immediately before scheduling.

The controller captures the exact listing pointer and data generation before background traversal. A result batch commits only while both identities still match. Navigation, refresh or another model replacement makes the result stale and suppresses the entire UI/model commit; calculated sizes from an old listing are never resolved by matching names into a new listing. A second request while the serial calculation queue is active is rejected as busy.

Provider traversal retains the existing size/error behavior: an unreadable source fails admission, while provider errors encountered during accepted traversal remain represented by the legacy calculation result. Cancellation is cooperative through the existing serial queue.

## Batch Rename admission and enqueue

Batch Rename requires a live window and operation queue, a committed uniform listing, exact current-directory non-parent entries from one provider and path-aware rename capability. The dialog receives an immutable copy of the accepted entries.

Confirmation revalidates the window, pane generation, listing identity, provider, directory and complete entry set before constructing an operation. The returned source and destination vectors must have equal non-zero size; sources must be a unique subset of the accepted entries; every destination must be an exact child of the captured directory with a valid filename; destination names must be unique under the provider's case-sensitivity rules and must not collide with an unselected current-listing item. A failed or stale plan beeps and enqueues nothing. An accepted plan uses the established `nc::ops::BatchRenaming`, and completion requests the normal filesystem refresh.

This is a conservative Queue 1 adapter over the legacy operation. Rename chains whose destination is another selected source are currently rejected. The ideal-spec atomic no-overwrite authority, operation-plan journal, persistent log and undo contract remain future work; a destination can still appear after confirmation and before the legacy operation performs its mutation.

## Properties boundary

`file.getInfo` remains a separate read-only Properties command and belongs with the Q1-3 details/preview surface. The current `OnFileAttributes:` action edits permissions, ownership and timestamps through `nc::ops::AttrsChanging`; Explorer now labels that action `File Attributes` instead of presenting it as read-only Get Info. A future Registry migration for that mutation uses the distinct `file.editPermissions` identity.

## Verification

- Debug `WinCommanderUT 'nc::core::FileMutationCommands*' --rng-seed 424242`: 15 cases / 442 assertions.
- Debug `WinCommanderUT 'nc::core::CommandRegistry*' --rng-seed 424242`: 17 / 194.
- Debug production selector and exact-context route: 1 / 61.
- Debug Explorer More disabled/enabled projection and Toolbar execution: 1 / 62.
- Full Debug `WinCommanderUT --rng-seed 424242`: 368 / 368 cases and 6,339 assertions.
- Full explicitly instrumented Release ASAN `WinCommanderUT --rng-seed 424242`: 368 / 368 and 6,339 without diagnostics. Two filesystem-fixture failures caused by an initial concurrent ASAN/UBSAN run passed as an isolated 2 / 54 rerun before the clean full ASAN rerun.
- Full explicitly instrumented Release UBSAN `WinCommanderUT --rng-seed 424242`: 368 / 368 and 6,339 without diagnostics.
- Unsigned arm64 Debug `WinCommander-Unsigned` build: passed.
