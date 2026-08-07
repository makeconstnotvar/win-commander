# Q1-3 Explorer Details and Preview Registry slice

> Status: production implementation locally verified on 2026-08-07
> Canonical requirements: `Docs/win_commander_ideal_file_manager_spec.md` sections 17, 18 and 32
> Execution tracker: `Docs/Development-Plan.md`, Queue 1 item Q1-3

## Product surface

Explorer mounts a right-hand details pane beside the active file panel. The split starts at 320 points, retains a user-adjusted width from 280 to 520 points for the lifetime of the state and preserves at least 360 points for the listing. The existing quick-search overlay remains attached to the panel side of the split.

The inspector renders the exact matching `PaneSnapshot` through a toolkit-independent model. It distinguishes initial, empty, loading, refreshing, failed, single-item and multi-selection states. A single item shows an embedded preview, filename, path, byte size, created/modified/accessed dates, tags, POSIX permissions and numeric owner/group. A multi-selection shows count and aggregate size when every size is known. Retained refresh content stays visible with a progress indicator, while typed pane errors appear as a separate banner with accessibility help.

Preview ownership reuses `NCPanelGalleryCentralView`, `QuickLookVFSBridge`, the application UTI database and the configured hazardous-extension list. Preview ticket invalidation prevents a late asynchronous result from repopulating cleared or detached content.

## Registry commands and entry points

The application Registry now owns three stable definitions:

| Command | Mouse surface | Keyboard surface | Live authority |
|---|---|---|---|
| `file.getInfo` | File menu, exact-item context menu, Explorer More | Command-I | exact Explorer pane/listing metadata presentation |
| `file.preview` | exact-item context menu, Explorer More | Space and existing Quick Look aliases | exact current-listing item revalidation before Quick Look |
| `view.togglePreviewPane` | View menu, Explorer View popover, Explorer More | Control-Command-P | compare-and-set visibility on the exact Explorer state |

Commander has no details-pane presenter and therefore fails closed for `file.getInfo` and `view.togglePreviewPane`; its existing mutating File Attributes action remains distinct. Floating Quick Look remains available through `file.preview` and uses the application-owned `QLPanelAdaptor` in Explorer independently of Commander split-view configuration.

Every surface projects the same `CommandState`, check state and localized disabled reason. Disabled AppKit items retain tooltip/accessibility help and no executable target/action.

## Exact-item and stale-state boundary

`file.getInfo` and `file.preview` receive the captured context-menu item vector rather than re-reading the current focus. A single Get Info request resolves exactly one matching item in the current committed listing, moves the cursor to its sort position and repeats revision, listing-generation, listing identity and focused item checks before presentation. Batch Get Info requires exact equality with the current selected vector and its order.

Quick Look receives one borrowed exact `ListingItem`, rejects invalid and parent entries, resolves its live sort position and repeats listing/index equality after cursor placement before handing off to the established preview action. Space retains its originating `NSEvent` and is classified as `Shortcut`. A stale listing, changed visibility snapshot, foreign panel, unavailable presenter or rejected preview produces no presentation mutation.

## Verification

- Debug command contracts: `file.getInfo` 6 cases / 125 assertions; `file.preview` 5 / 91; `view.togglePreviewPane` 6 / 72.
- Debug inspector model: 6 cases / 97 assertions.
- Debug Inspector/AppKit/state integration: 8 cases / 111 assertions.
- Debug exact production dispatcher route: 1 case / 46 assertions.
- Debug Explorer presentation geometry: 25 cases / 1,045 assertions.
- Full Debug `WinCommanderUT --rng-seed 424242`: 420 / 425 cases and 7,435 / 7,439 assertions; four failures are the established headless pasteboard-server host baseline and one case is an expected environment skip.
- Release ASAN and UBSAN Inspector/model/preview/dispatcher filters: 20 cases / 345 assertions in each runtime, with no sanitizer diagnostics.
- The arm64 Debug `WinCommander-Unsigned` application target built successfully with code signing disabled.
- `project.pbxproj`, string catalogs and `MainMenu.xib` parsed successfully; `git diff --check` passed.

Signed stable-development interaction, VoiceOver walkthrough and screenshot evidence remain Q1-10/release gates. The local closure proves the mounted production composition, exact Registry routing, fail-closed stale-state behavior and sanitizer-covered preview lifetime.
