# Explorer folder visual states

> Status: Queue 1 Q1-7 complete
>
> Execution tracker: `Docs/Development-Plan.md`, Queue 1 item Q1-7

## User-visible contract

The active Explorer folder surface presents its lifecycle as one coherent state. An initial load shows a bounded seven-row skeleton, activity indicator and localized loading message. A successfully loaded empty folder shows a neutral folder symbol and explicit empty message. Typed blocking states present permission denied, missing folder, disconnected volume, unavailable remote location, unsupported location and generic failure with distinct system symbols and the mapper-provided user explanation.

The mounted file view remains the content owner. A failed refresh with a committed listing keeps that listing visible; the pane-state overlay stays hidden while the breadcrumb and status projection retain the typed failure. A later Ready or Refreshing snapshot also keeps the overlay hidden. This preserves current files and interaction during recoverable refresh failures.

## State and presentation ownership

`VisualStateMapper` is the single semantic projection from the admitted `PaneSnapshot` and `FileManagerError` to `PaneVisualState`. `NCExplorerState` forwards only the exact active tab's generation- and `PaneId`-validated snapshot to `NCExplorerPaneStateView`; tab rebinding clears the previous presentation before the new active entry can publish.

`NCExplorerPaneStateView` is a render adapter. It selects the skeleton, symbol, tint and localized text from `PaneVisualKind` and `PaneStatusVisualState`, while provider and error classification remain upstream. The existing permission path maps a real Native `EACCES` failure with exact context to `PermissionError`. For a committed uniform Native listing, `PanelController` now binds that exact listing and generation to the `NativeFSManager::Info` resolved at commit. A failed refresh becomes `VolumeUnavailableError` only when that exact volume identity is absent from the manager snapshot; an unknown lookup remains the original typed failure, and a remount at the same path has a different identity. The mapper then projects `VolumeUnavailableError` to `VolumeDisconnected`.

This classification keeps the committed listing and location visible and suppresses invalid-directory recovery, so the user sees the disconnected state for the volume that actually owned the displayed content. A later successful refresh replaces the binding from the newly committed listing. The physical external-drive UI walkthrough remains release evidence; the production classification itself is covered deterministically with exact mount identities.

## Inline rename contract

Inline rename now validates and commits against one exact live folder context. `PanelController` captures the committed listing pointer, listing generation, source item/index, provider instance and directory, and `PlanInlineRename` admits only that exact source while the pane and window are available and the listing is stable. The plan checks the provider's path-aware writable and rename capabilities, obtains authoritative case sensitivity, canonicalizes filename comparison, and rejects invalid names and collisions before an operation is created.

Case-only rename is admitted on a case-insensitive Native provider, where the runtime check can prove that source and destination names identify the same device/inode entry. Generic VFS providers require a distinct collision-free destination name. Commit reuses `nc::ops::Copying` with `docopy = false`, `ExistBehavior::Stop` and `DestinationPathInterpretation::ExactItem`. Its provider-only runtime preflight repeats the source type/inode, destination absence or exact Native same-entry proof, writable capability and case-sensitivity claims without retaining `PanelController`, `PanelView` or window objects.

The field editor treats validation as an inline interaction. A typed validation failure keeps the editor and callbacks active, selects the proposed name, exposes the localized reason through its tooltip and accessibility help, and accepts a corrected submission. An unchanged name closes as a successful no-op. Escape cancels without submitting. Replacing the listing pointer discards the editor instead of reattaching it to a name-compatible item from a different listing. Accepted completion uses a weak controller callback and refreshes/focuses only while the same provider and directory remain current.

## Drag-and-drop contract

Local panel drag-and-drop now resolves Copy, Move or Link through the side-effect-free `DragDropPolicy`. Its immutable input contains normalized modifier intent, complete source facts, the exact destination directory, path-scoped `ProviderCapabilities`, provider identities and, for Native paths, exact mounted-volume identities. Automatic intent selects Move only inside a proven move domain with the required source and destination capabilities; cross-provider or cross-volume intent resolves to Copy. Explicit Copy, Move and Native Link remain subject to the same capability, path and identity checks. Same-folder, recursive, malformed, duplicate, read-only, unsupported and unknown-identity inputs are rejected before an operation is constructed.

