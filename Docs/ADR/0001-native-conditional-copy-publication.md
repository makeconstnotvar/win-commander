# ADR 0001: Native conditional Copy publication

> Status: accepted
> Date: 2026-08-02
> Scope: first production provider transaction for reviewed create-only Copy

## Context

The reviewed Copy chain already binds a structural plan, concrete VFS hosts, source identity/version evidence, destination-parent identity/version evidence, and an absent exact destination. The provider transaction must turn that reviewed intent into one publication authority without exposing a second namespace-mutation path.

macOS supplies `fclonefileat` for an atomic, exclusive, descriptor-anchored clone on clone-capable volumes. The source object and destination parent can be retained by file descriptor and revalidated against reviewed evidence immediately before publication. Native filesystems do not expose a general syscall that compares a historical source version and publishes an arbitrary staged payload as one indivisible operation.

## Decision

The first Native provider transaction uses this invariant:

1. review produces a move-only, private-constructible authority containing immutable semantic claims;
2. `BeginConditionalCopyTransaction` consumes that authority, opens the source and destination parent without following symlinks, validates their reviewed identity/version evidence, verifies an absent direct-child destination, and retains both descriptors;
3. Begin performs no namespace mutation;
4. the provider accepts only regular-file, create-only Copy on the exact same internal, local, writable APFS volume with known permission, clone, attribute, xattr, and extended-security support;
5. Begin captures exact source and destination-parent identity seals plus supported ownership, mode, timestamps, BSD flags, ACL, and extended-attribute evidence, and rejects metadata that cannot be reproduced exactly;
6. Commit revalidates the anchored source, destination parent, volume, absence, and metadata policy immediately before calling `fclonefileat`;
7. `fclonefileat(..., CLONE_ACL)` is the sole publication step and creates the destination exclusively;
8. after publication, Commit opens the destination without following symlinks and verifies metadata parity plus destination name/descriptor identity;
9. durability barriers execute in the order `fsync(destination)`, `fsync(parent)`, `F_FULLFSYNC(destination)` with `EINTR` retry;
10. the transaction reports `Published`, `NotPublished`, or `Unknown` conservatively and caches its terminal result; metadata and filesystem-sync failures after publication retain `Published` with independent primary and sync evidence;
11. unsupported volumes, metadata, and scopes fail closed before publication.

The reviewed source version is an admission and immediate pre-publication freshness condition. The source descriptor supplies stable object identity, while the clone syscall supplies a coherent copy-on-write snapshot and atomic destination creation.

## Consequences

- The first production scope is Native-to-Native, exact same internal writable APFS volume, single regular-file, create-only Copy.
- Cross-volume Copy and filesystems without clone support remain on the established operation path until provider-owned staging is implemented.
- The transaction can be implemented without a public named staging object and without a replaceable temp-name window.
- Metadata and ownership behavior is sealed and verified explicitly rather than inferred from the clone syscall.
- Unit and APFS fixture evidence cover the ordered durability policy and typed failure outcomes. Dedicated physical internal/external-volume and hardware power-loss fixtures remain release evidence.
- Provider-result mapping, the transaction-owning execution product, private reviewed-factory construction, production `CopyOperationOrchestrator` integration, read-only reconciliation, and exact reconciled Pool release are implemented without widening this provider scope.
- Application-owned typed review, cold pre-enqueue callback configuration, exact durable terminal presentation, one bounded mutation consumer, and cross-volume staging remain separate gates.

## Acceptance evidence

The provider implementation proves authority single consumption, Begin without namespace mutation, descriptor/evidence revalidation, exact supported-volume policy, metadata parity, destination-race preservation, ordered barriers, exactly-once terminal behavior, and conservative publication evidence. Current Debug evidence is ProviderCapabilities 16 / 548, Native conditional Copy 15 / 312, combined 31 / 860, and full `VFSUT` 94 / 43,531. Explicit Release ASAN and UBSAN runs pass the two focused filters at 16 / 548 and 15 / 312.

The Operations composition evidence is provider-result mapper 4 / 237, execution product 9 / 188, reviewed factory 8 / 225, Job lifecycle 10 / 608, journal 27 / 592, and orchestrator 13 / 558 with its production path at 3 / 138. Full `OperationsUT` passes 165 / 4,468 in Debug, Release ASAN, and Release UBSAN. The APFS happy-path fixture may skip when its required local capability is unavailable; typed rejection of external media is unit evidence, not a physical external-drive fixture.
