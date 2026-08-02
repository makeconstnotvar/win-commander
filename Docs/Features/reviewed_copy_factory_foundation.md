# Feature: reviewed Copy factory and conditional transaction foundation

> Status: reviewed evidence reaches a private production execution product and journal/Pool orchestrator; application consumer remains open
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 5, 7.2, 13.6, 14, 15, and 32
> Execution tracker: M3 in `Docs/Development-Plan.md`

## Purpose

This slice keeps accepted planning, explicit review, journal admission, provider authority, operation construction and queue admission as distinct capabilities. No plan ID, decoded plan, opened source descriptor or absent-destination check grants mutation authority by itself.

## Implemented chain

1. `MakeVFSOperationPlanningAccessChecker` projects application access into fail-closed planner evidence.
2. `VFSOperationPlanningProbes` binds exact hosts and source/destination version evidence to an accepted preflight.
3. `ReviewedVFSOperationPreflight::Review` creates a move-only approval value and requires explicit destructive confirmation when applicable.
4. `OperationPlanCodec` serializes the structural plan losslessly; `OperationJournal` durably admits it and issues exact move-only admission/run receipts.
5. `ReviewedOperationFactory` validates one Native-to-Native, create-only regular-file Copy and consumes the reviewed preflight into a private-constructible `ProviderConditionalCopyReviewedAuthority`.
6. The authority retains the moved reviewed preflight as a private seal and exposes immutable exact claims. `BeginConditionalCopyTransaction` consumes it once; a provider may then mint a move-only, single-use `ProviderConditionalCopyTransaction`.
7. `ProviderConditionalCopyOperationFactory` consumes the transaction into a move-only cold operation plus exact terminal journal-result accessor.
8. The production `CopyOperationOrchestrator` privately invokes that reviewed-factory path after durable admission, then orders Running, enqueue, terminal finalization, reconciliation, and Pool release; no application entry point uses it yet.

## Provider transaction contract

The authority contains the plan ID, source and destination host identities, exact source evidence, exact destination-parent evidence and an absent exact destination. Production callers cannot default-construct, copy, assign or mint it. Its private review seal owns the consumed exact preflight; the moved-from token cannot issue another authority. VFS validates canonical paths, object kinds/modes, timestamps, direct-child destination structure, seal presence and local host/provider relationships.

The transaction moves through `Pending → Committing → Consumed`. Provider commit/abort handlers execute at most once. Terminal calls replay the cached publication evidence; concurrent or moved-from use returns conservative `Unknown`. An abort can claim `NotPublished` only when the provider explicitly confirms it. Exceptions and ambiguous abort/commit results become `Unknown + ProviderFailure`.

`NativeHost` implements the bounded clone-only transaction from ADR 0001. The private `ReviewedOperationFactory::CreateExecutionProduct` path losslessly joins that transaction to `CopyOperationExecutionProduct`; the production orchestrator is its friend consumer. The public compatibility `ReviewedOperationFactory::Create` deliberately drops the cold product, resolves the transaction without publication, and returns a fail-closed authority/integration error. This prevents direct callers from bypassing durable admission and run-receipt custody.

## Accepted Native publication invariant

[`ADR 0001`](../ADR/0001-native-conditional-copy-publication.md) selects a bounded first provider scope: Native-to-Native, exact same `NativeHost`, exact same internal/local/writable APFS volume, single regular-file, create-only Copy. Review issues a private-constructible move-only authority with immutable semantic claims. Begin consumes it, opens the source with `O_NONBLOCK | O_NOFOLLOW_ANY`, opens the destination parent with `O_DIRECTORY | O_NOFOLLOW_ANY`, validates reviewed evidence and destination absence, verifies volume capabilities, seals supported ownership/mode/timestamps/flags/ACL/xattrs, and performs no namespace mutation. Commit repeats source, destination, parent, metadata-policy and volume checks immediately before one exclusive `fclonefileat(..., CLONE_ACL)` publication step. It then verifies destination metadata/name identity and orders destination fsync, parent fsync, and destination `F_FULLFSYNC`.

The reviewed source version is the admission and immediate pre-publication freshness condition. The retained descriptor supplies object identity; the clone syscall supplies a coherent copy-on-write snapshot and atomic destination creation. Post-publication metadata or sync failure remains `Published` with independent typed evidence. Other volumes and scopes return `Unsupported` and continue through the established operation path.

## Remaining application boundary

- Build the app-owned typed review step that issues the exact reviewed preflight consumed by the orchestrator.
- Add a cold pre-enqueue configurator so application callbacks are attached before `Pool` may start the operation.
- Present completion from exact durable terminal, publication, sync, recovery, `Reconcile`, and `ReleaseReconciled` evidence instead of generic `Operation::Completed`.
- Wire one bounded `CopyAs::Perform` consumer and retain zero-enqueue proof for blocked, stale, cancelled, unsupported, or unpersisted intent.
- Add provider-owned private/bounded staging for cross-volume data and metadata, with commit as the sole publish step.
- Execute dedicated physical internal/external-volume and power-loss evidence in the required environments.

Until the application contracts exist, public Copy mutation remains on established paths. Once an action selects the reviewed lifecycle, later failure must remain in its typed journal/recovery path and cannot fall back to a second legacy mutation attempt.

## Verified coverage

- `OperationPlan`: 8 / 113; planner: 13 / 228; VFS probes: 5 / 178; codec: 12 / 151.
- Reviewed factory: 8 / 225; VFS planning probes: 5 / 178.
- Provider capabilities: 16 / 548; Native conditional Copy: 15 / 312; combined Debug selection: 31 / 860; current full Debug `VFSUT`: 94 / 43,531.
- Earlier staged Native create-copy snapshot: 19 / 924; provider result mapper: 4 / 237; execution product: 9 / 188; journal: 27 / 592; Pool: 15 / 190; orchestrator: 13 / 558, including production construction at 3 / 138.
- Job lifecycle and worker-launch hardening: 10 / 608.
- Explicitly instrumented Release ASAN and UBSAN `VFSUT` both pass ProviderCapabilities at 16 / 548 and Native conditional Copy at 15 / 312.
- Full Debug, Release ASAN and Release UBSAN `OperationsUT`: 165 / 4,468 in each configuration.
- Current M0: unsigned Debug app and 10 aggregate binaries, 897 / 132,011 in the recorded seeded run.
- Docker-backed Debug ASAN integration: 163 / 89,392; fixture cleanup completed.

## Next slice

Implement typed application review, cold pre-enqueue callback configuration, and exact durable terminal presentation, then connect the bounded `CopyAs::Perform` consumer. Cross-volume staging and dedicated physical-volume fixtures follow as separate slices.
