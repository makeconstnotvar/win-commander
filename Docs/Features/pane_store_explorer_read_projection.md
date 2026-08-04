# Feature: PaneStore Explorer read projection

> Status: M1 committed-state read slice, production navigation/refresh lifecycle, `PaneStore` reduction, Explorer breadcrumb and footer projection, and Store-backed View/Back/Forward/Up/Refresh command presentation implemented; remaining pane consumers remain in progress
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 7.2, 11 and 17
> Execution tracker: `Docs/Development-Plan.md`, R2 in `Docs/refactor_plan.md`

## User problem

Explorer address and status surfaces need one coherent, observable view of the active pane. The first production seam publishes committed `PanelController` data as immutable snapshots while the mature panel and VFS engines continue to execute navigation and listing work.

## Product behavior

One adapter belongs to one Explorer pane. It reads the controller on the main queue, observes panel context invalidations and controller lifecycle, coalesces repeated context events, and publishes a `PaneSnapshot`. The snapshot includes the exact focused listing item after cursor restoration and the current hidden-file hard-filter setting. The breadcrumb renders location, activity and error/accessibility state from that snapshot, while the Explorer footer renders item count, selected count and selected bytes through the same `NCExplorerState` Store observation. The Explorer View popover and navigation toolbar use the matching snapshot for command presentation. Navigation, address editing, and command execution continue through the established `PanelController` paths.

## UI states

- `Empty`: initial pane with the engine empty-listing identity;
- `Loaded`: committed uniform or temporary listing with coherent title, path, provider, and statistics;
- request-scoped navigation and refresh events supply `Loading`, `Refreshing`, `Failed` and typed `visible_error` through the Store reducer;
- `Loading` retains committed path/content, shows activity and makes the address non-editable;
- blocking/recoverable failures render a localized persistent breadcrumb message plus accessibility value/help; cancellation and informational outcomes do not become blocking UI;
- `Refreshing` retains committed path/content while one running refresh worker and one latest pending intent are physically coalesced; exact terminal commit advances listing identity without changing location generation.
- The mounted Explorer footer maps `Unavailable`, `Loading`, `Counts`, `Empty` and typed `Error` through `VisualStateMapper`. Its count and selection text comes only from the matching immutable snapshot; the legacy `PanelData` statistics/listing callbacks are ignored in Explorer appearance.

## Commands

`file.copy`, `file.cut`, initiation `file.rename`, `file.open`, `view.toggleHiddenFiles`, `navigation.back`, `navigation.forward`, `navigation.up`, and `navigation.refresh` use the R3 Registry/Visual State path, while remaining commands continue through existing dispatchers until their legacy adapters are connected. Explorer View and navigation-toolbar presentation are Store-backed command-state consumers: they supply matching hidden-files, history-availability or derived Up/Refresh availability to the same Registry definitions used by menu/shortcut entry points. Before a same-pane snapshot arrives, or after a foreign-pane snapshot, the optional state is absent and Registry returns the localized missing-state reason. Execution samples the live controller through the shared dispatcher so a stale presentation snapshot cannot become write authority.

`file.open` uses a synchronous live command context. `OnOpenNatively:`, the native-open menu, ordinary-file Enter fallback, Shift-Return/explicit Shortcut, and context-menu execution converge on the same definition; the context menu supplies its exact captured items. Folder/archive Enter remains the navigation router, including the established non-uniform dot-dot Back path, and dot-dot submits one enclosing-folder request. `file.open` accepts one regular file from any readable provider, an all-regular batch from the exact same readable provider instance, or one native directory/special item. Dot-dot and remote directory/special-item payloads produce a structured disabled state. Registry `Executed` means the accepted exact payload was synchronously submitted to `FileOpener`.

## Data model

The published state contains pane identity, controller location generation, listing identity and generation, load phase, uniformity, path, display title, common host, exact focused `VFSListingItem`, immutable shared exact selected items in display order, hidden-file visibility, semantic sort/group state, actual view mode, optional valid layout slot, Back/Forward availability, runtime current History entry identity, item/selection statistics, and an optional typed visible error. Empty `focused_item` means no committed focus. A nonempty focus and every selected item are valid only for `Loaded` state, must reference the snapshot's exact listing, and must have an in-range raw index; selected identities are unique and their payload size equals `selected_count`. Empty state carries no focus, selection, item count, or selected bytes. Pane presentation and History state are projected before the unloaded-state early return because they belong to the pane rather than listing content.

