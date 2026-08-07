// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "ExplorerViewSettingsPersistence.h"

#include <WinCommander/Core/Pane/PaneSnapshot.h>

#include <cstdint>
#include <optional>
#include <string>

namespace nc::explorer {

/** Exact runtime identity and complete settings sampled from one pane snapshot. */
struct ExplorerViewSettingsObservation final {
    core::PaneId pane_id;
    /** Monotonic per-tab sampling sequence, including full-layout context notifications. */
    uint64_t observation_sequence = 0;
    /** PaneStore revision used independently to reject delayed snapshots. */
    uint64_t revision = 0;
    unsigned long location_generation = 0;
    core::PaneLoadPhase load_phase = core::PaneLoadPhase::Empty;
    bool is_uniform = false;
    const void *host_identity = nullptr;
    std::string path;
    std::optional<ExplorerViewSettings> settings;
};

enum class ExplorerViewSettingsBindingAction : uint8_t {
    Rejected,
    None,
    LoadLocation,
    StoreCurrent,
    RestoreSettled,
    RestoreDiverged
};

/**
 * Per-tab policy which fences stale locations and restore-generated intermediate snapshots.
 *
 * Persistence and controller mutation remain outside this pure policy. A caller acknowledges a
 * successful store (or an intentionally retained current value) through AcceptCurrent and starts
 * an applied restore through BeginRestore.
 */
class ExplorerViewSettingsBindingPolicy final
{
public:
    explicit ExplorerViewSettingsBindingPolicy(core::PaneId _pane_id) noexcept;

    [[nodiscard]] ExplorerViewSettingsBindingAction Observe(const ExplorerViewSettingsObservation &_observation);
    [[nodiscard]] bool AcceptCurrent(const ExplorerViewSettingsObservation &_observation) noexcept;
    [[nodiscard]] bool BeginRestore(const ExplorerViewSettings &_target) noexcept;

private:
    struct LocationKey final {
        unsigned long generation = 0;
        const void *host_identity = nullptr;
        std::string path;

        bool operator==(const LocationKey &) const noexcept = default;
    };

    [[nodiscard]] bool IsCurrent(const ExplorerViewSettingsObservation &_observation) const noexcept;

    core::PaneId m_PaneId;
    std::optional<LocationKey> m_Location;
    std::optional<uint64_t> m_LastRevision;
    std::optional<uint64_t> m_LastObservationSequence;
    std::optional<ExplorerViewSettings> m_Current;
    std::optional<ExplorerViewSettings> m_RestoreTarget;
    uint64_t m_RestoreFenceSequence = 0;
};

} // namespace nc::explorer
