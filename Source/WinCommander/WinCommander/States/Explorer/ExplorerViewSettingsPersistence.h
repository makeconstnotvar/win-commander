// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <WinCommander/Core/Pane/PaneSnapshot.h>
#include <WinCommander/States/FilePanels/PanelDataPersistency.h>
#include <WinCommander/States/FilePanels/PanelViewLayoutSupport.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>

namespace nc::config {
class Config;
}

namespace nc::explorer {

/** Exact toolkit-independent settings persisted for one Explorer location. */
struct ExplorerViewSettings final {
    int32_t layout_slot = -1;
    panel::PanelViewLayout layout;
    core::PaneSortState sort;
    core::PaneGroupingState grouping;

    bool operator==(const ExplorerViewSettings &) const noexcept = default;
};

/**
 * StateConfig-backed bounded per-location Explorer settings.
 *
 * Records are kept in deterministic MRU order. A footprint is only a lookup accelerator: a record
 * is accepted solely after its complete persisted location equals LocationToJSON for the request.
 * The supplied Config and PanelDataPersistency are non-owning and must outlive this store.
 */
class ExplorerViewSettingsPersistence final
{
public:
    static constexpr std::string_view ConfigPath = "filePanel.explorer.viewSettingsByLocation_v1";
    static constexpr int SchemaVersion = 1;
    static constexpr size_t Capacity = 512;

    ExplorerViewSettingsPersistence(config::Config &_config, panel::PanelDataPersistency &_location_persistency);

    [[nodiscard]] std::optional<ExplorerViewSettings> Load(const panel::PersistentLocation &_location);
    [[nodiscard]] bool Store(const panel::PersistentLocation &_location, const ExplorerViewSettings &_settings);

private:
    config::Config &m_Config;
    panel::PanelDataPersistency &m_LocationPersistency;
    std::mutex m_Mutex;
};

} // namespace nc::explorer
