// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "FileMutationCommands.h"
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

DisabledReason PasteDisabled(const FilePasteAvailability _availability)
{
    switch( _availability ) {
        case FilePasteAvailability::PaneUnavailable:
            return Disabled("context.paneTargetRequired",
                            "commands.file.paste.disabled.paneUnavailable",
                            "The file.paste command requires a live pane target.");
        case FilePasteAvailability::WindowUnavailable:
            return Disabled("context.operationQueueRequired",
                            "commands.file.paste.disabled.windowUnavailable",
                            "The file.paste command requires a live window operation queue.");
        case FilePasteAvailability::DestinationUnavailable:
            return Disabled("destination.unavailable",
                            "commands.file.paste.disabled.destinationUnavailable",
                            "The file.paste destination is unavailable or does not have one uniform provider.");
        case FilePasteAvailability::DestinationReadOnly:
            return Disabled("destination.readOnly",
                            "commands.file.paste.disabled.destinationReadOnly",
                            "The file.paste destination provider is read-only at the current path.");
        case FilePasteAvailability::ClipboardUnavailable:
            return Disabled("clipboard.fileListUnavailable",
                            "commands.file.paste.disabled.clipboardUnavailable",
                            "The clipboard does not contain a readable file list.");
        case FilePasteAvailability::ClipboardBusy:
            return Disabled("clipboard.moveInFlight",
                            "commands.file.paste.disabled.clipboardBusy",
                            "The clipboard file-list move intent is already being consumed.");
        case FilePasteAvailability::ClipboardChanged:
            return Disabled("clipboard.changed",
                            "commands.file.paste.disabled.clipboardChanged",
                            "The clipboard changed before the file.paste command could submit the operation.");
        case FilePasteAvailability::SourceUnavailable:
            return Disabled("source.unavailable",
                            "commands.file.paste.disabled.sourceUnavailable",
                            "At least one clipboard source item could not be resolved exactly.");
        case FilePasteAvailability::Available:
            break;
    }
    return Disabled("command.rejected",
                    "commands.disabled.generic",
                    "The file.paste execution port rejected an admitted request.");
}

CommandDescriptor PasteDescriptor()
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::FilePaste};
    descriptor.title_key = "commands.file.paste.title";
    descriptor.description_key = "commands.file.paste.description";
    descriptor.category = CommandCategory::Edit;
    descriptor.icon_name = "doc.on.clipboard";
    descriptor.analytics_name = "file.paste";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "paste:",
        .shortcut_action_names = {"menu.edit.paste"},
        .shortcut_tag = 12'010,
    };
    return descriptor;
}

CommandState DeletionState(const CommandContext &_context, const FileDeletionIntent _intent)
{
    const std::string_view command = _intent == FileDeletionIntent::Trash ? "trash" : "delete";
    CommandState state;
    if( _context.items.empty() ) {
        state.enabled = false;
        state.disabled_reason = Disabled("selection.empty",
                                         "commands.file.mutation.disabled.selectionEmpty",
                                         "The file." + std::string{command} + " command requires selected items.");
        return state;
    }
    if( std::ranges::any_of(_context.items, [](const vfs::ListingItem &_item) { return _item.IsDotDot(); }) ) {
        state.enabled = false;
        state.disabled_reason = Disabled(
            "selection.parentEntryUnsupported",
            "commands.file.mutation.disabled.parentEntryUnsupported",
            "The file." + std::string{command} + " command cannot mutate the parent-directory entry.");
        return state;
    }
    for( const vfs::ListingItem &item : _context.items ) {
        if( !item.Host() ) {
            state.enabled = false;
            state.disabled_reason = Disabled("provider.unavailable",
                                             "commands.file.mutation.disabled.providerUnavailable",
                                             "The file." + std::string{command} + " command requires a live provider.");
            return state;
        }
        const vfs::ProviderCapabilities capabilities =
            vfs::ProviderCapabilitiesResolver::Resolve(*item.Host(), item.Directory());
        if( !capabilities.can_write ) {
            state.enabled = false;
            state.disabled_reason = Disabled("provider.readOnly",
                                             "commands.file.mutation.disabled.providerReadOnly",
                                             "The file." + std::string{command} +
                                                 " command requires a writable provider path.");
            return state;
        }
        const bool supported = _intent == FileDeletionIntent::Trash ? capabilities.can_trash
                                                                    : capabilities.can_delete_permanently;
        if( !supported ) {
            state.enabled = false;
            state.disabled_reason = Disabled(
                _intent == FileDeletionIntent::Trash ? "provider.trashUnsupported" : "provider.deleteUnsupported",
                _intent == FileDeletionIntent::Trash
                    ? "commands.file.trash.disabled.providerUnsupported"
                    : "commands.file.delete.disabled.providerUnsupported",
                "The provider does not support the requested file." + std::string{command} + " mutation.");
            return state;
        }
    }
    if( !_context.native_target ) {
        state.enabled = false;
        state.disabled_reason = Disabled("context.paneTargetRequired",
                                         "commands.file.mutation.disabled.paneUnavailable",
                                         "The file." + std::string{command} + " command requires a live pane target.");
    }
    return state;
}

