# Q2-8 Delete: executing a reviewed Delete — a design, not a backlog entry

> Written while the engine was in context, in the same spirit as
> [`reviewed_batch_execution.md`](reviewed_batch_execution.md) and
> [`reviewed_move_execution.md`](reviewed_move_execution.md): the next session should start from a
> decision rather than from a survey.

## What already exists, and what the wall actually is

More is structurally in place than a survey would suggest, and — as with Move — the wall is not where
the plan item's one-line summary implies.

- **`OperationPlan` already knows both Delete shapes.** `OperationPlanType::Trash` and
  `OperationPlanType::PermanentDelete` exist, and `OperationPlan::Create` already enforces the shape
  that makes them different from Copy/Move: neither may carry a `destination`
  (`UnexpectedDestination`) and neither may carry a `conflict_policy`
  (`UnexpectedConflictPolicy`) — a delete has nothing to conflict with. `DeriveIntrinsicEffects`
  already answers `Trash → {Source: Relocated, Destination: None, DataLossRisk: Recoverable}` and
  `PermanentDelete → {Source: Deleted, Destination: None, DataLossRisk: Irreversible}`.
- **The provider capability surface already has the two questions.** `ProviderCapabilities::can_trash`
  and `can_delete_permanently` exist and are derived from `HostFeatures::Trash` and
  `HostFeatures::Unlink && HostFeatures::RemoveDirectory` respectively
  (`ProviderCapabilities.cpp`). Both are read today only by legacy/product code
  (`SupportsDeletion` in `Delete.mm`, drag-drop policy) — neither participates in anything resembling a
  reviewed contract.

**The wall is that `OperationPlanner::Run()` refuses the plan type outright.** `OperationPlanningRun::Run()`
branches on `Move`, then blocks everything that is not `Copy` with `UnsupportedPlanType` — exactly the
same shape the wall had for Move before Q2-8-Move, and there is no `RunTrash`/`RunPermanentDelete` at
all today. Below that refusal, the same four further gates Move found (`ReviewedOperationFactory`,
`CopyOperationOrchestrator` ×2, `OperationCenterCoordinator`) ask the identical `plan.Type() == Copy`
question and would need lifting together, for the same reason: lifting one at a time proves nothing.

**Two things are missing that Move did not have to invent, and this is the harder half of the design.**

## What Delete needs that Copy and Move did not

**1. The journal has no way to say "confirmed absent."** `OperationJournalItemResult` is built around
*publication*: `destination_publication` (`NotPublished` / `Published` / `Unknown`). Even Move's own
design leaned on this — a completed Move already implies the source is gone, because the rename that
published the destination is the same atomic event that removed it, so `destination_publication ==
Published` was sufficient and true. **Delete has no destination at all** (`OperationPlan::Create`
requires it absent), so there is nothing for the existing field to describe. A Delete item result needs
its own axis — call it `source_removal: NotRemoved / Removed / Unknown` — recording what happened to
the *source*, symmetric to how `destination_publication` records what happened to the destination. This
is new journal surface, not a reuse of existing fields; inventing a `Delete == "destination_publication
Published"` reading would be the same category of mistake as `Trash` being tagged `Relocated` while
having no destination — smuggling a fact under a name that means something else.

**2. There is no conditional-delete provider contract at all**, and one has to be designed from
scratch. Copy publishes from an open descriptor (`fclonefileat`); Move publishes by renaming through an
anchored parent (`renameatx_np`). Delete needs its own primitive:

- **`PermanentDelete` on a regular file has a clean anchored form.** `unlinkat(parent_fd, name, 0)`
  removes a directory entry given a name inside an already-open parent — exactly the same anchoring
  shape Move's `renameatx_np(from_parent_fd, from_name, ...)` already established as safe: open the
  parent, open the child through it with `O_NOFOLLOW` to seal its identity, re-verify the seal
  immediately before the syscall, then act. The window this leaves is the same one Move's design
  accepted and named for the rename case — the child's *name* could be re-pointed at another object
  between the last check and the call — and `unlinkat` gives no stronger guarantee than `renameatx_np`
  did, so the same acceptance applies for the same reason, not a new one invented for this slice.
