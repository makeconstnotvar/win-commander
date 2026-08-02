// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NavigationHistoryCommand.h"
#include "CommandIds.h"
#include <utility>

namespace nc::core {

namespace {

struct Definition {
    NavigationHistoryDirection direction;
    std::string_view id;
    std::string_view name;
    std::string_view icon;
    std::string_view selector;
    std::string_view shortcut_action;
    int shortcut_tag;
};

constexpr Definition Back{
    .direction = NavigationHistoryDirection::Back,
    .id = command_ids::NavigationBack,
    .name = "back",
    .icon = "chevron.left",
    .selector = "OnGoBack:",
    .shortcut_action = "menu.go.back",
    .shortcut_tag = 14'000,
};

constexpr Definition Forward{
    .direction = NavigationHistoryDirection::Forward,
    .id = command_ids::NavigationForward,
    .name = "forward",
    .icon = "chevron.right",
    .selector = "OnGoForward:",
    .shortcut_action = "menu.go.forward",
    .shortcut_tag = 14'010,
};

DisabledReason Disabled(std::string _code, std::string _key, std::string _technical)
{
    return DisabledReason{
        .code = std::move(_code),
        .user_message_key = std::move(_key),
        .technical_message = std::move(_technical),
    };
}

std::optional<bool> Availability(const CommandContext &_context,
                                 const NavigationHistoryDirection _direction) noexcept
{
    return _direction == NavigationHistoryDirection::Back ? _context.can_go_back
                                                          : _context.can_go_forward;
}

CommandState State(const CommandContext &_context, const Definition &_definition)
{
    CommandState state;
    const std::string command_name = "navigation." + std::string{_definition.name};
    if( !_context.native_target ) {
        state.enabled = false;
        state.disabled_reason = Disabled(
            "context.paneTargetRequired",
            "commands." + command_name + ".disabled.paneUnavailable",
            "The " + command_name + " command requires a live pane target.");
        return state;
    }

    const std::optional<bool> available = Availability(_context, _definition.direction);
    if( !available ) {
        state.enabled = false;
        state.disabled_reason = Disabled(
            "context.historyAvailabilityRequired",
            "commands." + command_name + ".disabled.stateUnavailable",
            "The " + command_name + " command requires a current pane history snapshot.");
        return state;
    }
    if( !*available ) {
        state.enabled = false;
        state.disabled_reason = Disabled(
            _definition.direction == NavigationHistoryDirection::Back ? "navigation.backUnavailable"
                                                                      : "navigation.forwardUnavailable",
            "commands." + command_name + ".disabled.historyBoundary",
            "The pane history has no " + std::string{_definition.name} + " destination.");
    }
    return state;
}

CommandRegistry::Registration MakeCommand(const Definition &_definition,
                                          NavigationHistoryExecutor _executor)
{
    const std::string command_name = "navigation." + std::string{_definition.name};
    CommandDescriptor descriptor;
    descriptor.id = CommandId{_definition.id};
    descriptor.title_key = "commands." + command_name + ".title";
    descriptor.description_key = "commands." + command_name + ".description";
    descriptor.category = CommandCategory::Navigation;
    descriptor.icon_name = _definition.icon;
    descriptor.is_destructive = false;
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = command_name;
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = std::string{_definition.selector},
        .shortcut_action_names = {std::string{_definition.shortcut_action}},
        .shortcut_tag = _definition.shortcut_tag,
    };

    CommandRegistry::Registration registration;
    registration.descriptor = std::move(descriptor);
    registration.state_provider = [_definition](const CommandContext &_context) {
        return State(_context, _definition);
    };
    if( _executor ) {
        registration.handler = [_definition, _executor = std::move(_executor)](
                                   const CommandContext &_context) {
            if( !_executor(_context.native_target, _definition.direction) )
                throw NavigationHistoryExecutionError{};
        };
    }
    return registration;
}

} // namespace

NavigationHistoryExecutionError::NavigationHistoryExecutionError()
    : std::runtime_error{"The pane history navigation could not be executed."}
{
}

CommandRegistry::Registration MakeNavigationBackCommand(NavigationHistoryExecutor _executor)
{
    return MakeCommand(Back, std::move(_executor));
}

CommandRegistry::Registration MakeNavigationForwardCommand(NavigationHistoryExecutor _executor)
{
    return MakeCommand(Forward, std::move(_executor));
}

} // namespace nc::core
