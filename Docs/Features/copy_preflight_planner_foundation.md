# Feature: copy preflight planner foundation

> Status: pure Copy preflight through production conditional transaction, execution product, durable orchestration, and recovery implemented; narrow Move intent preflight is verified, rejected by generic review, and has no execution adoption
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 13.6, 14, 15, and 32
> Execution tracker: M3 in `Docs/Development-Plan.md`, R5 in `Docs/refactor_plan.md`

## Purpose

`nc::ops::OperationPlanner` is the pure preflight boundary between a structurally valid `OperationPlan` and later review/factory stages. The implemented Copy slice and a deliberately narrower Move intent slice return an owning deterministic `AcceptedOperationPlan` or `BlockedOperationPlan` without UI, queue, execution, persistence, or concrete provider ownership. The companion production adapter wraps that result with the exact immutable VFS bindings used to derive it.

The C++ `OperationPlan` is the structural intent portion of the ideal specification's canonical UI-visible Operation Plan. The user-visible review model is composed from that intent and this planner's preflight report, including provider/access evidence, affected paths, estimates, conflicts, destructive effects, warnings, blockers, and confirmation requirements.

For Copy, `Accepted` means that the captured evidence is ready for explicit review. `ReviewedVFSOperationPreflight::Review` turns an accepted bound Copy result into a move-only factory input and requires a destructive confirmation decision for Replace. It rejects accepted Move with `UnsupportedPlanType`; every accepted report still requires runtime revalidation before any future mutation.

## Inputs and probe contract

The planner accepts an owning structural plan and an injected `OperationPlanningProbes` implementation. The probe boundary supplies:

- provider capability and path-identity evidence;
- item existence, kind, symlink presence, and optional file size;
- destination filename validity;
- required read/write/rename access state;
- recursive file/byte estimates;
- available destination space.

The planner preserves provider IDs and path spelling in probes and reports. Probe implementations must not retain references passed into a call. Exceptions, invalid enum payloads, unsupported identity comparison, cancellation, unavailable providers, and failed probes resolve to typed blockers or explicitly supported warnings; they never create a silently partial accepted result.

## Path identity and deterministic safety

Provider evidence declares one path-identity semantic:

- `ExactBytes` for byte-exact identity;
- `ASCIICaseSensitive` when byte-preserving ASCII comparison is authoritative;
- `ASCIICaseInsensitive` when the planner can safely fold ASCII names;
- `Unavailable` when the planner cannot prove identity.

Same-provider source/destination checks combine both provider snapshots conservatively. The planner fails closed when the required comparison is unavailable or a non-ASCII path exceeds the declared ASCII-only semantic. Multi-source collision detection uses destination identity only, rejects duplicate effective destinations, and preserves valid cross-provider sources whose parent paths contain non-ASCII text but whose destination names remain comparable.

Probe caches use stable provider/path/access keys. Destination readiness is evaluated before source-dependent work, identical probes are deduplicated, source order remains deterministic, and cancellation stops later dependent probes.

## Copy preflight report

For each supported source, the report owns source/destination paths, item kind, and optional estimate. It also owns provider/access evidence, conflicts, destructive effects, warnings, destination-space evidence, checked totals, and the confirmation requirement.

The implemented policy boundary:

- blocks missing or unreadable sources and missing, non-directory, or non-writable destinations;
- blocks same-path and recursive directory destinations;
- blocks intra-plan destination collisions;
- treats `Ask` as a required conflict decision;
- records `Replace` as destructive and requires confirmation;
- applies `Skip` without producing a planned copy item;
- fails closed for `MergeFolders`, directory replacement, and conflict decisions or scopes outside the current Copy subset;
- requires source read-symlink and destination create-symlink capability when a source or recursive estimate contains symlinks;
- blocks special-file sources;
- blocks checked estimate overflow and known insufficient space;
- records explicit warnings when estimates or space evidence are unavailable.

An empty effective item set becomes `NothingToDo`. A blocked result always owns at least one typed blocker. Neither result type exposes an execution method, and callers cannot fabricate accepted or blocked values through public constructors.

## Narrow Move intent preflight

The Move subset accepts exactly one regular file from one provider to an absent `ExactItem` destination on that same provider. Its structural policy must be `Ask/ThisItem`. It probes authoritative path identity, both source and destination parent directories, destination-name validity, `can_rename`, and `Rename` access on each parent namespace. The accepted report owns one exact source/destination item and records `RuntimeRevalidationRequired`.