`PaneNavigationAvailability` is a derived view over those immutable facts rather than new stored authority. Loading maps Up and Refresh to Busy; empty/no-content state disables both with hierarchy/content reasons; committed uniform child and valid provider-parent roots enable Up; top roots and non-uniform listings disable explicit Up while committed content remains refreshable. `MapMatchingPaneNavigationAvailability` returns no projection for an invalid or foreign `PaneId`. The controller builds the same facts from current live state before execution.

Live availability and lifecycle admission call the same controller queue-ownership helper. It correlates the controller's own navigation/reload workers with lifecycle active/tail request identity and classifies only unrelated occupancy or a stopped reload queue as external Busy work. Presentation and admission therefore share one conservative view of controller-owned versus foreign queue activity.

`Panel::data::Model` stays the listing and selection engine. Its `SelectionProjectionGeneration()` changes when the exact `SelectedEntriesSorted()` membership, order, visibility, or listing identity can change. The production bridge keys a cached `PaneSelectedItems` payload by that token and listing identity, rematerializes it only after invalidation, and reuses the prior shared payload when the exact values remain equal. Cursor-only reads therefore avoid both the O(N) model scan and O(K) selection copies. Each `PanelController` receives one `PaneId` from the common factory and retains that identity for its lifetime. `PanelController` owns `PanelControllerLifecycle` for that identity, and `PanelControllerPaneStoreAdapter` initializes its store from `controller.paneId`, so execution and read projection address the same pane. `PaneStoreAdapter` is the observable read boundary; the production adapter owns notification lifetime and main-queue scheduling.

## Lifecycle contract foundation

[`PaneLifecycleProducer`](../../Source/WinCommander/WinCommander/Core/Pane/PaneLifecycleProducer.h) is the completed toolkit-independent sequencer for one `PaneId`. It assigns stable request IDs and monotonic event sequences, copies the request descriptor into every event, and distinguishes `Started`, `Committed`, typed `Failed`, `Cancelled`, `Superseded`, and `Rejected` outcomes. A rejected attempt receives its own identity without becoming active and can own an optional typed `admission_error`. An accepted request retains terminal-sequence capacity; active rejections cannot consume that reserve, and supersession atomically publishes the old terminal before the replacement `Started` while preserving capacity for the replacement terminal.

Publication is main-queue, synchronous, and reentrant FIFO. Each batch is fully allocated before active state, counters, or the pending queue change; commit uses no-throw ownership transfer and list splicing. Strong lifetime guards keep the implementation alive through callback-driven producer destruction, shutdown publishes at most one `ProducerShutdown` cancellation, and observation tickets remain safe after producer destruction. Linearizable `Subscribe` installs live observation and returns either an active seed or a retained-failure seed/replay/checkpoint, closing construction gaps without synthetic event identity.

[`PanelControllerLifecycle`](../../Source/WinCommander/WinCommander/Core/Pane/PanelControllerLifecycle.h) is the completed pure app-layer coordination seam. It samples admission immediately before sequencing, applies the navigation/refresh busy and supersession matrix, serializes reentrant attempts, resolves actually queued deferred submissions exactly once, invokes a request-ID-aware scheduler, suppresses stale model commits, maps admission-probe exceptions to `Rejected(Unavailable)` with an owned `admission_error`, maps scheduler or commit exceptions to `Failed`, and delays shutdown until the current publication transaction is complete. Probe tail identity is correlation data only; production code must compare explicit controller-owned worker slots before classifying work as external.

