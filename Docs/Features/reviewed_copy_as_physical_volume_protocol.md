# Feature: reviewed CopyAs physical-volume protocol

> Status: opt-in `OperationsIT` fixture implemented; no physical run or hardware power-loss evidence recorded
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 14, 15, 31, and 32
> Execution tracker: M3 in `Docs/Development-Plan.md`

## Scope

The current production scope is one regular file copied create-only between two directories on the exact same `NativeHost` and one internal, local, writable APFS volume. The physical fixture proves the production `OperationPlan → bound preflight → review → journal → CopyOperationOrchestrator → Pool` path; it is not a substitute for the staged cross-volume design.

The fixture has two cases under `[reviewed-copy-as-physical]`:

- internal APFS: requires path eligibility `Supported`, submits the production orchestrator, verifies distinct source/destination identities, exact bytes and mode, journal `Completed`, `Published`, confirmed filesystem sync, durable terminal delivery before Pool removal, and an empty custodian;
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

## Hardware power-loss evidence

Hardware power loss remains a release gate outside the automated fixture. Unit fault injection, `kill -9`, shutdown, reboot and VM reset do not prove it. A valid run needs a disposable physical machine and an explicit test-only checkpoint harness that durably records a run id, journal identity and phase before waiting for the operator.

The required phases are `BeforePublish` and `AfterPublishBeforeFullFSync`. After a real abrupt power removal and boot, recovery first reopens the exact journal read-only and records its startup state before any retry, cleanup, enqueue or mutation. Preserve the source checksum and identity; record destination existence, checksum, metadata and observed publication state without treating post-publication presence as durable success. The journal must remain `Interrupted` without automatic resume, and the resulting evidence must include pre/post-reboot journal snapshots, checkpoint phase, power-cycle timestamps and recovery action `InspectDestination` where uncertainty remains.

The checkpoint harness and the first hardware run are still required before this protocol can close the M3 physical-volume/power-loss gate.
