// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace nc::core {

enum class ExplorerOperationLifecycle : uint8_t {
    Queued,
    Running,
    Paused,
    WaitingForUser,
    Finalizing
};

enum class ExplorerOperationProgressUnit : uint8_t {
    Bytes,
    Items
};

/** A copied progress source. It never retains an operation or its mutable Statistics object. */
struct ExplorerOperationProgressSource {
    uint64_t processed = 0;
    uint64_t total = 0;
    double speed_per_second = 0.;
    std::optional<std::chrono::nanoseconds> eta;

    bool operator==(const ExplorerOperationProgressSource &) const = default;
};

/** A single copied Pool projection. stable_order is the position in the copied Pool::Operations() result. */
struct ExplorerOperationProgressInput {
    uint64_t stable_order = 0;
    std::string title;
    std::optional<std::string> current_item_path;
    ExplorerOperationLifecycle lifecycle = ExplorerOperationLifecycle::Queued;
    ExplorerOperationProgressUnit preferred_unit = ExplorerOperationProgressUnit::Bytes;
    ExplorerOperationProgressSource bytes;
    ExplorerOperationProgressSource items;

    bool operator==(const ExplorerOperationProgressInput &) const = default;
};

/**
 * Bounded value-only projection for Explorer's compact operation surface.
 *
 * processed is clamped to total for determinate work. Invalid speed and ETA samples are omitted. active_count and
 * additional_count describe the full input set while the remaining fields describe its deterministic primary item.
 */
struct ExplorerOperationProgressSnapshot {
    std::string title;
    std::optional<std::string> current_item_path;
    ExplorerOperationLifecycle lifecycle = ExplorerOperationLifecycle::Queued;
    ExplorerOperationProgressUnit unit = ExplorerOperationProgressUnit::Bytes;
    uint64_t processed = 0;
    uint64_t total = 0;
    double fraction = 0.;
    std::optional<double> speed_per_second;
    std::optional<std::chrono::seconds> eta;
    bool indeterminate = true;
    size_t active_count = 0;
    size_t additional_count = 0;

    bool operator==(const ExplorerOperationProgressSnapshot &) const = default;
};

class ExplorerOperationProgressModel final
{
public:
    /** Returns no snapshot for an empty input and never retains storage from the supplied span. */
    [[nodiscard]] static std::optional<ExplorerOperationProgressSnapshot>
    Build(std::span<const ExplorerOperationProgressInput> _operations);
};

} // namespace nc::core
