# Reviewed `CopyAs` production consumer

> Status: bounded Native same-directory create-only consumer implemented and locally verified; cross-volume authority foundation is unactivated

## Scope

`CopyAs::Perform` routes one regular Native item through the reviewed M3 lifecycle when the destination is an absolute path in the source directory, default Copy options preserve the supported semantics, and the provider returns path-specific `SameVolumeClone` eligibility. `CrossVolumeStaged` is a distinct future selection scope and requires a separate signed authority. Known unsupported shapes retain the established Copy operation. Unavailable eligibility blocks the request.

## Production flow

1. Capture the active pane identity, data generation, exact focused item and requested destination.
2. Resolve path-specific conditional-Copy eligibility without granting execution authority.
3. Build the exact structural plan and VFS-bound preflight on a worker.
4. Revalidate the captured pane and item, then show source, destination, create-only scope, conflict policy, file/byte estimate, destination space, access evidence and warnings.
5. On explicit approval, revalidate intent again, mint the single-use reviewed authority and acquire a window submission ticket.
6. Submit through the process-owned journal, run-receipt custodian, private reviewed factory, window `Pool` and provider transaction.
7. Present item status and the owning durable terminal outcome. Successful publication refreshes and focuses the destination; cancellation is silent; failure or uncertainty preserves typed publication, system, sync and recovery evidence.

Blocked, stale, cancelled, unpersisted or insufficiently bound reviewed intent never reaches `Pool`. Window close and application termination cancel new submissions and wait for acquired submission tickets. Recovery is process-owned and bounded to the exact plan: `Retry`, then same-storage reopen, `Reconcile`, and `ReleaseReconciled` when required. Startup exposes interrupted Copy history without automatic resume.

The test-only physical checkpoint harness exercises this production route through an injected `NativeHost` at two provider commit boundaries: before clone publication and after publication plus ordinary file/parent `fsync`, before `F_FULLFSYNC`. It durably writes a descriptor-anchored manifest before an optional operator hold. Its separate recovery profile validates the retained workspace and manifest, reads the unfinished journal through a read-only descriptor-bound inspector, and only then permits normal journal startup classification from `Running` to `Interrupted`. Its authority is limited to the retained-artifact evidence boundary; a subsequent process-owned recovery decision supplies any execution authority.

## Verified evidence

- reviewed CopyAs selection, unavailable fail-closed behavior, submission gate and app boundary: 10 / 98;
- app boundary: blocked, stale, unpersisted and cancelled paths make zero submission calls; the exact review projection and owning durable failure dispatch before Pool removal with generic completion suppressed pass 4 / 70 in Debug, Release ASAN and Release UBSAN;
- recovery coordinator: 6 / 67;
- fresh unsigned Debug `WinCommander-Unsigned` build passes;
- historical Debug `WinCommanderUT` snapshot: 309 / 313 cases and 4,994 / 4,998 assertions pass; four AppKit pasteboard cases reproducibly fail because the service is unavailable;
- historical Debug `OperationsUT` snapshot: 169 / 170 and 4,744 / 4,748 pass; one existing set-ID NativeCreateCopy metadata case fails on this host;
- aggregate ASAN and UBSAN both reach the same pasteboard baseline after BaseUT and ConfigUT without sanitizer diagnostics;
- historical full Debug `VFSUT` run: 95 / 43,566; current full Debug `VFSUT` is 127 / 130 and 44,616 / 44,619 with three host-environment baselines, current full Debug `OperationsUT` is 197 / 5,295, and current full Debug `WinCommanderUT` is 333 / 5,376;
- ProviderCapabilities: 16 / 549; Native conditional Copy: 16 / 328.
- cross-volume authority foundation: rebuilt `VFSUT '*conditional Copy*' --rng-seed 424242` passes 26 / 850; focused `OperationsUT` provider/orchestrator/factory sets pass 44 / 1,608; rebuilt `WinCommanderUT 'reviewed CopyAs policy *' --rng-seed 424242` passes 11 / 101. The test seam sees only anchored FDs, a basename and complete scalar seals. The private V1 codec passes 4 / 74 and the injectable staging client passes 4 / 108, including FD duplication, exact correlation/lease binding, single terminal request, client-side cancellation gates before Begin, after Granted and before Commit dispatch, cleanup and conservative lost-reply handling. The helper's move-only `ValidatedBegin` exact-revalidates both duplicated descriptors and all scalar seals, rejects writable FD rights and sets/checks `FD_CLOEXEC`; its 4 / 208 VFSUT filter passes in Debug, Release ASAN and Release UBSAN. The bounded lease store now accepts only that validated authority, mints its opaque token with `SecRandomCopyBytes`, accepts one correlation per authenticated owner and closes descriptor rights on terminal take, revoke and rejected admission; its updated focused VFSUT filter passes 5 / 328. The protected-root ledger accepts an exact root-owned `0700` root FD, keeps one live owner per sealed root, and classifies a sealed artifact read-only only from an exact complete manifest and full no-follow artifact seal. It retains valid sealed, absent, incomplete, malformed and mismatched states; a reservation becomes removable only after the canonical artifact name is proven absent. Its focused VFSUT filter passes 8 / 207 in Debug, Release ASAN and Release UBSAN. Artifact materialization, cleanup and a user-visible namespace operation await the mutable helper layer. The helper dispatcher validates Begin but remains inert and therefore cannot select this authority. NonMAS owns V1 embed/privileged-helper declarations and the target requests Developer ID plus hardened runtime; signed artifact/SMJobBless validation awaits a usable local signing identity. Durable seal writing, final publish-barrier revalidation/cancellation and helper-only cleanup/recovery remain open.

The final tree passes `git diff --check`.

## Remaining M3 gates

- execute the implemented internal/external physical-volume fixture and both physical checkpoint/recovery profiles with real abrupt-power hardware evidence; see `Docs/Features/reviewed_copy_as_physical_volume_protocol.md`;
- signed NonMAS artifact and SMJobBless trust proof, then durable protected-root artifact materialization/seal writing, provider-owned bounded cross-volume staging, final publication-barrier revalidation/cancellation, helper-only cleanup/recovery and the two-internal-APFS physical proof;
- [`OperationCenterModel`/`OperationId` control projection and persistence](operation_center_model_and_control_projection.md), plus broader mutation consumers.
