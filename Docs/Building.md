# Building Win Commander
This guide outlines the steps to build Win Commander from source. Before changing the project, read [`AGENTS.md`](../AGENTS.md), the [canonical product specification](win_commander_ideal_file_manager_spec.md), and the [development plan](Development-Plan.md).

## Getting the code
Clone this repository with `git clone`.  
To minimize internet bandwidth, you can opt to fetch only the latest version with `git clone --depth=1`.

## Compiling the Project
After obtaining the source code, you'll need Xcode 26.5 to open and compile the project. From the repository root, open the project with:

`open Source/WinCommander/WinCommander.xcodeproj`

![](schema.png)

Select the `WinCommander-Unsigned` scheme in Xcode. Use `Cmd+B` to build and `Cmd+R` to run the project under Xcode's debugger.

## Exploring the Source Code
Win Commander has a medium-sized codebase (~150KSloC) written in C++, Objective-C++ and Swift. The source tree includes the main application and 11 library projects:
  * Base: Foundational, general-purpose tools.
  * Config: Configuration management.
  * CUI: Shared UI components.
  * Operations: File operation suite built on the VFS layer.
  * Panel: File panel components.
  * RoutedIO: Admin Mode functionality, including the privileged helper and its client interface.
  * Term: Integrated terminal emulator.
  * Utility: System-specific utilities.
  * VFS: Virtual File Systems, providing a generic interface along with various implementations.
  * VFSIcon: Generates icons and thumbnails for VFS entries.
  * Viewer: Integrated file viewer component.

## Testing
Win Commander employs two testing strategies: unit tests (`_UT` suffix) for individual components, and integration tests (`_IT` suffix) for checking how those components interact. Each type of test is easily identifiable by its unique filename suffix and corresponding build target. For example, `Term` represents the library, `TermUT` the unit tests for this library, and `TermIT` the integration tests.  
Unit tests are quick and standalone, not requiring any external setups. In contrast, integration tests might need specific conditions, like running Docker VMs (detailed in `Source/VFS/tests/data/docker/[start|stop].sh`), to properly execute.  

`IntegrationTests` also builds `WinCommanderIT`, the Docker-only application boundary target. Its FTP/SFTP/WebDAV cases drive a real `PanelController` through remote navigation and forced user refresh after a mutation from a distinct shadow host. Stop/restart proves the typed network path; `docker pause`/`unpause` keeps the listener reachable but withholds replies, proving the provider deadline, preserved listing/generation, lossless raw timeout and `TimeoutError`, then a fresh listing through the same controller and host. FTP bounds listing requests, WebDAV bounds only blocking control requests, and SFTP bounds connect/session work; these deadlines do not cap bulk transfers. The focused suite passes 9 cases / 376 assertions and remains deliberately excluded from `UnitTests` and M0.

### Physical Conditional Copy profile

`OperationsIT` contains an opt-in `[reviewed-copy-as-physical]` profile for the bounded reviewed `CopyAs` production path. It skips unless the dedicated marker roots are supplied; set `WINCOMMANDER_OPERATIONS_IT_REQUIRE_VOLUMES=1` so absent or invalid roots fail the physical job. The internal publication case requires an internal local writable APFS child root; the external-rejection case additionally requires an external/removable/ejectable APFS child root. Do not use a home directory, volume mount root, user data, disk image, or a non-APFS external root.

The same target also has hidden opt-in hardware-checkpoint profiles: `[.reviewed-copy-as-power-loss-checkpoint]` and `[.reviewed-copy-as-power-loss-recovery]`. The checkpoint profile requires the internal root and an explicit `WINCOMMANDER_OPERATIONS_IT_POWER_LOSS_PHASE` of `before-publish` or `after-publish-before-full-fsync`. Without `WINCOMMANDER_OPERATIONS_IT_POWER_LOSS_BLOCK=1`, it proves only that a durable manifest can be written and the operation then completes; with it set, the worker waits at the selected checkpoint for a human-operated abrupt power removal. After boot, pass the retained workspace through `WINCOMMANDER_OPERATIONS_IT_POWER_LOSS_RECOVERY_WORKSPACE` to the recovery profile; it reads the exact journal before normal startup mutates `Running` to `Interrupted`.

```sh
WINCOMMANDER_OPERATIONS_IT_REQUIRE_VOLUMES=1 \
WINCOMMANDER_OPERATIONS_IT_INTERNAL_ROOT=/absolute/internal/test-root \
WINCOMMANDER_OPERATIONS_IT_POWER_LOSS_PHASE=before-publish \
WINCOMMANDER_OPERATIONS_IT_POWER_LOSS_BLOCK=1 \
  /private/tmp/wincommander-physical-operations-it/Build/Products/Debug/OperationsIT \
  "[.reviewed-copy-as-power-loss-checkpoint]" --rng-seed 424242

WINCOMMANDER_OPERATIONS_IT_REQUIRE_VOLUMES=1 \
WINCOMMANDER_OPERATIONS_IT_INTERNAL_ROOT=/absolute/internal/test-root \
WINCOMMANDER_OPERATIONS_IT_POWER_LOSS_RECOVERY_WORKSPACE=/absolute/internal/test-root/conditional-copy-it-retained \
  /private/tmp/wincommander-physical-operations-it/Build/Products/Debug/OperationsIT \
  "[.reviewed-copy-as-power-loss-recovery]" --rng-seed 424242
```

These profiles are readiness tooling, not hardware evidence. A valid power-loss record needs the real power cycle for both phases, pre/post-reboot journal snapshots and observed source/destination identities and metadata. The exact root constraints and evidence protocol are in `Docs/Features/reviewed_copy_as_physical_volume_protocol.md`.

Run the repository suites from the project root:

```sh
Scripts/verify_m0.sh
Scripts/run_all_unit_tests.sh Debug
Scripts/run_all_integration_tests.sh
```

`verify_m0.sh` and the unit-test runner require only `xcodebuild`. Integration tests additionally require a running Docker daemon and `nc`; `xcpretty` is optional, and the runner owns provider fixture startup, readiness checks, and cleanup. Record the exact commands and results in [Development-Plan.md](Development-Plan.md) when closing a milestone.

## Limitations
Unsigned local builds do not contain distribution signing identities, notarization credentials, or release secrets. Consequently, some features are restricted:
  * Privileged Helper: requires proper signing to be installed and function correctly.

Unsigned tests prove deterministic permission policy only. Before a release that relies on sandboxed directory access, run the signed MAS target with clean bookmarks: obtain user consent through the actual panel, reopen the granted child path, verify a sibling with the same string prefix remains inaccessible, and verify the user-facing denial/recovery path. Run Full Disk Access/TCC checks separately with the release signing identity and an operator-controlled protected location; record the identity, OS profile, command and observed result in `Docs/Development-Plan.md`.
  
## Implementation Notes
  * [Syntax Highlighting](SyntaxHighlighting.md)
  * [Creating Image Templates from SF Symbols](ImageTemplatesFromSFSymbols.md)
  * [Unity Builds](https://kazakov.life/2025/12/12/win-commander-and-build-times/)
