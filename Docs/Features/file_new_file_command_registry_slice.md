# Q1-1 New File Registry slice

Status: implemented fifth Q1-1 slice.

## Contract

The application-owned Registry defines twenty-three stable production commands. `file.newFile` now owns the File menu, persisted `Option-Command-N` shortcut and Explorer New-popover entry. All three surfaces query the same typed state and execute through `NCPanelControllerActionsDispatcher`; disabled menu and popover rows retain a localized reason, and the popover removes its target and action while disabled.

State requires a live pane and window operation queue, a committed uniform listing, path-aware write access, `can_create_file`, a provider implementation with proven exclusive-create semantics and a valid collision-free provisional basename. Execution prepares the name once from the captured window, provider, directory and listing, then repeats loading, provider, path, listing, writable, capability and exclusive-create checks immediately before enqueue. A stale, read-only, unsupported or exhausted-name intent reaches no operation queue.

## Legacy operation boundary

The previous action mutated the provider directly from a detached background block. No existing Queue 1 operation represented empty-file creation, so this slice adds the narrow legacy `nc::ops::EmptyFileCreation` operation and its one-item Job. It requests the existing provider-independent no-overwrite flags through `VFSEasyCreateEmptyFile`, reports operation progress, exposes the standard Abort/Retry error flow and is submitted only through the window `Pool`. The reviewed planner, factory, journal, orchestrator and M3 authority chain are unchanged.

The operation accepts exactly one nonempty relative basename and rejects absolute, nested, dot, dot-dot and embedded-NUL names before provider access. Existing-file failure preserves the current Native file and its contents. On completion, the pane refresh and inline rename are scheduled only while the weak pane still points at the captured provider and directory.

The application admission allowlist contains Native and SFTP only. Their `OF_NoExist` paths map to `O_EXCL` and `LIBSSH2_FXF_EXCL`, so a concurrent creator cannot be replaced. FTP, WebDAV and unknown providers fail closed even when their broad capability reports `can_create_file`, because their current implementations do not establish the same exclusive publication contract.

## Verification

- Debug `OperationsIT 'Operations::EmptyFileCreation*'`: 4 cases / 21 assertions, covering empty regular-file creation, existing-file preservation, invalid names including embedded NUL, and one-item progress.
- Debug `WinCommanderUT '*FileMutationCommands*'`: 18 cases / 625 assertions for the complete current mutation Registry roster.
- Focused provider admission and Registry routing: 2 cases / 42 assertions; the exact admission case contributes 10 assertions.
- Full Debug `OperationsUT --rng-seed 424242`: 211 cases / 5,666 assertions.
- Full Debug `WinCommanderUT --rng-seed 424242`: 391 cases / 6,899 assertions.
- Release ASAN and UBSAN `OperationsIT 'Operations::EmptyFileCreation*'`: 4 cases / 21 assertions in each runtime.
- Debug `OperationsIT`, `OperationsUT` and `WinCommanderUT` scheme builds passed on arm64.
- The arm64 Debug `WinCommander-Unsigned` application target built successfully with code signing disabled after the complete mutating roster was linked.

The remaining Q1-1 work is the read-only `file.getInfo` production surface delivered with Q1-3.
