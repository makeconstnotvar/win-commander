# Q2-6 AR-1: Creatable archive formats

> Status: implemented and tested — see §Verification. Model increment: no user-visible surface yet.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-6.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §21 (archives), Q2-6 "Create/extract с планом, прогрессом и typed result".

## The gap this closes

Extraction support already existed — `IsExtensionInArchivesWhitelist`, consulted from `Actions/ExtractArchive.mm`. **Creation** support was not modelled anywhere.

Those are genuinely different sets. RAR extracts but cannot be created, because its compressor is proprietary; the same holds for several others. A surface that reasoned about "supported archive formats" as one set would offer a Create menu entry that fails at execution — the user picks a format, names a file, and only then learns it was never possible.

So this models the creatable set explicitly, and names it `ArchiveCreationFormat` rather than `ArchiveFormat`, so the asymmetry is visible at every call site.

## Longest match wins, and why it matters

`backup.tar.gz` must resolve to `TarGzip`, never to a bare gzip stream. The table is ordered longest-extension-first so resolution is correct by construction rather than by a separate length comparison bolted on afterwards.

The failure this prevents is quiet rather than loud: creating `backup.tar.gz` as plain gzip produces a file *named* like a tarball that is not one. It opens fine, yields a single unnamed blob, and the directory structure the user believed they had archived is silently absent — discovered, typically, when they need it back.

A dotfile is not an archive: `.zip` is a file called `.zip`, so the extension must follow a dot that is not the first character.

## The trade-off the model states

tar-based formats carry full POSIX ownership and permissions through a round trip; zip does not. That difference is the *reason* to offer more than one format, so the model records it rather than leaving every surface to fold it into its own tooltip. Plain `tar` is also marked as not compressing, which is what makes it the right pick for an already-compressed payload.

## Verification

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::ArchiveCreationFormat*' --rng-seed 424242`: **6/6 cases, 34/34 assertions** — compound extensions resolving whole; case-insensitivity; dotfiles refused while a leading dot elsewhere is fine; names that only look like archives refused, including `rar` and `7z` which are extractable but not creatable; a table invariant that every format is described exactly once, has a unique extension, and round-trips through its own extension; and the metadata/compression trade-off.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **733/733 cases, 11,384/11,384 assertions**.
- No sanitizer run: a pure table lookup with no ownership, concurrency or engine involvement.

### Coverage gaps

- **Nothing consumes this yet.** The Compress action still decides its own format; pointing it here, and building the Create picker from `SupportedArchiveCreationFormats()`, is the next increment.
- The creatable set is not cross-checked against the extraction whitelist, because that whitelist lives inside an action rather than in a shared contract. Making every creatable format provably extractable is worth doing when that whitelist moves somewhere both can see.
- 7z is deliberately absent: the engine's current compression path does not produce it, and listing a format the engine cannot write is exactly the failure this slice exists to prevent.