CommandDescriptor DeletionDescriptor(const FileDeletionIntent _intent)
{
    const bool permanent = _intent == FileDeletionIntent::Permanent;
    CommandDescriptor descriptor;
    descriptor.id = CommandId{permanent ? command_ids::FileDelete : command_ids::FileTrash};
    descriptor.title_key = permanent ? "commands.file.delete.title" : "commands.file.trash.title";
    descriptor.description_key = permanent ? "commands.file.delete.description" : "commands.file.trash.description";
    descriptor.category = CommandCategory::File;
    descriptor.icon_name = permanent ? "trash.slash" : "trash";
    descriptor.is_destructive = permanent;
    // Queue 1 deliberately keeps the reviewed engine frozen. The production port retains the
    // established deletion review and nc::ops::Deletion execution boundary.
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = permanent ? "file.delete" : "file.trash";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = permanent ? "OnDeletePermanentlyCommand:" : "OnMoveToTrash:",
        .shortcut_action_names = {permanent ? "menu.command.delete_permanently" : "menu.command.move_to_trash"},
        .shortcut_tag = permanent ? 15'180 : 15'160,
    };
    return descriptor;
}

CommandRegistry::Registration MakeDeletionCommand(const FileDeletionIntent _intent,
                                                   FileDeletionExecutor _executor)
{
    CommandRegistry::Registration registration;
    registration.descriptor = DeletionDescriptor(_intent);
    registration.state_provider = [_intent](const CommandContext &_context) {
        return DeletionState(_context, _intent);
    };
    if( _executor ) {
        registration.result_handler = [_intent, _executor = std::move(_executor)](const CommandContext &_context) {
            if( _executor(_context.native_target, _context.items, _intent, _context.native_sender) )
                return std::optional<DisabledReason>{};
            const bool permanent = _intent == FileDeletionIntent::Permanent;
            return std::optional{Disabled(
                "context.stale",
                "commands.file.mutation.disabled.stale",
                permanent ? "The permanent deletion review could not be presented for the exact live selection."
                          : "The trash operation could not be submitted for the exact live selection.")};
        };
    }
    return registration;
}

DisabledReason CreationDisabled(const FileCreationAvailability _availability, const FileCreationIntent _intent)
{
    const bool file = _intent == FileCreationIntent::File;
    const std::string command = file ? "file.newFile" : "file.newFolder";
    switch( _availability ) {
        case FileCreationAvailability::PaneUnavailable:
            return Disabled("context.paneTargetRequired",
                            file ? "commands.file.newFile.disabled.paneUnavailable"
                                 : "commands.file.newFolder.disabled.paneUnavailable",
                            "The " + command + " command requires a live pane target.");
        case FileCreationAvailability::WindowUnavailable:
            return Disabled("context.operationQueueRequired",
                            file ? "commands.file.newFile.disabled.windowUnavailable"
                                 : "commands.file.newFolder.disabled.windowUnavailable",
                            "The " + command + " command requires a live window operation queue.");
        case FileCreationAvailability::Loading:
            return Disabled("pane.loading",
                            file ? "commands.file.newFile.disabled.loading"
                                 : "commands.file.newFolder.disabled.loading",
                            "The " + command + " command waits for the current pane load to finish.");
        case FileCreationAvailability::DestinationUnavailable:
            return Disabled("destination.unavailable",
                            file ? "commands.file.newFile.disabled.destinationUnavailable"
                                 : "commands.file.newFolder.disabled.destinationUnavailable",
                            "The " + command + " command requires one committed destination provider and path.");
        case FileCreationAvailability::DestinationReadOnly:
            return Disabled("destination.readOnly",
                            file ? "commands.file.newFile.disabled.destinationReadOnly"
                                 : "commands.file.newFolder.disabled.destinationReadOnly",
                            "The " + command + " destination is read-only.");
        case FileCreationAvailability::ProviderUnsupported:
            return Disabled(file ? "provider.createFileUnsupported" : "provider.createFolderUnsupported",
                            file ? "commands.file.newFile.disabled.providerUnsupported"
                                 : "commands.file.newFolder.disabled.providerUnsupported",
                            file ? "The destination provider does not support empty-file creation."
                                 : "The destination provider does not support folder creation.");
        case FileCreationAvailability::StaleDestination:
            return Disabled("destination.stale",
                            file ? "commands.file.newFile.disabled.stale"
                                 : "commands.file.newFolder.disabled.stale",
                            "The " + command + " destination changed before submission.");
        case FileCreationAvailability::NameUnavailable:
            return Disabled("destination.nameUnavailable",
                            file ? "commands.file.newFile.disabled.nameUnavailable"
                                 : "commands.file.newFolder.disabled.nameUnavailable",
                            file ? "No collision-free provisional file name is available."
                                 : "No collision-free provisional folder name is available.");
        case FileCreationAvailability::Available:
            break;
    }
    return Disabled("command.rejected",
                    "commands.disabled.generic",
                    "The " + command + " execution port rejected an admitted request.");
}

CommandDescriptor CreationDescriptor(const FileCreationIntent _intent)
{
    const bool file = _intent == FileCreationIntent::File;
    CommandDescriptor descriptor;
    descriptor.id = CommandId{file ? command_ids::FileNewFile : command_ids::FileNewFolder};
    descriptor.title_key = file ? "commands.file.newFile.title" : "commands.file.newFolder.title";
    descriptor.description_key =
        file ? "commands.file.newFile.description" : "commands.file.newFolder.description";
    descriptor.category = CommandCategory::File;
    descriptor.icon_name = file ? "doc.badge.plus" : "folder.badge.plus";
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = file ? "file.newFile" : "file.newFolder";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = file ? "OnQuickNewFile:" : "OnQuickNewFolder:",
        .shortcut_action_names = {file ? "menu.file.new_file" : "menu.file.new_folder"},
        .shortcut_tag = file ? 11'120 : 11'090,
    };
    return descriptor;
}

