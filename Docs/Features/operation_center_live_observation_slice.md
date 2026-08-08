# Q2-4 OC-1…OC-4: Live observation, the live panel, and pause/resume

> Status: implemented and tested — see §Verification.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-4.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §13.5 and §14 (Operation Center: observer, progress, pause/resume/retry, persistent history, logs).
> Depends on: [`operation_center_model_and_control_projection.md`](operation_center_model_and_control_projection.md).

## Scope

`OperationCenterModel` already held everything a live Operation Center needs to *show* — immutable value records, per-record revisions, derived control availability — but nothing could learn that it had changed. Every consumer re-read a snapshot when it happened to open. That is the single reason the existing panel is documented as static and why reopening is its refresh action.

This increment adds the missing half: observation. It is the prerequisite for the rest of Q2-4 (a live panel, pause/resume/retry controls, persistent history views), and it is the part where getting the concurrency wrong would be expensive to discover later.

**OC-2** then consumes it: the Operation Center panel stays current while it is open instead of requiring a reopen.

**OC-3** adds the pause/resume control port on the coordinator, and **OC-4** puts it on the panel.

Not here: retry, log capture, and history persistence beyond the hydration the coordinator already performs.

## The contract

```cpp
using ObservationTicket = ScopedObservableBase::ObservationTicket;
[[nodiscard]] ObservationTicket ObserveChanges(std::function<void()> _callback);
```

Fired for every **accepted** change: publication, lifecycle transition, startup hydration, cold-history refresh. A rejected mutation — stale revision, invalid transition, unknown operation — fires nothing, so a consumer is never woken into a redraw that would render exactly what it already shows.

The callback carries **no payload**, deliberately. The consumer answers it with `Snapshot()`, which returns a self-consistent set of records taken under the model's own lock. Handing the callback a record instead would let a consumer assemble a view from several notifications delivered at different moments — precisely the "view built from two generations" failure the existing model design already avoids by returning complete replacement snapshots.

Observation uses the codebase's existing `base::ScopedObservableBase`, the same mechanism `Operation` and `Pool` already expose, rather than a new one. Tickets are move-only, retire their observer on destruction, and tolerate the observable being destroyed first.

## Firing outside the lock

Every mutator on this model takes `m_Impl->lock` for its whole body and returns from inside the guard. Firing observers from there would deadlock the first consumer that did the obvious thing — call `Snapshot()` from its callback — and that consumer is the entire intended use.

The fix is a small RAII helper, `DeferredNotification`, **declared before the lock guard** in each mutator and armed only once the mutation has actually succeeded. Locals destroy in reverse order of construction, so the guard releases the lock first and the notification fires after it. The ordering is a property of the declaration order rather than of remembering to call something at each return, which matters in `Transition` and `RefreshColdHistory` where there are many early returns and only one success path.

`RefreshColdHistory`'s early "nothing to add" return stays unarmed: it changed no record, so there is nothing for a consumer to redraw.

This is why the slice carries a genuine deadlock test — one that hangs rather than fails if the design regresses — and a lifetime test where the ticket outlives the model.

## OC-2: the panel goes live

`OperationCenterCoordinator::ObserveChanges` forwards to the model. It lives on the coordinator rather than being reached through `Model()` because registering an observer mutates the model's observer list while `Model()` is deliberately a const read handle — and because every consumer already holds the coordinator, not the model.

The panel subscribes when it is presented and drops the ticket when it is hidden, so nothing observes a closed panel.

Three things the callback deliberately does *not* do:

- **It does not touch views.** Notifications arrive on whichever thread accepted the change — Pool threads publish terminal outcomes — so the callback's entire body is a hop to the main queue.
- **It does not redraw per notification.** A copy operation moves through several states in milliseconds; rebuilding the record list for each would flicker while telling the user nothing extra. A queued flag coalesces a burst into one redraw.
- **It does not apply a delta.** The refresh re-reads a whole `Snapshot()`, so the panel can never assemble a view from two model generations — the same property the model's snapshot design already guarantees for a single read.

Presenting was split into `renderOperationCenterSnapshot:` (rebuild contents from one whole snapshot, never show or hide) and `presentOperationCenterSnapshot:` (ensure panel, render, observe, show), so the live path and the open path render through exactly the same code and cannot drift.

The refresh re-checks panel visibility after its main-queue hop, because the panel may have closed in between.

## OC-3: the pause/resume port

The model already had everything for this — `Running ↔ Paused` transitions and a `ControlsFor` table that offers `can_pause` only while Running and `can_resume` only while Paused. What was missing was the port that revalidates a request and reaches the executor.

`OperationCenterCoordinator::SetPaused(id, expected_revision, intent)` mirrors `Cancel` deliberately, because the failure it has to prevent is the same one: a control authorised against a record the user was *looking at* being applied to whatever the operation is doing *now*. So it revalidates the exact revision, then the record's own control projection, then live Pool residency — and re-checks all of it under the same `cancel_gate` the cancel path uses, so a cancellation landing concurrently cannot be overtaken by a pause authorised against the pre-cancel record.

**The record's own projection is the authority for direction.** A resume on a running operation and a pause on a paused one are refused here rather than handed to the executor to sort out, which is what keeps a redundant request from producing a state the panel would then have to explain.

Unlike `Cancel` there is no in-flight gate: pausing is reversible, so a duplicate request is simply refused by the projection rather than needing to be serialised against itself.

A request that cannot reach the executor leaves the model untouched. That is the point of the `ResidencyUnavailable` path: a panel must never start claiming an operation is paused when the executor never saw the request.

