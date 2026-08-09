# Q2-7 CL-1/GL-1/NW-1…NW-3: Cloud sync state, badge rule, Gallery eligibility, network volume state, the mount table and the probe

> Status: implemented and tested — see §Verification. Model increment: no user-visible surface yet.
> Execution tracker: [`Development-Plan.md`](../Development-Plan.md) row Q2-7.
> Canonical requirements: [`win_commander_ideal_file_manager_spec.md`](../win_commander_ideal_file_manager_spec.md) §11 (badge vocabulary: `CloudOnly`, `Downloading`, `Uploading`), §23 (Remote / Cloud), item attributes `isCloudOnly` / `isAvailableOffline`. The plan's own wording for Q2-7 is the operative constraint: *badges синхронизации только там, где они уместны*.

## Scope

CL-1 is the classification and the badge rule. **GL-1** then uses it for the one Gallery decision that has consequences.

**NW-1** covers Q2-7's third item, network volume states.

Not here: the Gallery view itself, or the provider adapters that produce these facts.

## The rule the plan actually asks for

`ShouldBadgeCloudSyncState` returns false for `Synced`, and that is the whole point of the slice.

Inside a cloud folder, synced is what everything is *supposed* to be. Badging it decorates every row identically, which conveys no information at all, and it makes the handful of rows that genuinely differ harder to find — the opposite of what a badge is for. So "only where they are appropriate" is implemented as a rule rather than left as a preference each surface interprets: a badge appears only for a state that differs from what the surrounding rows are expected to be.

`NotCloud` likewise gets nothing, for the same reason at a larger scale.

## Classification order

Facts are booleans a provider can actually answer, not a provider-specific enum — every service words its states differently, and that mapping belongs at the adapter rather than in the shared model.

Ordering is by what the user most needs told:

1. **Outside a cloud container, nothing else counts.** A stray `downloading` flag from a confused adapter must not turn an ordinary local file into a cloud one.
2. **A conflict outranks everything.** It is the only state where doing nothing loses data.
3. **A deliberate exclusion outranks transfer flags.** An excluded item is not waiting for anything, and a stale flag must not make it look like sync is still coming.
4. **Transfers outrank placeholder status.** A downloading placeholder is on its way; a stalled one is not. Reporting both as `CloudOnly` would hide exactly the difference someone about to open the file cares about.
5. Download outranks upload when an adapter reports both, because the missing bytes are the more consequential half.

## GL-1: what a Gallery row may show

`ClassifyGalleryItem` answers one question per item — thumbnail, placeholder, plain icon, or folder — and exists for a single judgement:

**A cloud-only media file is not thumbnailable.** Asking for its thumbnail makes the file manager fetch the bytes: potentially gigabytes, potentially on a metered link, *because the user switched view mode*. Switching a view is not consent to a transfer. So such items get a placeholder row and downloading stays an explicit action.

That is why GL-1 lives beside CL-1 rather than in a view layer — it needs `CloudSyncState`, and putting the rule where the state is keeps a future Gallery implementation from having to rediscover it.

Two smaller calls:

- **A folder is never a thumbnail of itself**, whatever it is named. A directory called `holiday.jpg` is still a directory, and the extension check runs after the directory check for exactly that reason.
- **An unrecognized extension degrades to an icon.** The media list is deliberately conservative: a wrong guess renders an empty frame, which reads as a *broken file*, while an icon simply looks correct. Being wrong in the safe direction costs a thumbnail; being wrong in the other direction makes intact files look damaged.

## NW-1: network volume state

The rule this exists for: **an unresponsive network mount must not be touched from the drawing thread.**

A `stat()` under a dead NFS or SMB mount does not fail — it *blocks* until the kernel's own timeout, measured in tens of seconds. Doing that while drawing is exactly how a file manager becomes the beachball, and it is why the state has to be known *before* the access rather than discovered by making it. `MayTouchSynchronously` admits only `Local` and `Responsive`.

