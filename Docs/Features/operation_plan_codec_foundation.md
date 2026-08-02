# Feature: versioned `OperationPlan` codec foundation

> Status: schema v1 codec implemented; durable storage is owned by `OperationJournal`
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 14.1 and 31
> Execution tracker: M3 in `Docs/Development-Plan.md`

## Purpose

`OperationPlanCodec` is the deterministic persistence boundary for the immutable structural plan. It converts a valid `OperationPlan` to canonical JSON and reconstructs the same value without granting review, admission, queue, provider, or execution authority.

## Contract

- Schema version is explicit and unknown versions fail closed.
- Opaque plan IDs, provider IDs, and filesystem paths use canonical Base64, preserving valid non-UTF-8 bytes losslessly.
- Operation types, destination kinds, conflict decisions, and conflict scopes use stable string tokens rather than compiler enum ordinals.
- `created_at` is encoded as signed Unix-epoch nanoseconds with representability checks.
- Decoding rejects unknown, missing, and duplicate members, invalid types/tokens/Base64, malformed JSON, invalid reconstructed plans, and configured resource-limit violations.
- Encoding the same plan produces byte-identical output.

The codec is pure. Filesystem I/O, durability, recovery, approval, provider binding, and execution remain outside this type.

## Limits

The schema accepts at most 100,000 sources, 1 MiB per opaque field, 64 MiB of decoded opaque data, and 96 MiB of JSON. These are parser safety limits rather than supported interactive batch-size targets.

## Verified coverage

The focused Debug suite passes 12 cases / 151 assertions. Coverage includes all structural operation types, deterministic output, non-UTF-8 round trips, signed timestamps, strict member/type/enum validation, schema mismatch, invalid Base64, reconstructed-plan validation, and resource limits.

Full Debug, explicitly instrumented Release ASAN, and explicitly instrumented Release UBSAN `OperationsUT` each pass 143 cases / 3,612 assertions without diagnostics.

## Remaining boundary

Schema migration policy is required before introducing schema v2. Production execution consumes journal-bound admission authority, not codec output or a plan ID alone.