The controller lifecycle seam is in production for navigation and refresh. `PanelController` owns `PanelControllerLifecycle` for its `PaneId`, and synchronous/asynchronous `GoToDirWithContext` requests pass through fresh admission, request-ID-correlated `NavigationWorkerSlot`, typed failure/cancellation, and transactional commit that updates model/generation before terminal success. Asynchronous navigation runs `FetchNavigationRequestDetached` on the shared loading `SerialQueue`; the task retains request/queue/token state and reaches main only through a weak controller box. Queue occupancy only detects work outside correlated lifecycle slots. Admission feedback and accepted-worker feedback have separate validity tokens; `DirectoryChangeResultSource` distinguishes admission rejection from VFS fetch results, so provider `EBUSY` retains provider semantics. Non-GoTo content intents invalidate applicable feedback and cancel the navigation worker token. A controller-wide content-intent epoch also invalidates running/pending refresh, recovery and legacy loading commits. Persistency recovery/fallback callbacks capture the controller weakly. `PanelController` receives its `Config &` explicitly; the production factory supplies `GlobalConfig()`, while tests use an in-memory config and the injected-config `PanelViewLayoutsStorage` overload.

`refreshPanelDiscardingCaches` submits only through `PanelControllerLifecycle::SubmitRefresh`. Admission captures an immutable work snapshot: source listing identity, controller/location generation, uniform or temporary type, host/path, fetch flags, error context and native recovery host. Uniform work calls `VFSHost::FetchDirectoryListing`; temporary work calls `VFSListing::ProduceUpdatedTemporaryPanelListing`. The fetch captures no controller lifetime and owns a per-worker cancellation token. Registry `navigation.refresh` calls the forced user boundary, setting `initiated_by_user` and `F_ForceRefresh`; automatic `refreshPanel` callers remain soft, non-user-initiated lifecycle submissions outside Registry. `Accepted` or `Deferred` reports successful submission only; lifecycle commit remains separately correlated by the minted request identity.

Only one refresh worker occupies the reload `SerialQueue`; one additional latest pending intent is stored outside the queue. A newer accepted refresh cancels the running token and replaces the previous pending intent. Navigation supersedes refresh, common content intents cancel both refresh slots, and ESC cancels the active lifecycle request even before the delayed spinner becomes visible. Exact request id plus shared finished token and `SerialQueue::SetOnDry` prevent ABA cleanup and keep physical queue length at 0/1.

Completion first checks the exact active request, content-intent epoch, source listing pointer/type, controller generation and, for uniform listings, source host/path. `Model::ReLoad` runs on a prepared model copy before transactional lifecycle commit. `Committed` preserves controller/location generation while replacing the listing; UI/model notifications run only after the terminal event was published. Provider failure emits one typed `Failed` and retains the committed model; cancellation emits one `Cancelled`. `ENOENT`, `ENOTDIR` or `ESTALE` emits refresh `Failed` first, then starts ordinary asynchronous navigation lifecycle recovery to an accessible ancestor or native home.

[`PaneLifecycleReducer`](../../Source/WinCommander/WinCommander/Core/Pane/PaneLifecycleReducer.h) composes immutable committed Empty/Loaded projections with ordered request overlays. Navigation starts as `Loading` while retaining the committed listing, cancellation returns to committed state, failure publishes `Failed` plus typed `visible_error`, and same-kind supersession preserves a continuous public loading phase. A `Committed` event is accepted only with the exact post-model controller projection.

`PanelControllerPaneStoreAdapter` consumes the producer's atomic subscription: it seeds an already-active request or replays the retained failure at its checkpoint, then reduces subsequent events synchronously on the main queue. Contract rejection or exception in this production boundary is fail-stop rather than a silently stale Store. Context notifications update committed selection, focus, filter, sort/group/view and history-availability projections without inferring lifecycle. A lifecycle commit occurs after model replacement but before `PanelView` restores its cursor; focus is therefore suppressed for the exact committed projection. The deferred context rebuild samples the final live item and current pane presentation while reusing the cached selection payload unless the Model token changed. Reducer validation is O(1) for a previously validated shared payload and performs exact listing/range/count/duplicate checks only for a new payload. When refresh commits a new listing at the same controller/location generation, `PaneStoreAdapter` advances `listing_generation` because the referenced listing identity changed; presentation-only changes advance only snapshot `revision`.