Folders, symlinks, batches, cross-provider destinations, directory destinations, same paths, existing destinations, and conflict policies other than `Ask/ThisItem` are blocked. The Move path uses no Copy read/create capability, recursive estimate, or free-space claim. It produces no reviewed token: `ReviewedVFSOperationPreflight::Review` rejects its bound accepted result before factory, journal, or `Pool` authority. Existing `Copying(docopy = false)` Move execution remains outside this slice.

## Architectural boundary

The production [`VFSOperationPlanningProbes`](vfs_operation_planning_probes_foundation.md) adapter now provides provider binding plus provider, item, destination-name, access, estimate, and space evidence. Its bound result retains the exact `VFSOperationPlanningBindings::Ptr`; the pure planner itself remains independent of VFS ownership.

The combined foundation now includes application access composition, schema-v1 structural-plan persistence, durable journal admission, explicit Copy review, private-sealed conditional authority, a clone-only Native provider transaction, lossless provider-result mapping, a transaction-owning execution product, and production journal/`Pool` orchestration. `ReviewedOperationFactory` exposes its production execution-product authority only to the orchestrator; the public compatibility construction surface remains fail closed. The accepted Move report deliberately stops before this authority boundary. Other providers, cross-volume targets and filesystems without clone capability return `Unsupported`.

The bounded `CopyAs` consumer now supplies exact create-only review, app coordination of restricted cold hooks and durable result/recovery presentation. Its production boundary proves zero enqueue for blocked, stale, unpersisted and cancelled intent plus exact review and durable outcome dispatch. Remaining boundaries are live permission/conflict, Move review/factory/execution adoption, broader mutation-consumer adoption, cross-volume staging, execution of the physical-volume/power-loss protocol, live/full Operation Center presentation, and preflight for Rename, Trash, Permanent Delete or archive operations. Committed Rename and Move execution still reach `nc::ops::Copying(docopy = false)`, and Cut remains clipboard Move intent until Paste.

## Verified coverage

The focused Debug `OperationPlanner` suite passed 13 cases / 228 assertions. It covers owning result lifetime after probe destruction, Copy-only admission, deterministic probe ordering and deduplication, exception/error/enum fail-closed behavior, access and capability blockers, preserved provider paths, exact and ASCII-only identity semantics including non-ASCII rejection, same/recursive paths, intra-plan collisions, unsupported policies and directory replacement, destructive confirmation, estimates, special files, symlink capability, unknown evidence, overflow, and insufficient space.

Final Move-preflight evidence (2026-08-06): focused Debug planner passes 5 cases / 69 assertions; VFS rename-capability mapping passes 1 / 4 and generic-review rejection passes 1 / 5; application access-checker coverage passes 4 / 65. The full Debug `OperationsUT` run records 203 / 204 cases and 5,379 / 5,383 assertions with only the established NativeCreateCopy set-ID metadata host baseline. The full Debug `WinCommanderUT` run records 330 / 334 and 5,317 / 5,321 with four headless pasteboard host baselines. These cases cover accepted one-file same-provider intent, both parent rename capabilities/access, same-path and destination-conflict rejection, special/batch/cross-provider rejection, absence of Copy estimate/space/read claims, and `Review(...Move...) == UnsupportedPlanType`; they prove no execution authority.

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
- `CopyOperationOrchestrator`: 17 / 806, including production construction at 3 / 138 and receipt-aware no-re-admission;
- historical foundation snapshot: Debug, Release ASAN, and Release UBSAN `OperationsUT` passed 170 / 4,748 in each configuration, with sanitizer runtimes confirmed and no diagnostics; the current coordinator/control subset separately passes Release ASAN and UBSAN at 28 / 999 without diagnostics.

The current-tree M0 run from 2026-08-01 passed the unsigned Debug application and all 10 seeded aggregate binaries: 897 cases / 132,011 assertions in the recorded run. Docker-backed seeded ASAN integration passed 163 / 89,392; hosted CI remains open.

## Next slice

Add physical-volume proof for the bounded `CopyAs::Perform` consumer, preserving tri-state publication, terminal durability, read-only post-rename reconciliation and exact reconciled Pool release. Cross-volume Copy requires separate provider-owned bounded staging; Move review/factory/execution and other non-Copy preflight follow as separate slices.
