// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "FileGetInfoCommand.h"

#include "CommandIds.h"

#include <VFS/VFS.h>
#include <algorithm>
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
                                         "commands.file.getInfo.disabled.paneTargetRequired",
                                         "The file.getInfo command requires a live pane presentation target.");
    }
    else if( _context.items.empty() ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.empty",
                                         "commands.file.getInfo.disabled.selectionEmpty",
                                         "The file.getInfo command requires at least one exact item.");
    }
    else if( std::ranges::any_of(_context.items, [](const vfs::ListingItem &_item) { return !_item; }) ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.invalidItem",
                                         "commands.file.getInfo.disabled.invalidItem",
                                         "The file.getInfo command received an invalid item.");
    }
    else if( std::ranges::any_of(_context.items, [](const vfs::ListingItem &_item) { return _item.IsDotDot(); }) ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.parentEntryUnsupported",
                                         "commands.file.getInfo.disabled.parentEntryUnsupported",
                                         "The file.getInfo command cannot present the parent-directory entry.");
    }
    return state;
}

FileGetInfoPresentation CopyPresentation(const CommandContext &_context)
{
    FileGetInfoPresentation presentation;
    presentation.source = _context.source;
    presentation.items.reserve(_context.items.size());
    for( const vfs::ListingItem &item : _context.items )
        presentation.items.emplace_back(CopyFileMetadataSnapshot(item));
    return presentation;
}

DisabledReason LivePresentationRejected()
{
    return Disabled("context.fileGetInfoPresentationRejected",
                    "commands.file.getInfo.disabled.presentationRejected",
                    "The admitted pane target rejected file.getInfo after live authority validation.");
}

} // namespace

CommandRegistry::Registration MakeFileGetInfoCommand(FileGetInfoPresenter _presenter)
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::FileGetInfo};
    descriptor.title_key = "commands.file.getInfo.title";
    descriptor.description_key = "commands.file.getInfo.description";
    descriptor.category = CommandCategory::File;
    descriptor.icon_name = "info.circle";
    descriptor.is_destructive = false;
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "file.getInfo";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "OnFileGetInfo:",
        .shortcut_action_names = {"menu.file.get_info"},
        .shortcut_tag = 11'190,
    };

    CommandRegistry::Registration registration;
    registration.descriptor = std::move(descriptor);
    registration.state_provider = State;
    if( _presenter ) {
        registration.result_handler = [_presenter = std::move(_presenter)](const CommandContext &_context) {
            if( !_presenter(_context.native_target, CopyPresentation(_context)) )
                return std::optional<DisabledReason>{LivePresentationRejected()};
            return std::optional<DisabledReason>{};
        };
    }
    return registration;
}

} // namespace nc::core