The Explorer breadcrumb is the first pane Store renderer in this slice. The mounted Explorer footer is the second: `NCExplorerState` forwards the same snapshot through `PanelView`, which rejects a foreign `PaneId`; the footer maps it through `VisualStateMapper` and does not accept the legacy `PanelData` statistics/listing writer. It renders only count, selected count and selected bytes, while free-volume, focused-item metadata and view controls remain legacy footer responsibilities. The breadcrumb retains committed address/content through lifecycle-only changes, gates address editing during loading, and presents localized blocking/nonblocking errors through a persistent label and accessibility value/help. Cancellation and informational outcomes remain nonblocking. Focused production E2E drives the real `PanelController` navigation and refresh workers and covers commit ordering, failure, supersession/cancellation, latest-wins coalescing, exact recovery sequencing, same-generation listing advancement and teardown. A real temporary-directory `NativeHost` navigation/forced-refresh case passes 1 / 30 and an owned `0000` directory proves `EACCES` → `PermissionError` at 1 / 22. Deterministic security-scope coverage passes 4 / 47: a normalized bookmark scope authorizes only an exact path or component descendant; user denial and automatic navigation publish `EPERM` before fetch/model replacement; only user initiation can prompt; denial returns one Admission callback. Docker FTP/SFTP/WebDAV navigation with a shadow-host mutation, forced refresh, and endpoint outage/reconnect passes 6 / 244: FTP and WebDAV supersede a successful user refresh with a provider-observed soft successor that commits the fresh listing under latest-wins, while SFTP commits the direct user refresh without provider observation/cache. Each outage emits `Failed(NetworkError)` with the original listing/generation retained; FTP and SFTP retain their provider-domain `Unavailable` classification, and WebDAV retains `POSIX/ECONNREFUSED` or `ECONNRESET`; the same controller then reconnects through the next user Refresh. A deferred navigation behind a `Started` observer's external loading task passes 1 / 22: it resolves once as `Rejected(Busy)` with `EBUSY` admission feedback and cannot fetch or replace the model. FTP/SFTP/WebDAV timeout and real signed MAS/FDA permission remain broader acceptance scope.

The recorded remote-transport evidence superseded that earlier 6 / 244 snapshot: at that slice Debug `VFSUT` was 95 / 43,563, Debug `WinCommanderUT` was 321 / 5,206, `IntegrationTests` built, and focused Docker `WinCommanderIT` passed 9 / 376. FTP listings, WebDAV blocking control and SFTP TCP/session work use 30-second production deadlines and isolated 500 ms test seams. Live endpoint pause/unpause preserves raw FTP `operation_timeout`, SFTP `timeout` and WebDAV `POSIX/ETIMEDOUT`, produces `TimeoutError` with the prior listing/generation intact, and recovers through the same controller. The SFTP blackhole VFSIT remains 1 / 2; negative `readdir` fails closed and unavailable/timeout connections are discarded rather than pooled. Current full Debug evidence is `VFSUT` 96 / 43,606 and `WinCommanderUT` 333 / 5,376.

The loading and reload executors are ref-counted and captured by their detached workers; only weak controller boxes cross asynchronous main-callback boundaries. Controller teardown invalidates navigation/refresh tokens, stops queues, shuts down lifecycle publication and returns without `Wait`, even when a provider ignores cancellation. Queue-owned request/listing/provider resources remain alive until that provider returns, while the controller deallocates promptly. Factory-injected configuration and native-provider dependencies remain raw non-owning references with lifetime requirements documented at the controller boundary.

## Provider capabilities

The snapshot retains the common `VFSHost` for uniform listings. Capability resolution remains a separate `ProviderCapabilities` projection. `file.open` resolves `Read` against the exact live item provider, while a batch additionally requires the same provider instance for every regular file; this state is direct command context rather than Store authority.

## Operation lifecycle

This read-only seam observes the final model state after operation-driven refreshes. Operation planning and result history enter through M3.

## Error states

Committed empty and loaded states remain authoritative. The pure sequencer enforces stable identity, ordered main-queue events, terminal reservation, and one terminal outcome per accepted request. Navigation and refresh admission, deferred rejection, cancellation, fetch/commit exceptions and VFS errors map into the controller lifecycle, and `Committed` follows the corresponding model mutation. The production reducer retains the previous committed listing until an exact commit arrives. Invalid-location refresh publishes its typed failure before recovery navigation starts, so error history and replacement location remain causally ordered.

## Edge cases

