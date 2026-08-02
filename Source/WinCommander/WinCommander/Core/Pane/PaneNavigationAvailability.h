// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstdint>
#include <optional>

namespace nc::core {

struct PaneId;
struct PaneSnapshot;
struct PaneState;

enum class NavigationUpAvailability : uint8_t {
    PaneUnavailable,
    Busy,
    AtTop,
    HierarchyUnavailable,
    Available
};

enum class NavigationRefreshAvailability : uint8_t {
    PaneUnavailable,
    Busy,
    NoCommittedContent,
    Available
};

struct PaneNavigationAvailability {
    NavigationUpAvailability up = NavigationUpAvailability::PaneUnavailable;
    NavigationRefreshAvailability refresh = NavigationRefreshAvailability::PaneUnavailable;

    constexpr bool operator==(const PaneNavigationAvailability &) const noexcept = default;
};

/**
 * Maps one final pane projection to advisory Up/Refresh availability without mutating or retaining
 * pane state. Command execution must still re-read the live controller state before submitting work.
 */
[[nodiscard]] PaneNavigationAvailability MapPaneNavigationAvailability(const PaneState &_state) noexcept;

/** Returns no projection until the consumer has a snapshot for its exact pane identity. */
[[nodiscard]] std::optional<PaneNavigationAvailability>
MapMatchingPaneNavigationAvailability(PaneId _expected_pane, const PaneSnapshot &_snapshot) noexcept;

} // namespace nc::core
