# Feature: reviewed CopyAs physical-volume protocol

> Status: opt-in physical-volume, checkpoint and recovery profiles implemented; no physical run or hardware power-loss evidence recorded
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 14, 15, 31, and 32
> Execution tracker: M3 in `Docs/Development-Plan.md`

## Scope

The current production scope is one regular file copied create-only between two directories on the exact same `NativeHost` and one internal, local, writable APFS volume. The physical fixture proves the production `OperationPlan → bound preflight → review → journal → CopyOperationOrchestrator → Pool` path; it is not a substitute for the staged cross-volume design.

The fixture has two cases under `[reviewed-copy-as-physical]`:

- internal APFS: requires path eligibility `SameVolumeClone`, submits the production orchestrator, verifies distinct source/destination identities, exact bytes and mode, journal `Completed`, `Published`, confirmed filesystem sync, durable terminal delivery before Pool removal, and an empty custodian;
- external APFS: requires the exact volume decision `UnsupportedExternalMedia` and path eligibility `Unsupported`, verifies `ExecutionFactoryFailed` with `ConditionalCommitAuthorityUnavailable`, zero Pool admission, no output, unchanged source and a journalled `Failed` admission without item execution.

The external case proves the bounded transaction rejects that medium. It says nothing about the established legacy fallback path.

## Fixture safety and execution

The fixture does nothing when its roots are absent, reporting Catch2 `SKIP`. Set `WINCOMMANDER_OPERATIONS_IT_REQUIRE_VOLUMES=1` for a physical run: missing or invalid inputs then fail the test instead of appearing as a skipped profile.

Each root must be an existing canonical absolute directory, owned by the effective user, mode `0700` (or otherwise not group/world writable), below its volume mount root, and contain an ordinary non-linked marker named `.wincommander-operations-it-root` with exactly this content:

```text
wincommander-operations-it-root
```

The fixture opens every root component with `openat` and `O_NOFOLLOW`, derives volume identity from the opened descriptor, creates a random direct-child workspace, and removes only that workspace by descriptor after rechecking the marker and `(device,inode)` identity. It never creates a root, formats/ejects/detaches a volume, invokes a shell command, or cleans a retained workspace after a rebind/loss of authority.

Provision dedicated empty roots manually, preserving user ownership and the marker. Then build from the repository root and execute the profile explicitly:

```sh
xcodebuild -project Source/WinCommander/WinCommander.xcodeproj \
  -scheme OperationsIT -configuration Debug \
  -destination "platform=macOS,arch=$(uname -m)" \
  -derivedDataPath /private/tmp/wincommander-physical-operations-it \
  ONLY_ACTIVE_ARCH=YES COMPILER_INDEX_STORE_ENABLE=NO CODE_SIGNING_ALLOWED=NO build

WINCOMMANDER_OPERATIONS_IT_REQUIRE_VOLUMES=1 \
WINCOMMANDER_OPERATIONS_IT_INTERNAL_ROOT=/absolute/internal/test-root \
WINCOMMANDER_OPERATIONS_IT_EXTERNAL_ROOT=/Volumes/External/test-root \
  /private/tmp/wincommander-physical-operations-it/Build/Products/Debug/OperationsIT \
  "[reviewed-copy-as-physical]" --rng-seed 424242
```

Before recording an evidence run, capture the commit SHA, UTC timestamp, macOS/Xcode version, architecture, operator, root paths, `diskutil info -plist` for each root, mount paths, volume UUIDs, APFS/media flags, source/destination `stat` identities, hashes, metadata fixture profile, exact command and output. The normal aggregate integration profile may execute the binary without roots; these two cases skip and must not be reported as physical evidence.

## Checkpoint and retained-artifact profiles

The hidden `[.reviewed-copy-as-power-loss-checkpoint]` profile is a test-only instrumented execution of the same reviewed `CopyAs` route. It is enabled only by an explicit `WINCOMMANDER_OPERATIONS_IT_POWER_LOSS_PHASE` value:

- `before-publish` stops after every conditional-Copy seal has been revalidated and immediately before clone publication;
- `after-publish-before-full-fsync` stops after clone publication, destination metadata verification, destination `fsync` and parent `fsync`, immediately before the destination `F_FULLFSYNC` barrier.

At the selected phase the instrumented `ConditionalCopyIO` writes `power-loss-checkpoint.manifest` as a user-owned `0600` regular file through the workspace descriptor with `O_EXCL|O_NOFOLLOW`, fsyncs that file, then fsyncs the workspace directory. The manifest carries schema, run ID, workspace name, phase, capture time, journal filename and `(device,inode)` identity, journal-parent identity, and source/destination identity observations. The execution profile also takes an injected `NativeHost`, so the checkpoint belongs to the real provider transaction rather than an emulated copy path.

Setting `WINCOMMANDER_OPERATIONS_IT_POWER_LOSS_BLOCK=1` preserves the descriptor-anchored workspace after the manifest durability barrier and holds the worker for an operator-controlled interruption. The test reports the retained workspace path and phase. The ordinary profile mode exercises manifest creation before allowing the operation to finish, recording checkpoint-manifest setup evidence.

After reboot, run the hidden `[.reviewed-copy-as-power-loss-recovery]` profile with `WINCOMMANDER_OPERATIONS_IT_POWER_LOSS_RECOVERY_WORKSPACE` set to the exact retained workspace absolute path. The profile accepts only a canonical direct child of the configured descriptor-anchored internal root. It opens the workspace and manifest with `O_NOFOLLOW`, validates the manifest schema, ownership, mode, bounded size, journal identity and journal-parent identity, then calls `OperationJournalTesting::InspectPersistedReadOnly` before any journal `Open`. That snapshot must show the admitted checkpoint record as `Running` with an empty item-result set. Only then does ordinary `OperationJournal::Open` classify it as `Interrupted`; recovery authority is limited to artifact verification and journal-state classification, while the retained workspace remains intact.

## Hardware power-loss evidence

Hardware power loss remains a release gate outside these automated profiles. Unit fault injection, `kill -9`, shutdown, reboot and VM reset do not prove it. A valid run needs a disposable physical machine, the checkpoint profile, operator-controlled abrupt power removal, and the subsequent recovery profile.

Run both phases. Preserve the source checksum and identity; record destination existence, checksum, metadata and observed publication state without treating post-publication presence as durable success. The resulting evidence includes the durable manifest, pre-`Open` read-only journal snapshot, post-`Open` `Interrupted` snapshot, checkpoint phase, power-cycle timestamps, hardware/volume facts and recovery action `InspectDestination` where uncertainty remains.

The first hardware run and resulting evidence are still required before this protocol can close the M3 physical-volume/power-loss gate.
