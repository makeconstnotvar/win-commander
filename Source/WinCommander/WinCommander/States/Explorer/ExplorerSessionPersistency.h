// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Config/RapidJSON.h>
#include <WinCommander/States/FilePanels/PanelDataPersistency.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace nc::explorer {

enum class ExplorerWindowSessionMode : uint8_t {
    Commander,
    Explorer
};

/** One ordered Explorer tab. A missing location is the explicit Native Home fallback. */
struct ExplorerSessionTab final {
    std::optional<panel::PersistentLocation> location;
};

struct ExplorerTabsSession final {
    std::vector<ExplorerSessionTab> tabs;
    size_t active_index = 0;
};

/**
 * One window's Explorer pane layout. The right side is engaged exactly when dual-pane mode is on,
 * so its presence - not a separate flag - is the single authority for that mode; `right_focused`
 * and `divider_ratio` are meaningful only alongside it. `divider_ratio` is the left side's share of
 * the split's usable width; an absent value means the even default split.
 */
struct ExplorerPanesSession final {
    ExplorerTabsSession left;
    std::optional<ExplorerTabsSession> right;
    bool right_focused = false;
    std::optional<double> divider_ratio;
};

/**
 * Versioned window payload. The Commander value is retained as an opaque owned blob; Explorer
 * view settings remain owned by ExplorerViewSettingsPersistence and are deliberately absent.
 */
struct ExplorerWindowSession final {
    ExplorerWindowSessionMode mode = ExplorerWindowSessionMode::Commander;
    config::Value commander_state;
    std::optional<ExplorerPanesSession> explorer;
};

/** Pure codec for the Cocoa/StateConfig window-session value. */
class ExplorerSessionPersistency final
{
public:
    /** v2 adds the dual-pane layout; v1 remains readable and decodes as a single left pane. */
    static constexpr int SchemaVersion = 2;
    static constexpr int MinimumReadableSchemaVersion = 1;
    static constexpr size_t MaximumTabs = 64;

    /** Usable band for a persisted divider ratio; a value outside it degrades to the even split. */
    static constexpr double MinimumDividerRatio = 0.05;
    static constexpr double MaximumDividerRatio = 0.95;

    explicit ExplorerSessionPersistency(panel::PanelDataPersistency &_location_persistency) noexcept;

    /** Returns null when the supplied value model cannot form a valid v2 envelope. */
    [[nodiscard]] config::Value Encode(const ExplorerWindowSession &_session) const;

    /**
     * Returns false only when an existing StateConfig root declares a well-formed future schema.
     * Legacy, current, older and malformed roots remain replaceable by a valid v1 write.
     */
    [[nodiscard]] static bool CanReplaceStoredSession(const config::Value &_root) noexcept;

    /**
     * Decodes v1 or v2, or migrates the legacy panels_v1 root into Commander mode. A malformed
     * individual Explorer tab is retained as a Home tab, and a malformed focused side or divider
     * ratio degrades to its exact safe default; an invalid envelope - including a present but
     * undecodable right pane, which carries its own ordered tab locations - is rejected atomically.
     */
    [[nodiscard]] std::optional<ExplorerWindowSession> Decode(const config::Value &_root) const;

private:
    panel::PanelDataPersistency &m_LocationPersistency;
};

} // namespace nc::explorer
