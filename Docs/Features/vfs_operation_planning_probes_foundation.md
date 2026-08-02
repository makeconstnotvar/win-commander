# Feature: production VFS operation-planning probes foundation

> Status: production Copy preflight adapter through conditional transaction, execution product, durable orchestration, and recovery implemented; application mutation adoption remains open
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 13.6, 14, 15, and 32
> Execution tracker: M3 in `Docs/Development-Plan.md`, R5 in `Docs/refactor_plan.md`

## Purpose

`nc::ops::VFSOperationPlanningProbes` is the synchronous production adapter from explicitly bound `VFSHost` instances to the pure [`OperationPlanner`](copy_preflight_planner_foundation.md) probe contract. It supplies filesystem evidence without opening UI, constructing an operation, mutating files, or entering `Operations::Pool`.

The adapter creates `VFSBoundOperationPreflight`, which owns both the typed preflight result and the exact immutable `VFSOperationPlanningBindings::Ptr` used to produce that result. `ReviewedVFSOperationPreflight` consumes this value through an explicit decision. `ReviewedOperationFactory` then consumes the reviewed token into a private-sealed move-only provider authority, so provider IDs cannot be rebound and one token cannot issue two transactions. Durable journal admission remains a separate single-use authority; `CopyOperationOrchestrator` orders it before private factory construction and queue admission.

The structural `OperationPlan` is the intent component of the ideal specification's canonical UI-visible Operation Plan. The bound preflight report supplies the evidence component used for review: provider capabilities, access, affected paths, estimates, conflicts, destructive effects, warnings, blockers, and confirmation requirements.

## Binding and namespace contract

`VFSOperationPlanningBindings` owns strong references from non-empty opaque provider IDs to hosts. Creation rejects:

- an empty provider ID or missing host;
- duplicate provider IDs;
- the same host pointer or shared ownership under multiple IDs;
- distinct hosts that expose the same semantic namespace;
- multiple same-type hosts when either host cannot provide authoritative `SemanticNamespaceIdentity()` evidence.

Different host types have distinct namespaces by contract. For two hosts with the same `Tag()`, both namespace identities must exist and differ. Native hosts expose one stable native namespace identity, so aliases cannot bypass same-path or recursive-destination checks.

`VFSBoundOperationPreflight::Bindings()` returns the same `Bindings::Ptr` held by the probes instance. This pointer identity, rather than a reconstructed string mapping, remains the provider namespace through review and factory construction.

## Evidence supplied to the planner

### Provider capabilities and path identity

`ProbeProvider` resolves `ProviderCapabilities` at the requested path and projects:

- source read and destination file/folder creation;
- file and directory replacement capability;
- destination symlink creation capability;
- authoritative path case semantics.

`VFSHost::CaseSensitivityAtPath()` returns optional authoritative evidence. Missing evidence becomes `Unavailable`. Native evidence is exposed as ASCII case-sensitive or ASCII case-insensitive semantics; the planner blocks a comparison containing non-ASCII text under either ASCII-only mode. No Unicode folding or normalization is inferred.

### Host errors

`VFSHost::ClassifyError()` classifies `Missing`, `PermissionDenied`, `Unsupported`, `Cancelled`, `Unavailable`, and `Other` without text matching. The base implementation covers POSIX errors; `SFTPHost` classifies its provider error domain, including missing, permission, unsupported, and disconnected/unavailable states. Source item probing preserves `Missing` as item evidence. Other failures become typed probe errors and fail closed unless the planner explicitly defines a warning for that probe.

### Item and filename evidence

`ProbeItem` uses no-follow stat semantics. It accepts regular files and directories, and accepts symlinks only when the source provider declares symlink-read capability. Missing items remain distinct from provider failure. Unknown mode evidence, FIFOs, sockets, devices, and other special files return `UnsupportedItem` and are blocked.

`ProbeDestinationName` delegates to the destination host's `ValidateFilename()`. Native recursive estimation also validates every nested name when copying across providers, because validating only the root destination name would be incomplete.

### Access evidence

Provider capability is checked before the injected `AccessChecker` for `Read`, `Write`, `ReplaceFile`, or `ReplaceDirectory`. An unsupported capability returns denied evidence. A configured checker supplies application-specific account, sandbox, or permission state and is followed by cancellation revalidation.

The adapter deliberately does not request permission UI. Without an injected checker it returns `PermissionRequired`, so production planning cannot silently assume access. `MakeVFSOperationPlanningAccessChecker` is the production application composition over `DirectoryAccessProvider::HasAccess`: `Write` checks the exact destination directory, while `Read` and replacement access check the normalized parent/root. A denied, malformed, or throwing check maps to `PermissionRequired`; permission recovery remains an outer UI policy.

### Recursive estimate and space

Native recursive estimation:

- walks directories without following symlinks;
- collects entry paths in the listing callback and performs `Stat` after the callback, avoiding reentrant provider calls;
- counts regular files and copyable symlinks, sums known byte sizes, and records `contains_symlinks`;
- validates nested destination names for cross-provider copies;
- fails on special files, overflow, vanished children, unreadable entries, and partial directory iteration.

Native `IterateDirectoryListing` now resets and inspects `errno` for each `readdir` completion, so a partial listing followed by an error no longer appears successful. Unknown item size and non-native recursive estimation return `Unsupported`; the planner records estimate-unavailable warning where safe. A directory without an estimate is still blocked when destination symlink creation cannot be proved, and cross-provider directory planning also requires enough estimate evidence to validate nested destination names.

`ProbeSpace` uses `StatFS`. An all-zero result means unknown space, while a known zero available-byte value is preserved. Known insufficient space blocks; unavailable space evidence becomes the planner's explicit warning.

### Cancellation and exception containment

Every probe checks cancellation before and after provider or injected callbacks. A throwing cancellation checker is treated as cancellation. Provider and callback exceptions are contained and become typed failure; no exception can create a partial accepted report.

## Planner safety boundary

The combined planner and VFS adapter enforce the current Copy subset:

- source and destination namespace/path identity must be provable;
- source and destination capability, access, filename, and space evidence remain fail closed;
- direct and recursively discovered symlinks require source read-symlink and destination create-symlink support;
- special files are unsupported;
- `MergeFolders`, generated-name policies, unsupported scopes, and other unsupported policies are blocked;
- directory replacement is blocked; supported file replacement requires replacement capability, replacement access, destructive warning, and confirmation;
- a non-native recursive estimate can be a warning, but it never licenses an unsafe symlink or nested-name assumption.

`AcceptedOperationPlan` remains review/factory readiness only. Runtime revalidation is recorded in every report, and the bound result supplies the exact provider instances for that later boundary.

`ReviewedOperationFactory` is fail closed at its public compatibility boundary. Native consumes exact source, destination-parent and absent-destination claims into a same-host/internal-writable-APFS clone-only transaction with supported metadata parity and ordered durability barriers. The private friend path maps the provider result, creates the transaction-owning execution product, and supplies it to the production orchestrator; direct public construction still resolves the cold transaction without publication and returns an error.

`OperationPlanCodec` and `OperationJournal` provide schema-v1 structural intent persistence, durable admission/run receipts, atomic item-plus-terminal finalization, tri-state publication evidence and restart classification. The Native transaction and execution product perform anchored verification, cancellation/commit linearization and exact typed outcomes. `CopyOperationOrchestrator` owns the production journal/`Pool` composition, including read-only reconciliation and exact reconciled Pool release. The application review/presentation adapter and mutation consumer remain open.

## Verified coverage

Current Debug evidence:

- `OperationPlan_UT`: 8 cases / 113 assertions;
- `OperationPlanner_UT`: 13 / 228;
- `VFSOperationPlanningProbes_UT`: 5 / 178;
- `OperationPlanCodec_UT`: 12 / 151;
- `ReviewedOperationFactory_UT`: 8 / 225;
- provider conditional result mapper: 4 / 237;
- provider conditional execution product: 9 / 188;
- earlier staged `NativeCreateCopy_UT` snapshot: 19 / 924;
- `OperationJournal_UT`: 27 / 592;
- Job lifecycle: 10 / 608;
- `Pool_UT`: 15 / 190;
- `CopyOperationOrchestrator_UT`: 13 / 558, including production construction at 3 / 138;
- full Debug, Release ASAN, and Release UBSAN `OperationsUT`: 165 / 4,468 in each configuration.

Coverage includes binding lifetime and semantic alias rejection, authoritative case identity, POSIX/SFTP classification seams, item/name/access/space/cancellation evidence, missing checker behavior, native recursive estimation, non-reentrant stat, partial directory-read failure, vanished entries, symlinks on both sides, special files, unsupported policies, and directory replacement.

The current-tree M0 run from 2026-08-01 passed the unsigned Debug application and all 10 seeded aggregate binaries: 897 cases / 132,011 assertions in the recorded run. Docker-backed seeded ASAN integration passed 163 / 89,392; hosted CI remains open.

## Remaining integration

The next M3 slices must:

1. compose the exact bound preflight into app-owned typed review;
2. configure callbacks while the execution product remains cold, before enqueue can start it;
3. present exact durable terminal, publication, sync, retry, reconcile, and Pool-release evidence;
4. wire one bounded `CopyAs::Perform` consumer with zero-enqueue proof for Blocked, stale, rebound, unpersisted, cancelled, batch, and unsupported-provider inputs;
5. add provider-owned bounded staging for cross-volume scope;
6. execute dedicated physical internal/external-volume fixtures.

Operation Center presentation, non-Copy preflight, and broader remote/archive execution identity remain later increments.
