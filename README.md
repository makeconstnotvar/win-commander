# Win Commander

![Win Commander — Commander mode](Docs/nc.png)

Win Commander is a native macOS file manager built on the Nimble Commander engine. It combines the established dual-pane Commander workflow with an Explorer-style interface over the same `Panel`, `VFS`, and `Operations` foundations.

The [ideal file manager specification](Docs/win_commander_ideal_file_manager_spec.md) is the product and architecture source of truth. The [development plan](Docs/Development-Plan.md) is the active tracker for priorities, dependencies, acceptance evidence, and remaining work.

## Current status

- Commander mode is the established implementation; Explorer mode is the active migration surface.
- Navigation and refresh publish request-scoped state through `PaneStore`. Eleven stable command IDs are defined, and nine production Registry definitions now cover Copy, Cut, Rename initiation, Open, hidden files, Back, Forward, Up, and user Refresh.
- `file.open` unifies the native Open menu/selector, ordinary-file Enter fallback, Shift-Return shortcut, and context-menu exact-item route. Enter continues to route folder/archive navigation and terminal execution. Accepted Open execution is a synchronous handoff to the existing `FileOpener`; application or remote-provider completion remains outside that Registry result.
- The safe-operation foundation includes immutable planning, bound VFS evidence, private-sealed single-use review authority, deterministic schema-v1 serialization, durable journaling, provider-minted conditional transactions, typed publication outcomes, and hardened `Job`/`Operation`/`Pool` lifecycle.
- `CopyOperationOrchestrator` now has a production constructor over the journal, `Pool`, and run-receipt custodian. It reaches the private `ReviewedOperationFactory` path, which returns a sealed move-only provider execution product; injected construction remains a test-only seam. Pre-enqueue persistence failure retains exact retry authority. Post-rename uncertainty requires storage-bound journal reopen, read-only reconciliation, and an explicit `ReleaseReconciled` handshake with the exact `Pool` residency. No application mutation entry point uses this boundary yet.

The first Native conditional provider scope is implemented for one create-only regular file on the exact same internal, local, writable APFS volume. It consumes typed reviewed authority, anchors source and destination-parent descriptors, seals and revalidates supported ownership, mode, timestamps, flags, ACLs and extended attributes, publishes exclusively through `fclonefileat(..., CLONE_ACL)`, verifies the destination, and orders destination, parent, and full-filesystem durability barriers. Published metadata and filesystem-sync failures remain explicit typed outcomes. The provider result now maps losslessly into the journal, and a transaction-backed operation owns commit/stop/destruction authority. The public compatibility `ReviewedOperationFactory::Create` still aborts and fails closed; only its private orchestrator-facing path can create the runnable product. Cross-volume Copy still requires provider-owned bounded staging, and existing application mutation entry points continue to use the established operations.

## Build and verify

Open `Source/WinCommander/WinCommander.xcodeproj` and use the `WinCommander-Unsigned` scheme for local development.

```sh
Scripts/verify_m0.sh
Scripts/build_unsigned_and_run.sh
Scripts/run_all_unit_tests.sh Debug
Scripts/run_all_integration_tests.sh
```

Current local evidence:

- current focused Debug journal, provider-result mapper, execution product, reviewed factory, Job and Copy orchestrator: 27 / 592, 4 / 237, 9 / 188, 8 / 225, 10 / 608 and 13 / 558; the production-orchestrator subset is 3 / 138;
- current Debug `VFSUT`: 94 / 43,531;
- current full `OperationsUT`: 165 / 4,468 in Debug, explicitly instrumented Release ASAN, and explicitly instrumented Release UBSAN; both sanitizer runtimes were confirmed and emitted no diagnostics;
- current ProviderCapabilities and Native conditional Copy: 16 / 548 and 15 / 312 in Debug, explicit Release ASAN, and explicit Release UBSAN; combined Debug selection 31 / 860;
- current-tree M0 gate: 897 / 132,011 across all ten aggregate unit-test binaries;
- seeded ASAN integration total: 163 / 89,392.

Focused `file.open` evidence is 24 cases / 267 assertions: command eligibility and execution 8 / 102, Registry and legacy shortcut binding 15 / 99, and production menu/Enter/context-menu/Shift-Return routing 1 / 66. The complete production Registry fixture passes 3 / 122; explicit Release ASAN passes the core command and production route filters at 9 / 168. The first hosted CI run remains pending.

See [Building.md](Docs/Building.md) and [Scripts/README.md](Scripts/README.md) for prerequisites, signing limits, schemes, and integration fixtures.

## Documentation

- [Canonical specification](Docs/win_commander_ideal_file_manager_spec.md)
- [Active development plan](Docs/Development-Plan.md)
- [Architecture audit](Docs/current_architecture_audit.md), [ADR 0001](Docs/ADR/0001-native-conditional-copy-publication.md), [refactor plan](Docs/refactor_plan.md), [gap matrix](Docs/feature_gap_matrix.md), and [risk register](Docs/implementation_risks.md)
- Command slices: [`file.open`](Docs/Features/file_open_command_registry_slice.md), [`file.rename`](Docs/Features/file_rename_command_registry_slice.md), [`view.toggleHiddenFiles`](Docs/Features/view_toggle_hidden_files_command_registry_slice.md), [navigation history](Docs/Features/navigation_history_command_registry_slice.md), and [Up/Refresh](Docs/Features/pane_navigation_up_refresh_command_registry.md)
- Operation foundations: [plan](Docs/Features/operation_plan_foundation.md), [Copy preflight](Docs/Features/copy_preflight_planner_foundation.md), [VFS evidence](Docs/Features/vfs_operation_planning_probes_foundation.md), [review/factory](Docs/Features/reviewed_copy_factory_foundation.md), [codec](Docs/Features/operation_plan_codec_foundation.md), [native execution](Docs/Features/native_create_copy_execution_foundation.md), [journal](Docs/Features/operation_journal_foundation.md), [conditional execution product](Docs/Features/provider_conditional_copy_execution_product.md), and [Copy orchestrator](Docs/Features/copy_operation_orchestrator_foundation.md)
- [Changelog](changelog.md), [user guide](Docs/Help.md), and [contribution guide](CONTRIBUTING.md)

## Origin and license

Win Commander derives from [Nimble Commander](https://github.com/mikekazakov/nimble-commander) by Michael Kazakov. Upstream copyright notices remain in the source. The project is licensed under [GPLv3](LICENSE.md).