Two states that both mean "broken" are kept apart because they need opposite handling:

- **`Unresponsive`** blocks. It must go off the main thread, but it must *not* be refused up front — the server may simply be slow, and refusing would make a recoverable stall look permanent.
- **`Stale`** answers immediately and fails. It should be refused before attempting, because an optimistic attempt fails per item — potentially thousands of times, each with its own error. One refusal naming the volume is more useful than a thousand naming files.

`Unresponsive` also outranks a rejected export: if the server is not answering, "the export is gone" is a conclusion that cannot have been reached, and reading it as `Stale` would send the optimistic fast-failing path into a mount that actually blocks.

## Verification

- `xcodebuild -scheme WinCommanderUT -configuration Debug build` — **BUILD SUCCEEDED**.
- `xcodebuild -scheme WinCommander-Unsigned -configuration Debug build` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::NetworkVolumeState*' --rng-seed 424242`: **6/6 cases, 19/19 assertions** — a local volume never read as a network state whatever flags it carries; unresponsive outranking a rejected export; the blocking mount and the fast-failing one distinguished and given opposite handling; unmounted reported before anything else; the drawing thread kept away from every state that can block; and a healthy mount.
- Focused `WinCommanderUT 'nc::core::GalleryEligibility*' --rng-seed 424242`: **6/6 cases, 24/24 assertions** — case-insensitive extension matching; unknown, empty and over-long extensions degrading to an icon; a directory named like an image still a folder; a cloud-only media file held to a placeholder while all six other cloud states thumbnail; a cloud-only non-media file still an icon.
- Focused `WinCommanderUT 'nc::core::CloudSyncState*' --rng-seed 424242`: **7/7 cases, 17/17 assertions** — every other fact ignored outside a container; conflict outranking a maximally-set fact struct; exclusion beating a stale transfer flag; an arriving placeholder distinguished from a stalled one; upload, and download winning over it; a quiet present item; and the badge rule for all seven states.
- Full unfiltered `WinCommanderUT --rng-seed 424242`: **727/727 cases, 11,350/11,350 assertions**.
- No sanitizer run: a pure `noexcept` classifier over a value struct.

### Coverage gaps

- **Nothing produces `CloudItemFacts` yet.** The macOS side (`NSURLUbiquitousItemDownloadingStatusKey` and friends for iCloud, and per-provider equivalents) is the next increment; this slice deliberately fixes the vocabulary and the ordering first, so each adapter has one place to map onto rather than inventing its own.
- The Gallery view itself is untouched; GL-1 fixes the eligibility rule, not the rendering.
- **Nothing produces `NetworkVolumeFacts` yet.** Reading the mount table and running a non-blocking probe are the next increment; NW-1 fixes what the answers mean.

---

# NW-2: where the network-volume facts come from

NW-1 decided what a network volume's state means and, crucially, that an unresponsive one **must not be touched from the drawing thread**. Nothing produced those facts, so the rule had no input.

`MountTable` is that producer: it reads the mount table and places a path on a volume, without touching the path or the volume.

## The read must not wait

`getmntinfo` is asked **not** to refresh each filesystem's statistics. Requesting fresh statistics makes the call wait on every mounted filesystem in turn, and a network mount whose server has gone away will not answer — so the very call meant to *discover* unresponsive volumes would hang on one. That is the whole failure this model exists to prevent, arriving through the back door.

## Network-ness comes from the mount flag, not a list of names

Matching `smbfs`, `nfs`, `afpfs` and friends by name would go stale the moment a new network filesystem appears — and the failure is silent and in the worst direction: an unrecognised network volume would be reported as local and then probed synchronously while drawing. The kernel's own "not local" flag does not go stale.

## Containment is by path component, never by string prefix

`/Volumes/data` does not contain `/Volumes/database`. A prefix match would attribute one volume's state to another — reporting a live local disk as an unresponsive network mount, or, worse, the reverse. The innermost containing mount point wins, so a volume mounted inside another owns the paths beneath it, and a mount point belongs to its own volume rather than to its parent.

