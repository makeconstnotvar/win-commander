// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "FilePreviewCommand.h"

#include "CommandIds.h"

#include <VFS/VFS.h>
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
    if( !_context.native_target ) {
        state.enabled = false;
        state.disabled_reason = Disabled("context.paneTargetRequired",
                                         "commands.file.preview.disabled.paneTargetRequired",
                                         "The file.preview command requires a live pane target.");
    }
    else if( _context.items.empty() ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.empty",
                                         "commands.file.preview.disabled.selectionEmpty",
                                         "The file.preview command requires exactly one item.");
    }
    else if( _context.items.size() != 1 ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.singleItemRequired",
                                         "commands.file.preview.disabled.singleItemRequired",
                                         "The file.preview command requires exactly one item.");
    }
    else if( !_context.items.front() ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.invalidItem",
                                         "commands.file.preview.disabled.invalidItem",
                                         "The file.preview command received an invalid item.");
    }
    else if( _context.items.front().IsDotDot() ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.parentEntryUnsupported",
                                         "commands.file.preview.disabled.parentEntryUnsupported",
                                         "The file.preview command cannot preview the parent-directory entry.");
    }
    return state;
}

DisabledReason LivePreviewRejected()
{
    return Disabled("preview.unavailable",
                    "commands.file.preview.disabled.previewUnavailable",
                    "The live pane rejected file.preview after revalidating its current item.");
}

} // namespace

CommandRegistry::Registration MakeFilePreviewCommand(FilePreviewHandler _handler)
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::FilePreview};
    descriptor.title_key = "commands.file.preview.title";
    descriptor.description_key = "commands.file.preview.description";
    descriptor.category = CommandCategory::File;
    descriptor.icon_name = "eye";
    descriptor.is_destructive = false;
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "file.preview";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "OnFileViewCommand:",
        .shortcut_action_names = {"menu.command.quick_look", "panel.show_preview"},
        .shortcut_tag = 15'070,
    };

    CommandRegistry::Registration registration;
    registration.descriptor = std::move(descriptor);
    registration.state_provider = State;
    if( _handler ) {
        registration.result_handler = [_handler = std::move(_handler)](const CommandContext &_context) {
            if( !_handler(_context.native_target,
                          _context.items.front(),
                          FilePreviewIntent{
                              .source = _context.source,
                              .native_sender = _context.native_sender,
                          }) )
                return std::optional<DisabledReason>{LivePreviewRejected()};
            return std::optional<DisabledReason>{};
        };
    }
    return registration;
}

} // namespace nc::core
