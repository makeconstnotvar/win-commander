// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "FileOpenCommand.h"
#include "CommandIds.h"
#include <VFS/ProviderCapabilities.h>
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
                                         "commands.file.open.disabled.paneUnavailable",
                                         "The file.open command requires a live pane target.");
        return state;
    }
    if( _context.items.empty() ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.empty",
                                         "commands.file.open.disabled.selectionEmpty",
                                         "The file.open command requires at least one item.");
        return state;
    }

    for( const vfs::ListingItem &item : _context.items ) {
        if( !item || !item.Host() ) {
            state.enabled = false;
            state.disabled_reason = Disabled("provider.unavailable",
                                             "commands.file.open.disabled.providerUnavailable",
                                             "Every file.open item requires a live provider.");
            return state;
        }
    }

    const auto &host = _context.items.front().Host();
    if( _context.items.size() > 1 &&
        !std::ranges::all_of(_context.items, [&](const vfs::ListingItem &_item) {
            return _item.Host() == host;
        }) ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.sameProviderRequired",
                                         "commands.file.open.disabled.sameProviderRequired",
                                         "A file.open batch must belong to one provider instance.");
        return state;
    }

    if( std::ranges::any_of(_context.items, &vfs::ListingItem::IsDotDot) ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.parentEntryUnsupported",
                                         "commands.file.open.disabled.parentEntryUnsupported",
                                         "The file.open command cannot open the parent-directory entry.");
        return state;
    }

    if( _context.items.size() > 1 &&
        !std::ranges::all_of(_context.items, &vfs::ListingItem::IsReg) ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.regularFilesRequired",
                                         "commands.file.open.disabled.regularFilesRequired",
                                         "A file.open batch accepts regular files only.");
        return state;
    }

    if( _context.items.size() == 1 && !_context.items.front().IsReg() ) {
        if( !host->IsNativeFS() ) {
            state.enabled = false;
            state.disabled_reason = Disabled("provider.remoteItemTypeUnsupported",
                                             "commands.file.open.disabled.remoteItemTypeUnsupported",
                                             "A non-native provider can externally open regular files only.");
        }
        return state;
    }

    if( !std::ranges::all_of(_context.items, [](const vfs::ListingItem &_item) {
            return vfs::ProviderCapabilitiesResolver::Resolve(*_item.Host(), _item.Directory()).can_read;
        }) ) {
        state.enabled = false;
        state.disabled_reason = Disabled("provider.readUnsupported",
                                         "commands.file.open.disabled.providerReadUnsupported",
                                         "Every regular file requires a provider that declares read capability.");
    }
    return state;
}

} // namespace

FileOpenExecutionError::FileOpenExecutionError()
    : std::runtime_error{"The selected items could not be submitted for external opening."}
{
}

CommandRegistry::Registration MakeFileOpenCommand(FileOpenExecutor _executor)
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::FileOpen};
    descriptor.title_key = "commands.file.open.title";
    descriptor.description_key = "commands.file.open.description";
    descriptor.category = CommandCategory::File;
    descriptor.icon_name = "arrow.up.forward.app";
    descriptor.is_destructive = false;
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "file.open";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "OnOpenNatively:",
        .shortcut_action_names = {"menu.file.open"},
        .shortcut_tag = 11'020,
    };

    CommandRegistry::Registration registration;
    registration.descriptor = std::move(descriptor);
    registration.state_provider = State;
    if( _executor ) {
        registration.handler = [_executor = std::move(_executor)](const CommandContext &_context) {
            if( !_executor(_context.native_target, _context.items) )
                throw FileOpenExecutionError{};
        };
    }
    return registration;
}

} // namespace nc::core
