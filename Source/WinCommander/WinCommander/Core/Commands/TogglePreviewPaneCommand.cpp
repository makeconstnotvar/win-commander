// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "TogglePreviewPaneCommand.h"

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
    if( _context.preview_pane_visible )
        state.check_state = *_context.preview_pane_visible ? CommandCheckState::On : CommandCheckState::Off;

    if( !_context.native_target ) {
        state.enabled = false;
        state.disabled_reason = Disabled("context.paneTargetRequired",
                                         "commands.view.togglePreviewPane.disabled.paneUnavailable",
                                         "The view.togglePreviewPane command requires a live pane target.");
    }
    else if( !_context.preview_pane_visible ) {
        state.enabled = false;
        state.disabled_reason = Disabled(
            "context.previewPaneStateRequired",
            "commands.view.togglePreviewPane.disabled.stateUnavailable",
            "The view.togglePreviewPane command requires a current pane visibility snapshot.");
    }
    return state;
}

DisabledReason LiveUpdateRejected()
{
    return Disabled("context.previewPaneStateChanged",
                    "commands.view.togglePreviewPane.disabled.stateChanged",
                    "The preview pane visibility changed after the command snapshot was projected.");
}

} // namespace

CommandRegistry::Registration MakeViewTogglePreviewPaneCommand(PreviewPaneVisibilitySetter _setter)
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::ViewTogglePreviewPane};
    descriptor.title_key = "commands.view.togglePreviewPane.title";
    descriptor.description_key = "commands.view.togglePreviewPane.description";
    descriptor.category = CommandCategory::View;
    descriptor.icon_name = "sidebar.right";
    descriptor.is_destructive = false;
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "view.togglePreviewPane";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "OnTogglePreviewPane:",
        .shortcut_action_names = {"menu.view.toggle_preview_pane"},
        .shortcut_tag = 13'280,
    };

    CommandRegistry::Registration registration;
    registration.descriptor = std::move(descriptor);
    registration.state_provider = State;
    if( _setter ) {
        registration.result_handler = [_setter = std::move(_setter)](const CommandContext &_context)
            -> std::optional<DisabledReason> {
            const bool expected_visibility = *_context.preview_pane_visible;
            if( !_setter(_context.native_target, expected_visibility, !expected_visibility) )
                return LiveUpdateRejected();
            return std::nullopt;
        };
    }
    return registration;
}

} // namespace nc::core
