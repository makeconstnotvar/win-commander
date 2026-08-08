# Q2-4 OC-1: Live observation of the Operation Center model

> Status: implemented and tested — see §Verification.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-4.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §13.5 and §14 (Operation Center: observer, progress, pause/resume/retry, persistent history, logs).
> Depends on: [`operation_center_model_and_control_projection.md`](operation_center_model_and_control_projection.md).

## Scope

`OperationCenterModel` already held everything a live Operation Center needs to *show* — immutable value records, per-record revisions, derived control availability — but nothing could learn that it had changed. Every consumer re-read a snapshot when it happened to open. That is the single reason the existing panel is documented as static and why reopening is its refresh action.

This increment adds the missing half: observation. It is the prerequisite for the rest of Q2-4 (a live panel, pause/resume/retry controls, persistent history views), and it is the part where getting the concurrency wrong would be expensive to discover later.

Not here: the live panel itself, pause/resume/retry control ports, log capture, and history persistence beyond the hydration the coordinator already performs.

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

## Verification

Built and run in this session (Xcode 26.6 toolchain):

- `xcodebuild -scheme OperationsUT -configuration Debug build` — **BUILD SUCCEEDED**.
- Focused Debug `OperationsUT '[operation-center-model]' --rng-seed 424242`: **8/8 cases, 101/101 assertions** — 4 pre-existing plus 4 new: every accepted change notifies and every rejected one does not, and reads never notify; an observer calling `Snapshot()` from its callback completes and already sees the change that triggered it; releasing one ticket retires only that observer; a ticket outliving the model is safe to release afterwards.
- Full Debug `OperationsUT --rng-seed 424242`: **220/220 cases, 5,763/5,763 assertions**.
- **Release ASAN** `OperationsUT --rng-seed 424242`: **220/220 cases, 5,763/5,763 assertions**, `libclang_rt.asan_osx_dynamic.dylib` verified linked via `otool -L`, **0** AddressSanitizer diagnostics and 0 `SUMMARY:` lines. (The three `Error:` lines in that output are the suite's own intentional throwing fixtures, not sanitizer findings.)
- **Release UBSAN** `OperationsUT --rng-seed 424242`: **220/220 cases, 5,763/5,763 assertions**, `libclang_rt.ubsan_osx_dynamic.dylib` verified linked, **0** `runtime error` diagnostics with `print_stacktrace=1`.
- `AGENTS.md` scopes sanitizer runs to changes *in* `Operations`, concurrency, ownership or lifetime code. This slice is all three, so unlike the Q2-1…Q2-3 presentation slices it does not qualify for the focused-filter tier — hence both runtimes above.

### Coverage gaps

- **No multi-threaded stress case.** The tests exercise the reentrancy and lifetime contracts single-threaded; `ScopedObservableBase` is documented and already relied upon as thread-safe, and this slice adds no new synchronization of its own beyond moving the fire point outside an existing lock. A concurrent producer/observer stress test would still be worth adding when the live panel starts consuming this from the main thread while the Pool transitions records from its own.
- `Hydrate` and `RefreshColdHistory` notify, but neither has an observation test: both are coordinator-private, and widening the test-only surface purely to observe them is a worse trade than leaving them to the coordinator's own coverage. Their notifications follow the same `DeferredNotification` pattern as the paths that are covered.