## A path that cannot be placed is reported as such

This one changed shape while being tested. The first version returned facts saying "not a network mount, not mounted" — and `ClassifyNetworkVolume` reads that as **`Local`**, which means *safe to touch on the drawing thread*, for exactly the path we could not account for. The test caught it immediately.

The fix was not to revise NW-1's precedence, which is deliberate and pinned by its own test. Neither available answer is honest here: `Local` invites the synchronous touch, and `Unmounted` refuses operations up front — which would refuse **everything** on a machine where the mount table could not be read at all. So the absence is reported, and the caller, which knows which of those risks applies to it, decides.

A local volume is also reported as answering by construction, whatever probe result is passed alongside it: carrying a stale probe into a local volume would report a working disk as unresponsive.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::MountTable*'`: **8/8 cases, 58 assertions** — the `/Volumes/data` versus `/Volumes/database` collision in both directions; innermost-wins including a mount point belonging to its own volume; the root-volume fallback and a path that cannot be placed; normalization; a local volume answering by construction despite a contrary probe; a network volume classifying as responsive, unresponsive and stale from the probe it is given, with the unresponsive one asserted unsafe to touch; the unplaceable path reported rather than invented; and one case against the **real** mount table, finding the root volume and placing `/` on it.
- Full `WinCommanderUT --rng-seed 424242`: **789/789 cases, 11,698 assertions**.

### Coverage gap at NW-2

**The probe itself does not exist** — closed by NW-3 below.

---

# NW-3: asking a volume whether it is still there

NW-2 supplies everything the mount table knows. Whether the server still *answers* is the one fact that cannot be had without touching it — and touching it is the thing this whole model exists to keep off the drawing thread.

## The wait has a deadline, because the call cannot

There is no timeout on `statfs`. Under a dead network mount the kernel holds it for tens of seconds, and nothing can interrupt it. The only way to have a deadline at all is to stop *waiting* for it: the call runs on a thread of its own and is abandoned when the budget is spent.

The abandoned thread owns everything it touches — a heap-allocated block shared with it — so its eventual return writes somewhere valid and unread, whatever else has been destroyed by then.

**A late answer is not an answer.** Once the budget is spent the volume is reported unresponsive, and a reply arriving afterwards does not retract that. Something that takes half a minute to respond is precisely what must not go on the drawing thread, whatever it eventually says.

## Silence and refusal are different answers

A server that replies "no such export" has answered — the mount is `Stale`, which fails fast and can be refused up front. A server that says nothing is `Unresponsive`, which blocks. The probe distinguishes them because NW-1 treats them differently, and collapsing them would send the optimistic fast-failing path into a mount that hangs.

## The answer is remembered, and it expires

Probing to draw a row would be the synchronous touch being avoided, so `Known` never blocks and never probes — that is the call the drawing thread may make. But a remembered answer expires: a `Responsive` from a minute ago would hide a mount that has died since, and hiding it is how the synchronous touch happens anyway. An aged-out answer is withheld rather than repeated, which turns into "needs a refresh" instead of a stale reassurance.

**The prober runs outside the lock.** It is the part that can take tens of seconds, and holding the lock across it would block every drawing-thread read for exactly that long — reintroducing the stall this class exists to prevent, one indirection further away. A test holds a probe inside the prober and asserts reads still answer while it is stuck.

A slower probe that started earlier does not overwrite a fresher answer: it is older news whatever order the two land in.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- Focused `WinCommanderUT 'nc::core::NetworkVolumeProbe*'`: **7/7 cases, 42 assertions** — an answer served from memory without asking again, and asking what is known never probing; an aged-out answer withheld and then refreshed; mount points kept apart and one forgotten; an out-of-order refresh not overwriting fresher news; **reads answering while a probe is stuck inside the prober**; the real prober against a path that answers, against a budget nothing can meet, and against a path that is not a mount point — checking that refusal and silence come out different; and the result feeding through to `Unresponsive`, asserted unsafe to touch.
- The same suite plus `RemoteConnectionRegistry` under **TSan**: **15/15 cases, 93 assertions** — this slice detaches a thread and shares state with it.
- Full `WinCommanderUT --rng-seed 424242`: **796/796 cases, 11,740 assertions**.
- The TSan build again needed the frame-size warning relaxed for two unrelated tests; see the note in `git_status_foundation.md`.

