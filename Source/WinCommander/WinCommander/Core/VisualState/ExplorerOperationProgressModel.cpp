// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ExplorerOperationProgressModel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace nc::core {

namespace {

constexpr uint8_t Priority(const ExplorerOperationLifecycle _lifecycle) noexcept
{
    switch( _lifecycle ) {
        case ExplorerOperationLifecycle::WaitingForUser:
            return 0;
        case ExplorerOperationLifecycle::Running:
            return 1;
        case ExplorerOperationLifecycle::Paused:
            return 2;
        case ExplorerOperationLifecycle::Finalizing:
            return 3;
        case ExplorerOperationLifecycle::Queued:
            return 4;
    }
    return std::numeric_limits<uint8_t>::max();
}

constexpr bool HasProgress(const ExplorerOperationProgressSource &_source) noexcept
{
    return _source.total != 0 || _source.processed != 0;
}

const ExplorerOperationProgressSource &Source(const ExplorerOperationProgressInput &_input,
                                              const ExplorerOperationProgressUnit _unit) noexcept
{
    return _unit == ExplorerOperationProgressUnit::Bytes ? _input.bytes : _input.items;
}

constexpr ExplorerOperationProgressUnit Other(const ExplorerOperationProgressUnit _unit) noexcept
{
    return _unit == ExplorerOperationProgressUnit::Bytes ? ExplorerOperationProgressUnit::Items
                                                         : ExplorerOperationProgressUnit::Bytes;
}

ExplorerOperationProgressUnit SelectUnit(const ExplorerOperationProgressInput &_input) noexcept
{
    if( HasProgress(Source(_input, _input.preferred_unit)) )
        return _input.preferred_unit;

    const auto alternate = Other(_input.preferred_unit);
    return HasProgress(Source(_input, alternate)) ? alternate : _input.preferred_unit;
}

std::optional<double> NormalizeSpeed(const double _speed) noexcept
{
    if( !std::isfinite(_speed) || _speed <= 0. )
        return std::nullopt;
    return _speed;
}

std::optional<std::chrono::seconds> NormalizeETA(const std::optional<std::chrono::nanoseconds> &_eta) noexcept
{
    if( !_eta || _eta->count() < 0 )
        return std::nullopt;

    const auto whole_seconds = std::chrono::duration_cast<std::chrono::seconds>(*_eta);
    if( *_eta == whole_seconds )
        return whole_seconds;
    return whole_seconds + std::chrono::seconds{1};
}

} // namespace

std::optional<ExplorerOperationProgressSnapshot>
ExplorerOperationProgressModel::Build(const std::span<const ExplorerOperationProgressInput> _operations)
{
    if( _operations.empty() )
        return std::nullopt;

    const auto primary = std::ranges::min_element(_operations, {}, [](const auto &_operation) {
        return std::tuple{Priority(_operation.lifecycle), _operation.stable_order};
    });
    const ExplorerOperationProgressUnit unit = SelectUnit(*primary);
    const ExplorerOperationProgressSource &source = Source(*primary, unit);
    const bool indeterminate = source.total == 0;
    const uint64_t processed = indeterminate ? source.processed : std::min(source.processed, source.total);
    const double fraction = indeterminate ? 0. : static_cast<double>(processed) / static_cast<double>(source.total);

    ExplorerOperationProgressSnapshot snapshot;
    snapshot.title = primary->title;
    snapshot.current_item_path = primary->current_item_path;
    snapshot.lifecycle = primary->lifecycle;
    snapshot.unit = unit;
    snapshot.processed = processed;
    snapshot.total = source.total;
    snapshot.fraction = std::clamp(fraction, 0., 1.);
    snapshot.speed_per_second = NormalizeSpeed(source.speed_per_second);
    snapshot.eta = NormalizeETA(source.eta);
    snapshot.indeterminate = indeterminate;
    snapshot.active_count = _operations.size();
    snapshot.additional_count = _operations.size() - 1;
    return snapshot;
}

} // namespace nc::core
