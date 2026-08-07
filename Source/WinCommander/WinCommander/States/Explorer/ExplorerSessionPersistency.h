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
 * Versioned window payload. The Commander value is retained as an opaque owned blob; Explorer
 * view settings remain owned by ExplorerViewSettingsPersistence and are deliberately absent.
 */
struct ExplorerWindowSession final {
    ExplorerWindowSessionMode mode = ExplorerWindowSessionMode::Commander;
    config::Value commander_state;
    std::optional<ExplorerTabsSession> explorer;
};

/** Pure codec for the Cocoa/StateConfig window-session value. */
class ExplorerSessionPersistency final
{
public:
    static constexpr int SchemaVersion = 1;
    static constexpr size_t MaximumTabs = 64;

    explicit ExplorerSessionPersistency(panel::PanelDataPersistency &_location_persistency) noexcept;

    /** Returns null when the supplied value model cannot form a valid v1 envelope. */
    [[nodiscard]] config::Value Encode(const ExplorerWindowSession &_session) const;

    /**
     * Returns false only when an existing StateConfig root declares a well-formed future schema.
     * Legacy, current, older and malformed roots remain replaceable by a valid v1 write.
     */
    [[nodiscard]] static bool CanReplaceStoredSession(const config::Value &_root) noexcept;

    /**
     * Decodes v1 or migrates the legacy panels_v1 root into Commander mode. A malformed individual
     * Explorer tab is retained as a Home tab; an invalid envelope is rejected atomically.
     */
    [[nodiscard]] std::optional<ExplorerWindowSession> Decode(const config::Value &_root) const;

private:
    panel::PanelDataPersistency &m_LocationPersistency;
};

} // namespace nc::explorer
