// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PaneNavigationCommand.h"
#include "CommandIds.h"
#include <utility>

namespace nc::core {

namespace {

DisabledReason Disabled(std::string _code, std::string _key, std::string _technical)
{
    return DisabledReason{
        .code = std::move(_code),
        .user_message_key = std::move(_key),
        .technical_message = std::move(_technical),
    };
}

CommandState UpState(const CommandContext &_context)
{
    CommandState state;
    if( !_context.native_target ) {
        state.enabled = false;
        state.disabled_reason = Disabled("context.paneTargetRequired",
                                         "commands.navigation.up.disabled.paneUnavailable",
                                         "The navigation.up command requires a live pane target.");
        return state;
    }
    if( !_context.navigation_up_availability ) {
        state.enabled = false;
        state.disabled_reason = Disabled(
            "context.navigationUpAvailabilityRequired",
            "commands.navigation.up.disabled.stateUnavailable",
            "The navigation.up command requires a current availability projection.");
        return state;
    }

    switch( *_context.navigation_up_availability ) {
        case NavigationUpAvailability::PaneUnavailable:
            state.enabled = false;
            state.disabled_reason = Disabled("pane.unavailable",
                                             "commands.navigation.up.disabled.paneUnavailable",
                                             "The pane is unavailable for navigation.up.");
            break;
        case NavigationUpAvailability::Busy:
            state.enabled = false;
            state.disabled_reason = Disabled("pane.busy",
                                             "commands.navigation.up.disabled.busy",
                                             "The pane is busy and cannot navigate up.");
            break;
        case NavigationUpAvailability::AtTop:
            state.enabled = false;
            state.disabled_reason = Disabled("navigation.up.atTop",
                                             "commands.navigation.up.disabled.atTop",
                                             "The current pane location has no enclosing location.");
            break;
        case NavigationUpAvailability::HierarchyUnavailable:
            state.enabled = false;
            state.disabled_reason = Disabled(
                "navigation.up.hierarchyUnavailable",
                "commands.navigation.up.disabled.hierarchyUnavailable",
                "The current pane location does not expose a navigable hierarchy.");
            break;
        case NavigationUpAvailability::Available:
            break;
        default:
            state.enabled = false;
            state.disabled_reason = Disabled(
                "context.navigationUpAvailabilityInvalid",
                "commands.navigation.up.disabled.stateUnavailable",
                "The navigation.up availability projection is invalid.");
            break;
    }
    return state;
}

CommandState RefreshState(const CommandContext &_context)
{
    CommandState state;
    if( !_context.native_target ) {
        state.enabled = false;
        state.disabled_reason = Disabled(
            "context.paneTargetRequired",
            "commands.navigation.refresh.disabled.paneUnavailable",
            "The navigation.refresh command requires a live pane target.");
        return state;
    }
    if( !_context.navigation_refresh_availability ) {
        state.enabled = false;
        state.disabled_reason = Disabled(
            "context.navigationRefreshAvailabilityRequired",
            "commands.navigation.refresh.disabled.stateUnavailable",
            "The navigation.refresh command requires a current availability projection.");
        return state;
    }

    switch( *_context.navigation_refresh_availability ) {
        case NavigationRefreshAvailability::PaneUnavailable:
            state.enabled = false;
            state.disabled_reason = Disabled("pane.unavailable",
                                             "commands.navigation.refresh.disabled.paneUnavailable",
                                             "The pane is unavailable for navigation.refresh.");
            break;
        case NavigationRefreshAvailability::Busy:
            state.enabled = false;
            state.disabled_reason = Disabled("pane.busy",
                                             "commands.navigation.refresh.disabled.busy",
                                             "The pane is busy and cannot refresh.");
            break;
        case NavigationRefreshAvailability::NoCommittedContent:
            state.enabled = false;
            state.disabled_reason = Disabled(
                "navigation.refresh.noCommittedContent",
                "commands.navigation.refresh.disabled.noCommittedContent",
                "The pane has no committed content to refresh.");
            break;
        case NavigationRefreshAvailability::Available:
            break;
        default:
            state.enabled = false;
            state.disabled_reason = Disabled(
                "context.navigationRefreshAvailabilityInvalid",
                "commands.navigation.refresh.disabled.stateUnavailable",
                "The navigation.refresh availability projection is invalid.");
            break;
    }
    return state;
}

CommandDescriptor UpDescriptor()
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::NavigationUp};
    descriptor.title_key = "commands.navigation.up.title";
    descriptor.description_key = "commands.navigation.up.description";
    descriptor.category = CommandCategory::Navigation;
    descriptor.icon_name = "chevron.up";
    descriptor.is_destructive = false;
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "navigation.up";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "OnGoToUpperDirectory:",
        .shortcut_action_names = {"menu.go.enclosing_folder", "panel.go_into_enclosing_folder"},
        .shortcut_tag = 14'020,
    };
    return descriptor;
}

CommandDescriptor RefreshDescriptor()
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::NavigationRefresh};
    descriptor.title_key = "commands.navigation.refresh.title";
    descriptor.description_key = "commands.navigation.refresh.description";
    descriptor.category = CommandCategory::Navigation;
    descriptor.icon_name = "arrow.clockwise";
    descriptor.is_destructive = false;
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "navigation.refresh";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "OnRefreshPanel:",
        .shortcut_action_names = {"menu.view.refresh"},
        .shortcut_tag = 13'040,
    };
    return descriptor;
}

} // namespace

NavigationUpExecutionError::NavigationUpExecutionError()
    : std::runtime_error{"The pane could not navigate to its enclosing location."}
{
}

NavigationRefreshExecutionError::NavigationRefreshExecutionError()
    : std::runtime_error{"The pane contents could not be refreshed."}
{
}

CommandRegistry::Registration MakeNavigationUpCommand(NavigationUpExecutor _executor)
{
    CommandRegistry::Registration registration;
    registration.descriptor = UpDescriptor();
    registration.state_provider = UpState;
    if( _executor ) {
        registration.handler = [_executor = std::move(_executor)](const CommandContext &_context) {
            if( !_executor(_context.native_target) )
                throw NavigationUpExecutionError{};
        };
    }
    return registration;
}

CommandRegistry::Registration MakeNavigationRefreshCommand(NavigationRefreshExecutor _executor)
{
    CommandRegistry::Registration registration;
    registration.descriptor = RefreshDescriptor();
    registration.state_provider = RefreshState;
    if( _executor ) {
        registration.handler = [_executor = std::move(_executor)](const CommandContext &_context) {
            if( !_executor(_context.native_target) )
                throw NavigationRefreshExecutionError{};
        };
    }
    return registration;
}

} // namespace nc::core
