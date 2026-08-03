# ADR 0002: Cross-volume reviewed Copy requires isolated staging authority

> Status: accepted
> Date: 2026-08-03
> Scope: future Native-to-Native cross-volume reviewed create-only Copy

## Context

ADR 0001 deliberately supports only an exact same-volume APFS clone. A cross-volume Copy needs a complete staged payload before the destination name becomes visible, then a create-only publication bound to that exact staged object. `NativeCreateCopy` is a characterization capsule, not a production candidate: it records that POSIX validation of a named stage and `renameatx_np(..., RENAME_EXCL)` are separate namespace operations. A same-UID process can replace the stage name after inode validation and before rename.

Random names, a `0700` directory, `O_NOFOLLOW`, descriptor rechecks, `flock`, and `RENAME_EXCL` do not bind the source stage inode to the final publish step. The current journal has no descriptor- or inode-bound authority to delete a retained stage after restart. Reusing that capsule in the reviewed `CopyAs` path would weaken the clone transaction's mutation guarantee.

## Decision

The production reviewed transaction remains fail closed for different volumes. `ProviderConditionalCopyPathSupport::Supported` continues to mean only the existing same-volume clone scope. Different volumes keep the established legacy operation route until a separate provider-owned staging service is present.

The future first cross-volume scope must be one regular file, create-only, exact same `NativeHost`, and two distinct internal, local, writable APFS volumes. External, removable, ejectable, network, read-only, unknown-permission, and unavailable volumes remain unsupported.

That service must be a narrow signed helper with a distinct staging authority, protected roots on each participating volume, a bounded manifest/lease and descriptor-bound cleanup. Its transaction must:

1. validate the exact reviewed source and destination-parent descriptors without namespace mutation in Begin;
2. create a helper-owned stable source snapshot and a helper-owned destination stage;
3. copy permitted data and metadata into the stage, synchronize it, then revalidate source, destination parent, absent destination and staged identity;
4. perform the sole user-visible create-only publication through a helper-held atomic namespace operation;
5. no-follow reopen and verify destination identity/metadata, then order `fsync(destination) → fsync(parent) → F_FULLFSYNC(destination)`;
6. retain all orphan-stage cleanup authority in the helper and report only conservative `NotPublished`, `Published`, or `Unknown` results to the existing transaction/journal path.

No user-facing journal recovery action may delete a staging pathname.

## Consequences

- The generic provider transaction, mapper, execution product, orchestrator, exact run-receipt custody and durable outcome contracts can remain unchanged when the helper returns the existing tri-state result.
- A later capability result must distinguish the clone scope from a helper-available staged scope; it must not silently reinterpret today’s `Supported` value.
- The helper protocol, trust boundary, quota/retention policy, cancellation, helper-crash recovery and two-internal-volume physical evidence require a separate implementation and review.
- Until then, the existing legacy operation is the only cross-volume execution path. This preserves the stricter reviewed mutation boundary instead of treating a named temporary file as equivalent authority.

## Acceptance evidence for a future implementation

Prove helper unavailability causes zero enqueue; stage substitution by the client cannot affect publication; source/destination stale and destination-race cases fail closed; cancellation and pre-publish failures leave no visible destination; post-publish metadata/sync failures retain `Published`; helper crash returns conservative recovery evidence; helper-only bounded artifact cleanup survives restart; and the full production plan/review/journal/orchestrator path passes an opt-in two-internal-APFS-volume fixture plus final Release ASAN/UBSAN.
