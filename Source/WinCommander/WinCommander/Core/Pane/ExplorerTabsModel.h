// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "PaneSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace nc::core {

enum class ExplorerTabsFailure : uint8_t {
    ZeroPaneId,
    DuplicatePaneId,
    PaneNotFound,
    IndexOutOfRange,
    LastTab
};

using ExplorerTabsMutationResult = std::expected<void, ExplorerTabsFailure>;

struct ExplorerTabObservationToken {
    PaneId pane_id;
    uint64_t generation = 0;

    friend bool operator==(const ExplorerTabObservationToken &, const ExplorerTabObservationToken &) = default;
};

/**
 * Active-tab observation identity. Each bind or invalidation retires every previously issued
 * token; acceptance additionally requires the current model-active PaneId and snapshot PaneId.
 */
class ExplorerTabObservationGate final
{
public:
    using BindResult = std::expected<ExplorerTabObservationToken, ExplorerTabsFailure>;

    [[nodiscard]] BindResult Bind(PaneId _pane_id) noexcept;
    void Invalidate() noexcept;
    [[nodiscard]] bool
    Accepts(ExplorerTabObservationToken _token, PaneId _model_active, PaneId _snapshot_pane) const noexcept;

private:
    ExplorerTabObservationToken m_Current;
};

/**
 * Toolkit-independent ordered Explorer tab identity model.
 *
 * Every valid instance contains at least one unique nonzero PaneId and exactly one active tab.
 * Failed mutations are atomic. Insert uses an index in [0, Size()], while Reorder uses the desired
 * index in the final ordering in [0, Size() - 1].
 */
class ExplorerTabsModel final
{
public:
    using CreateResult = std::expected<ExplorerTabsModel, ExplorerTabsFailure>;

    [[nodiscard]] static CreateResult Create(PaneId _initial_pane);

    [[nodiscard]] std::span<const PaneId> Panes() const noexcept { return m_Panes; }
    [[nodiscard]] PaneId Active() const noexcept { return m_Panes[m_ActiveIndex]; }
    [[nodiscard]] size_t ActiveIndex() const noexcept { return m_ActiveIndex; }
    [[nodiscard]] size_t Size() const noexcept { return m_Panes.size(); }

    [[nodiscard]] ExplorerTabsMutationResult Activate(PaneId _pane);
    [[nodiscard]] ExplorerTabsMutationResult Append(PaneId _pane);
    [[nodiscard]] ExplorerTabsMutationResult Insert(size_t _index, PaneId _pane);
    [[nodiscard]] ExplorerTabsMutationResult Close(PaneId _pane);
    [[nodiscard]] ExplorerTabsMutationResult Reorder(PaneId _pane, size_t _final_index);

private:
    explicit ExplorerTabsModel(PaneId _initial_pane);

    std::vector<PaneId> m_Panes;
    size_t m_ActiveIndex = 0;
};

} // namespace nc::core