- **`PermanentDelete` on a directory is out of scope for this slice, and not merely deferred.**
  `RemoveDirectory` only ever succeeds on an *empty* directory; removing a non-empty one is `Unlink` of
  every descendant plus `RemoveDirectory` of every directory in the tree, bottom-up — an unbounded,
  multi-syscall recursive operation with partial-failure states no single conditional transaction can
  seal as one atomic unit. Legacy `DeletionJob::DoScan` already does exactly this recursive walk today,
  outside review. Folding it into a reviewed single-transaction model is real design work with its own
  wall (what does "review" even mean for a tree whose membership can change between review and
  execution?), and belongs to a later slice if it is ever worth the cost — the same way cross-volume
  Move was named `Unsupported` rather than half-built. This slice's `PermanentDelete` claims File only,
  exactly as Copy's and Move's first slices did.
- **`Trash` has no anchored form, and that is a fact about the platform, not a gap in this design.**
  `NativeHost::Trash` calls `RoutedIO::trash(path)` — a path-based system service call
  (`FSPathMoveObjectToTrashSync` under the hood), not a descriptor-based syscall. There is no
  `trashat(parent_fd, name)` primitive to anchor through, the way there is for rename and unlink. A
  conditional Trash could still open-and-seal the object first and then call the path-based API
  immediately after — but that reintroduces the exact TOCTOU window Copy's descriptor-anchoring and
  Move's parent-anchoring both exist specifically to close, with no narrower substitute available,
  because the system service does not accept a descriptor. Reviewing a Trash therefore buys
  measurably less safety than reviewing a Copy, a Move, or a Delete, for a `Recoverable` data-loss risk
  the product already treats as the lower-stakes half of deletion. **This slice puts `Trash` out of
  scope on that basis**, not because it was not considered: the highest-value, cleanest-primitive
  target is `PermanentDelete` — `Irreversible` risk, and a real anchored primitive exists for it.

## What must not be given up

- **One review, one authority per accepted item** — unchanged; a Delete authority must be as
  single-use as a Copy or Move one, for the same reason: an authority is proof a human looked at
  exactly this operation, and letting one review yield two spendable authorities lets the second be
  spent on something nobody reviewed.
- **Fail-closed on any identity or permission-surface change.** The child's device/inode/birth_time and
  the parent's identity are checked exactly, always, regardless of tolerance — nothing about Delete
  changes that discipline.
- **Batch reuses `MonotonicShrink`, not a new tolerance.** A `PermanentDelete` batch removing several
  files from the same parent directory needs exactly the tolerance the `MoveTo` batch slice just
  built and proved on a real filesystem: the parent's size and link_count recede as the batch's own
  earlier items are unlinked, its timestamps still only advance, and everything else about it stays
  exact. This is not new design work — `ProviderConditionalCopyExpectationTolerance::MonotonicShrink`
  and the `source_parents_targeted`-style first-item-Exact/later-items-Shrink tracking pattern in
  `ReviewedOperationFactory::prepare_item` are directly reusable, because a Delete batch's own parent
  shrinks for the identical reason a Move batch's own source parent does: each item indivisibly removes
  one entry from a directory every other item in the batch may also be touching.
- **`Trash`'s absence from this slice is a decision, named, not a silent gap.** If a future slice finds
  a narrower window acceptable for `Trash` — or a platform primitive appears that anchors it — that is
  a decision for that slice to make deliberately, the same way this one is deciding `PermanentDelete`'s
  window deliberately rather than by omission.

## The decomposition

**Step A — DONE: provider eligibility, read-only, plus the intent-only preflight.**
`ProviderConditionalDeletePathSupport` (`SameVolumeUnlink` / `Unsupported` / `Unavailable`) and
`Host::ConditionalDeletePathSupport(path)` mirror `ConditionalMovePathSupport`'s shape exactly, except
for arity: one path, not a source/destination pair, because a delete has no destination side to ask
about at all. `NativeHost`'s implementation resolves the path's own volume and asks
`EvaluateConditionalDeleteVolume`, a new thin wrapper over the same
`EvaluateConditionalCopyVolumeImpl` three variants (`Copy`, `Move`, `Staging`) already shared —
`ConditionalPublicationInterface::None`, the same "nothing to publish through" answer the cross-volume
staging volume already uses, since `unlinkat` removes an entry rather than writing or renaming one. It
therefore demands the same baseline every conditional operation demands (APFS, local, internal
non-removable media, writable, known permissions, a complete metadata API) and *neither* `clone` nor
`rename_excl` — proven in both directions by test, the same discipline the Move-vs-Copy independence
test used.

`OperationPlanner::RunPermanentDelete()` is the intent-only preflight this eligibility feeds, and it
turned out simpler than Move's own Step A in one real way: **a Delete plan has no destination, so there
is no same-provider constraint to check at all** — each source stands entirely on its own, resolved
against its own parent's provider capability and access evidence, and a plan naming sources on several
different providers is not a structural refusal the way a cross-provider Move is. Two decisions made
while implementing rather than left open:

