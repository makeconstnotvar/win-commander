# Q2-6 AR-1…AR-3: Creatable archive formats, creating them, and choosing one

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

---

# AR-2: the engine can now create what the model describes

AR-1 named four creatable formats. The compression engine could produce exactly one of them — `archive_write_set_format_zip`, hardcoded, with `.zip` appended to every filename it generated. The model described a choice nothing could act on.

## The model moved down

`ArchiveCreationFormat` was in the application layer, where a picker would use it. But the code that actually creates archives lives in `Operations`, which the application already depends on and which cannot depend back. Two copies of the table would have been the alternative, and the day they disagreed the picker would offer a format the engine could not produce — the exact failure the type was introduced to prevent. It is now `nc::ops::ArchiveCreationFormat`, with one owner.

## Container and compressor are separate choices

Which is what the tar family is about: the same tar container is what gets gzipped or bzipped, and it is also a valid archive on its own. `pax_restricted` is the container for all three — it stays plain ustar until an entry needs more than ustar can express, and only then writes extended headers, which is what lets a tarball carry the ownership and permission metadata the format is chosen for without becoming unreadable to plain `tar`.

Zip keeps `bytes_in_last_block = 1`; the tar family does not. Zip has no block structure to preserve, while tar does, and other tools warn about a truncated final block.

## A passphrase is refused, not dropped

Only zip can carry one. Producing a tarball anyway and quietly discarding the protection is the worst outcome available: the archive looks finished, and nothing tells the user that its contents are readable by anyone who gets the file. The job stops before writing anything, so there is not even a half-made archive to mistake for a protected one, and it reports the refusal as its own state — the request could not be honoured as asked, which is a different thing from a failure partway through the work.

## Verification

- `OperationsUT`, `OperationsIT`, `WinCommanderUT` and `WinCommander-Unsigned` — all **BUILD SUCCEEDED**.
- New `OperationsIT 'Operations::Compression Creates each format*'`: **4 formats, 36 assertions** — each archive is named with its own extension and then **read back**, which is the only proof that matters: a wrong container would still have produced a file with the right name. The nesting is checked explicitly rather than inferred from an entry count, because a lost directory structure is precisely the silent failure AR-1 describes.
- New `'Refuses a passphrase for a format that cannot carry one'`: **3 formats, 9 assertions** — stopped, reported as a rejected request, and no archive path produced.
- New `'Carries POSIX permissions through the formats that claim to'`: **12 assertions** — a `0700` script survives tar and tar.gz with its group and other bits still clear, which is what makes AR-1's `preserves_posix_metadata` an answerable claim rather than a comment.
- Full `OperationsIT 'Operations::Compression*'`: **17/17 cases, 284 assertions**, and again under **ASAN+UBSAN** and under **TSan** — the slice changes an operation that writes files, which is where that budget applies.
- Focused `OperationsUT 'nc::ops::ArchiveCreationFormat*'`: **6/6 cases, 34 assertions** (moved from `WinCommanderUT` unchanged). Full `OperationsUT`: **228/228, 5,965 assertions**. Full `WinCommanderUT`: **745/745, 11,441 assertions** — six fewer cases than before, exactly the ones that moved.

### Coverage gap at AR-2

**The user still cannot choose** — closed by AR-3 below.

---

# AR-3: choosing one, and the rule the choice creates

## The menu is built from the model, not listed in the nib

Four items in a nib would be a second copy of the table, and the day it drifted the picker would offer a format the engine cannot produce — the exact failure `ArchiveCreationFormat` exists to prevent. The pop-up ships empty and is filled from `SupportedArchiveCreationFormats()`, in the order the model gives.

## One choice creates a rule: only zip can carry a passphrase

That is now a fact on the format (`supports_encryption`), not knowledge scattered across surfaces. AR-2 made the compression job refuse an impossible combination; AR-3 makes the dialog prevent it, so the user is stopped where they are working rather than told no after pressing a button. Both read the same model, so they cannot reach different conclusions — and the job's refusal remains as the last line of defence for any caller that does not ask first.

`EvaluateArchiveCreationRequest` reports **`PasswordUnsupported` before `PasswordMissing`**. Typing a password would not help, and telling someone to enter one they cannot use is the more misleading of the two answers.

Switching to a format that cannot encrypt **withdraws** the request rather than leaving a ticked box that would produce an unprotected archive. What was typed stays in the field, so returning to a format that can encrypt does not ask for it again — but protection is not silently re-engaged either; asking for it is the user's call.

## What the nib test caught

Disabling the checkbox by assigning `enabled` on the control **did not stick**. A control whose `value` is bound has its `enabled` managed by the bindings machinery, and the assignment was overwritten the moment the bound value changed — which is the very next line, where protection is withdrawn. The result would have shipped as a checkbox that stayed clickable for tar, letting a user tick it and get an unprotected archive.

Nothing short of loading the real nib would have found it: the logic was right, the model was right, and every value the code set was right. `formatSupportsEncryption` is now a bound property, so AppKit owns the enabled state and there is nothing to overwrite.

## Verification

- `OperationsUT`, `OperationsIT` and `WinCommander-Unsigned` — all **BUILD SUCCEEDED** (the nib compiles, which is what validates the added row).
- New `OperationsIT 'Operations::CompressDialog*'`: **2/2 cases, 24 assertions** — the pop-up exists and is connected (a mistyped outlet would leave it nil, every menu call would quietly do nothing, and the picker would ship empty), its items match the model item for item in title and tag, and the password interaction holds in both directions.
- Focused `OperationsUT 'nc::ops::ArchiveCreationFormat*'`: **8/8 cases, 47 assertions** — the encryption fact for all four formats, every verdict including the two orderings that matter, and a stale password in a field nobody reads not blocking a submission.
- Full `OperationsUT`: **230/230, 5,978 assertions**. Full `OperationsIT 'Operations::Compress*'`: **19/19, 308 assertions**, and again under **ASAN+UBSAN**.

### Coverage gaps

- **The destination field still means a directory.** Typing `backup.tar.gz` there creates a directory path, not a format choice; `ArchiveCreationFormatForFilename` remains unused by any surface. Deciding whether that field should accept a filename is its own increment, and it interacts with the picker.
- Nothing yet checks the creatable set against the extraction whitelist, and archive operations do not go through Operation Center plans — both still open in Q2-6.
