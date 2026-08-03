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

- reviewed CopyAs selection, unavailable fail-closed behavior, submission gate and app boundary: 10 / 98;
- app boundary: blocked, stale, unpersisted and cancelled paths make zero submission calls; the exact review projection and owning durable failure dispatch before Pool removal with generic completion suppressed pass 4 / 70 in Debug, Release ASAN and Release UBSAN;
- recovery coordinator: 6 / 67;
- fresh unsigned Debug `WinCommander-Unsigned` build passes;
- fresh full Debug `WinCommanderUT`: 309 / 313 cases and 4,994 / 4,998 assertions pass; four AppKit pasteboard cases reproducibly fail because the service is unavailable;
- fresh full Debug `OperationsUT`: 169 / 170 and 4,744 / 4,748 pass; one existing set-ID NativeCreateCopy metadata case fails on this host;
- aggregate ASAN and UBSAN both reach the same pasteboard baseline after BaseUT and ConfigUT without sanitizer diagnostics;
- latest full Debug `VFSUT` run: 95 / 43,566;
- ProviderCapabilities: 16 / 549; Native conditional Copy: 16 / 328.

The final tree passes `git diff --check`.

## Remaining M3 gates

- execute the implemented internal/external physical-volume fixture and add the required power-loss checkpoint harness/evidence; see `Docs/Features/reviewed_copy_as_physical_volume_protocol.md`;
- provider-owned bounded cross-volume staging;
- [`OperationCenterModel`/`OperationId` control projection and persistence](operation_center_model_and_control_projection.md), plus broader mutation consumers.