### Coverage gap

**No scheduler and no caller.** Something has to notice which volumes a listing touches, refresh the stale ones off the drawing thread, and redraw when an answer changes. The cloud-facts adapter and the Gallery view also remain.

---

# GL-2: what a Gallery actually shows

GL-1 decided what a single Gallery row may show. What was missing was the view's own question: given a folder, which rows exist at all, in what order, and what does it say when there are none.

## Non-media is left out, not shown as an icon

Gallery is a way to look at pictures. A folder of source files rendered as a grid of identical document icons is a *worse* view of them than the list the user came from — and it hides the photographs among them, which is the one thing the mode exists to make easy.

## Folders stay, and come first

Dropping them would strand the user in a leaf directory with no way out but switching view modes back. A Gallery that cannot be navigated is a dead end.

`..` is not a row. It is navigation, and counting it as content would make an empty folder look like it holds something.

## A cloud-only photo keeps its place

Leaving it out would make the Gallery quietly disagree with the folder about what is in it. Downloading it to find out is precisely what GL-1 refuses to do behind the user's back — switching a view mode is not consent to a transfer — so it appears as a placeholder and stays counted.

## Empty and "nothing to look at" are different answers

A folder with nothing in it, and a folder full of source files, both show an empty Gallery — but they send the user to different places, so they are told apart rather than collapsed into one blank view.

Within each group the order is the listing's own, so whatever sort the user chose still decides what comes first, and each row carries the index it came from so a selection survives the regrouping.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- New `WinCommanderUT 'nc::core::BuildGalleryContents*'`: **6/6 cases, 27 assertions** — folders first then media, each in listing order, with indices mapping back; non-media dropped entirely; a folder kept where nothing else survives; a cloud-only photo held as a placeholder and counted; empty told apart from nothing-to-show, including a folder holding only `..`; and `..` never counted as content.
- Full `WinCommanderUT --rng-seed 424242`: **854/854 cases, 11,971 assertions**.

### Coverage gap at GL-2

**No view rendered it** — closed by GL-3 below.

---

# GL-3: the Gallery surface

Built in code rather than in a nib, and deliberately. It has no static layout to design — every subview it owns exists because `GalleryContents` said so — and a view assembled this way can be constructed and asked questions in a test, which a nib-loaded one in this project cannot without a window.

## A placeholder is not a failed thumbnail

The bytes are simply elsewhere. It is drawn dimmed with a download symbol rather than a broken-image one, because "damaged" and "not here yet" call for different reactions and only one of them is true.

## Each tile is one accessibility element, not three

Announced as a name plus a kind — folder, photo, or photo not downloaded. Left as an image and a separate label, a screen reader would read the filename twice and the state not at all. The cloud placeholder is the case that matters: a user who cannot see the dimming has no other way to learn the file is not local.

## The empty view says which kind of empty it is

`FolderEmpty` and `NothingToShow` get different sentences, because they send the user to different places, and the message becomes an accessibility element when it appears — an empty view that says nothing to a screen reader is indistinguishable from one that failed to load.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- New `WinCommanderUT 'NCGalleryView*'`: **3/3 cases, 14 assertions** — one tile per row, and re-applying replacing rather than appending, so a folder change cannot leave the previous folder's photographs on screen; the two empty states producing *different* non-empty messages and both clearing once there is something to show; and the view announcing itself with an identifier and a label, since an unlabelled view is announced by its class name and tells a VoiceOver user nothing.
- Six new strings, all translated, with the catalog guard passing 4/4.
- Full `WinCommanderUT --rng-seed 424242`: **864/864 cases, 12,015 assertions**.