CommandRegistry::Registration MakeCreationCommand(const FileCreationIntent _intent,
                                                  FileCreationAvailabilityProvider _availability,
                                                  FileCreationExecutor _executor)
{
    CommandRegistry::Registration registration;
    registration.descriptor = CreationDescriptor(_intent);
    registration.state_provider = [_intent, _availability = std::move(_availability)](const CommandContext &_context) {
        CommandState state;
        const FileCreationAvailability availability =
            !_context.native_target
                ? FileCreationAvailability::PaneUnavailable
                : (_availability ? _availability(_context.native_target, _intent)
                                 : FileCreationAvailability::DestinationUnavailable);
        if( availability != FileCreationAvailability::Available ) {
            state.enabled = false;
            state.disabled_reason = CreationDisabled(availability, _intent);
        }
        return state;
    };
    if( _executor ) {
        registration.result_handler =
            [_intent, _executor = std::move(_executor)](const CommandContext &_context) {
                const FileCreationAvailability result =
                    _executor(_context.native_target, _intent, _context.native_sender);
                if( result == FileCreationAvailability::Available )
                    return std::optional<DisabledReason>{};
                return std::optional{CreationDisabled(result, _intent)};
            };
    }
    return registration;
}

DisabledReason SelectionDisabled(const PaneSelectionAvailability _availability,
                                 const PaneSelectionIntent _intent)
{
    const std::string_view command =
        _intent == PaneSelectionIntent::SelectAll ? "pane.selectAll" : "pane.invertSelection";
    switch( _availability ) {
        case PaneSelectionAvailability::PaneUnavailable:
            return Disabled("context.paneTargetRequired",
                            "commands.pane.selection.disabled.paneUnavailable",
                            "The " + std::string{command} + " command requires a live pane target.");
        case PaneSelectionAvailability::Loading:
            return Disabled("pane.loading",
                            "commands.pane.selection.disabled.loading",
                            "The " + std::string{command} + " command waits for the current pane load to finish.");
        case PaneSelectionAvailability::ListingUnavailable:
            return Disabled("pane.listingUnavailable",
                            "commands.pane.selection.disabled.listingUnavailable",
                            "The " + std::string{command} + " command requires a committed listing.");
        case PaneSelectionAvailability::Empty:
            return Disabled("selection.noVisibleItems",
                            "commands.pane.selection.disabled.empty",
                            "The " + std::string{command} + " command requires visible selectable items.");
        case PaneSelectionAvailability::Available:
            break;
    }
    return Disabled("command.rejected",
                    "commands.disabled.generic",
                    "The " + std::string{command} + " execution port rejected an admitted request.");
}

CommandDescriptor SelectionDescriptor(const PaneSelectionIntent _intent)
{
    const bool select_all = _intent == PaneSelectionIntent::SelectAll;
    CommandDescriptor descriptor;
    descriptor.id = CommandId{select_all ? command_ids::PaneSelectAll : command_ids::PaneInvertSelection};
    descriptor.title_key = select_all ? "commands.pane.selectAll.title" : "commands.pane.invertSelection.title";
    descriptor.description_key =
        select_all ? "commands.pane.selectAll.description" : "commands.pane.invertSelection.description";
    descriptor.category = CommandCategory::Edit;
    descriptor.icon_name = select_all ? "checklist.checked" : "arrow.triangle.2.circlepath";
    descriptor.analytics_name = select_all ? "pane.selectAll" : "pane.invertSelection";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = select_all ? "selectAll:" : "OnMenuInvertSelection:",
        .shortcut_action_names = {select_all ? "menu.edit.select_all" : "menu.edit.invert_selection"},
        .shortcut_tag = select_all ? 12'020 : 12'040,
    };
    return descriptor;
}

CommandRegistry::Registration MakeSelectionCommand(const PaneSelectionIntent _intent,
                                                   PaneSelectionAvailabilityProvider _availability,
                                                   PaneSelectionExecutor _executor)
{
    CommandRegistry::Registration registration;
    registration.descriptor = SelectionDescriptor(_intent);
    registration.state_provider = [_intent, _availability = std::move(_availability)](const CommandContext &_context) {
        CommandState state;
        const PaneSelectionAvailability availability =
            !_context.native_target
                ? PaneSelectionAvailability::PaneUnavailable
                : (_availability ? _availability(_context.native_target, _intent)
                                 : PaneSelectionAvailability::ListingUnavailable);
        if( availability != PaneSelectionAvailability::Available ) {
            state.enabled = false;
            state.disabled_reason = SelectionDisabled(availability, _intent);
        }
        return state;
    };
    if( _executor ) {
        registration.result_handler = [_intent, _executor = std::move(_executor)](const CommandContext &_context) {
            const PaneSelectionAvailability result =
                _executor(_context.native_target, _intent, _context.native_sender);
            if( result == PaneSelectionAvailability::Available )
                return std::optional<DisabledReason>{};
            return std::optional{SelectionDisabled(result, _intent)};
        };
    }
    return registration;
}