- initial engine empty listing;
- uniform local or virtual directory;
- temporary non-uniform listing with a title and multiple hosts;
- selection-only and cursor-only notifications;
- exact selected-item order, clear, duplicate suppression, retained snapshot lifetime, and malformed selection projections;
- hidden-file filter changes before loading and over committed content;
- missing or foreign-pane command-bar snapshot;
- foreign, out-of-range and pre-restoration focused items;
- multiple notifications in one run-loop turn;
- listing replacement at the same location generation;
- same-generation refresh commit advances listing generation exactly once;
- burst refresh coalescing retains one running worker and only the newest pending intent;
- navigation supersedes refresh and common content intents cancel running/pending refresh;
- provider cancellation and failure retain the old model and publish one exact terminal;
- invalid-location refresh failure followed by ordinary navigation recovery to an ancestor/native home;
- controller teardown during a navigation or refresh provider that does not immediately cooperate with cancellation;
- delayed notification after adapter teardown;
- retained snapshot after controller data replacement;
- adapter attachment during an active navigation;
- adapter attachment after a retained navigation failure and reentrant construction during failure delivery;
- navigation failure and cancellation over retained committed content;
- same-kind supersession without an intermediate committed publication;
- deferred acceptance and rejection feedback;
- admission `EBUSY` versus provider `EBUSY` presentation;
- asynchronous production worker success, failure, supersession and cancellation against a deterministic VFS host;
- lifecycle and context notifications after adapter teardown.

## Accessibility

The breadcrumb publishes the rendered path or fallback title as its accessibility value. Store-backed errors publish the same localized safe message as visible text, tooltip, accessibility value and accessibility help. Snapshot changes that leave the address projection unchanged preserve focus; loading disables address editing without discarding committed path/content.

## Acceptance criteria

- one production adapter owns one observable store and one panel-context subscription;
- controller and store snapshots retain the same factory-injected `PaneId`;
- all controller/model reads occur on the main queue;
- the breadcrumb path, provider, and fallback title come from `PaneSnapshot`;
- selection-only changes do not rebuild the address presentation;
- non-uniform and empty states avoid common-host access;
- adapter teardown makes queued invalidations harmless;
- the pure lifecycle sequencer preserves stable identity, FIFO ordering, terminal reservation, transactional batches, and callback-safe lifetime;
- synchronous/asynchronous `GoToDirWithContext` uses the controller-owned coordinator and explicit navigation worker correlation;
- production worker tests observe commit only after model mutation and suppress superseded or cancelled worker callbacks;
- lifecycle is reduced from explicit controller producer events, not inferred from queue occupancy or view notifications;
- atomic subscription plus active/retained-failure seed/replay cannot miss an in-flight request or terminal failure;
- terminal commit publishes only the exact post-model projection;
- nonempty focus belongs to the exact current listing, is in range, and appears only in Loaded state;
- selected identities are unique, belong to the exact current listing, match `selected_count`, retain display order, and appear only in Loaded state;
- lifecycle commit does not reinterpret an old numeric cursor against a new listing; deferred context projection publishes the restored focus;
- focus-only updates advance snapshot revision without advancing listing generation;
- selection-only updates advance snapshot revision without advancing listing generation; cursor-only updates reuse the immutable selection payload and do not rescan the listing;
- hidden-file visibility is available for unloaded and loaded panes, and filter-only updates advance revision without advancing listing generation;
- Explorer hidden-files presentation requires a matching pane snapshot and otherwise retains the Registry's missing-state disabled reason;
- semantic sort key, direction, collation and directory flags are available before listing load and survive lifecycle recomposition;
- sort-only updates advance revision without listing generation, rematerialize selected display order only when needed, and reuse an unchanged immutable payload;
- Explorer Sort presentation marks exactly one active criterion/direction only for its matching pane snapshot;
- grouping projects the effective current key, including all supported date groupings; disabled grouping carries `Unknown`;
- view projection separates actual `Icons`/`Details`/`Gallery` mode from an optional valid configured layout slot, so a disabled-slot fallback has no false preset marker;
- effective grouping/layout changes emit one scoped rebuild, no-op changes remain silent, and lifecycle overlays retain current presentation state;
- Back/Forward availability and nonzero runtime current entry identity are projected before listing load, update immediately after effective History mutation, and remain coherent through lifecycle overlays;
- Up/Refresh availability is derived from immutable pane facts; explicit Up is available only for uniform child/provider-parent hierarchy, while Refresh requires committed content;
- Explorer toolbar starts disabled, accepts only its matching pane snapshot for Back/Forward/Up/Refresh, and does not forward a foreign snapshot to the breadcrumb;
- `file.open` presentation and execution use the same exact live items, provider identity and capability rules across native-open selector/menu, ordinary-file Enter, Shortcut and context-menu entry points;
- `file.open` reports `Executed` after synchronous submission to `FileOpener`;
- navigation execution re-reads live controller state; Registry user Refresh is forced and user initiated, while automatic soft refresh remains outside Registry;
- Refresh `Accepted`/`Deferred` means submitted to lifecycle admission; request-correlated terminal commit remains a separate outcome;
- refresh admission captures immutable source identity and every accepted refresh has exactly one terminal outcome;
- refresh commit preserves controller/location generation and advances Store listing generation for a new listing identity;
- physical refresh execution is bounded to one running worker plus one latest pending intent;
- invalid-location recovery preserves the `refresh Started → Failed`, `navigation Started → Committed` sequence;
- teardown is main-thread nonblocking, performs no queue `Wait`, and queue-owned asynchronous resources outlive the worker that uses them;
- breadcrumb activity, editability, persistent error text and accessibility state come from the mapped snapshot;
- Explorer footer count, selected count and selected bytes come only from its matching Store snapshot; its legacy statistics/listing callbacks cannot overwrite that projection;
- admission and fetch feedback remain source-distinct; cancellation/informational outcomes do not become blocking error UI;
- focused tests pass; aggregate WinCommander evidence records the separate unavailable-AppKit-pasteboard baseline.