### Coverage gap at GL-3

**No thumbnails** — the pipeline arrives in GL-4 below. Nothing hosts the view in the Explorer yet either.

---

# GL-4: thumbnails, and what must never be generated

The same shape as the network-volume probe cache, for the same reason: the expensive answer is taken off the drawing thread once and remembered, and the drawing thread only ever asks what is already known.

## A cloud-only file is never generated for

Generating a thumbnail is what would **fetch the bytes**. Switching to Gallery is not consent to a download — the rule GL-1 exists for, now enforced at the point where it could actually be broken. Such a row is recorded as `Withheld` rather than left unknown, so nothing keeps reconsidering it and a surface can say why the tile has no picture.

## A failure is an answer

A folder holding one file the generator cannot read would otherwise re-attempt it on every single redraw. `Failed` is remembered, and a generator that threw is a failure rather than a crash.

## Eviction follows what is being drawn, not what arrived first

The bound exists because a folder can hold fifty thousand photographs and an unbounded cache of them is a memory problem the user did not ask for. **Asking for a thumbnail marks it as the freshest thing in the cache**, because that call means "I am drawing this now".

This is where a test found a real flaw: the first implementation ordered by insertion, so a long scroll back through a folder would evict exactly the thumbnails on screen — the ones certain to be wanted again immediately. The test asserted the opposite and failed, which is what a test is for.

A dropped entry is generated again if it comes back into view, and a folder change clears everything, since none of it applies any more and holding it would spend memory on a folder nobody is looking at.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- New `WinCommanderUT 'nc::core::GalleryThumbnailCache*'`: **7/7 cases, 37 assertions** — answered from memory with asking never generating; a cloud-only file never generated for and recorded as withheld; a failure remembered rather than retried; a throwing generator treated as a failure; the bound honoured with the least recently *drawn* entry dropped and a dropped one regenerated; a folder change forgetting everything; and a folder row never generated for.
- The same suite under **TSan** — the cache is read from the drawing thread while a worker writes it.
- Full `WinCommanderUT --rng-seed 424242`: **871/871 cases, 12,052 assertions**.

### Coverage gap at GL-4

**Nothing drove it, and no real generator was wired** — closed by GL-5 below.

---

# GL-5: the pipeline, connected

The view now owns the cache, generates through it, and draws what comes back.

## The placeholder is answered without being scheduled

A cloud-only row costs nothing to answer, and the answer is the point: `Withheld` is what lets a tile say why it has no picture instead of looking like a thumbnail that has not arrived yet. It is recorded inline and no work is queued for it — which is also the enforcement point for the rule that Gallery must not trigger a download.

A test caught this: the first version only called into the cache for rows that *needed generating*, so the placeholder was never recorded at all and stayed indistinguishable from a photo still loading.

## Generation runs where the caller puts it; drawing runs on the main thread

The scheduler is a parameter, defaulting to a utility queue. That is what makes the pipeline testable — a test passes one that runs inline, so the result is there to assert on rather than raced against — and it keeps the choice of queue out of the view. The redraw that follows is dispatched to the main thread explicitly: generation ran wherever the scheduler put it, and touching a view from there is not something to leave to chance.

## The real generator waits, with a bound

QuickLook answers asynchronously, and this already runs on a queue whose whole purpose is to wait for it, so it waits — with a ten-second ceiling, because one unreadable file must not hold that queue indefinitely. A timeout returns nothing, which the cache records as a failure and never retries.

The produced image is handed to the cache as a `shared_ptr` whose deleter returns it to ARC, so eviction releases it rather than leaking it.

## Changing folder drops the previous folder's thumbnails

They apply to nothing here, and keeping them would spend memory on a folder nobody is looking at.

## Verification