DisabledReason ArchiveCreateDisabled(const ArchiveCreateAvailability _availability)
{
    switch( _availability ) {
        case ArchiveCreateAvailability::PaneUnavailable:
            return Disabled("context.paneTargetRequired",
                            "commands.file.mutation.disabled.paneUnavailable",
                            "The archive.create command requires a live pane target.");
        case ArchiveCreateAvailability::WindowUnavailable:
            return Disabled("context.operationQueueRequired",
                            "commands.file.operation.disabled.windowUnavailable",
                            "The archive.create command requires a live operation queue.");
        case ArchiveCreateAvailability::Loading:
            return Disabled("pane.loading",
                            "commands.file.operation.disabled.loading",
                            "The archive.create command waits for the current pane load to finish.");
        case ArchiveCreateAvailability::SelectionUnavailable:
            return Disabled("selection.empty",
                            "commands.file.mutation.disabled.selectionEmpty",
                            "The archive.create command requires selected or focused items.");
        case ArchiveCreateAvailability::ParentEntryUnsupported:
            return Disabled("selection.parentEntryUnsupported",
                            "commands.file.mutation.disabled.parentEntryUnsupported",
                            "The archive.create command cannot archive the parent-directory entry.");
        case ArchiveCreateAvailability::SourceUnreadable:
            return Disabled("source.unreadable",
                            "commands.file.operation.disabled.sourceUnreadable",
                            "Every archive.create source must be readable and supported.");
        case ArchiveCreateAvailability::SourceNameCollision:
            return Disabled("source.nameCollision",
                            "commands.file.operation.disabled.sourceNameCollision",
                            "The archive.create sources contain colliding top-level names.");
        case ArchiveCreateAvailability::DestinationUnavailable:
            return Disabled("destination.unavailable",
                            "commands.file.operation.disabled.destinationUnavailable",
                            "The archive.create destination is unavailable.");
        case ArchiveCreateAvailability::DestinationReadOnly:
            return Disabled("destination.readOnly",
                            "commands.file.operation.disabled.destinationReadOnly",
                            "The archive.create destination is read-only.");
        case ArchiveCreateAvailability::ProviderUnsupported:
            return Disabled("provider.createFileUnsupported",
                            "commands.file.operation.disabled.providerUnsupported",
                            "The archive.create destination cannot create archive files.");
        case ArchiveCreateAvailability::StaleContext:
            return Disabled("context.stale",
                            "commands.file.mutation.disabled.stale",
                            "The archive.create source or destination changed before submission.");
        case ArchiveCreateAvailability::Available:
            break;
    }
    return Disabled("command.rejected", "commands.disabled.generic", "The archive.create command was rejected.");
}

CommandDescriptor ArchiveCreateDescriptor()
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::ArchiveCreate};
    descriptor.title_key = "commands.archive.create.title";
    descriptor.description_key = "commands.archive.create.description";
    descriptor.category = CommandCategory::Archive;
    descriptor.icon_name = "archivebox";
    // Queue 1 retains the established nc::ops::Compression lifecycle.
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "archive.create";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "onCompressItemsHere:",
        .shortcut_action_names = {"menu.command.compress_here"},
        .shortcut_tag = 15'100,
    };
    return descriptor;
}

DisabledReason ArchiveExtractDisabled(const ArchiveExtractAvailability _availability)
{
    switch( _availability ) {
        case ArchiveExtractAvailability::PaneUnavailable:
            return Disabled("context.paneTargetRequired",
                            "commands.archive.extract.disabled.paneUnavailable",
                            "The archive.extract command requires a live pane target.");
        case ArchiveExtractAvailability::WindowUnavailable:
            return Disabled("context.operationQueueRequired",
                            "commands.archive.extract.disabled.windowUnavailable",
                            "The archive.extract command requires a live operation queue.");
        case ArchiveExtractAvailability::Loading:
            return Disabled("pane.loading",
                            "commands.archive.extract.disabled.loading",
                            "The archive.extract command waits for the current pane load to finish.");
        case ArchiveExtractAvailability::SelectionUnavailable:
            return Disabled("selection.singleArchiveRequired",
                            "commands.archive.extract.disabled.selectionUnavailable",
                            "The archive.extract command requires exactly one selected or focused archive.");
        case ArchiveExtractAvailability::ParentEntryUnsupported:
            return Disabled("selection.parentEntryUnsupported",
                            "commands.file.mutation.disabled.parentEntryUnsupported",
                            "The archive.extract command cannot extract the parent-directory entry.");
        case ArchiveExtractAvailability::SourceUnsupported:
            return Disabled("source.archiveUnsupported",
                            "commands.archive.extract.disabled.sourceUnsupported",
                            "The selected item is not a supported regular archive file.");
        case ArchiveExtractAvailability::SourceUnreadable:
            return Disabled("source.unreadable",
                            "commands.file.operation.disabled.sourceUnreadable",
                            "The selected archive source is not readable.");
        case ArchiveExtractAvailability::DestinationUnavailable:
            return Disabled("destination.unavailable",
                            "commands.file.operation.disabled.destinationUnavailable",
                            "The archive.extract destination is unavailable.");
        case ArchiveExtractAvailability::DestinationReadOnly:
            return Disabled("destination.readOnly",
                            "commands.file.operation.disabled.destinationReadOnly",
                            "The archive.extract destination is read-only.");
        case ArchiveExtractAvailability::ProviderUnsupported:
            return Disabled("provider.archiveExtractionUnsupported",
                            "commands.archive.extract.disabled.providerUnsupported",
                            "The destination provider cannot safely materialize every supported archive item kind.");
        case ArchiveExtractAvailability::CaseSensitivityUnavailable:
            return Disabled("destination.caseSensitivityUnavailable",
                            "commands.archive.extract.disabled.caseSensitivityUnavailable",
                            "The destination case-sensitivity contract could not be established.");
        case ArchiveExtractAvailability::StaleContext:
            return Disabled("context.stale",
                            "commands.file.mutation.disabled.stale",
                            "The archive.extract source or destination changed before submission.");
        case ArchiveExtractAvailability::Available:
            break;
    }
    return Disabled("command.rejected", "commands.disabled.generic", "The archive.extract command was rejected.");
}

