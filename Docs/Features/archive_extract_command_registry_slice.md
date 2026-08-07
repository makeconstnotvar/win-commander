# Q1-1 Archive Extract Registry slice

Status: implemented final mutating Q1-1 command slice.

## Command and surfaces

The application-owned Registry defines `archive.extract` as one stable Archive command. The File menu, persisted shortcut binding, exact-item context menu and Explorer More menu query the same typed state and execute through `NCPanelControllerActionsDispatcher`. Context-menu execution retains the exact clicked or selected item payload; menu and Explorer execution bind the current selected-or-focused payload. Disabled states provide localized reasons for missing pane or window context, loading, invalid selection, unsupported or unreadable source, read-only or unsupported destination, unavailable case-sensitivity evidence and stale context.

The V1 command accepts exactly one regular, non-symlink archive with a whitelisted extension from the current listing. The source provider must be readable. The destination is the current uniform pane directory and must be a writable Native provider with file, directory and symlink creation capabilities plus authoritative case-sensitivity reporting.

## Typed acquisition and namespace admission

`VFSArchiveProxy::OpenFileAsArchiveResult` preserves typed archive-open failures, password cancellation, operational source failures and raw-archive fallback while the legacy nullable wrapper remains source-compatible. Planning runs on the panel cancelable-loading queue; password UI synchronizes through the existing main-thread password controller, and cancellation produces no error alert.

Before any operation is enqueued, the action recursively materializes the immutable archive namespace and rejects:

- empty, dot, dot-dot, slash-bearing or embedded-NUL components;
- provider-invalid destination names and paths deeper than 127 components;
- special entry kinds;
- duplicate paths, file/directory type conflicts and file or symlink ancestors;
- case-folding and canonical-normalization collisions on case-insensitive destinations;
- manifests above the bounded entry limit;
- an unavailable destination root, an existing symlink anywhere in a materialized prefix, or a non-directory intermediate prefix.

Planning captures the exact pane, window, listing generation, source listing, destination provider, destination path and case-sensitivity result. The source additionally carries a no-follow inode/size/mtime seal checked before and after archive-host acquisition and again before enqueue; the Native destination root carries a device/inode/birth-time seal. Main-thread commit repeats those bindings and live command admission before constructing the operation. A stale, cancelled, unsupported or unsafe intent reaches no operation queue.

## Legacy operation boundary

Accepted extraction reuses `nc::ops::Copying` over the immutable archive host. It preserves archive symlinks as symlinks, rejects final-component symlink traversal, treats the captured destination as a directory and uses the established conflict UI. A runtime preflight repeats the sealed destination-namespace check at the existing Copying preflight points. Completion refreshes only a live pane that still shows the captured provider and directory.

This is a Queue 1 legacy-operation slice. It does not extend `OperationPlan`, reviewed authority, Journal admission or the M3 execution chain. The runtime preflight narrows the legacy namespace race but is not a descriptor-bound publication barrier; the remaining check-to-use window is part of the established Copying boundary. Native-only destination admission prevents weaker remote namespace semantics from being presented as equivalent.

## Verification

- Debug `VFSUT 'VFSArchive proxy typed acquisition*'`: 4 cases / 23 assertions.
- Full Debug `VFSUT --rng-seed 424242`: 173 cases / 46,692 assertions.
- Debug `WinCommanderUT 'nc::core::ArchiveExtractionManifest*'`: 7 cases / 75 assertions.
- Debug `WinCommanderUT 'nc::panel::actions::ExtractArchive*'`: 4 cases / 29 assertions, covering source-seal drift and incomplete evidence plus early depth and aggregate traversal budgets.
- Debug `WinCommanderUT 'nc::core::FileMutationCommands*'`: 18 cases / 625 assertions, including exact payload, all invocation sources, stable metadata and typed rejections.
- Debug `WinCommanderUT 'Explorer presentation geometry*'`: 24 cases / 994 assertions, including disabled/enabled Explorer More projection and execution.
- Full Debug `WinCommanderUT --rng-seed 424242`: 391 cases / 6,899 assertions.
- Release ASAN and UBSAN `VFSUT 'VFSArchive proxy typed acquisition*'`: 4 cases / 23 assertions in each runtime.
- Release ASAN and UBSAN `WinCommanderUT '*ArchiveExtractionManifest*,*ExtractArchive*' --rng-seed 424242`: 11 cases / 104 assertions in each runtime, with no sanitizer diagnostics.
- The arm64 Debug `WinCommander-Unsigned` application target built successfully with code signing disabled.

The remaining Q1-1 work is the read-only `file.getInfo` production surface delivered with Q1-3; all mutating roster commands are implemented and gated.