- `WinCommanderUT`, `WinCommanderIT` and `WinCommander-Unsigned` — all **BUILD SUCCEEDED**.
- `WinCommanderUT 'NCGalleryView*'`: **5/5 cases, 22 assertions** — including generation asked for exactly the one eligible row, the placeholder recorded as withheld and never generated for, a re-apply of the same folder costing nothing, and a folder change dropping the previous folder's thumbnails while keeping the new one's.
- Full `WinCommanderUT --rng-seed 424242`: **873/873 cases, 12,060 assertions**.
- `QuickLookThumbnailing` is now linked; it is macOS 10.15+ and this project targets 11.0, so a hard link is correct rather than a weak one.

### Coverage gap

**The Explorer does not host the view.** Everything the Gallery needs to draw a folder is connected; what is missing is the mode switch that puts it on screen.

---

# CL-2: reading cloud facts from a real file, without touching it

`ClassifyCloudSyncState` has taken facts since CL-1. Nothing produced them from an actual file — so every Gallery decision downstream was reasoning about a state nobody could supply.

## Nothing here opens anything

Reading the *content* of a placeholder is what triggers the download. Every answer comes from the item's name and its metadata instead. That is not a performance choice: it is the same rule GL-1 and GL-4 enforce, applied at the layer that could most easily break it by accident, because "just stat it to see" is a very natural thing to write here.

## The name a placeholder wears is not the name to show

A not-yet-downloaded file is stored as `.name.ext.icloud`. Presented verbatim, that is a hidden file with the wrong extension standing where the photograph should be — and **every extension-driven decision above it would then be made about `.icloud` rather than `.jpg`**, including whether Gallery may show it at all.

The unmasking is deliberately narrow. The leading dot is as much a part of the convention as the suffix, so a file genuinely named `notes.icloud` is left alone: unmasking it would report a file that does not exist. `.icloud` and `..icloud` unmask to nothing rather than to an empty name.

## A file outside any container has no sync state

Whatever else a probe carries, an item that is not in a provider's folder is `NotCloud` — inventing a state for it would badge ordinary files, which is the failure CL-1's badge rule exists to avoid.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- New `WinCommanderUT`: **5/5 cases, 18 assertions** — placeholders unmasked to the name the user is looking for; five ways of not being a placeholder all refused, including the plain `notes.icloud`; a placeholder read as exactly "known to the provider, bytes not here" and classifying as `CloudOnly`; every reportable state carried through, with a conflict outranking a simultaneous download and placeholder; and an item outside a container reported as not cloud whatever else its probe says.
- Full `WinCommanderUT --rng-seed 424242`: **878/878 cases, 12,078 assertions**.

### Filling the probe

`ProbeNativeCloudItem` asks the filesystem, and asks it only for resource values — metadata the system already holds. **It never opens the file**, which is the whole point: opening a placeholder is what fetches it.

Being in a container is asked first and on its own. Everything else is meaningless outside one, and an item that is not in a container must come back as plainly not cloud rather than as a half-filled answer that a surface might read as partial truth. A path that cannot be read answers the same way: reporting an unreadable item as a placeholder would badge it *and* tell everything above that its bytes are elsewhere, when in fact nobody knows.

`NSURLUbiquitousItemIsExcludedFromSyncKey` arrived in macOS 11.3 against an 11.0 target, so it is asked for only where it exists. Without it an excluded item reads as an ordinary synced one — a milder wrong answer than refusing to say anything about the item at all.

**What the tests can and cannot reach:** an ordinary local file is asserted to come back as not cloud with every other field false, and unreadable, empty and directory paths are asserted to answer safely. A real iCloud container is not available to a unit test, so the mapping from a live placeholder to `CloudOnly` is exercised through `CloudItemFactsFromProbe` with a constructed probe rather than against the system.

- New `WinCommanderUT 'nc::core::ProbeNativeCloudItem*'`: **2/2 cases, 12 assertions**.
- Full `WinCommanderUT --rng-seed 424242`: **880/880 cases, 12,090 assertions**.

### Coverage gap after the probe

**Nothing called it from a listing** — closed below.

---

# CL-3: from a native listing to Gallery items