## OC-4: pause/resume on the panel

Each record that offers a direction gets one button. Pause and Resume are mutually exclusive by construction — `ControlsFor` offers `can_pause` only while Running and `can_resume` only while Paused — so a record never grows both and the panel never has to decide which to show.

The button carries the **revision of the record it was drawn for**. The coordinator refuses the request if that revision has moved on, which is what stops a click landing on a state other than the one the button was drawn against — the panel is live now, so a record genuinely can change between the draw and the click.

A refusal beeps rather than being swallowed: a silently ignored click looks identical to one that worked. Either outcome re-renders, because on success the state moved and on refusal the panel was already showing a record that had changed underneath it.

One bug fixed while wiring this: the control loop began with `if( !record.controls.can_cancel ) continue;`, which would have skipped any record that offers only pause or resume. Cancel is available in every state that offers pause or resume today, so it was latent rather than visible — but it made the loop's structure depend on that coincidence.

## Verification

Built and run in this session (Xcode 26.6 toolchain):

- `xcodebuild -scheme OperationsUT -configuration Debug build` — **BUILD SUCCEEDED**.
- Focused Debug `OperationsUT '[operation-center-model]' --rng-seed 424242`: **8/8 cases, 101/101 assertions** — 4 pre-existing plus 4 new: every accepted change notifies and every rejected one does not, and reads never notify; an observer calling `Snapshot()` from its callback completes and already sees the change that triggered it; releasing one ticket retires only that observer; a ticket outliving the model is safe to release afterwards.
- Full Debug `OperationsUT --rng-seed 424242`: **221/221 cases, 5,808/5,808 assertions** (re-run after OC-2's forwarder and OC-3's port; the extra case is OC-3's).
- Full Debug `WinCommanderUT --rng-seed 424242`: **666/666 cases, 11,038/11,038 assertions**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED** (re-run after OC-4, which also adds two localized strings to `Localizable.xcstrings` in both en and ru).
- **Release ASAN** `OperationsUT --rng-seed 424242`: **221/221 cases, 5,808/5,808 assertions**, `libclang_rt.asan_osx_dynamic.dylib` verified linked via `otool -L`, **0** AddressSanitizer diagnostics and 0 `SUMMARY:` lines. (The three `Error:` lines in that output are the suite's own intentional throwing fixtures, not sanitizer findings.)
- **Release UBSAN** `OperationsUT --rng-seed 424242`: **221/221 cases, 5,808/5,808 assertions**, `libclang_rt.ubsan_osx_dynamic.dylib` verified linked, **0** `runtime error` diagnostics with `print_stacktrace=1`.
- **Release ThreadSanitizer** — the run that actually proves this slice's claim. `libclang_rt.tsan_osx_dynamic.dylib` verified linked. Focused `'[operation-center-model]' --rng-seed 424242`: **9/9 cases, 224/224 assertions, 0 data races, 0 ThreadSanitizer warnings**. Full `OperationsUT`: **222/222 cases, 5,931/5,931 assertions**, with one pre-existing warning unrelated to this slice — see below.
- `AGENTS.md` scopes sanitizer runs to changes *in* `Operations`, concurrency, ownership or lifetime code. This slice is all three, so unlike the Q2-1…Q2-3 presentation slices it does not qualify for the focused-filter tier — hence both runtimes above.

### Pre-existing defect found and recorded, not fixed here

The full TSan run reports one data race on the global `std::cerr`, between `Job::Run` (`Source/Operations/source/Job.cpp:63`, main thread) and `Job::FinishExecution` (`Job.cpp:141`, worker thread). Both are pre-existing error-logging statements that stream several chained `<<` operations to `std::cerr` without synchronization; they execute only when a stop or finish callback throws, which today only the intentional fixtures in `Job_UT.cpp` and `Pool_UT.mm` do.

It does not intersect this slice's dependency surface — `Job.cpp` is untouched here, and the model's own notification path is race-free in both the focused and full runs. Per `AGENTS.md` it is recorded once with its scope and left for its own change rather than widened into this one. Notably Debug, ASAN and UBSAN all pass without surfacing it; only TSan does.

### Coverage gaps

- **The live panel has no automated coverage.** `WinCommanderUT` does not bootstrap the application's operation-center composition, so the subscribe/coalesce/re-render path is exercised only by construction and by the model-level tests underneath it. The pieces that *can* be tested without that bootstrap — when the model notifies, and that an observer may re-read from its callback — are covered above.
- **OC-3's executor-reaching path is untested.** The three fail-closed gates that do not need a live Pool — unknown operation, stale revision, and the control projection refusing a direction — are covered. Reaching `ResidencyUnavailable` or an accepted pause needs a record in `Running`, and `ReduceStarted` is coordinator-private, so driving one there from a test would mean widening that surface. The accepted path shares its revalidation and residency code with `Cancel`, which the engine exercises through its own composition.
- ~~No multi-threaded stress case.~~ **Closed.** Four producer threads drive disjoint operations through Running/Paused while two observers answer every notification with `Snapshot()` and a third thread reads continuously — the shape production actually has once OC-2 puts a panel on the main thread while the Pool transitions records from its own. Every snapshot is checked for internal consistency (no duplicate ids, expected size). Producers own disjoint records so a failure is a real race rather than two threads contending for one record.
- `Hydrate` and `RefreshColdHistory` notify, but neither has an observation test: both are coordinator-private, and widening the test-only surface purely to observe them is a worse trade than leaving them to the coordinator's own coverage. Their notifications follow the same `DeferredNotification` pattern as the paths that are covered.
