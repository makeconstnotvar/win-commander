# Explorer Search Mode

> Status: Queue 1 Q1-9 complete
>
> Execution tracker: `Docs/Development-Plan.md`, Queue 1 item Q1-9

## User-visible contract

Explorer mounts Search Mode above the active panel content. `Find Files` opens the mode with the current-folder scope and carries an entered query into the form; the Spotlight action opens the same surface with whole-Mac scope selected. Commander retains its established Find Files and Spotlight flows.

The form exposes an explicit scope for the current folder, its subfolders, the current Native disk or Spotlight across the local Mac. It supports contains/exact name matching, extension, file type, minimum and maximum size, modified-time bounds, content and hidden-item filters. The surface identifies the selected backend, current location, scanned/found counts, determinate or indeterminate progress and every declared backend limitation. Running work has an explicit Cancel action.

Terminal states distinguish completed, partial, permission-limited, no-results, too-many-results, cancelled, unavailable/index-unavailable and failed searches. Successful and bounded partial results replace the pane content with an ordinary non-uniform VFS listing. Reveal Original is enabled only for an exact focused result from the controller's last committed result listing.

Search Mode belongs to one Explorer tab. Switching tabs restores that tab's own snapshot; repeated Find Files while the mode is presented focuses the existing query instead of discarding its request or result state. Closing the mode restores the panel's quick-search presenter.

## State, planning and backend ownership

`SearchStore` is the pure pane-bound state owner. It issues monotonic `SearchRunId` values, owns normalized request/backend/progress/result/failure values and accepts events only for the active run. A later run, cancellation or reset makes delayed callbacks stale. Result publication is represented by an opaque controller-owned token and generation; the Store retains no VFS listing or provider object.

`SearchPlanning` validates textual criteria, range ordering, name/extension capability coherence, whole-Mac backend identity and the exact support/limitation descriptor pairing. It selects Direct Scan or Spotlight from exact runtime facts and retains explicit capabilities and limitations. Unsupported filters and scopes are surfaced before execution. Runtime skipped locations can add permission or vanished-result limitations only to the matching active run.

Each Explorer tab owns one `ExplorerSearchController`. Presentation captures the exact uniform origin listing, pane identity and data generation. Backend progress and completion are accepted only while the run, pane and captured panel content still match. Before pane commit, the controller requires a titled non-uniform listing whose exact item count equals the backend claim, verifies that completion kind and runtime limitations form a valid pair and converts malformed or backend-inconsistent terminal payloads into a typed invalid-backend failure. Runtime index/service availability replies must also match the planned backend kind. External navigation or replacement content closes the mode; the controller recognizes its own committed result listing and preserves it. Cancel and teardown stop the backend synchronously without waiting on the main thread, then move blocking wait/destruction to a background reaper. Weak callbacks prevent backend completion from retaining a closed controller or pane.

## Direct and Spotlight execution

`ExplorerDirectSearchBackend` executes through `VFS::SearchForFiles`. Current-folder and recursive scopes reuse the exact origin provider; current-disk scope is admitted only for a Native provider with an exact `statfs` volume root and fences descent to the same device. Failure to establish that exact disk boundary is mapped to a typed permission or execution failure before scan. Name, extension, type, size, modified time, content and hidden-item predicates are applied without changing provider semantics. A failed root or non-permission item/descendant I/O failure is terminal; denied items or descendants produce a permission-limited result. Search result count and owned path bytes are capped at 50,000 and 64 MiB.

`SearchForFiles` now optionally reports owning `SkippedLocation` values for root, descendant and content-item failures and accepts a descent predicate. Exact create/open/attach and content read/validation errors cross this boundary. Existing callers retain the compatibility defaults. Predicate exclusions are deliberate scope boundaries, while cancellation suppresses skipped-location diagnostics.

`ExplorerSpotlightSearchBackend` owns an asynchronous `NSMetadataQuery` lifecycle behind an injectable seam. Its query plan uses constant predicate fragments with request values passed as separate arguments and the local-computer scope. Each notification replaces the current exact result snapshot, hidden path components are filtered consistently, and the final snapshot alone supplies terminal paths. Those paths are deduplicated and materialized through the same bounded ordinary-listing builder. Spotlight service/index failures and cancellation remain typed terminal outcomes.

## Result listing and reveal

`BuildSearchResultListing` sorts and deduplicates exact provider/path identities, fetches each item through its provider and constructs a titled non-uniform listing. Its bounded summary separates permission denial, vanished-result races and other I/O failure while usable results remain: permissions become an explicit limited result, vanished paths become a partial limitation and other I/O fails the run with typed evidence. Cancellation returns no listing, and count/path-byte caps are enforced before the controller commits a result.

Reveal reads the exact focused item from that committed listing and submits one navigation request to the item's original directory with its filename selected. A focus from another listing, stale generation, non-result content or failed navigation admission cannot redirect Reveal.

## Accessibility and limitations

The mounted surface uses stable accessibility identifiers and labels for query, scope, match mode, filters, progress, limitations, Start, Cancel, Reveal Original and Close. Search phase, backend, location, counts and limitations are expressed as text in addition to the native progress indicator.

Whole-Mac scope currently uses the local Native Spotlight backend. The application has no authoritative FDA/TCC grant detector, so production planning conservatively reports the Full Disk Access limitation for whole-Mac searches. Signed permission/FDA walkthrough evidence is part of the Q1-10/release gate. Provider-native remote indexing, archive-specific search, saved searches, mounted-volume aggregation, tags, regex, fuzzy search and index diagnostics remain later search increments.

## Verification

Confirmed current-tree evidence:

- focused Debug `WinCommanderUT '*Search*'`: 62 cases / 1,010 assertions passed;
- full Debug `WinCommanderUT`: 580 cases / 10,147 assertions passed;
- focused Debug `VFSUT '*SearchForFiles*'`: 9 cases / 68 assertions passed;
- full Debug `VFSUT`: 179 cases / 46,786 assertions passed;
- Debug test-target builds passed;
- Debug `WinCommander-Unsigned` build passed;
- focused Release ASAN `WinCommanderUT '*Search*' --rng-seed 424242`: 62 cases / 1,010 assertions passed, `libclang_rt.asan_osx_dynamic.dylib` is linked and emitted no diagnostics;
- focused Release UBSAN with the same filter and seed: 62 cases / 1,010 assertions passed, `libclang_rt.ubsan_osx_dynamic.dylib` is linked and emitted no diagnostics.

The focused Search set covers pure planning/reduction, descriptor consistency, stale-run and progress fencing, direct-scan scopes and filters, content-item errors, Spotlight snapshot replacement and hidden paths, typed bounded listing construction, malformed backend payload rejection, AppKit states/key handling, Commander/Explorer action routing, per-tab presentation, exact result commit/reveal, cancellation and weak asynchronous teardown. `SearchForFiles` coverage proves root, descendant and item failure reporting, descent exclusion, compatibility reuse and cancellation suppression.

Signed Spotlight/FDA, VoiceOver and live-provider walkthroughs remain Q1-10/release evidence.
