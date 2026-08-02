# Feature: structural `OperationPlan` foundation

> Status: structural intent, Copy preflight/review, durable codec/journal, anchored Native Copy capsule, typed outcome mapping and bounded `CopyAs` consumer implemented
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` section 14.1
> Execution tracker: M3 in `Docs/Development-Plan.md`, R5 in `Docs/refactor_plan.md`

## Purpose

`nc::ops::OperationPlan` is the owning immutable value for a structurally valid filesystem-mutation intent. The foundation establishes one exact vocabulary for Copy, Move, Rename, Trash, and Permanent Delete before provider-dependent preflight or execution begins.

The canonical UI-visible Operation Plan in the ideal specification is the composed review model: this structural intent plus the preflight report and its provider, access, conflict, estimate, space, warning, blocker, and confirmation evidence. The C++ `OperationPlan` deliberately represents the structural portion of that canonical model; `AcceptedOperationPlan` and `BlockedOperationPlan` provide the current Copy preflight projection.

Creation is pure and deterministic. Callers provide every identity and the creation timestamp; the plan performs no filesystem, provider, clock, UI, persistence, or queue access.

## Owned structural contract

A valid plan owns:

- an opaque non-empty plan ID;
- one of `Copy`, `Move`, `Rename`, `Trash`, or `PermanentDelete`;
- one or more sources, each identified by a non-empty provider ID and absolute path;
- an optional destination with its own provider ID, absolute path, and `Directory` or `ExactItem` semantics;
- an optional conflict policy whose decision and scope are valid for operation types that use conflicts;
- a caller-injected `std::chrono::system_clock::time_point`;
- type-derived intrinsic source, destination, and data-loss effects.

The public value stores its own strings and source collection. It has no default construction, execution method, acceptance method, provider object, listing item, or UI reference.

## Operation-specific validation

`OperationPlan::Create` returns either an immutable plan or one exact `OperationPlanValidationError`.

- Every plan requires a valid plan ID, injected timestamp, valid operation type, and a non-empty source collection.
- Provider IDs are non-empty opaque strings without embedded NUL characters.
- Source and destination paths begin with `/` and contain no embedded NUL characters.
- A source identity is the `(provider ID, absolute path)` pair; duplicate pairs are rejected while equal paths on distinct providers remain distinct.
- Destination kind, conflict decision, and conflict scope reject unknown enum values.
- Copy and Move require a destination and conflict policy. A directory destination accepts multiple sources; an exact-item destination accepts one source.
- Rename requires one source, an exact-item destination on the same provider, and a conflict policy.
- Trash and Permanent Delete have no destination or conflict policy.

The conflict policy records requested structural intent. Collision discovery, provider support, policy applicability to a concrete item type, generated names, destructive confirmation, and the final resolved action belong to preflight. The companion copy planner now resolves the supported Copy subset into an owning review-ready report.

## Intrinsic effects

The plan derives only effects that are unavoidable from the operation type:

| Type | Source effect | Destination effect | Data-loss risk |
|---|---|---|---|
| Copy | Unchanged | CreateOrUpdate | None |
| Move | Relocated | CreateOrUpdate | None |
| Rename | Relocated | CreateOrUpdate | None |
| Trash | Relocated | None | Recoverable |
| PermanentDelete | Deleted | None | Irreversible |

Requested conflict policy does not alter these intrinsic effects. Resolved overwrite, merge, skip, and generated-name effects require provider and filesystem evidence, so they belong to the separate preflight report rather than this structural value.

## Architectural boundary

The structural value ends after construction and validation. The separate [`OperationPlanner`](copy_preflight_planner_foundation.md) adds Copy-only provider, item, destination, access, conflict, estimate, space, warning, and blocker evidence. [`VFSOperationPlanningProbes`](vfs_operation_planning_probes_foundation.md) supplies the production provider bindings and probes; `ReviewedVFSOperationPreflight` records explicit approval.

`OperationPlanCodec` provides strict schema-v1 serialization for the structural intent, and `OperationJournal` provides durable admission, lifecycle snapshots, item results, terminal state, and restart classification. These layers preserve the authority boundary: serialization, persistence, review, and queue admission are distinct capabilities.

`ReviewedOperationFactory` consumes a private-sealed reviewed authority and obtains the bounded Native clone-only transaction. Its private execution-product path is the production construction authority used by `CopyOperationOrchestrator`; its public compatibility `Create` surface still resolves the cold product and fails closed so callers cannot bypass journal admission and run-receipt custody.

The Native clone-only provider transaction includes a strict internal-writable-APFS predicate, exact supported metadata seals/parity, post-clone verification, ordered destination/parent/full-filesystem durability, and typed post-publication failure evidence. The lossless provider mapper, transaction-owning execution product, exact journal finalization, restricted cold hooks, owning durable-outcome delivery, preallocated `Pool` finalization barrier, `ReleaseWithoutCompletion`, production orchestrator, read-only reconciliation, exact reconciled Pool release and bounded `CopyAs` app review/presentation are implemented and unit-tested. Dedicated physical-volume fixtures, cross-volume provider-owned staging and broader mutation adoption remain open.

## Verified coverage

The focused `OperationPlan` suite passed 8 cases / 113 assertions. It covers value ownership and construction traits, all five operation types, exact identity/path validation, duplicate-source identity, destination kind/cardinality/provider rules, conflict-policy presence and enum validation, injected timestamp, and intrinsic effects.

Current Debug evidence for the operation pipeline:

- `OperationPlan`: 8 / 113;
- `OperationPlanner`: 13 / 228;
- `VFSOperationPlanningProbes`: 5 / 178;
- `OperationPlanCodec`: 12 / 151;
- `ReviewedOperationFactory`: 8 / 225;
- provider conditional result mapper: 4 / 237;
- provider conditional execution product: 9 / 188;
- earlier staged `NativeCreateCopy` snapshot: 19 / 924;
- `OperationJournal`: 27 / 592;
- Job lifecycle: 10 / 608;
- `Pool`: 17 / 219;
- `CopyOperationOrchestrator`: 15 / 758, including production construction at 3 / 138;
- full Debug, Release ASAN, and Release UBSAN `OperationsUT`: 170 / 4,748 in each configuration, with sanitizer runtimes confirmed and no diagnostics.

The current-tree M0 run from 2026-08-01 passed the unsigned Debug application and all 10 seeded aggregate binaries: 897 cases / 132,011 assertions in the recorded run. Docker-backed seeded ASAN integration passed 163 / 89,392; hosted CI remains open.

## Companion preflight and next slice

The implemented copy-first pure [`OperationPlanner`](copy_preflight_planner_foundation.md):

1. accepts an owning structural plan and injected provider/item/destination-name/access/estimate/space probes;
2. resolves provider capability evidence, source/destination state, recursive/same-path constraints, conflicts, permission requirements, estimates, and available-space evidence;
3. returns an owning typed `AcceptedOperationPlan` or `BlockedOperationPlan` with a deterministic fail-closed report;
4. keeps user permission requests, review UI, concrete operation construction, queueing, persistence, and Operation Center outside the planner.

The production [`VFSOperationPlanningProbes`](vfs_operation_planning_probes_foundation.md) adapter creates a bound preflight that retains the exact immutable provider bindings used for its evidence. [`ReviewedVFSOperationPreflight`](reviewed_copy_factory_foundation.md) records explicit approval and issues one private-sealed provider authority. `OperationJournal` issues exact admission/run receipts, and the production `CopyOperationOrchestrator` privately consumes the reviewed factory's transaction-owning execution product while preserving durable queue, terminal, reconcile, and Pool-release ordering.

The next vertical slice is live application-boundary proof for the bounded `CopyAs::Perform` consumer, followed by physical-volume evidence. Cross-volume staging and non-Copy preflight remain later sequences.
