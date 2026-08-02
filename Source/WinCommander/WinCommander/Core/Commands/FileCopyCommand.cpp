// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "FileCopyCommand.h"
#include "CommandIds.h"
#include <VFS/VFS.h>
#include <algorithm>

namespace nc::core {

namespace {

DisabledReason EmptySelectionReason()
{
    return DisabledReason{
        .code = "selection.empty",
        .user_message_key = "commands.file.copy.disabled.selectionEmpty",
        .technical_message = "The file.copy command requires at least one source item.",
        .suggested_action = std::nullopt,
    };
}

DisabledReason NativeItemsRequiredReason()
{
    return DisabledReason{
        .code = "provider.nativeItemsRequired",
        .user_message_key = "commands.file.copy.disabled.nativeItemsRequired",
        .technical_message = "The file.copy pasteboard writer accepts native filesystem items only.",
        .suggested_action = std::nullopt,
    };
}

} // namespace

CommandRegistry::Registration MakeFileCopyCommand(FileCopyWriter _writer)
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::FileCopy};
    descriptor.title_key = "commands.file.copy.title";
    descriptor.description_key = "commands.file.copy.description";
    descriptor.category = CommandCategory::File;
    descriptor.icon_name = "doc.on.doc";
    descriptor.analytics_name = "file.copy";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "copy:",
        .shortcut_action_names = {"menu.edit.copy"},
        .shortcut_tag = 12'000,
    };

    CommandRegistry::Registration registration;
    registration.descriptor = std::move(descriptor);
    registration.state_provider = [](const CommandContext &_context) {
        CommandState state;
        if( _context.items.empty() ) {
            state.enabled = false;
            state.disabled_reason = EmptySelectionReason();
        }
        else if( !std::ranges::all_of(_context.items, [](const vfs::ListingItem &_item) {
                     return _item.Host() && _item.Host()->IsNativeFS();
                 }) ) {
            state.enabled = false;
            state.disabled_reason = NativeItemsRequiredReason();
        }
        return state;
    };
    if( _writer )
        registration.handler =
            [_writer = std::move(_writer)](const CommandContext &_context) { _writer(_context.items); };
    return registration;
}

} // namespace nc::core
