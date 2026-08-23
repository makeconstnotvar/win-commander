# Design QA: Explorer `.pen` conformance

## Target and capture

- Source: `win-commander.pen`, FM-02 `rA2v2` and FM-03 `Qefa3`.
- Source frame: 1440 × 920 at 1x.
- Implementation: signed `~/Applications/WinCommander-Codex.app`, bundle ID `com.wincommander.App.CodexDev`.
- Captured viewport: 1196 × 600 at 1x in Light and Dark application appearances.
- Compared state: home-folder details listing, visible sidebar and command bar, selected folder, mounted inspector.
- Comparison input: `/tmp/wincommander-design-qa/reference-selected-vs-implementation.png`; the reference was cropped to the same top-left 1196 × 600 viewport before side-by-side inspection.

## Comparison history

1. The first signed-app capture exposed the command bar beneath the unified toolbar and a sidebar whose populated accessibility rows were visually covered.
2. Safe-area anchoring placed the command bar directly below the toolbar. Mounting the sidebar above the legacy pane renderer restored the visible sections, rows, icons, selection, and scroll state.
3. The final Light comparison confirms the 50 pt command bar with aligned native 28 pt accessory buttons, 252 pt sidebar, row/header rhythm, adaptive palette, details list, 306 pt inspector, and bottom status projection. The Dark capture confirms the same hierarchy and button alignment under Dark Aqua.

The native toolbar uses the platform title-bar height and system font metrics. Live filesystem names, dates, icons, and metadata supply the production content while preserving the reference hierarchy and density.

## Functional checks

- Sidebar location selection navigates the active pane.
- Light and Dark application themes switch live on a Dark system appearance.
- All ten command buttons retain one height and baseline; AppKit centers each SF Symbol and title as one unit.
- Sort and View popovers open and dismiss from the command bar.
- Selecting a listing row enables the relevant commands and refreshes inspector icon and metadata.
- The signed application remains responsive through the visual checks and preserves its designated requirement.

## Final result

passed
