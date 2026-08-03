# Feature: anchored Native create-copy execution foundation

> Status: isolated staged capsule retained for characterization; authoritative clone transaction, execution product, private factory, and orchestrator integration implemented
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 14, 15, 31, and 32
> Execution tracker: M3 in `Docs/Development-Plan.md`

## Purpose

`NativeCreateCopy` proves one anchored staged `NativeHost → NativeHost` regular-file Copy without resolving display paths during execution. Tests supply an already-open source descriptor, an anchored destination-parent descriptor and exact identities. The separate Native clone-only transaction owns atomic publication, supported metadata parity, and ordered full-filesystem durability for its bounded internal-APFS scope. `ReviewedOperationFactory` keeps the staged capsule outside production; the transaction-backed execution product is now the implemented production construction boundary.

## Implemented contract

- Construction and execution are create-only and single-item. Unsupported providers, replacement, batches, special files, stale evidence, indirect access and cancellation fail closed before mutation.
- Source opens are no-follow, nonblocking and descriptor anchored. Identity includes device, inode, type, full mode, BSD flags, link count, size, mtime, ctime and birthtime.
- The copy buffer is allocated before staging. A named same-directory temporary file is created with a random 128-bit name, `O_EXCL | O_NOFOLLOW`, mode `0600` and link count one.
- Any inherited ACL is cleared and read back before data is written. The named staging item stays owner-only while it is discoverable.
- Data copy handles partial I/O and `EINTR`. Source size and exact identity are revalidated after data and metadata reads.
- Extended attributes are copied before publication. ACL, permission bits, atime, mtime, birthtime and the supported user flags `UF_NODUMP | UF_IMMUTABLE | UF_APPEND | UF_HIDDEN` are applied after publication, with flags last, then read back exactly. Set-ID/sticky bits and unsupported flags are rejected before staging because ownership parity is outside this slice.
- Descriptor/name seals and `st_nlink == 1` are checked at staging creation, before publish and after publish. Hostile hard-link and namespace rebind attempts fail closed.
- Cancellation is linearized against exclusive publication. The current Native implementation reports exact `NotPublished` or `Published`; the journal mapper also understands a future provider-level `Unknown` commit result.
- Published failures still attempt file and parent-directory `fsync`. Typed outcomes preserve primary and secondary errno, bytes, publication state, filesystem-sync evidence and recovery guidance. The declared policy is `FileSystemSyncOnly`.
- Cleanup never unlinks a rebound pathname. A retained temporary artifact is reported explicitly and carries no executable recovery action because the journal lacks descriptor/name/inode authority for safe removal.

The common `Job`/`Operation`/`Pool` lifecycle retains the owning operation through callbacks. `Pool` keeps terminal operations in `Finalizing` until their durable finalizer returns `Release`; failed finalization can be retried without exposing removal early.

## Production boundary

The isolated capsule remains a characterization and fault-injection foundation with a declared `FileSystemSyncOnly` promise and a named staging artifact. The production Native transaction is the authoritative same-volume path: it consumes unforgeable reviewed authority, applies the internal-writable-APFS capability policy, owns exact metadata seals, publishes only through `fclonefileat(..., CLONE_ACL)`, verifies the destination, and orders `fsync(destination) → fsync(parent) → F_FULLFSYNC(destination)`.

The provider commit result is now losslessly mapped into journal evidence, owned by a typed operation product, constructed through the private reviewed factory, and submitted by the production `CopyOperationOrchestrator`. Restricted cold hooks, owning exact durable-terminal delivery, preallocated Pool finalization and `ReleaseWithoutCompletion` are implemented. The bounded `CopyAs` application seam composes typed review and dispatches the owning durable outcome before non-success Pool removal. Cross-volume reviewed Copy stays fail closed: ADR 0002 requires a separate helper-owned staging and recovery authority. Replacement, directories, symlinks, batches and remote providers remain separate later slices.

## Verified coverage

- Native create-copy: 19 Debug cases / 924 assertions.
- This 19 / 924 staged-capsule total is an earlier focused snapshot retained for that characterization boundary.
- Provider conditional result mapper: 4 / 237; execution product: 9 / 188.
- Reviewed factory: 8 / 225; orchestrator: 17 / 806, including production construction at 3 / 138 and receipt-aware no-re-admission.
- Job lifecycle and worker-launch hardening: 10 / 608; journal: 27 / 592.
- Pool lifecycle/finalization: 17 / 219.
- Historical foundation snapshot: Debug, Release ASAN and Release UBSAN `OperationsUT` passed 170 / 4,748 in each configuration, with sanitizer runtimes confirmed and no diagnostics; the current coordinator/control subset separately passes Release ASAN and UBSAN at 28 / 999 without diagnostics.
- Current M0: unsigned Debug app plus 10 aggregate binaries, 897 cases / 132,011 assertions in the recorded seeded run.
- Docker-backed Debug ASAN integration: 163 cases / 89,392 assertions across Term, Operations, VFS and VFSIcon.

## Next slice

Run the implemented opt-in physical-volume fixture and record its hardware power-loss companion protocol for the bounded reviewed `CopyAs::Perform` consumer; `Docs/Features/reviewed_copy_as_physical_volume_protocol.md` defines the roots, evidence and remaining checkpoint harness. Cross-volume support remains a separate provider-owned bounded-staging slice.