CommandDescriptor ArchiveExtractDescriptor()
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::ArchiveExtract};
    descriptor.title_key = "commands.archive.extract.title";
    descriptor.description_key = "commands.archive.extract.description";
    descriptor.category = CommandCategory::Archive;
    descriptor.icon_name = "archivebox.fill";
    // Queue 1 retains typed acquisition and namespace admission over the established Copying lifecycle.
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "archive.extract";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "onExtractArchiveHere:",
        .shortcut_action_names = {"menu.command.extract_archive_here"},
        .shortcut_tag = 15'250,
    };
    return descriptor;
}

DisabledReason DuplicateDisabled(const FileDuplicateAvailability _availability)
{
    switch( _availability ) {
        case FileDuplicateAvailability::PaneUnavailable:
            return Disabled("context.paneTargetRequired",
                            "commands.file.mutation.disabled.paneUnavailable",
                            "The file.duplicate command requires a live pane target.");
        case FileDuplicateAvailability::WindowUnavailable:
            return Disabled("context.operationQueueRequired",
                            "commands.file.operation.disabled.windowUnavailable",
                            "The file.duplicate command requires a live operation queue.");
        case FileDuplicateAvailability::Loading:
            return Disabled("pane.loading",
                            "commands.file.operation.disabled.loading",
                            "The file.duplicate command waits for the current pane load to finish.");
        case FileDuplicateAvailability::SelectionUnavailable:
            return Disabled("selection.empty",
                            "commands.file.mutation.disabled.selectionEmpty",
                            "The file.duplicate command requires selected or focused items.");
        case FileDuplicateAvailability::ParentEntryUnsupported:
            return Disabled("selection.parentEntryUnsupported",
                            "commands.file.mutation.disabled.parentEntryUnsupported",
                            "The file.duplicate command cannot duplicate the parent-directory entry.");
        case FileDuplicateAvailability::SourceUnreadable:
            return Disabled("source.unreadable",
                            "commands.file.operation.disabled.sourceUnreadable",
                            "Every file.duplicate source must be readable.");
        case FileDuplicateAvailability::DestinationUnavailable:
            return Disabled("destination.unavailable",
                            "commands.file.operation.disabled.destinationUnavailable",
                            "The file.duplicate destination is unavailable.");
        case FileDuplicateAvailability::DestinationReadOnly:
            return Disabled("destination.readOnly",
                            "commands.file.operation.disabled.destinationReadOnly",
                            "The file.duplicate destination is read-only.");
        case FileDuplicateAvailability::ProviderUnsupported:
            return Disabled("provider.copyUnsupported",
                            "commands.file.operation.disabled.providerUnsupported",
                            "The destination provider cannot create every duplicate item kind.");
        case FileDuplicateAvailability::NameUnavailable:
            return Disabled("destination.nameUnavailable",
                            "commands.file.operation.disabled.nameUnavailable",
                            "No collision-free duplicate name is available.");
        case FileDuplicateAvailability::StaleContext:
            return Disabled("context.stale",
                            "commands.file.mutation.disabled.stale",
                            "The file.duplicate selection or destination changed before submission.");
        case FileDuplicateAvailability::Available:
            break;
    }
    return Disabled("command.rejected", "commands.disabled.generic", "The file.duplicate command was rejected.");
}

CommandDescriptor DuplicateDescriptor()
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::FileDuplicate};
    descriptor.title_key = "commands.file.duplicate.title";
    descriptor.description_key = "commands.file.duplicate.description";
    descriptor.category = CommandCategory::File;
    descriptor.icon_name = "plus.square.on.square";
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "file.duplicate";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "OnDuplicate:",
        .shortcut_action_names = {"menu.file.duplicate"},
        .shortcut_tag = 11'150,
    };
    return descriptor;
}

DisabledReason CopyPathDisabled(const FileCopyPathAvailability _availability)
{
    switch( _availability ) {
        case FileCopyPathAvailability::PaneUnavailable:
            return Disabled("context.paneTargetRequired",
                            "commands.file.mutation.disabled.paneUnavailable",
                            "The file.copyPath command requires a live pane target.");
        case FileCopyPathAvailability::Loading:
            return Disabled("pane.loading",
                            "commands.file.operation.disabled.loading",
                            "The file.copyPath command waits for the current pane load to finish.");
        case FileCopyPathAvailability::SelectionUnavailable:
            return Disabled("selection.empty",
                            "commands.file.mutation.disabled.selectionEmpty",
                            "The file.copyPath command requires selected or focused items.");
        case FileCopyPathAvailability::ParentEntryUnsupported:
            return Disabled("selection.parentEntryUnsupported",
                            "commands.file.mutation.disabled.parentEntryUnsupported",
                            "The file.copyPath command cannot copy the parent-entry path.");
        case FileCopyPathAvailability::StaleContext:
            return Disabled("context.stale",
                            "commands.file.mutation.disabled.stale",
                            "The file.copyPath selection changed before clipboard submission.");
        case FileCopyPathAvailability::ClipboardUnavailable:
            return Disabled("clipboard.unavailable",
                            "commands.file.operation.disabled.clipboardUnavailable",
                            "The file.copyPath command could not write to the clipboard.");
        case FileCopyPathAvailability::Available:
            break;
    }
    return Disabled("command.rejected", "commands.disabled.generic", "The file.copyPath command was rejected.");
}

CommandDescriptor CopyPathDescriptor()
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::FileCopyPath};
    descriptor.title_key = "commands.file.copyPath.title";
    descriptor.description_key = "commands.file.copyPath.description";
    descriptor.category = CommandCategory::Edit;
    descriptor.icon_name = "link";
    descriptor.analytics_name = "file.copyPath";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "OnCopyCurrentFilePath:",
        .shortcut_action_names = {"menu.command.copy_file_path"},
        .shortcut_tag = 15'040,
    };
    return descriptor;
}

