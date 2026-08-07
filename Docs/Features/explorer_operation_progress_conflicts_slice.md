# Explorer operation progress and copy conflicts

> Status: Queue 1 Q1-8 complete
>
> Execution tracker: `Docs/Development-Plan.md`, Queue 1 item Q1-8

## User-visible contract

Explorer mounts a compact operation strip directly below its command bar. The strip belongs to the window's exact `nc::ops::Pool`, appears at a fixed 44-point height while that pool contains work and collapses to zero height after the final operation leaves the pool.

The strip presents one deterministic primary operation: work waiting for a user decision has first priority, followed by running, paused, finalizing and queued work; equal lifecycle states retain their copied pool order. An additional-operation count preserves the rest of the active queue. The primary projection shows the operation title, current file, lifecycle, processed and total bytes or items, transfer rate and ETA. Unknown totals use an indeterminate indicator. A complete total uses a bounded fraction, and unavailable or invalid rate/ETA samples are omitted.

The existing copy-conflict sheet presents the destination path plus source and destination size and modification time. Its explicit actions are Replace, Replace Older, Skip, Keep Both, Append when supported and Cancel. Replace uses the native destructive-action style. Apply to all is available for a batch decision and is removed from the single-item interaction together with its keyboard authority. The established modal response codes remain the execution contract consumed by `Copying`.

## Progress ownership and polling

`ExplorerOperationProgressController` retains the exact per-window pool and observes its addition/removal boundary. While the pool contains work, a weak main-queue timer refreshes the copied projection every 100 ms. Controller and view teardown therefore end presentation polling without extending their lifetime through a scheduled callback.

Each refresh copies title, current-item path, lifecycle and both `Statistics` sources into `ExplorerOperationProgressInput`. `ExplorerOperationProgressModel` owns the toolkit-independent normalization and returns an immutable `ExplorerOperationProgressSnapshot`; it retains no `Pool`, `Operation`, `Job` or mutable `Statistics` object. `NCExplorerOperationProgressView` is the AppKit renderer for that snapshot and receives no operation-control authority.

`Job::CurrentItemPath()` returns an owning copy under a dedicated mutex. `CopyingJob` publishes the source path immediately after selecting each item for processing. Worker termination clears the path before the terminal operation notification, so a completed or stopped operation cannot leave a stale filename in the strip. Earlier snapshots remain independent of later publication.

## Conflict ownership

`NCOpsFileAlreadyExistDialog` remains the conflict authority already used by the established `Copying` operation. Q1-8 aligns its user language and accessibility surface while retaining the underlying response mapping. Stable accessibility identifiers cover the window, conflict title, destination path, source/destination metadata, Replace/Replace Older, Skip, Keep Both, Append, Cancel and Apply to all.

The sheet remains bound to the exact window through the existing pool dialog callback. A waiting operation is projected as `WaitingForUser`, so the progress strip makes the blocked lifecycle visible while the sheet owns the decision.

## Accessibility and appearance

The strip is one labelled accessibility group with a stable identifier. Its value composes the visible operation title, full current-item path, lifecycle, progress, rate, ETA and additional-operation count, and each refresh posts `NSAccessibilityValueChangedNotification`. The current-file label shows the final path component and retains the full path as its tooltip. Lifecycle text, numeric progress and the native determinate/indeterminate indicator carry the state independently of color.

The conflict sheet exposes stable labels and identifiers for its compared metadata and every action. Replace combines its title with native destructive styling. This deterministic accessibility contract is covered in focused AppKit tests; a signed manual VoiceOver walkthrough remains part of the Q1-10/release evidence.

## Q1-8 boundary

Q1-8 closes the per-window Explorer surface for active Pool progress and the established `Copying` conflict decision. The separately bounded `operationCenter.open` panel continues to own its copied active/terminal record snapshot and Registry-gated Cancel path. A future live Operation Center adds persistent history, result/error detail, logs, filters and sealed pause/resume/retry controls over `OperationCenterModel`.

The signed app walkthrough for a real long-running copy, conflict decision, cancellation and terminal transition remains external release evidence. Q1-8 closure uses deterministic model/AppKit/Operations coverage plus the current unsigned application build.

## Verification

Confirmed current-tree evidence:

- focused Debug `WinCommanderUT` progress model/view/controller coverage: 11 cases / 107 assertions passed;
- full Debug `WinCommanderUT`: 518 cases / 9,137 assertions passed;
- Debug `WinCommander-Unsigned` build passed;
- focused Debug copy-conflict dialog coverage: 3 cases / 41 assertions passed;
- full Debug `OperationsUT`: 216 cases / 5,726 assertions passed;
- focused Debug current-item publication coverage: 2 cases / 19 assertions passed;
- focused Release ASAN and Release UBSAN current-item coverage: 2 cases / 19 assertions passed in each runtime without diagnostics;
- focused Release ASAN and Release UBSAN `WinCommanderUT` progress model/view/controller coverage: 11 cases / 107 assertions passed in each runtime, with `libclang_rt.asan_osx_dynamic.dylib` and `libclang_rt.ubsan_osx_dynamic.dylib` confirmed by `otool` and no sanitizer diagnostics.

The progress tests cover empty-pool layout collapse, exact per-window pool isolation, deterministic primary selection, bytes/items source fallback, determinate and indeterminate progress, large and overflowing counters, invalid speed/ETA samples, copied current-item paths, additional-operation count, waiting state, accessibility composition, operation removal and weak presentation teardown. The Operations tests cover concurrent owning current-item snapshots, publication replacement, terminal clearing and stopped-worker clearing. The conflict tests cover accessible explicit actions, destructive Replace, single-item removal of Apply to all authority and exact legacy response mappings.
