# Reviewed `CopyAs` production consumer

> Status: bounded Native same-directory create-only consumer implemented and locally verified

## Scope

`CopyAs::Perform` routes one regular Native item through the reviewed M3 lifecycle when the destination is an absolute path in the source directory, default Copy options preserve the supported semantics, and the provider returns path-specific `Supported` eligibility. Known unsupported shapes retain the established Copy operation. Unavailable eligibility blocks the request.

## Production flow

1. Capture the active pane identity, data generation, exact focused item and requested destination.
2. Resolve path-specific conditional-Copy eligibility without granting execution authority.
3. Build the exact structural plan and VFS-bound preflight on a worker.
4. Revalidate the captured pane and item, then show source, destination, create-only scope, conflict policy, file/byte estimate, destination space, access evidence and warnings.
5. On explicit approval, revalidate intent again, mint the single-use reviewed authority and acquire a window submission ticket.
6. Submit through the process-owned journal, run-receipt custodian, private reviewed factory, window `Pool` and provider transaction.
7. Present item status and the owning durable terminal outcome. Successful publication refreshes and focuses the destination; cancellation is silent; failure or uncertainty preserves typed publication, system, sync and recovery evidence.

Blocked, stale, cancelled, unpersisted or insufficiently bound reviewed intent never reaches `Pool`. Window close and application termination cancel new submissions and wait for acquired submission tickets. Recovery is process-owned and bounded to the exact plan: `Retry`, then same-storage reopen, `Reconcile`, and `ReleaseReconciled` when required. Startup exposes interrupted Copy history without automatic resume.

## Verified evidence

- full Debug `WinCommanderUT`: 309 cases / 4,995 assertions;
- reviewed CopyAs selection, unavailable fail-closed behavior and submission gate: 6 / 28;
- recovery coordinator: 6 / 67;
- full Debug `OperationsUT`: 170 / 4,748;
- explicitly instrumented Release ASAN and UBSAN `OperationsUT`: 170 / 4,748 each, with runtime linkage confirmed and no sanitizer diagnostics;
- latest full Debug `VFSUT` run: 95 / 43,566;
- ProviderCapabilities: 16 / 549; Native conditional Copy: 16 / 328.

Debug `UnitTests` and `WinCommander-Unsigned` arm64 builds pass. The Xcode project files pass `plutil`, and the final tree passes `git diff --check`.

## Remaining M3 gates

- live application boundary tests for blocked, stale, cancelled and persistence-failure zero-enqueue behavior plus exact review/outcome UI dispatch;
- dedicated internal/external physical-volume and power-loss fixtures;
- provider-owned bounded cross-volume staging;
- Operation Center persistence and broader mutation consumers.
