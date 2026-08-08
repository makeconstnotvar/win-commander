# Q2-10 RH-1: `WriteAtomically` was atomic but not durable

> Status: defect found and fixed; see §Verification.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-10 ("atomic persistence").

## The defect

`nc::base::WriteAtomically` wrote to a `mkstemp` temporary and published it with `rename`, which makes the *replacement* atomic — a reader never sees a half-written file. But it was missing both durability barriers:

1. **No `fsync` on the file before `rename`.** The directory entry can reach stable storage while the data blocks have not. After an unclean shutdown that leaves a correctly named file holding garbage — which is worse than losing the write outright, because nothing downstream can tell it happened.
2. **No `fsync` on the parent directory after `rename`.** A rename is a directory mutation and is durable only once the directory is flushed. Without it a crash can roll the entry back and the file silently reverts to its previous contents, with the call having already reported success.

This is not theoretical for this codebase: `Config` and `FileOverwritesStorage` both write through this function, and `FileOverwritesStorage` exists to detect external modification. Losing its write silently is how the application would later overwrite a user's changes believing nothing had changed.

The repository already knew the correct ordering — `AGENTS.md` records `fsync(destination) → fsync(parent) → F_FULLFSYNC(destination)` for the reviewed copy path, and `ConditionalCopy.cpp` implements it. `WriteAtomically` simply predates it.

## The fix, and one deliberate asymmetry

`fsync(fd)` now runs before `close`, and its failure aborts the write: the temporary is removed and the error is reported, because publishing data that is not on disk is the failure this whole function exists to avoid.

The directory `fsync` after `rename` is deliberately **not** treated the same way. Its failure is ignored and the write is **not** rolled back:

> The rename has already succeeded, so the new contents are visible and correct to everything running now. Only durability across an unclean shutdown is in doubt, and undoing a completed publication would turn a durability question into certain data loss.

That asymmetry is the judgement in this fix. Treating both barriers identically would be tidier and wrong.

## What is deliberately not done

`F_FULLFSYNC` is **not** used here, unlike the reviewed copy path. `fsync` on macOS flushes to the drive, not necessarily through its cache; `F_FULLFSYNC` adds the full barrier at a real latency cost. The copy path pays that because it also proves the media supports it (internal, non-removable APFS — see `ConditionalCopy.cpp`), and `WriteAtomically` has no such volume knowledge: it is called on arbitrary paths, including network and removable volumes where `F_FULLFSYNC` may be acknowledged without stable persistence anyway. Paying the cost for a guarantee that cannot be verified here would buy latency, not durability. Config writes are frequent and small, which makes that trade worse still.

## Verification

- `xcodebuild -scheme BaseUT -configuration Debug build` — **BUILD SUCCEEDED**.
- Focused `BaseUT 'WriteAtomically*' --rng-seed 424242`: **5/5 cases, 48/48 assertions** — the three pre-existing cases plus two new: publishing twice leaves exactly the target and no `mkstemp` artefact (one hidden file per save would otherwise accumulate in the user's own directory), and a rejected write leaves the previous contents intact for both a relative and an empty path.
- Full `BaseUT --rng-seed 424242`: **80/80 cases, 70,576/70,576 assertions**.
- Full `ConfigUT --rng-seed 424242`: **38/38 cases, 76/76 assertions** — the consumer that writes through this function.
- No sanitizer run: the change adds two syscalls and an error branch, with no new allocation, ownership or concurrency.

### Coverage gap

**Nothing here proves durability.** These tests prove atomicity, cleanliness and error behaviour; whether the bytes survive a power cut can only be shown by cutting power, which is the hardware evidence `Development-Plan.md` §5.1 records as unsupported in the development cycle. What the fix does is make the code do what the durability contract requires, so that when such a fixture exists it has something correct to confirm.
