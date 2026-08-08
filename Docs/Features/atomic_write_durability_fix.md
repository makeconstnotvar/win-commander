# Q2-10 RH-1…RH-3: Atomic-write durability, localization, and an accessibility gap

> Status: defect found and fixed; see §Verification.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-10 ("atomic persistence", "локализация").

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

---

# RH-2: the localization catalogue

## What an audit of `Localizable.xcstrings` found

547 keys, two languages. Two distinct problems, neither of which any test would have caught:

**Four user-visible strings had no Russian translation** and would render in English inside a Russian interface. All four are consequential rather than incidental — they are the confirmations shown before opening a large file or a set of items, and the explanations that the application will first copy them to a temporary location. Exactly the text a user needs in their own language, because it precedes a decision.

**Five keys were dead.** `__BYTECOUNTFORMATTER_BYTE_POSTFIX`, `__BYTECOUNTFORMATTER_BYTES_WORD`, `__BYTECOUNTFORMATTER_SI_LETTERS_ARRAY`, `__CLASSICPRESENTATION_FOLDER_WORD` and `__CLASSICPRESENTATION_UP_WORD` are referenced from nowhere — not from `.mm`, `.cpp`, `.h`, `.xib` or `.storyboard`, and not assembled dynamically. They are remnants of an earlier localization scheme. Removed, following the Q1-10 precedent for confirmed-dead code.

Note what was *not* touched: `⏎` and `␛` remain untranslated on purpose — they are symbols, identical in every language, and "translating" them would be noise.

The apparent "201 keys missing English" is not a defect: in this format the key doubles as the source-language value, so an absent `en` entry is the normal case.

## A mistake worth recording

The first attempt at this edit was wrong, and the way it was wrong is instructive. Three of the four translations landed **inside the wrong keys** — as duplicate `"ru"` members of `"No"` and `"The window has %@ tabs…"`.

The cause: those four keys have no `localizations` block at all, so a naive "find the next `localizations` after this key" anchor walked past the key entirely and into its neighbour. And it went unnoticed at first because **JSON with duplicate keys still parses** — `json.load` silently keeps the last one. A validity check would have reported the file as fine while it was quietly overwriting real translations.

The fix was to locate each key's block by brace matching, edit only within those bounds, and then verify by re-parsing with a hook that *rejects* duplicate keys rather than resolving them. The final edit is 30 insertions and 58 deletions — targeted, not a reformat.

## Verification

- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED** (the target that compiles the catalogue).
- The catalogue parses with a duplicate-rejecting hook; exactly 4 keys changed and exactly the 5 dead keys removed, with nothing else altered — asserted mechanically rather than eyeballed.
- Every remaining key without a Russian entry is a symbol (`⏎`, `␛`), verified by listing them.

### Coverage gap

**No automated guard against regression.** Nothing prevents the next added key from shipping untranslated; catching that needs a check over the catalogue in the build or a script, which is worth adding but is its own increment. This slice fixed the state, not the process.

---

# RH-3: an accessibility gap in this session's own code

## What was wrong

The Pause/Resume buttons added to the Operation Center panel in OC-4 set only a title, target and action. Their sibling in the same panel — Cancel — receives a tooltip *and* `accessibilityHelp` from the shared `CommandPresentationAdapter`, because it is routed through the Registry. The pause control is not, so it inherited nothing and was the only control in that panel announcing no context to VoiceOver.

A title alone does give VoiceOver something to read, so the button was not silent. But "Pause op-3" says what the control is named, not what pressing it does or whether the effect can be undone — which is precisely what the help text on every neighbouring control provides. Being the one inconsistent control is its own defect: a user who has learned that these controls explain themselves gets no explanation from exactly the one that changes an operation's state.

Fixed by supplying both from the same string, with the reason recorded at the call site so the next control added outside the Registry path does not repeat it.

## Verification

- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**; full `WinCommanderUT --rng-seed 424242` passes **733/733 cases, 11,384/11,384 assertions**.
- Two new keys added to the catalogue in both languages, verified with the same duplicate-rejecting parse RH-2 introduced. Every key without a Russian entry is still only the two symbols.

### Coverage gap

This was found by reading the code I had just written, not by a check. Nothing enforces that a newly added control carries accessibility help — an audit pass over the existing Explorer surfaces, and some form of guard, both remain open under Q2-10's "полный accessibility".
