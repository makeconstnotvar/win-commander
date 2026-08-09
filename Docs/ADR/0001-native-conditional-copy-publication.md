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
- Cross-volume Copy and filesystems without clone support remain on the established operation path until provider-owned staging is implemented. ADR 0002 records why a named in-process stage cannot join the reviewed transaction and defines the required isolated helper authority.
- The transaction can be implemented without a public named staging object and without a replaceable temp-name window.
- Metadata and ownership behavior is sealed and verified explicitly rather than inferred from the clone syscall.
- Unit and APFS fixture evidence cover the ordered durability policy and typed failure outcomes. An opt-in descriptor-anchored `OperationsIT` physical-volume fixture now covers the internal production path and external `UnsupportedExternalMedia` rejection; its actual physical run plus checkpoint-driven hardware power-loss evidence remain release evidence. The protocol is `Docs/Features/reviewed_copy_as_physical_volume_protocol.md`.
- Provider-result mapping, the transaction-owning execution product, private reviewed-factory construction, production `CopyOperationOrchestrator` integration, restricted cold submission hooks, owning durable terminal outcomes, read-only reconciliation, and exact reconciled Pool release are implemented without widening this provider scope.
- Application-owned typed bound-plan review, presenter/coordinator composition and one bounded `CopyAs` mutation consumer are connected. The production app-boundary seam proves zero enqueue and durable UI dispatch before non-success Pool release; physical-volume evidence and cross-volume staging remain separate gates.
- The destination-parent identity/version seal admits a second, narrower mode (Q2-8 slice 2): every item still opens, revalidates, and re-verifies the same identity and full ownership/permission surface (device, inode, birth time, mode, uid, gid, BSD flags, ACL, extended attributes) exactly, but an item other than the first to target a shared destination parent within one reviewed batch may find that parent's size, content timestamps and link count advanced rather than unchanged — exactly what its own batch's prior, authorized publication produces, link count included because APFS advances a directory's link count for a regular-file child and not only for a subdirectory. No revalidation is skipped or weakened; the accepted seal widens from equality to a documented, one-directional bound on content-derived fields only, decided per item at prepare time from the plan the review already covered, not from anything observed at runtime.

## Acceptance evidence

The provider implementation proves authority single consumption, Begin without namespace mutation, descriptor/evidence revalidation, exact supported-volume policy, metadata parity, destination-race preservation, ordered barriers, exactly-once terminal behavior, and conservative publication evidence. Current Debug evidence is ProviderCapabilities 16 / 549, Native conditional Copy 16 / 328, combined 32 / 877, and the latest full `VFSUT` run 95 / 43,566. Previously recorded explicit Release ASAN and UBSAN runs pass the earlier focused provider filters at 16 / 548 and 15 / 312; they are separate from the current Operations snapshot rerun below.

At the ADR evidence snapshot, Debug Operations composition was provider-result mapper 4 / 237, execution product 9 / 188, reviewed factory 8 / 225, Job lifecycle 10 / 608, journal 27 / 592, Pool 17 / 219, and orchestrator 15 / 758 with its production path at 3 / 138. The corresponding full `OperationsUT` snapshot passed 170 / 4,748 in Debug and explicitly instrumented Release ASAN/UBSAN; runtime linkage was confirmed and neither sanitizer run emitted diagnostics. Current model/coordinator evidence is tracked in `Development-Plan.md`. The APFS happy-path fixture may skip when its required local capability is unavailable; typed rejection of external media is unit evidence, not a physical external-drive fixture.

The submission-hook contract preserves the publication decision: transport lifecycle observations exclude generic Completion and Finish, while terminal presentation receives the owning exact journal outcome synchronously on the active caller thread. Failed, cancelled and reconciled `Interrupted` outcomes leave Pool through `ReleaseWithoutCompletion`; retry and reconciliation consume the terminal observer at most once before Pool removal or success reporting. Pool preallocates accepted terminal-finalization ownership so operation completion does not need to construct that authority.