## Tests

- pure state extraction for empty, uniform, and non-uniform models, including hidden-file visibility before and after loading;
- exact focused and selected items, deterministic selection order, selection statistics, malformed-projection rejection, and retained payload/listing lifetime;
- coalesced notification delivery and object scoping;
- listing-generation change without location-generation change;
- teardown and queued-main-work safety;
- completed pure producer coverage: admission/rejection, monotonic IDs and sequences, stale/duplicate terminal suppression, multi-observer reentrancy, callback-driven destruction, shutdown/ticket lifetime, transactional overflow boundaries, terminal reservation, and the last valid terminal sequence;
- focused `PaneLifecycleProducer` final Debug suite: 24 cases / 267 assertions, including linearizable active/retained-failure subscription, preallocated transactional finish, accepted-terminal classification, reentry rejection and callback-time destruction;
- completed pure coordinator final Debug coverage: 21 cases / 211 assertions for admission priority, owned admission diagnostics, navigation/refresh supersession, synchronous/asynchronous scheduling, deferred resolution, transactional post-model commit, stale suppression, phase-specific exception mapping, callback cancellation/destruction and retryable serialized shutdown;
- completed Store focused Debug coverage: main-queue behavior, focus/selection/filter/sort/group/view/history-only revision, immutable payload reuse, duplicate suppression, exact same-generation listing advancement, lifecycle commit/failure/cancellation and teardown; 18 cases / 220 assertions;
- completed reducer focused Debug coverage: lifecycle bootstrap/terminal composition plus exact focus, selection, sort, group, view and History identity invariants, including invalid enum/combination, Empty/foreign/out-of-range/item-count/selected-count/duplicate rejection and trusted-payload reuse; 19 cases / 335 assertions;
- completed production bridge focused Debug coverage: empty/uniform/non-uniform exact focus/selection/filter/sort/group/view/history state, complete legacy sort/group mapping, valid/disabled layout-slot fallback, history availability/current-identity event and lifecycle sequencing, selection-generation caching/reordering, cursor-only payload identity, commit-before-cursor-restoration, retained lifetime, atomic subscription/replay, same-generation refresh commit, failure/cancellation, supersession and teardown; 28 cases / 461 assertions;
- combined Store/reducer/bridge focused run: 65 cases / 1,016 assertions; with dedicated History 5 / 457, total 70 / 1,473;
- Panel `SelectionProjectionGeneration` contract: 1 case / 42 assertions; Explorer matching-pane presentation/toolbar: 2 / 266;
- completed Visual State/breadcrumb final Debug coverage: mapper 8 cases / 181 assertions; breadcrumb lifecycle/error/AX/source-discrimination case 1 / 32 assertions;
- Explorer footer Store presentation: 2 cases / 18 assertions for unavailable, loading, counts/selection bytes, empty and typed error plus legacy statistics/listing writer rejection; the `PanelView` boundary also rejects a foreign `PaneId` before it reaches the footer;
- production implementation present: controller ownership, navigation/refresh worker correlation, typed result sources, latest-wins coalescing, exact terminal outcomes, recovery, Store reduction and breadcrumb presentation;
- completed broader [`PanelControllerNavigation_UT.mm`](../../Source/WinCommander/WinCommander/Tests/PanelControllerNavigation_UT.mm) production navigation prefix: 8 cases / 103 assertions for model-before-commit publication, failure retention, supersession/cancellation, explicit Up semantics, dot-dot compatibility and non-cooperative provider teardown. The explicit-Up teardown increment passes 1 / 12. The fixture uses the real controller worker orchestration, a deterministic VFS host and in-memory injected config;
- completed production refresh prefix: 16 cases / 154 assertions for identity/snapshot gates on the uniform provider branch, running+latest coalescing, navigation/content supersession, failure/cancellation, invalid-location recovery, same-generation Store advancement and nonblocking teardown. The implemented temporary-listing fetch branch remains outside this focused controller fixture;
- live local `NativeHost` navigation plus forced refresh: 1 case / 30 assertions against a real temporary directory;
- live local `NativeHost` permission denial: 1 case / 22 assertions against an owned `0000` directory, including typed error context and no committed-model replacement;
- deterministic security-scope policy: 4 cases / 47 assertions for exact path-component containment, user denial, automatic no-prompt denial, granted continuation and an Admission callback without fetch;
- Docker FTP/SFTP/WebDAV navigation/refresh and endpoint outage/reconnect: 6 cases / 244 assertions; shadow-host mutations drive the fresh listing, FTP and WebDAV cover the exact `Superseded` → soft successor → `Committed` latest-wins chain, SFTP covers direct user-refresh commit, and all endpoint restarts cover typed `NetworkError`, retained committed state and reconnect;
- deferred external-loading Busy: 1 case / 22 assertions for exact `Cancelled(InternalAbort)` → deferred `Rejected(Busy)` ordering, one `EBUSY` Admission callback, no fetch/model commit and queue drain;
- remaining production coverage: FTP/SFTP/WebDAV timeout and signed MAS/FDA security-scoped permission;
- supporting `SerialQueue` exception-safety coverage is part of the locally passed `BaseUT`: 78 cases / 70,566 assertions;
- command evidence in the same final tree: `file.copy` 6 cases / 58 assertions; `file.cut` 8 / 73; `file.rename` 5 / 64; rename editor regression 2 / 23; command presentation 10 / 69; Visual State mapper 8 / 181;
- `file.open` evidence: core definition/Registry/legacy shortcut 23 / 201, focused production route 1 / 66, combined 24 / 267; the full Registry fixture passes 3 / 122;
- focused Up/Refresh evidence covers pure command state, pane availability mapping, legacy shortcut aliases, dispatcher/menu/shortcut/toolbar routing, exact Up request semantics, forced-user versus soft refresh descriptors, missing/foreign Store presentation and live execution guards; total 32 / 549: core 22 / 281, production navigation 4 / 50, production Refresh 3 / 37, Registry surfaces 1 / 27, Explorer model/toolbar 2 / 154;
- completion snapshot for the structural `OperationPlan` foundation: 8 / 109; full `OperationsUT` 32 / 389 in Debug, Release ASAN, and Release UBSAN;
- completion snapshot for Debug, Release ASAN, and Release UBSAN `WinCommanderUT`: 213 / 2,638 in each configuration, with no sanitizer diagnostics; current aggregate evidence is maintained in `Docs/Development-Plan.md`;
- completion snapshot for local `Scripts/verify_m0.sh`: exit 0, unsigned Debug application built and all 10 aggregate unit-test binaries passed, 775 cases / 128,572 assertions; current aggregate evidence is maintained in `Docs/Development-Plan.md`.

Hosted CI has not run for the `file.open` slice. The first hosted M0 workflow run remains separate release-gate evidence.
