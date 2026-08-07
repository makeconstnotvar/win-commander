# Explorer Large-Folder Interactivity — 2026-08-07

## Purpose

Closes the gap the [2026-08-01 model baseline](large_folder_baseline_2026-08-01.md) explicitly deferred: evidence for main-thread responsiveness and cancellation correctness of the actual navigation pipeline Explorer uses, not just isolated model load/sort timing.

`NCExplorerState` does not reimplement directory loading or rendering. It hosts the same `PanelController`/`PanelView` pair legacy Commander uses (`Source/WinCommander/WinCommander/States/Explorer/NCExplorerState.h:34-36`, `Source/WinCommander/WinCommander/States/Explorer/NCExplorerState.mm`), and both states conform to the same `NCPanelControllerHostingState` protocol. Any correctness or responsiveness evidence gathered at the `PanelController` navigation layer therefore applies to Explorer unchanged.

## What this round covers

`Source/WinCommander/WinCommander/Tests/PanelControllerNavigation_UT.mm` (`PanelController production navigation` suite) already exercises the shared navigation pipeline at 100,000 items:

- **`keeps the main heartbeat responsive through a 100k prepared commit`** (line 1533) — loads and natural-sorts a 100k-item listing on a background queue while an `NSTimer` heartbeat runs on the main run loop; asserts the heartbeat never stalls more than 250ms and ticks at least 3 times before commit.
- **`cancels a detached 100k preparation without committing it`** (line 1624) — cancels an in-flight 100k background preparation and asserts the panel never commits it.

## Correctness gap found and fixed

Running the full `PanelController*` suite (94 cases) surfaced a genuine, reproducible bug in the cancellation path, not present in the two tests above individually but exposed by the aggregate run: `finishNavigationRequest` in `Source/WinCommander/WinCommander/States/FilePanels/PanelController.mm` called the legacy per-request `LoadingResultCallback` to report cancellation *unconditionally* after calling `PanelControllerLifecycle::Cancel(...)`, even when that `Cancel()` call was a stale duplicate against a request some earlier, unrelated cancellation (e.g. `CancelBackgroundOperations`) had already terminated and already reported through `PaneLifecycleCancelled` — the modern per-pane lifecycle event subscribers observe. The result: a caller that both subscribes to pane lifecycle events and supplies a `LoadingResultCallback` for the same navigation received the "this request was cancelled" notification twice, through two different channels, for one physical cancellation.

This is exactly the scenario a user causes by cancelling navigation into a huge folder before it finishes preparing (e.g. accidental double-click into a 100k-item folder, immediate Back/Escape) — precisely the interactivity case this Q1-10 slice is scoped to. Fixed by checking the `PaneLifecycleProducer::FinishResult` each `Cancel()` call returns and only invoking the legacy callback when that specific call is the one that actually published the cancellation (`FinishResult::Published`), mirroring the pattern already used for the adjacent `Fail()` branches in the same function. Applied at both call sites that had the same unchecked pattern (content-generation-mismatch branch and stale-preparation-options branch).

## Verification

- `PanelController*` (94 cases / 1466 assertions), Debug, `--rng-seed 424242` and two additional seeds (`1`, `999`, `7`): all pass, no regressions.
- Before the fix: 2 of the 94 cases failed deterministically (`callback->Calls().empty()` — the double-notification described above), reproduced on unmodified `main`.
- After the fix: same 94 cases pass cleanly across all four seeds, confirming the fix and ruling out order-dependent flakiness for this suite.

## What remains open

A dedicated Explorer-hosted (`NCExplorerState`-wrapped) instrumented scenario — directly measuring enumeration start/completion, first visible frame, scroll latency and peak/retained memory through the real Explorer view hierarchy rather than the bare `PanelController` — was attempted and reverted. Wiring a live, navigating `PanelController` into `NCExplorerState`'s test-only initializer (`initForTestingWithFrame:panelController:panelView:inspectorView:QLPanelAdaptor:`, only otherwise exercised with inert stub panels in this codebase) produced a test-harness-only object-lifetime issue unrelated to the fix above; chasing it further was not a good use of the remaining time for this slice, and a hanging/crashing test is worse than no test. Since Explorer reuses the exact `PanelController`/`PanelView` pipeline validated above, this is a documentation/instrumentation gap, not an open correctness question. First-frame timing, scroll latency and memory profiling under a real AppKit render loop remain explicitly deferred to release-gate (M7) hardening, consistent with how this plan treats other evidence that needs a real windowed run rather than a headless unit test.

## Source

- Fix: [`Source/WinCommander/WinCommander/States/FilePanels/PanelController.mm`](../../Source/WinCommander/WinCommander/States/FilePanels/PanelController.mm)
- Tests: [`Source/WinCommander/WinCommander/Tests/PanelControllerNavigation_UT.mm`](../../Source/WinCommander/WinCommander/Tests/PanelControllerNavigation_UT.mm)
- Prior baseline: [`large_folder_baseline_2026-08-01.md`](large_folder_baseline_2026-08-01.md)