Two decisions carry the weight.

**A placeholder's name is unmasked before anything reads its extension.** A not-yet-downloaded photograph is on disk as `.holiday.jpg.icloud`. Taken at face value its extension is `icloud`, Gallery decides it is not media at all, and **the one row the user most wants to see silently vanishes from the view** — the worst outcome available, because nothing is wrong on screen and nothing says anything is missing. The name shown is the unmasked one; the name probed is the one on disk, which is what actually exists there.

**Directories are not probed.** A folder is a folder to Gallery whatever its sync state, so asking would spend a filesystem call per row to learn something nothing reads.

Extensions are read the way the rest of the application reads them, including that a leading dot starts a hidden name rather than an extension: `.profile` has none. Treating `profile` as one would put dotfiles in front of every extension-driven rule.

The items hold views into the source's own strings, so copying is deleted and moving is not: a moved vector keeps its buffer, and the views stay valid.

## Verification

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**.
- New `WinCommanderUT 'nc::core::BuildGalleryListing*'`: **6/6 cases, 32 assertions** — a placeholder unmasked, probed under its on-disk name, and followed **all the way through `BuildGalleryContents` to a placeholder row** rather than a missing one; directories not probed; five extension shapes including the dotfile; the parent entry marked so nothing counts it as content; views surviving a move; and an empty listing and an absent prober both answered honestly.
- The same suite under **ASAN**, which is what would catch a view outliving its string.
- Full `WinCommanderUT --rng-seed 424242`: **886/886 cases, 12,122 assertions**.

### Hosting it

The Gallery is now mounted in each pane's container, **above the file view and below the state overlay**: it replaces the listing, but a blocking or empty pane state still has to be able to cover both.

**The file view is hidden, not unmounted.** It keeps its selection, its scroll position and its first-responder status, so coming back from Gallery returns the user to where they were rather than to the top of the folder.

Switching is an `IBAction` reached through the responder chain rather than a new menu tag — the way the cross-pane commands were added in DP-2. No shortcut table and no menu file change, so nothing here can drift out of step with either of them.

Two smaller decisions in the refresh:

- **The cloud probe is only asked on the native filesystem.** Everywhere else every item is simply not cloud, and asking would be a per-row filesystem call answering nothing.
- **A listing that changed underneath leaves the previous contents alone.** Half-drawing is worse than being briefly stale: a folder assembled from two different moments is a folder that was never there.

- `WinCommanderUT`, `WinCommanderIT` and `WinCommander-Unsigned` — all **BUILD SUCCEEDED**.
- `WinCommanderUT 'NCExplorerState*'`: **23/23 cases, 286 assertions**, unchanged — the mounting adds a hidden view and changes nothing about the existing pane behaviour, which is what those tests pin.
- Full `WinCommanderUT --rng-seed 424242`: **886/886 cases, 12,122 assertions**.

### Reaching it

Gallery now has a place in the View menu, targeting the first responder so it reaches whichever pane has focus.

**It ships with no shortcut, deliberately.** Gallery is a mode a user chooses, not one to land in by mistyping — and every free combination near the view keys is one keystroke away from another mode. An unbound action is also one that can never conflict, which the shipped-table guard checks either way.

That guard earned its keep immediately: the first tag chosen for the action, `13'270`, was **already taken** by `menu.view.sorting_extensionless_folders`. A duplicate tag makes one of the two actions unreachable, and which one depends on which side of the lookup you come from — exactly what the guard from Q2-3 was written to catch, caught here before a build.

- `WinCommanderUT` and `WinCommander-Unsigned` — **BUILD SUCCEEDED**; the menu compiles, which is what validates the added item.
- `WinCommanderUT 'nc::core::ShippedShortcutTables*'`: **5/5**, so the new action's name and tag are distinct and its default names a real action.
- Full `WinCommanderUT --rng-seed 424242`: **890/890 cases, 12,150 assertions**.
- The menu title is translated, and the catalog guard covers it.