- **A new report item type, not a reused one.** `OperationPlannedCopyItem` carries a mandatory
  `destination`, and `OperationPlan::Create` already refuses one on a Trash/PermanentDelete plan
  (`UnexpectedDestination`) — so reusing it would mean a field that is always empty for every Delete
  item forever. `OperationPlannedDeleteItem` (source, kind, estimate) is its own type, and
  `OperationPreflightReport` gained a second vector, `deleted_items`, next to `items` rather than a
  variant over the two - a plan carries exactly one of the two populated, never both, so the existing
  `CalculateTotals()` accumulation was generalized to walk both without needing to know which one a
  given plan uses.
- **`OperationPlanningRequiredAccess::Delete` is its own value, not `Rename` reused.** Both mutate a
  parent namespace rather than the item itself, the same relationship `Rename` already has to `Write` —
  and the existing comment on `Rename` gave the reason to keep doing this rather than collapse it:
  namespace-mutating access is not one undifferentiated thing just because two operations both happen
  to need it.

Directory sources are refused (`ProviderCapabilityUnsupported`) — not merely unimplemented. This is the
same category of refusal cross-volume Move got: `RemoveDirectory` only succeeds on an *empty*
directory, so a non-empty one needs an unbounded recursive walk (exactly what legacy `DeletionJob` does
today, outside review) that no single conditional transaction can seal as one atomic unit. Folding that
in is real design work belonging to a later slice, if it is ever worth the cost.

Creates no execution authority, promises nothing about `Trash`, and does not touch the journal's
missing `source_removal` axis at all — that is Step B's problem, not this one's, exactly as Move's own
Step A never touched the journal either.

### Verification

New `VFSUT` case, `answers conditional Delete eligibility independently of Copy's and Move's own
interfaces`: a supported volume answers `SameVolumeUnlink`; a volume missing `rename_excl` and,
separately, a volume missing `clone` each still answer eligible, proving neither Copy's nor Move's own
interface is silently required; an unresolvable volume or a relative/empty path answer `Unavailable`.
14 assertions, in Debug and under both Release ASAN and Release UBSAN with no diagnostics; full `VFSUT`
205/205 (82,833).

New `OperationPlanner` cases (tag `[delete-preflight]`): a single File source with no destination is
accepted, with exactly one `Delete`-required access probe and no space probe (a delete does not need
destination free space, the same reasoning `RunMove` already used); several sources on different
providers are all accepted in one plan; provider-capability refusal, a missing source, a directory
source, and denied parent-namespace access each fail closed with the expected blocker. 33 assertions,
in Debug and under both Release ASAN and Release UBSAN with no diagnostics. The pre-existing "Delete
plans are blocked before probing" case lost its `PermanentDelete` section, since that is no longer
true and re-asserting it would have meant testing a claim this slice deliberately makes false.

One test file (`LegacyOperationFactory_UT.cpp`) needed an unrelated mechanical split: its largest
`TEST_CASE` sat just under the project's 32 KiB stack-frame limit, and `OperationPreflightReport`
gaining a second item vector pushed it over. Split into two `TEST_CASE`s along an existing `SECTION`
boundary, the same fix already used for `Theme_UT.mm` and `PanelPresentationGeometry_UT.mm` for the
identical limit - no assertion moved or changed.

Full `OperationsUT` 273/273 (6,603) in Debug; full `WinCommanderUT` 908/908 (12,250) in Debug.

**Step B1 — DONE: the journal's missing axis.** `OperationJournalRemovalState` (`NotRemoved` / `Removed`
/ `Unknown`) and a `source_removal` field on `OperationJournalItemResult` mirror
`OperationJournalPublicationState`/`destination_publication` exactly, including a mirrored
`OperationJournalRecoveryAction::InspectSource` for the `Unknown` case a Delete's own ambiguous commit
can reach - the same uncertainty class `InspectDestination` already exists for, on the axis a Delete
actually uses instead. Two decisions the earlier draft of this document left open, resolved by
implementing them:

- **Which axis is active is a property of the plan type, decided once.**
  `OperationJournalPublishesDestination`/`OperationJournalRemovesSource` partition all five
  `OperationPlanType` values exhaustively and without overlap - `Rename` joins Copy/Move on the
  publication side, because its own journal representation is destination-shaped like theirs, not
  because it creates new bytes. Whichever axis a plan type does not use is required to stay at its one
  inert value always; this is what makes the inactive axis's own `Unknown` structurally unreachable,
  which is what lets the `Failed`-status handling treat "this axis is `Unknown`" as "this plan type's
  *own* axis is `Unknown`" without asking the type again.
