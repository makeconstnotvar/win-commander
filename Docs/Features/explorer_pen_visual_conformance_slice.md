# Explorer `.pen` visual-conformance slice

## Result

Explorer now implements the common shell drawn by `win-commander.pen` frames FM-02 (`rA2v2`) and FM-03 (`Qefa3`) while retaining the established AppKit window, Registry, pane, selection, provider, and inspector contracts.

The production layout is:

1. the native unified title bar and toolbar;
2. a 50 pt full-width command bar;
3. a workspace with a 252 pt locations sidebar, details list, and optional 306 pt inspector;
4. the existing status projection at the bottom of the pane.

The Explorer-owned palette resolves at draw time from the selected application appearance. Aqua uses the light values measured from the `.pen` frames; Dark Aqua uses paired elevation-preserving values. An explicit Light theme therefore reproduces the reference on a system configured for Dark appearance, while the normal Dark theme remains coherent with the same hierarchy.

## Implemented presentation contract

- `ExplorerPalette` owns the Explorer chrome, command-bar, workspace, header, and divider tokens; native controls resolve their own semantic fills and states.
- The command bar keeps the mockup height, insets, gaps, single accent action, and right-aligned Sort/View/More group. Its actions are native AppKit accessory-bar buttons at the 28 pt macOS default control size; the system cell aligns each SF Symbol and localized title as one optical unit.
- The sidebar keeps the mockup width and vertical padding while using AppKit's source-list row metric, so its scale follows the user's macOS sidebar-size preference. Location glyphs use a 20 pt optical frame; section labels use the 13 pt semibold system font. Opaque row and scroll backgrounds, single location selection, whole-header disclosure, inactive selection, and the right hairline complete the native source-list behavior.
- Details rows use the mockup 38 pt rhythm and Explorer-local insets while Commander keeps its prior geometry.
- The mounted inspector keeps a 306 pt preferred width and continues to consume the exact active pane snapshot.
- The command bar is pinned to the root safe-area layout guide, so it starts immediately below the native unified toolbar.
- The visible path belongs to the editable address control. The synchronized native window title remains available as a toolbar-hidden fallback and is visually suppressed while the Explorer toolbar is present.
- The unified toolbar uses 32 × 30 pt navigation buttons with normalized 15 pt SF Symbols. Its address surface is 34 pt high with 28 pt address/search controls; leaving the editor restores the breadcrumb without taking focus back from the control the user clicked.
- The sidebar is mounted above the legacy pane renderer. This preserves its visible rows when an established panel view paints outside its split-view bounds during bootstrap.

The remaining `.pen` frames define independent product slices over this shell: FM-20 context actions, FM-12 media browsing, FM-27 extended preview controls, FM-29 file associations, FM-34 hotkey profiles, and FM-35 terminal settings. Their existing application routes continue to use the same shell and theme foundation.

## Verification

- `xcodebuild -project Source/WinCommander/WinCommander.xcodeproj -scheme UnitTests -configuration Debug -derivedDataPath /tmp/wincommander-design-derived build` — passed.
- `WinCommanderUT 'Explorer presentation geometry *' --rng-seed 424242` — 38 cases, 1,418 assertions passed, including command-button alignment, toolbar/address metrics, breadcrumb focus/shortcut routing, and opaque List hover/selection states.
- `WinCommanderUT 'NCExplorerState tabs *' --rng-seed 424242` — 25 cases, 306 assertions passed, including hidden-files validation/execution while focus is outside the panel view.
- `WinCommanderUT 'NCMainWindowController default Explorer *' --rng-seed 424242` — 4 cases, 35 assertions passed.
- Full Debug `WinCommanderUT --rng-seed 424242` — 925 cases, 12,507 assertions passed.
- `Scripts/build_stable_dev_and_run.sh` — built, signed, installed, and launched the canonical development app.
- `Scripts/verify_stable_dev_identity.sh` — bundle ID `com.wincommander.App.CodexDev`, certificate fingerprint `66CA97F3581582C97871BCC0DFC11BEAB4C65C83`, and exact designated requirement verified.
- Native Light/Dark smoke — command-button symbol/title alignment, sidebar navigation, Sort and View popovers, row selection, and inspector refresh exercised in the signed app; the original Dark preference was restored after the Light check.
- Pixel comparison — FM-02 and FM-03 references were compared beside matching signed-app Light captures at a normalized 1x, 1196 × 600 viewport; see `design-qa.md`.
- The interaction inventory, Apple/WCAG engineering baseline, executable-test boundaries, and remaining end-to-end accessibility gates are recorded in [`explorer_interaction_contract_slice.md`](explorer_interaction_contract_slice.md).
