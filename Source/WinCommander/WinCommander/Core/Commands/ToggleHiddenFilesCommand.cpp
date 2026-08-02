// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ToggleHiddenFilesCommand.h"
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

CommandState State(const CommandContext &_context)
{
    CommandState state;
    if( _context.shows_hidden_files )
        state.check_state = *_context.shows_hidden_files ? CommandCheckState::On : CommandCheckState::Off;

    if( !_context.native_target ) {
        state.enabled = false;
        state.disabled_reason = Disabled("context.paneTargetRequired",
                                         "commands.view.toggleHiddenFiles.disabled.paneUnavailable",
                                         "The view.toggleHiddenFiles command requires a live pane target.");
        return state;
    }
    if( !_context.shows_hidden_files ) {
        state.enabled = false;
        state.disabled_reason = Disabled(
            "context.hiddenFilesStateRequired",
            "commands.view.toggleHiddenFiles.disabled.stateUnavailable",
            "The view.toggleHiddenFiles command requires a current pane visibility snapshot.");
    }
    return state;
}

} // namespace

ViewToggleHiddenFilesError::ViewToggleHiddenFilesError()
    : std::runtime_error{"The hidden-files visibility could not be changed."}
{
}

CommandRegistry::Registration MakeViewToggleHiddenFilesCommand(HiddenFilesVisibilitySetter _setter)
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::ViewToggleHiddenFiles};
    descriptor.title_key = "commands.view.toggleHiddenFiles.title";
    descriptor.description_key = "commands.view.toggleHiddenFiles.description";
    descriptor.category = CommandCategory::View;
    descriptor.icon_name = "eye";
    descriptor.is_destructive = false;
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "view.toggleHiddenFiles";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "ToggleViewHiddenFiles:",
        .shortcut_action_names = {"menu.view.sorting_view_hidden"},
        .shortcut_tag = 13'140,
    };

    CommandRegistry::Registration registration;
    registration.descriptor = std::move(descriptor);
    registration.state_provider = State;
    if( _setter ) {
        registration.handler = [_setter = std::move(_setter)](const CommandContext &_context) {
            const bool desired_visibility = !*_context.shows_hidden_files;
            if( !_setter(_context.native_target, desired_visibility) )
                throw ViewToggleHiddenFilesError{};
        };
    }
    return registration;
}

} // namespace nc::core
