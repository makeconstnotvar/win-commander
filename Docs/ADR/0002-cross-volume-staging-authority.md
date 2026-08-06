# ADR 0002: Cross-volume reviewed Copy requires isolated staging authority

> Status: accepted
> Date: 2026-08-03
> Scope: future Native-to-Native cross-volume reviewed create-only Copy

## Context

ADR 0001 deliberately supports only an exact same-volume APFS clone. A cross-volume Copy needs a complete staged payload before the destination name becomes visible, then a create-only publication bound to that exact staged object. `NativeCreateCopy` is a characterization capsule, not a production candidate: it records that POSIX validation of a named stage and `renameatx_np(..., RENAME_EXCL)` are separate namespace operations. A same-UID process can replace the stage name after inode validation and before rename.

Random names, a `0700` directory, `O_NOFOLLOW`, descriptor rechecks, `flock`, and `RENAME_EXCL` do not bind the source stage inode to the final publish step. The current journal has no descriptor- or inode-bound authority to delete a retained stage after restart. Reusing that capsule in the reviewed `CopyAs` path would weaken the clone transaction's mutation guarantee.

## Decision

The production reviewed transaction remains fail closed for different volumes. `SameVolumeClone` denotes only the existing same-volume clone scope; `CrossVolumeStaged` is selectable only with an available production staging authority. Without it, different volumes retain the established legacy operation route.

The future first cross-volume scope must be one regular file, create-only, exact same `NativeHost`, and two distinct internal, local, writable APFS volumes. External, removable, ejectable, network, read-only, unknown-permission, and unavailable volumes remain unsupported.

That service must be a narrow signed helper with a distinct staging authority, protected roots on each participating volume, a bounded manifest/lease and descriptor-bound cleanup. Its transaction must:

1. validate the exact reviewed source and destination-parent descriptors without namespace mutation in Begin;
2. create a helper-owned stable source snapshot and a helper-owned destination stage;
3. copy permitted data and metadata into the stage, synchronize it, then revalidate source, destination parent, absent destination and staged identity;
4. perform the sole user-visible create-only publication through a helper-held atomic namespace operation;
5. no-follow reopen and verify destination identity/metadata, then order `fsync(destination) → fsync(parent) → F_FULLFSYNC(destination)`;
6. retain all orphan-stage cleanup authority in the helper and report only conservative `NotPublished`, `Published`, or `Unknown` results to the existing transaction/journal path.

No user-facing journal recovery action may delete a staging pathname.

### Protocol boundary V1

`RoutedIO/CrossVolumeStagingProtocol` defines the typed, VFS-independent ABI before the helper implementation. Every request and reply carries protocol version 1 and a nonzero 128-bit correlation ID. A Begin claim carries complete scalar source and destination-parent seals plus one bounded byte destination component; its private XPC codec accepts only that grammar and carries exactly the two duplicated descriptor rights beside the value claim. The component accepts 1–255 bytes and represents an APFS name, while a helper-minted opaque 256-bit lease binds the Begin correlation to exactly one Commit or Abort request. Completion replies preserve the existing conservative `NotPublished`/`Published`/`Unknown` result and independent filesystem-sync evidence.

The public protocol contains neither user paths nor helper artifact names. The private codec and injectable VFS client duplicate the two reviewed FDs, reject malformed or mismatched replies, and retain a granted lease through exactly one terminal request or conservative `Unknown`. `CrossVolumeStagingHelperV1` authenticates the signed peer before strict V1 decode, then consumes the two owned FDs into private `ValidatedBegin` only after setting and checking `FD_CLOEXEC`, rejecting writable rights and exact-`fstat` revalidating every scalar source/destination-parent seal. Invalid claims, source drift, destination-parent drift and local helper failures receive typed Begin rejection.

For the current bounded lifecycle increment, a valid `ValidatedBegin` is admitted to the helper-private `LeaseStore` through `LeaseLifecycle`. Begin only retains that exact validated FD pair and mints a one-use opaque lease: it selects no root, creates no artifact and performs no namespace mutation. An exact Commit atomically consumes the lease and, until a staging executor exists, reports confirmed `NotPublished` with `HelperFailure`/`EOPNOTSUPP`; an exact Abort consumes it as `NotPublished`/`Aborted`. A malformed reply path or peer disconnect revokes the owner and closes every still-retained descriptor pair. `Granted` at this protocol boundary is deliberately not production staging availability: the production VFS client and signed transport remain absent, so `CrossVolumeStaged` is not selected and cross-volume Copy retains the legacy route.

The fixed-capacity `LeaseStore` accepts only move-only `ValidatedBegin`, uses `SecRandomCopyBytes`, binds one non-reusable correlation to one authenticated peer and retains the two validated descriptor rights until terminal take or peer revoke. The helper-private `ProtectedRootLedger` admits a root-owned `0700` directory only through a borrowed root FD, holds one process-local ledger owner per sealed root identity, and durably records a random artifact ID, root seal, correlation and role in a `0600` one-link manifest. Its read-only restart classifier recognizes a sealed manifest only when the complete root/header/role/artifact-ID claims and the artifact's full no-follow scalar seal match exactly. It retains sealed, absent, incomplete, malformed and mismatched states for a later helper-owned recovery decision; a reservation record becomes removable only after exact record revalidation, directory synchronization and proof that its canonical private artifact name is absent. The current layer has no artifact materialization, cleanup or destination namespace authority. NonMAS packaging owns the target dependency, `CodeSignOnCopy` embed and reciprocal `SMPrivilegedExecutables`/`SMAuthorizedClients` requirements; Unsigned has its own plist without a privileged-helper declaration and MAS has no V1 dependency. The target configuration requires Developer ID and hardened runtime. The available machine has zero valid code-signing identities, so a signed artifact and SMJobBless trust proof remain release evidence. A concrete signed helper transport must still provide a durable artifact materialization/seal writer, final descriptor/destination/stage revalidation plus cancellation at its publish barrier, helper-only cleanup and recovery. Production cross-volume selection remains closed until those parts exist.

## Consequences

- The generic provider transaction, mapper, execution product, orchestrator, exact run-receipt custody and durable outcome contracts can remain unchanged when the helper returns the existing tri-state result.
- The capability result distinguishes `SameVolumeClone`, `CrossVolumeStaged`, `Unsupported`, and `Unavailable`; it must not reinterpret the clone scope as staged authority.
- The helper protocol, trust boundary, quota/retention policy, cancellation, helper-crash recovery and two-internal-volume physical evidence require a separate implementation and review.
- Until then, the existing legacy operation is the only cross-volume execution path. This preserves the stricter reviewed mutation boundary instead of treating a named temporary file as equivalent authority.

## Acceptance evidence for a future implementation

Prove helper unavailability causes zero enqueue; stage substitution by the client cannot affect publication; source/destination stale and destination-race cases fail closed; cancellation and pre-publish failures leave no visible destination; post-publish metadata/sync failures retain `Published`; helper crash returns conservative recovery evidence; helper-only bounded artifact cleanup survives restart; and the full production plan/review/journal/orchestrator path passes an opt-in two-internal-APFS-volume fixture plus final Release ASAN/UBSAN.