DisabledReason CalculateSizesDisabled(const FileCalculateSizesAvailability _availability)
{
    switch( _availability ) {
        case FileCalculateSizesAvailability::PaneUnavailable:
            return Disabled("context.paneTargetRequired",
                            "commands.file.mutation.disabled.paneUnavailable",
                            "The file.calculateSizes command requires a live pane target.");
        case FileCalculateSizesAvailability::Loading:
            return Disabled("pane.loading",
                            "commands.file.operation.disabled.loading",
                            "The file.calculateSizes command waits for the current pane load to finish.");
        case FileCalculateSizesAvailability::ListingUnavailable:
            return Disabled("pane.listingUnavailable",
                            "commands.pane.selection.disabled.listingUnavailable",
                            "The file.calculateSizes command requires a committed listing.");
        case FileCalculateSizesAvailability::SelectionUnavailable:
            return Disabled("selection.empty",
                            "commands.file.mutation.disabled.selectionEmpty",
                            "The file.calculateSizes command requires selected or focused items.");
        case FileCalculateSizesAvailability::ParentEntryUnsupported:
            return Disabled("selection.parentEntryUnsupported",
                            "commands.file.mutation.disabled.parentEntryUnsupported",
                            "The file.calculateSizes command cannot traverse the parent-directory entry.");
        case FileCalculateSizesAvailability::DirectoryRequired:
            return Disabled("selection.directoryRequired",
                            "commands.file.calculateSizes.disabled.directoryRequired",
                            "The file.calculateSizes command requires at least one directory.");
        case FileCalculateSizesAvailability::SourceUnreadable:
            return Disabled("source.unreadable",
                            "commands.file.operation.disabled.sourceUnreadable",
                            "Every file.calculateSizes directory must be readable.");
        case FileCalculateSizesAvailability::Busy:
            return Disabled("operation.busy",
                            "commands.file.calculateSizes.disabled.busy",
                            "A directory-size calculation is already running in this pane.");
        case FileCalculateSizesAvailability::StaleContext:
            return Disabled("context.stale",
                            "commands.file.mutation.disabled.stale",
                            "The file.calculateSizes selection changed before submission.");
        case FileCalculateSizesAvailability::Available:
            break;
    }
    return Disabled("command.rejected", "commands.disabled.generic", "The file.calculateSizes command was rejected.");
}

CommandDescriptor CalculateSizesDescriptor()
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::FileCalculateSizes};
    descriptor.title_key = "commands.file.calculateSizes.title";
    descriptor.description_key = "commands.file.calculateSizes.description";
    descriptor.category = CommandCategory::File;
    descriptor.icon_name = "sum";
    descriptor.analytics_name = "file.calculateSizes";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "OnCalculateSizes:",
        .shortcut_action_names = {"menu.file.calculate_sizes"},
        .shortcut_tag = 11'030,
    };
    return descriptor;
}

DisabledReason BatchRenameDisabled(const FileBatchRenameAvailability _availability)
{
    switch( _availability ) {
        case FileBatchRenameAvailability::PaneUnavailable:
            return Disabled("context.paneTargetRequired",
                            "commands.file.mutation.disabled.paneUnavailable",
                            "The file.batchRename command requires a live pane target.");
        case FileBatchRenameAvailability::WindowUnavailable:
            return Disabled("context.operationQueueRequired",
                            "commands.file.operation.disabled.windowUnavailable",
                            "The file.batchRename command requires a live operation queue.");
        case FileBatchRenameAvailability::Loading:
            return Disabled("pane.loading",
                            "commands.file.operation.disabled.loading",
                            "The file.batchRename command waits for the current pane load to finish.");
        case FileBatchRenameAvailability::ListingUnavailable:
            return Disabled("pane.listingUnavailable",
                            "commands.pane.selection.disabled.listingUnavailable",
                            "The file.batchRename command requires one committed uniform listing.");
        case FileBatchRenameAvailability::SelectionUnavailable:
            return Disabled("selection.empty",
                            "commands.file.mutation.disabled.selectionEmpty",
                            "The file.batchRename command requires selected or focused items.");
        case FileBatchRenameAvailability::ParentEntryUnsupported:
            return Disabled("selection.parentEntryUnsupported",
                            "commands.file.mutation.disabled.parentEntryUnsupported",
                            "The file.batchRename command cannot rename the parent-directory entry.");
        case FileBatchRenameAvailability::ProviderUnavailable:
            return Disabled("provider.unavailable",
                            "commands.file.mutation.disabled.providerUnavailable",
                            "The file.batchRename command requires one committed provider and directory.");
        case FileBatchRenameAvailability::MixedProviders:
            return Disabled("provider.mixed",
                            "commands.file.batchRename.disabled.mixedProviders",
                            "All file.batchRename items must use the current provider and directory.");
        case FileBatchRenameAvailability::ProviderUnsupported:
            return Disabled("provider.renameUnsupported",
                            "commands.file.rename.disabled.providerUnsupported",
                            "The provider does not support rename at the selected item path.");
        case FileBatchRenameAvailability::InvalidPlan:
            return Disabled("plan.invalid",
                            "commands.file.batchRename.disabled.invalidPlan",
                            "The reviewed batch-rename plan is invalid.");
        case FileBatchRenameAvailability::DestinationConflict:
            return Disabled("destination.conflict",
                            "commands.file.batchRename.disabled.destinationConflict",
                            "A reviewed batch-rename destination conflicts with the current listing.");
        case FileBatchRenameAvailability::StaleContext:
            return Disabled("context.stale",
                            "commands.file.mutation.disabled.stale",
                            "The file.batchRename selection changed before submission.");
        case FileBatchRenameAvailability::Available:
            break;
    }
    return Disabled("command.rejected", "commands.disabled.generic", "The file.batchRename command was rejected.");
}