`FilesDraggingSource` seals the source listing pointer, listing generation and every dragged listing item at drag creation. `DragReceiver::Validate` additionally seals the target listing pointer, generation and exact item under the pointer. `PanelView` retains that validated receiver for the matching dragging session and target index, then consumes the same instance in `Receive`; an unvalidated or replaced receiver cannot execute.

Each AppKit validation pass re-reads the current operation mask and modifier state, rebuilds the policy input and returns the matching Copy, Move or Link badge together with the valid-item count. Immediately before enqueue, local receive repeats the exact source and target seals, rebuilds the policy from fresh provider, capability and Native-volume facts, and requires the complete decision input and operation to match validation. Only that identical decision constructs the established `nc::ops::Copying` or `nc::ops::Linkage` operation. Stale listings/items, modifier drift, read-only paths, capability changes, unresolved providers or unknown Native volumes therefore fail closed without reaching the window operation queue.

## Accessibility and appearance

The overlay is one accessibility group with a stable identifier, localized label and current message as its accessibility value. Skeleton rows are decorative and excluded from the accessibility tree, so VoiceOver announces one loading state. Blocking states pair their color with a symbol and text. The opaque surface uses semantic AppKit colors and SF Symbols, preserving appearance under system light/dark changes.

## Q1-7 boundary

Q1-7 is split into four independently verifiable slices:

- Q1-7a: active-folder loading, empty and typed blocking-state presentation — complete.
- Q1-7b: production permission and exact disconnected-volume classification — complete; physical external-drive UI walkthrough remains release evidence.
- Q1-7c: exact inline rename validation and legacy `Copying(docopy = false)` commit — complete.
- Q1-7d: exact local drag-and-drop Copy/Move/Link policy, modifier-reactive AppKit badges/count and matching legacy operation routing — complete.

Copy-conflict warning and resolver presentation belongs to Q1-8 together with operation progress; it is outside the Q1-7d intent-routing boundary.

## Verification

The confirmed current-tree results are:

- focused mapper, pane-state view, active Explorer integration and refresh classification: 40 cases / 585 assertions passed;
- Q1-7b Release ASAN and Release UBSAN: 40 cases / 585 assertions passed in each runtime without diagnostics;
- focused pure inline-rename planning/runtime coverage: 7 cases passed;
- focused `PanelViewFieldEditor` interaction coverage: 3 cases / 23 assertions passed;
- focused pure drag/drop policy and exact production source-seal coverage: 10 cases / 113 assertions passed;
- combined Q1-7 `WinCommanderUT` coverage: 19 cases / 179 assertions passed;
- full Debug `WinCommanderUT`: 507 cases / 9,030 assertions passed;
- full `PanelUT`: 54 cases / 1,349 assertions total, with 53 cases / 1,348 assertions passed and one expected host-UI failure / one assertion; process exit status was zero;
- Debug `WinCommander-Unsigned` build passed.

The focused set covers initial loading, empty folder, all typed pane error kinds, localization fallback, accessibility value/role/identifier, semantic appearance, retained-content refresh failure, exact active-tab mounting, real Native permission denial, absent exact mount identity, same-path remount rejection, manager-unavailable fail-closed behavior and recovery after the replacement volume is committed. Inline-rename coverage exercises exact live-context admission, invalid/stale/read-only/unsupported states, case-aware collisions, Native case-only proof, runtime source and destination drift, operation construction, retained validation and correction, empty-name validation, and Escape cancellation. Drag/drop coverage exercises modifier normalization, automatic and explicit Copy/Move/Link selection, path-scoped source/destination capabilities, provider and Native-volume identity, same-folder/recursive/malformed/duplicate rejection, multi-source inputs, decision-input retention and the exact source listing/generation/item seal including an equal-value replacement listing. The combined focused, full `WinCommanderUT`, full `PanelUT` and unsigned application build gates close Q1-7.