- **Filesystem-sync durability is tracked once, for whichever axis is active, and only once that axis
  has positively happened.** The existing shape rules (`Confirmed` needs `errno == 0`, `Failed` needs
  `errno != 0`, anything not-yet-positive stays `NotAttempted`) now govern whichever axis a plan type
  uses, and a `Succeeded` item still additionally requires that axis's sync specifically `Confirmed` -
  not merely shape-valid - the same requirement Copy/Move always had, now stated once for both axes
  instead of copied for a second one. Reused `filesystem_sync_status` rather than adding a second sync
  field: it already read as "was the change durably synced" without naming which side, so a
  Delete's post-`unlinkat` parent-directory fsync fits it without a schema addition, the same way a
  Move's rename-publish sync already did.

**Schema version 4, with a real migration, not a soft default.** Every journal entry written before
this field existed was written before a reviewed Delete plan could exist at all - the planner refused
every `Trash`/`PermanentDelete` plan outright until this slice - so a migrated entry's plan type can
never be one this field means anything for, and defaulting `source_removal` to `NotRemoved` on
migration is not a guess, it is the only value `OperationJournalValidItemResult` would ever accept for
that entry regardless. Decode reads the 12-field shape only at the current schema version and the
pre-existing 11-field shape for versions 1-3 unchanged, the same branching pattern entry-level
`operation_id` already used for the v1→v2 boundary. Encode always writes the current shape; `Open()`'s
existing at-rest upgrade (already proven by the v1 and v2 migration tests) carries every legacy entry
forward the first time the journal is next written.

**A real bug the existing test matrix caught, not a hypothetical one.** The first draft of the
`Succeeded` rule dropped the `filesystem_sync_status == Confirmed` requirement while folding it into
the new shared shape-validation block - the shape block accepts `Confirmed` *or* `Failed` for a
positively-active axis, because both are legal shapes for a `Failed` item's uncertain durability, but a
`Succeeded` item has no business being uncertain. The existing `invalid_results` matrix
(`filesystem_sync_status = Failed` on an otherwise-valid `Succeeded` Copy item) failed
`REQUIRE_FALSE` immediately, before any Delete-specific code had even run - a pre-existing case,
unrelated to Delete, that this refactor's own generalization briefly broke. Restored explicitly rather
than left to the generic block to imply.

### Verification

