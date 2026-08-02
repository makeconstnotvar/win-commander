// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "FileRenameCommand.h"
#include "CommandIds.h"
#include <VFS/ProviderCapabilities.h>
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
    if( _context.items.empty() ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.empty",
                                         "commands.file.rename.disabled.selectionEmpty",
                                         "The file.rename command requires a focused item.");
        return state;
    }
    if( _context.items.size() != 1 ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.singleItemRequired",
                                         "commands.file.rename.disabled.singleItemRequired",
                                         "The inline file.rename command accepts exactly one item.");
        return state;
    }

    const vfs::ListingItem &item = _context.items.front();
    if( item.IsDotDot() ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.parentEntryUnsupported",
                                         "commands.file.rename.disabled.parentEntryUnsupported",
                                         "The file.rename command cannot edit the parent-directory entry.");
        return state;
    }
    if( !item.Host() ) {
        state.enabled = false;
        state.disabled_reason = Disabled("provider.unavailable",
                                         "commands.file.rename.disabled.providerUnavailable",
                                         "The file.rename command requires a live provider.");
        return state;
    }

    const vfs::ProviderCapabilities capabilities =
        vfs::ProviderCapabilitiesResolver::Resolve(*item.Host(), item.Directory());
    if( !capabilities.can_rename ) {
        state.enabled = false;
        state.disabled_reason = Disabled("provider.renameUnsupported",
                                         "commands.file.rename.disabled.providerUnsupported",
                                         "The provider does not expose rename capability at the item path.");
        return state;
    }
    if( !_context.native_target ) {
        state.enabled = false;
        state.disabled_reason = Disabled("context.paneTargetRequired",
                                         "commands.file.rename.disabled.paneUnavailable",
                                         "The file.rename initiation requires a live pane target.");
    }
    return state;
}

} // namespace

FileRenameInitiationError::FileRenameInitiationError()
    : std::runtime_error{"The inline rename editor could not be started."}
{
}

CommandRegistry::Registration MakeFileRenameCommand(FileRenameInitiator _initiator)
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::FileRename};
    descriptor.title_key = "commands.file.rename.title";
    descriptor.description_key = "commands.file.rename.description";
    descriptor.category = CommandCategory::File;
    descriptor.icon_name = "pencil";
    descriptor.is_destructive = false;
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "file.rename";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "OnRenameFileInPlace:",
        .shortcut_action_names = {"menu.command.rename_in_place"},
        .shortcut_tag = 15'141,
    };

    CommandRegistry::Registration registration;
    registration.descriptor = std::move(descriptor);
    registration.state_provider = State;
    if( _initiator ) {
        registration.handler = [_initiator = std::move(_initiator)](const CommandContext &_context) {
            if( !_initiator(_context.native_target, _context.items.front()) )
                throw FileRenameInitiationError{};
        };
    }
    return registration;
}

} // namespace nc::core
