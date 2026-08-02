// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PaneNavigationAvailability.h"

#include "PaneSnapshot.h"
#include <VFS/Host.h>
#include <string_view>

namespace nc::core {

namespace {

[[nodiscard]] bool HasCommittedPaneContent(const PaneState &_state) noexcept
{
    return _state.listing && _state.listing != VFSListing::EmptyListing();
}

[[nodiscard]] bool IsCanonicalPaneDirectoryPath(const std::string_view _path) noexcept
{
    if( _path.empty() || _path.front() != '/' || _path.back() != '/' )
        return false;
    if( _path == "/" )
        return true;

    size_t component_begin = 1;
    while( component_begin < _path.size() ) {
        const size_t component_end = _path.find('/', component_begin);
        if( component_end == std::string_view::npos )
            return false;
        const std::string_view component = _path.substr(component_begin, component_end - component_begin);
        if( component.empty() || component == "." || component == ".." )
            return false;
        component_begin = component_end + 1;
    }
    return true;
}

[[nodiscard]] bool EmptyPaneFactsAreConsistent(const PaneState &_state) noexcept
{
    return !HasCommittedPaneContent(_state) && !_state.is_uniform && !_state.host && _state.path.empty();
}

} // namespace

PaneNavigationAvailability MapPaneNavigationAvailability(const PaneState &_state) noexcept
{
    if( _state.load_phase == PaneLoadPhase::Loading ) {
        return {
            .up = NavigationUpAvailability::Busy,
            .refresh = NavigationRefreshAvailability::Busy,
        };
    }

    if( _state.load_phase == PaneLoadPhase::Empty ) {
        if( !EmptyPaneFactsAreConsistent(_state) )
            return {};
        return {
            .up = NavigationUpAvailability::HierarchyUnavailable,
            .refresh = NavigationRefreshAvailability::NoCommittedContent,
        };
    }

    switch( _state.load_phase ) {
        case PaneLoadPhase::Loaded:
        case PaneLoadPhase::Refreshing:
        case PaneLoadPhase::Failed:
            break;
        case PaneLoadPhase::Empty:
        case PaneLoadPhase::Loading:
            return {};
        default:
            return {};
    }

    if( !HasCommittedPaneContent(_state) ) {
        return {
            .up = NavigationUpAvailability::HierarchyUnavailable,
            .refresh = NavigationRefreshAvailability::NoCommittedContent,
        };
    }

    PaneNavigationAvailability availability{
        .up = NavigationUpAvailability::HierarchyUnavailable,
        .refresh = NavigationRefreshAvailability::Available,
    };
    if( !_state.is_uniform )
        return availability;

    if( !_state.host || !IsCanonicalPaneDirectoryPath(_state.path) ) {
        availability.up = NavigationUpAvailability::PaneUnavailable;
        return availability;
    }

    if( _state.path != "/" ) {
        availability.up = NavigationUpAvailability::Available;
        return availability;
    }

    if( !_state.host->Parent() ) {
        availability.up = NavigationUpAvailability::AtTop;
        return availability;
    }
    availability.up = _state.host->JunctionPath().empty() ? NavigationUpAvailability::PaneUnavailable
                                                          : NavigationUpAvailability::Available;
    return availability;
}

std::optional<PaneNavigationAvailability>
MapMatchingPaneNavigationAvailability(const PaneId _expected_pane,
                                      const PaneSnapshot &_snapshot) noexcept
{
    if( _expected_pane.value == 0 || _snapshot.pane_id != _expected_pane )
        return std::nullopt;
    return MapPaneNavigationAvailability(_snapshot.state);
}

} // namespace nc::core
