// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "FileCutCommand.h"
#include "CommandIds.h"
#include <VFS/VFS.h>
#include <algorithm>

namespace nc::core {

namespace {

DisabledReason EmptySelectionReason()
{
    return DisabledReason{
        .code = "selection.empty",
        .user_message_key = "commands.file.cut.disabled.selectionEmpty",
        .technical_message = "The file.cut command requires at least one source item.",
        .suggested_action = std::nullopt,
    };
}

DisabledReason ParentEntryUnsupportedReason()
{
    return DisabledReason{
        .code = "selection.parentEntryUnsupported",
        .user_message_key = "commands.file.cut.disabled.parentEntryUnsupported",
        .technical_message = "The file.cut command cannot stage the parent-directory entry.",
        .suggested_action = std::nullopt,
    };
}

DisabledReason NativeItemsRequiredReason()
{
    return DisabledReason{
        .code = "provider.nativeItemsRequired",
        .user_message_key = "commands.file.cut.disabled.nativeItemsRequired",
        .technical_message = "The file.cut move-intent writer accepts native filesystem items only.",
        .suggested_action = std::nullopt,
    };
}

} // namespace

FileCutWriteError::FileCutWriteError()
    : std::runtime_error{"The selected items could not be staged for moving."}
{
}

CommandRegistry::Registration MakeFileCutCommand(FileCutWriter _writer)
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::FileCut};
    descriptor.title_key = "commands.file.cut.title";
    descriptor.description_key = "commands.file.cut.description";
    descriptor.category = CommandCategory::File;
    descriptor.icon_name = "scissors";
    descriptor.is_destructive = false;
    descriptor.requires_operation_plan = false;
    descriptor.analytics_name = "file.cut";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "cut:",
    };

    CommandRegistry::Registration registration;
    registration.descriptor = std::move(descriptor);
    registration.state_provider = [](const CommandContext &_context) {
        CommandState state;
        if( _context.items.empty() ) {
            state.enabled = false;
            state.disabled_reason = EmptySelectionReason();
        }
        else if( std::ranges::any_of(_context.items, [](const vfs::ListingItem &_item) {
                     return _item.IsDotDot();
                 }) ) {
            state.enabled = false;
            state.disabled_reason = ParentEntryUnsupportedReason();
        }
        else if( !std::ranges::all_of(_context.items, [](const vfs::ListingItem &_item) {
                     return _item.Host() && _item.Host()->IsNativeFS();
                 }) ) {
            state.enabled = false;
            state.disabled_reason = NativeItemsRequiredReason();
        }
        return state;
    };
    if( _writer ) {
        registration.handler = [_writer = std::move(_writer)](const CommandContext &_context) {
            if( !_writer(_context.items, FileCutIntent::Move) )
                throw FileCutWriteError{};
        };
    }
    return registration;
}

} // namespace nc::core