Extended the existing `OperationJournal_UT.cpp` migration fixtures (`AsV1`/`AsV2` now derive from a new
`AsV3`, which strips `source_removal` the same way `AsV1` already strips `operation_id`) rather than
adding parallel ones, and updated every literal `"version":3`/`SchemaVersion == 3` expectation in the
file to `4` - it was checking "the schema version this journal currently writes," which did not stop
being a meaningful thing to check just because the number changed. Extended the ordered-JSON-shape
assertion to expect `source_removal` between `destination_publication` and `filesystem_sync_status`, its
real position in the encoder. Extended the existing "delete" plan lifecycle case (already present from
before this step, exercising `PermanentDelete`'s destination-side shape) with `source_removal`'s own
positive case, its own missing-axis refusal, and a full `Unknown`/`InspectSource` round trip - the
newest, least-precedented path this step added, and the one path Copy/Move's own tests give it no cover
by proxy. Full `OperationJournal` focused run 910 assertions across every case in the file, in Debug and
under both Release ASAN and Release UBSAN with no diagnostics; full `OperationsUT` 273/273 (6,613) in
Debug and under both Release sanitizer schemes with no diagnostics; full `WinCommanderUT` 908/908
(12,252) in Debug.

**Step B2 — DONE: the provider contract and its Native implementation.**
`ProviderConditionalDeleteReviewedAuthority` (its own type, not shared with Copy/Move, for the same
non-negotiable reason `ProviderConditionalMoveReviewedAuthority` is its own type: an authority minted
from a plan approved as a *copy* or *move* must be structurally unable to authorize a delete). Claims
name the source and its parent, the same shape Move's `source`/`source_parent` claims already take,
minus a destination side entirely. Begin anchors parent + child exactly as Move's Begin does; Commit
re-verifies both seals (parent tolerant per the batch rule above, child always exact) and then
`unlinkat`s. No `RENAME_EXCL` equivalent is needed or meaningful — there is no destination to protect
from replacement, only a source whose continued, unchanged existence is the entire claim being spent.

Two decisions this step settled by implementing them, not just naming them:

- **The commit result type is reused whole, not given its own Delete-shaped twin.**
  `ProviderConditionalCopyCommitResult`'s `publication` field is reinterpreted as "the removal
  happened" for Delete, unrenamed at this layer — the rename to `source_removal` is the journal's own
  vocabulary and belongs at the mapping boundary Step C builds, not here. `ProviderConditionalCopyResultIsValid`
  needed no change at all to accept a Delete result: it was already generic over what "published" means,
  which is the same reason it needed no change when Move was added.
- **An ambiguous `unlinkat` failure is resolved the same way an ambiguous rename failure is, halved.**
  Move's post-rename ambiguity check probes both source and destination identity; Delete has no
  destination, so it probes only the source name: still present with matching identity means the
  `unlinkat` genuinely failed (`ProviderFailure`), anything else (gone, or present but not matching)
  means the outcome can't be told apart from a success that failed to report and is surfaced as
  `Unknown` rather than guessed at either way.

Durability follows the same publish-then-sync order Copy/Move already established: `unlinkat` on the
anchored parent, `fsync` then `F_FULLFSYNC` on the parent fd, then a post-condition read (`FStatAt`
expected to fail `ENOENT`) before reporting success — a delete that returns success but was never synced
is exactly the class of lie this whole engine exists to make impossible.

### Verification

Six new real-filesystem `TEST_CASE`s in `NativeConditionalCopyTransaction_UT.cpp`, built the same way
Move's own transaction tests were: no mocks below the syscall layer, actual directories under a temp
volume. Happy-path publish through an anchored `unlinkat`; the source name re-pointed at a different
object before commit (`SourceStale`); the source parent changed since review (`SourceParentStale`);
cancellation abandons the delete without touching the source; four `Begin`-time refusal sections (source
gone, source changed, parent changed, claimed parent not actually holding the source) each mapping to
the exact `ProviderConditionalDeleteTransactionBeginError` the mismatch implies. A seventh case is the
one this step was actually worried about: two Delete transactions `Begin` against different files in
the same folder while both are still live, the first commits and shrinks the parent, and the second — whose
claims carry `MonotonicShrink` — still commits successfully afterward, proving the batch tolerance added
for Move's own shared-destination-parent case works unchanged for a shared *source* parent shrinking
from a batch's own prior removals. 94 assertions across the six named cases plus 18 for the batch case,
in Debug and under both Release ASAN and Release UBSAN with no diagnostics; full `VFSUT` 211/211
(82,857 in Debug; 82,921 under Release ASAN; 82,959 under Release UBSAN — assertion counts differ
because sanitizer instrumentation adds its own internal checks, not because any test ran differently).
Full `OperationsUT` 273/273 (6,613) and full `WinCommanderUT` 908/908 (12,252) in Debug, confirming
nothing outside `VFS` regressed from the `Host` interface growing a third conditional-transaction entry
point.

**Step C — lift the gates.** All five (`Review`, `ReviewedOperationFactory`, `CopyOperationOrchestrator`
×2, `OperationCenterCoordinator`), at once, for the reason Move's own Step C restated: lifting one at a
time proves nothing, because none of them individually is the wall — the wall was the missing
authority/transaction machinery Steps B1/B2 build.

**Step D — the producer.** `Delete`/`MoveToTrash`/`context::DeletePermanently` in
`Actions/Delete.mm` gain a `reviewed_delete` policy namespace mirroring `reviewed_move`'s
`Select`/`SelectIntoDirectory`/`SelectBatch` shape — though Delete has no destination-directory
question at all, so likely only a `Select`/`SelectBatch` pair (item eligibility, whole-selection
answer), not a `SelectIntoDirectory`. Routes only the `PermanentDelete` path this slice built; `Trash`
keeps going through `nc::ops::Deletion` unconditionally, exactly as this document names it should.

## What this document deliberately does not decide

- The exact new `OperationPlanningRequiredAccess`/blocker vocabulary for Delete — Step A's
  implementation decides that against the real enum, not this prose.
- Whether `source_removal`'s three states are exactly `NotRemoved`/`Removed`/`Unknown` or a different
  shape — Step B1 is where that gets decided against the journal's actual finalization contract
  (`Finalize`, the single-use terminal gate, how `Pending`/`Inconsistent` are computed today).
- Whether a future slice ever narrows `Trash`'s TOCTOU window enough to review it — named as future
  work, not designed here.