CommandDescriptor BatchRenameDescriptor()
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::FileBatchRename};
    descriptor.title_key = "commands.file.batchRename.title";
    descriptor.description_key = "commands.file.batchRename.description";
    descriptor.category = CommandCategory::File;
    descriptor.icon_name = "rectangle.and.pencil.and.ellipsis";
    // Queue 1 keeps the established nc::ops::BatchRenaming lifecycle. The full §44 operation-plan
    // and undo contract remains a separate target capability.
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "file.batchRename";
    descriptor.legacy = LegacyCommandMetadata{
        .selector_name = "OnBatchRename:",
        .shortcut_action_names = {"menu.command.batch_rename"},
        .shortcut_tag = 15'220,
    };
    return descriptor;
}

template <class Availability, class Provider, class DisabledMapper>
CommandState PayloadState(const CommandContext &_context,
                          const Availability _pane_unavailable,
                          const Availability _selection_unavailable,
                          const Availability _parent_unsupported,
                          const Availability _fallback,
                          const Provider &_provider,
                          DisabledMapper _disabled)
{
    CommandState state;
    Availability availability = _fallback;
    if( !_context.native_target )
        availability = _pane_unavailable;
    else if( _context.items.empty() )
        availability = _selection_unavailable;
    else if( std::ranges::any_of(_context.items, [](const vfs::ListingItem &_item) { return _item.IsDotDot(); }) )
        availability = _parent_unsupported;
    else if( _provider )
        availability = _provider(_context.native_target, _context.items);
    if( availability != Availability::Available ) {
        state.enabled = false;
        state.disabled_reason = _disabled(availability);
    }
    return state;
}

} // namespace

CommandRegistry::Registration MakeFilePasteCommand(FilePasteAvailabilityProvider _availability,
                                                   FilePasteExecutor _executor)
{
    CommandRegistry::Registration registration;
    registration.descriptor = PasteDescriptor();
    registration.state_provider = [_availability = std::move(_availability)](const CommandContext &_context) {
        CommandState state;
        const FilePasteAvailability availability =
            !_context.native_target ? FilePasteAvailability::PaneUnavailable
                                    : (_availability ? _availability(_context.native_target)
                                                     : FilePasteAvailability::DestinationUnavailable);
        if( availability != FilePasteAvailability::Available ) {
            state.enabled = false;
            state.disabled_reason = PasteDisabled(availability);
        }
        return state;
    };
    if( _executor ) {
        registration.result_handler = [_executor = std::move(_executor)](const CommandContext &_context) {
            const FilePasteAvailability result = _executor(_context.native_target, _context.native_sender);
            if( result == FilePasteAvailability::Available )
                return std::optional<DisabledReason>{};
            return std::optional{PasteDisabled(result)};
        };
    }
    return registration;
}

CommandRegistry::Registration MakeFileTrashCommand(FileDeletionExecutor _executor)
{
    return MakeDeletionCommand(FileDeletionIntent::Trash, std::move(_executor));
}

CommandRegistry::Registration MakeFileDeleteCommand(FileDeletionExecutor _executor)
{
    return MakeDeletionCommand(FileDeletionIntent::Permanent, std::move(_executor));
}

CommandRegistry::Registration MakeFileNewFolderCommand(FileCreationAvailabilityProvider _availability,
                                                       FileCreationExecutor _executor)
{
    return MakeCreationCommand(FileCreationIntent::Folder, std::move(_availability), std::move(_executor));
}

CommandRegistry::Registration MakeFileNewFileCommand(FileCreationAvailabilityProvider _availability,
                                                     FileCreationExecutor _executor)
{
    return MakeCreationCommand(FileCreationIntent::File, std::move(_availability), std::move(_executor));
}

CommandRegistry::Registration MakePaneSelectAllCommand(PaneSelectionAvailabilityProvider _availability,
                                                       PaneSelectionExecutor _executor)
{
    return MakeSelectionCommand(PaneSelectionIntent::SelectAll, std::move(_availability), std::move(_executor));
}

CommandRegistry::Registration MakePaneInvertSelectionCommand(PaneSelectionAvailabilityProvider _availability,
                                                             PaneSelectionExecutor _executor)
{
    return MakeSelectionCommand(PaneSelectionIntent::Invert, std::move(_availability), std::move(_executor));
}

CommandRegistry::Registration MakeArchiveCreateCommand(ArchiveCreateAvailabilityProvider _availability,
                                                       ArchiveCreateExecutor _executor)
{
    CommandRegistry::Registration registration;
    registration.descriptor = ArchiveCreateDescriptor();
    registration.state_provider = [_availability = std::move(_availability)](const CommandContext &_context) {
        return PayloadState(_context,
                            ArchiveCreateAvailability::PaneUnavailable,
                            ArchiveCreateAvailability::SelectionUnavailable,
                            ArchiveCreateAvailability::ParentEntryUnsupported,
                            ArchiveCreateAvailability::DestinationUnavailable,
                            _availability,
                            ArchiveCreateDisabled);
    };
    if( _executor ) {
        registration.result_handler = [_executor = std::move(_executor)](const CommandContext &_context) {
            const ArchiveCreateAvailability result =
                _executor(_context.native_target, _context.items, _context.native_sender);
            if( result == ArchiveCreateAvailability::Available )
                return std::optional<DisabledReason>{};
            return std::optional{ArchiveCreateDisabled(result)};
        };
    }
    return registration;
}

CommandRegistry::Registration MakeArchiveExtractCommand(ArchiveExtractAvailabilityProvider _availability,
                                                        ArchiveExtractExecutor _executor)
{
    CommandRegistry::Registration registration;
    registration.descriptor = ArchiveExtractDescriptor();
    registration.state_provider = [_availability = std::move(_availability)](const CommandContext &_context) {
        return PayloadState(_context,
                            ArchiveExtractAvailability::PaneUnavailable,
                            ArchiveExtractAvailability::SelectionUnavailable,
                            ArchiveExtractAvailability::ParentEntryUnsupported,
                            ArchiveExtractAvailability::DestinationUnavailable,
                            _availability,
                            ArchiveExtractDisabled);
    };
    if( _executor ) {
        registration.result_handler = [_executor = std::move(_executor)](const CommandContext &_context) {
            const ArchiveExtractAvailability result =
                _executor(_context.native_target, _context.items, _context.native_sender);
            if( result == ArchiveExtractAvailability::Available )
                return std::optional<DisabledReason>{};
            return std::optional{ArchiveExtractDisabled(result)};
        };
    }
    return registration;
}

CommandRegistry::Registration MakeFileDuplicateCommand(FileDuplicateAvailabilityProvider _availability,
                                                       FileDuplicateExecutor _executor)
{
    CommandRegistry::Registration registration;
    registration.descriptor = DuplicateDescriptor();
    registration.state_provider = [_availability = std::move(_availability)](const CommandContext &_context) {
        return PayloadState(_context,
                            FileDuplicateAvailability::PaneUnavailable,
                            FileDuplicateAvailability::SelectionUnavailable,
                            FileDuplicateAvailability::ParentEntryUnsupported,
                            FileDuplicateAvailability::DestinationUnavailable,
                            _availability,
                            DuplicateDisabled);
    };
    if( _executor ) {
        registration.result_handler = [_executor = std::move(_executor)](const CommandContext &_context) {
            const FileDuplicateAvailability result =
                _executor(_context.native_target, _context.items, _context.native_sender);
            if( result == FileDuplicateAvailability::Available )
                return std::optional<DisabledReason>{};
            return std::optional{DuplicateDisabled(result)};
        };
    }
    return registration;
}

CommandRegistry::Registration MakeFileCopyPathCommand(FileCopyPathAvailabilityProvider _availability,
                                                      FileCopyPathExecutor _executor)
{
    CommandRegistry::Registration registration;
    registration.descriptor = CopyPathDescriptor();
    registration.state_provider = [_availability = std::move(_availability)](const CommandContext &_context) {
        return PayloadState(_context,
                            FileCopyPathAvailability::PaneUnavailable,
                            FileCopyPathAvailability::SelectionUnavailable,
                            FileCopyPathAvailability::ParentEntryUnsupported,
                            FileCopyPathAvailability::StaleContext,
                            _availability,
                            CopyPathDisabled);
    };
    if( _executor ) {
        registration.result_handler = [_executor = std::move(_executor)](const CommandContext &_context) {
            const FileCopyPathAvailability result =
                _executor(_context.native_target, _context.items, _context.native_sender);
            if( result == FileCopyPathAvailability::Available )
                return std::optional<DisabledReason>{};
            return std::optional{CopyPathDisabled(result)};
        };
    }
    return registration;
}

CommandRegistry::Registration
MakeFileCalculateSizesCommand(FileCalculateSizesAvailabilityProvider _availability,
                              FileCalculateSizesExecutor _executor)
{
    CommandRegistry::Registration registration;
    registration.descriptor = CalculateSizesDescriptor();
    registration.state_provider = [_availability = std::move(_availability)](const CommandContext &_context) {
        return PayloadState(_context,
                            FileCalculateSizesAvailability::PaneUnavailable,
                            FileCalculateSizesAvailability::SelectionUnavailable,
                            FileCalculateSizesAvailability::ParentEntryUnsupported,
                            FileCalculateSizesAvailability::StaleContext,
                            _availability,
                            CalculateSizesDisabled);
    };
    if( _executor ) {
        registration.result_handler = [_executor = std::move(_executor)](const CommandContext &_context) {
            const FileCalculateSizesAvailability result =
                _executor(_context.native_target, _context.items, _context.native_sender);
            if( result == FileCalculateSizesAvailability::Available )
                return std::optional<DisabledReason>{};
            return std::optional{CalculateSizesDisabled(result)};
        };
    }
    return registration;
}

CommandRegistry::Registration
MakeFileBatchRenameCommand(FileBatchRenameAvailabilityProvider _availability, FileBatchRenameExecutor _executor)
{
    CommandRegistry::Registration registration;
    registration.descriptor = BatchRenameDescriptor();
    registration.state_provider = [_availability = std::move(_availability)](const CommandContext &_context) {
        return PayloadState(_context,
                            FileBatchRenameAvailability::PaneUnavailable,
                            FileBatchRenameAvailability::SelectionUnavailable,
                            FileBatchRenameAvailability::ParentEntryUnsupported,
                            FileBatchRenameAvailability::StaleContext,
                            _availability,
                            BatchRenameDisabled);
    };
    if( _executor ) {
        registration.result_handler = [_executor = std::move(_executor)](const CommandContext &_context) {
            const FileBatchRenameAvailability result =
                _executor(_context.native_target, _context.items, _context.native_sender);
            if( result == FileBatchRenameAvailability::Available )
                return std::optional<DisabledReason>{};
            return std::optional{BatchRenameDisabled(result)};
        };
    }
    return registration;
}

} // namespace nc::core
