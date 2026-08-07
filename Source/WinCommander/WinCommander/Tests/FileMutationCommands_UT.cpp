// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/FileMutationCommands.h>
#include <array>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <vector>

namespace {

using nc::core::CommandContext;
using nc::core::CommandId;
using nc::core::CommandInvocationSource;
using nc::core::CommandRegistry;
using nc::core::ArchiveCreateAvailability;
using nc::core::ArchiveExtractAvailability;
using nc::core::FileCopyPathAvailability;
using nc::core::FileCalculateSizesAvailability;
using nc::core::FileBatchRenameAvailability;
using nc::core::FileDeletionIntent;
using nc::core::FileCreationAvailability;
using nc::core::FileCreationIntent;
using nc::core::FileDuplicateAvailability;
using nc::core::FilePasteAvailability;
using nc::core::MakeFileDeleteCommand;
using nc::core::MakeArchiveCreateCommand;
using nc::core::MakeArchiveExtractCommand;
using nc::core::MakeFileCopyPathCommand;
using nc::core::MakeFileCalculateSizesCommand;
using nc::core::MakeFileBatchRenameCommand;
using nc::core::MakeFileDuplicateCommand;
using nc::core::MakeFileNewFolderCommand;
using nc::core::MakeFileNewFileCommand;
using nc::core::MakeFilePasteCommand;
using nc::core::MakeFileTrashCommand;
using nc::core::MakePaneInvertSelectionCommand;
using nc::core::MakePaneSelectAllCommand;
using nc::core::PaneSelectionAvailability;
using nc::core::PaneSelectionIntent;
using nc::vfs::ListingItem;

class MutationTestHost final : public nc::vfs::Host
{
public:
    MutationTestHost(const bool _native,
                     const bool _writable,
                     const bool _trash,
                     const bool _delete)
        : Host("/", nullptr, "file_mutation_test"), m_Native(_native), m_Writable(_writable)
    {
        AddFeatures(nc::vfs::HostFeatures::Read);
        if( _trash )
            AddFeatures(nc::vfs::HostFeatures::Trash);
        if( _delete )
            AddFeatures(nc::vfs::HostFeatures::Unlink | nc::vfs::HostFeatures::RemoveDirectory);
    }

    bool IsNativeFS() const noexcept override { return m_Native; }
    bool IsWritable() const override { return m_Writable; }

private:
    bool m_Native;
    bool m_Writable;
};

std::vector<ListingItem> Items(const std::shared_ptr<nc::vfs::Host> &_host,
                               const std::initializer_list<std::string_view> _names)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/";
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = _host;
    for( const std::string_view name : _names ) {
        const bool directory = name == "..";
        input.filenames.emplace_back(name);
        input.unix_modes.emplace_back((directory ? S_IFDIR : S_IFREG) | S_IRUSR | S_IWUSR);
        input.unix_types.emplace_back(directory ? DT_DIR : DT_REG);
    }
    const auto listing = VFSListing::Build(std::move(input));
    std::vector<ListingItem> result;
    for( unsigned index = 0; index < listing->Count(); ++index )
        result.emplace_back(listing->Item(index));
    return result;
}

void *Target()
{
    static int target;
    return &target;
}

CommandId PasteId() { return CommandId{nc::core::command_ids::FilePaste}; }
CommandId TrashId() { return CommandId{nc::core::command_ids::FileTrash}; }
CommandId DeleteId() { return CommandId{nc::core::command_ids::FileDelete}; }
CommandId NewFolderId() { return CommandId{nc::core::command_ids::FileNewFolder}; }
CommandId NewFileId() { return CommandId{nc::core::command_ids::FileNewFile}; }
CommandId SelectAllId() { return CommandId{nc::core::command_ids::PaneSelectAll}; }
CommandId InvertSelectionId() { return CommandId{nc::core::command_ids::PaneInvertSelection}; }
CommandId ArchiveCreateId() { return CommandId{nc::core::command_ids::ArchiveCreate}; }
CommandId ArchiveExtractId() { return CommandId{nc::core::command_ids::ArchiveExtract}; }
CommandId DuplicateId() { return CommandId{nc::core::command_ids::FileDuplicate}; }
CommandId CopyPathId() { return CommandId{nc::core::command_ids::FileCopyPath}; }
CommandId CalculateSizesId() { return CommandId{nc::core::command_ids::FileCalculateSizes}; }
CommandId BatchRenameId() { return CommandId{nc::core::command_ids::FileBatchRename}; }

} // namespace

#define PREFIX "nc::core::FileMutationCommands "

TEST_CASE(PREFIX "defines stable paste, trash and permanent-delete metadata")
{
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeFilePasteCommand([](void *) { return FilePasteAvailability::Available; },
                                                   [](void *, const void *) {
                                                       return FilePasteAvailability::Available;
                                                   })) == CommandRegistry::RegisterResult::Registered);
    const auto deletion_executor =
        [](void *, std::span<const ListingItem>, FileDeletionIntent, const void *) { return true; };
    REQUIRE(registry.Register(MakeFileTrashCommand(deletion_executor)) ==
            CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakeFileDeleteCommand(deletion_executor)) ==
            CommandRegistry::RegisterResult::Registered);

    const auto *const paste = registry.Find(PasteId());
    const auto *const trash = registry.Find(TrashId());
    const auto *const deletion = registry.Find(DeleteId());
    REQUIRE(paste);
    REQUIRE(trash);
    REQUIRE(deletion);
    CHECK(paste->title_key == "commands.file.paste.title");
    CHECK(paste->category == nc::core::CommandCategory::Edit);
    CHECK(paste->legacy->selector_name == "paste:");
    CHECK(paste->legacy->shortcut_action_names == std::vector<std::string>{"menu.edit.paste"});
    CHECK(paste->legacy->shortcut_tag == 12'010);
    CHECK_FALSE(trash->is_destructive);
    CHECK(trash->legacy->selector_name == "OnMoveToTrash:");
    CHECK(trash->legacy->shortcut_tag == 15'160);
    CHECK(deletion->is_destructive);
    CHECK_FALSE(deletion->requires_operation_plan);
    CHECK_FALSE(deletion->supports_undo);
    CHECK(deletion->legacy->selector_name == "OnDeletePermanentlyCommand:");
    CHECK(deletion->legacy->shortcut_tag == 15'180);
}

TEST_CASE(PREFIX "paste uses one live port for menu, toolbar, context and shortcut")
{
    int executions = 0;
    void *received_target = nullptr;
    const void *received_sender = nullptr;
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeFilePasteCommand(
                [](void *_target) {
                    CHECK(_target == Target());
                    return FilePasteAvailability::Available;
                },
                [&](void *_target, const void *_sender) {
                    ++executions;
                    received_target = _target;
                    received_sender = _sender;
                    return FilePasteAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);
    int sender = 0;
    constexpr std::array sources{CommandInvocationSource::Menu,
                                 CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut};
    for( std::size_t index = 0; index < sources.size(); ++index ) {
        const CommandContext context{
            .source = sources[index],
            .native_sender = &sender,
            .native_target = Target(),
        };
        CHECK(registry.Execute(PasteId(), context).status == CommandRegistry::ExecutionStatus::Executed);
        CHECK(executions == static_cast<int>(index + 1));
        CHECK(received_target == Target());
        CHECK(received_sender == &sender);
    }
}

TEST_CASE(PREFIX "paste reports every unavailable state and rejects a stale live submission")
{
    constexpr std::array unavailable{
        FilePasteAvailability::WindowUnavailable,
        FilePasteAvailability::DestinationUnavailable,
        FilePasteAvailability::DestinationReadOnly,
        FilePasteAvailability::ClipboardUnavailable,
        FilePasteAvailability::ClipboardBusy,
        FilePasteAvailability::ClipboardChanged,
        FilePasteAvailability::SourceUnavailable,
    };
    for( const FilePasteAvailability availability : unavailable ) {
        CommandRegistry registry;
        int executions = 0;
        REQUIRE(registry.Register(MakeFilePasteCommand(
                    [availability](void *) { return availability; },
                    [&](void *, const void *) {
                        ++executions;
                        return FilePasteAvailability::Available;
                    })) == CommandRegistry::RegisterResult::Registered);
        const CommandContext context{.native_target = Target()};
        const auto state = registry.QueryState(PasteId(), context).state;
        CHECK_FALSE(state.enabled);
        REQUIRE(state.disabled_reason);
        CHECK_FALSE(state.disabled_reason->user_message_key.empty());
        CHECK(registry.Execute(PasteId(), context).status == CommandRegistry::ExecutionStatus::Disabled);
        CHECK(executions == 0);
    }

    CommandRegistry stale;
    REQUIRE(stale.Register(MakeFilePasteCommand([](void *) { return FilePasteAvailability::Available; },
                                               [](void *, const void *) {
                                                   return FilePasteAvailability::ClipboardChanged;
                                               })) == CommandRegistry::RegisterResult::Registered);
    const auto rejected = stale.Execute(PasteId(), CommandContext{.native_target = Target()});
    CHECK(rejected.status == CommandRegistry::ExecutionStatus::Rejected);
    REQUIRE(rejected.disabled_reason);
    CHECK(rejected.disabled_reason->code == "clipboard.changed");
    CHECK(stale.QueryState(PasteId(), {}).state.disabled_reason->code == "context.paneTargetRequired");
}

TEST_CASE(PREFIX "trash and delete validate exact capabilities without downgrading")
{
    const auto native = std::make_shared<MutationTestHost>(true, true, true, true);
    const auto remote = std::make_shared<MutationTestHost>(false, true, false, true);
    const auto read_only = std::make_shared<MutationTestHost>(true, false, true, true);
    const auto native_items = Items(native, {"first", "second"});
    const auto remote_items = Items(remote, {"remote"});
    const auto read_only_items = Items(read_only, {"locked"});
    const auto parent = Items(native, {".."});
    int executions = 0;
    std::span<const ListingItem> received;
    FileDeletionIntent intent = FileDeletionIntent::Trash;
    const auto executor = [&](void *_target,
                              std::span<const ListingItem> _items,
                              FileDeletionIntent _intent,
                              const void *) {
        CHECK(_target == Target());
        ++executions;
        received = _items;
        intent = _intent;
        return true;
    };
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeFileTrashCommand(executor)) == CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakeFileDeleteCommand(executor)) == CommandRegistry::RegisterResult::Registered);

    const CommandContext native_context{.native_target = Target(), .items = native_items};
    CHECK(registry.Execute(TrashId(), native_context).status == CommandRegistry::ExecutionStatus::Executed);
    CHECK(intent == FileDeletionIntent::Trash);
    CHECK(received.data() == native_items.data());
    CHECK(registry.Execute(DeleteId(), native_context).status == CommandRegistry::ExecutionStatus::Executed);
    CHECK(intent == FileDeletionIntent::Permanent);
    CHECK(executions == 2);

    const CommandContext remote_context{.native_target = Target(), .items = remote_items};
    const auto remote_trash = registry.QueryState(TrashId(), remote_context).state;
    CHECK_FALSE(remote_trash.enabled);
    REQUIRE(remote_trash.disabled_reason);
    CHECK(remote_trash.disabled_reason->code == "provider.trashUnsupported");
    CHECK(registry.Execute(TrashId(), remote_context).status == CommandRegistry::ExecutionStatus::Disabled);
    CHECK(registry.Execute(DeleteId(), remote_context).status == CommandRegistry::ExecutionStatus::Executed);

    for( const std::span<const ListingItem> invalid : {std::span<const ListingItem>{},
                                                       std::span<const ListingItem>{parent},
                                                       std::span<const ListingItem>{read_only_items}} ) {
        const CommandContext context{.native_target = Target(), .items = invalid};
        CHECK_FALSE(registry.QueryState(TrashId(), context).state.enabled);
        CHECK_FALSE(registry.QueryState(DeleteId(), context).state.enabled);
    }
    CHECK(executions == 3);
}

TEST_CASE(PREFIX "deletion repeats live admission and reports executor rejection")
{
    const auto native = std::make_shared<MutationTestHost>(true, true, true, true);
    const auto items = Items(native, {"document"});
    int executions = 0;
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeFileTrashCommand(
                [&](void *, std::span<const ListingItem>, FileDeletionIntent, const void *) {
                    ++executions;
                    return false;
                })) == CommandRegistry::RegisterResult::Registered);
    const CommandContext context{.native_target = Target(), .items = items};
    CHECK(registry.QueryState(TrashId(), context).state.enabled);
    const auto result = registry.Execute(TrashId(), context);
    CHECK(result.status == CommandRegistry::ExecutionStatus::Rejected);
    REQUIRE(result.disabled_reason);
    CHECK(result.disabled_reason->code == "context.stale");
    CHECK(executions == 1);

    CommandRegistry missing;
    CHECK(missing.Register(MakeFilePasteCommand({}, {})) == CommandRegistry::RegisterResult::MissingHandler);
    CHECK(missing.Register(MakeFileTrashCommand({})) == CommandRegistry::RegisterResult::MissingHandler);
    CHECK(missing.Register(MakeFileDeleteCommand({})) == CommandRegistry::RegisterResult::MissingHandler);
}

TEST_CASE(PREFIX "defines stable creation and visible-selection metadata")
{
    CommandRegistry registry;
    const auto creation_availability = [](void *, FileCreationIntent) {
        return FileCreationAvailability::Available;
    };
    const auto creation_executor = [](void *, FileCreationIntent, const void *) {
        return FileCreationAvailability::Available;
    };
    const auto selection_availability = [](void *, PaneSelectionIntent) {
        return PaneSelectionAvailability::Available;
    };
    const auto selection_executor = [](void *, PaneSelectionIntent, const void *) {
        return PaneSelectionAvailability::Available;
    };
    REQUIRE(registry.Register(MakeFileNewFolderCommand(creation_availability, creation_executor)) ==
            CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakeFileNewFileCommand(creation_availability, creation_executor)) ==
            CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakePaneSelectAllCommand(selection_availability, selection_executor)) ==
            CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakePaneInvertSelectionCommand(selection_availability, selection_executor)) ==
            CommandRegistry::RegisterResult::Registered);

    const auto *const new_folder = registry.Find(NewFolderId());
    const auto *const new_file = registry.Find(NewFileId());
    const auto *const select_all = registry.Find(SelectAllId());
    const auto *const invert = registry.Find(InvertSelectionId());
    REQUIRE(new_folder);
    REQUIRE(new_file);
    REQUIRE(select_all);
    REQUIRE(invert);
    CHECK(new_folder->title_key == "commands.file.newFolder.title");
    CHECK(new_folder->category == nc::core::CommandCategory::File);
    CHECK(new_folder->legacy->selector_name == "OnQuickNewFolder:");
    CHECK(new_folder->legacy->shortcut_action_names == std::vector<std::string>{"menu.file.new_folder"});
    CHECK(new_folder->legacy->shortcut_tag == 11'090);
    CHECK_FALSE(new_folder->requires_operation_plan);
    CHECK(new_file->title_key == "commands.file.newFile.title");
    CHECK(new_file->category == nc::core::CommandCategory::File);
    CHECK(new_file->icon_name == "doc.badge.plus");
    CHECK(new_file->legacy->selector_name == "OnQuickNewFile:");
    CHECK(new_file->legacy->shortcut_action_names == std::vector<std::string>{"menu.file.new_file"});
    CHECK(new_file->legacy->shortcut_tag == 11'120);
    CHECK_FALSE(new_file->requires_operation_plan);
    CHECK(select_all->title_key == "commands.pane.selectAll.title");
    CHECK(select_all->category == nc::core::CommandCategory::Edit);
    CHECK(select_all->legacy->selector_name == "selectAll:");
    CHECK(select_all->legacy->shortcut_tag == 12'020);
    CHECK(invert->title_key == "commands.pane.invertSelection.title");
    CHECK(invert->legacy->selector_name == "OnMenuInvertSelection:");
    CHECK(invert->legacy->shortcut_tag == 12'040);
}

TEST_CASE(PREFIX "new folder shares one typed live port and fails closed after stale admission")
{
    int executions = 0;
    FileCreationIntent received_intent = FileCreationIntent::File;
    const void *received_sender = nullptr;
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeFileNewFolderCommand(
                [](void *_target, FileCreationIntent _intent) {
                    CHECK(_target == Target());
                    CHECK(_intent == FileCreationIntent::Folder);
                    return FileCreationAvailability::Available;
                },
                [&](void *_target, FileCreationIntent _intent, const void *_sender) {
                    CHECK(_target == Target());
                    ++executions;
                    received_intent = _intent;
                    received_sender = _sender;
                    return executions == 4 ? FileCreationAvailability::StaleDestination
                                           : FileCreationAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);

    int sender = 0;
    constexpr std::array sources{CommandInvocationSource::Menu,
                                 CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut};
    for( std::size_t index = 0; index < sources.size(); ++index ) {
        const CommandContext context{
            .source = sources[index],
            .native_sender = &sender,
            .native_target = Target(),
        };
        const auto result = registry.Execute(NewFolderId(), context);
        if( index + 1 == sources.size() ) {
            CHECK(result.status == CommandRegistry::ExecutionStatus::Rejected);
            REQUIRE(result.disabled_reason);
            CHECK(result.disabled_reason->code == "destination.stale");
        }
        else {
            CHECK(result.status == CommandRegistry::ExecutionStatus::Executed);
        }
    }
    CHECK(executions == 4);
    CHECK(received_intent == FileCreationIntent::Folder);
    CHECK(received_sender == &sender);
    CHECK(registry.QueryState(NewFolderId(), {}).state.disabled_reason->code == "context.paneTargetRequired");

    constexpr std::array unavailable{
        FileCreationAvailability::WindowUnavailable,
        FileCreationAvailability::Loading,
        FileCreationAvailability::DestinationUnavailable,
        FileCreationAvailability::DestinationReadOnly,
        FileCreationAvailability::ProviderUnsupported,
        FileCreationAvailability::NameUnavailable,
    };
    for( const auto availability : unavailable ) {
        CommandRegistry unavailable_registry;
        REQUIRE(unavailable_registry.Register(MakeFileNewFolderCommand(
                    [availability](void *, FileCreationIntent) { return availability; },
                    [](void *, FileCreationIntent, const void *) {
                        return FileCreationAvailability::Available;
                    })) ==
                CommandRegistry::RegisterResult::Registered);
        const auto state = unavailable_registry.QueryState(NewFolderId(),
                                                           CommandContext{.native_target = Target()})
                               .state;
        CHECK_FALSE(state.enabled);
        REQUIRE(state.disabled_reason);
        CHECK_FALSE(state.disabled_reason->code.empty());
    }
}

TEST_CASE(PREFIX "new file shares one typed live port and reports file-specific disabled states")
{
    int executions = 0;
    FileCreationIntent received_intent = FileCreationIntent::Folder;
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeFileNewFileCommand(
                [](void *_target, FileCreationIntent _intent) {
                    CHECK(_target == Target());
                    CHECK(_intent == FileCreationIntent::File);
                    return FileCreationAvailability::Available;
                },
                [&](void *_target, FileCreationIntent _intent, const void *) {
                    CHECK(_target == Target());
                    ++executions;
                    received_intent = _intent;
                    return executions == 4 ? FileCreationAvailability::StaleDestination
                                           : FileCreationAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);

    constexpr std::array sources{CommandInvocationSource::Menu,
                                 CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut};
    for( std::size_t index = 0; index < sources.size(); ++index ) {
        const CommandContext context{.source = sources[index], .native_target = Target()};
        const auto result = registry.Execute(NewFileId(), context);
        CHECK(result.status == (index + 1 == sources.size() ? CommandRegistry::ExecutionStatus::Rejected
                                                            : CommandRegistry::ExecutionStatus::Executed));
        if( index + 1 == sources.size() ) {
            REQUIRE(result.disabled_reason);
            CHECK(result.disabled_reason->code == "destination.stale");
            CHECK(result.disabled_reason->user_message_key == "commands.file.newFile.disabled.stale");
        }
    }
    CHECK(executions == 4);
    CHECK(received_intent == FileCreationIntent::File);

    constexpr std::array unavailable{
        FileCreationAvailability::WindowUnavailable,
        FileCreationAvailability::Loading,
        FileCreationAvailability::DestinationUnavailable,
        FileCreationAvailability::DestinationReadOnly,
        FileCreationAvailability::ProviderUnsupported,
        FileCreationAvailability::NameUnavailable,
    };
    for( const auto availability : unavailable ) {
        CommandRegistry unavailable_registry;
        REQUIRE(unavailable_registry.Register(MakeFileNewFileCommand(
                    [availability](void *, FileCreationIntent) { return availability; },
                    [](void *, FileCreationIntent, const void *) {
                        return FileCreationAvailability::Available;
                    })) == CommandRegistry::RegisterResult::Registered);
        const auto state = unavailable_registry.QueryState(NewFileId(), CommandContext{.native_target = Target()}).state;
        CHECK_FALSE(state.enabled);
        REQUIRE(state.disabled_reason);
        CHECK(state.disabled_reason->user_message_key.starts_with("commands.file.newFile.disabled."));
    }
    CHECK(registry.QueryState(NewFileId(), {}).state.disabled_reason->code == "context.paneTargetRequired");
}

TEST_CASE(PREFIX "select all and invert selection preserve intent across every invocation source")
{
    PaneSelectionAvailability live = PaneSelectionAvailability::Available;
    std::vector<PaneSelectionIntent> executed;
    const auto availability = [&](void *_target, PaneSelectionIntent) {
        CHECK(_target == Target());
        return live;
    };
    const auto executor = [&](void *_target, PaneSelectionIntent _intent, const void *) {
        CHECK(_target == Target());
        executed.push_back(_intent);
        return live;
    };
    CommandRegistry registry;
    REQUIRE(registry.Register(MakePaneSelectAllCommand(availability, executor)) ==
            CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakePaneInvertSelectionCommand(availability, executor)) ==
            CommandRegistry::RegisterResult::Registered);

    constexpr std::array sources{CommandInvocationSource::Menu,
                                 CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut};
    for( const auto source : sources ) {
        const CommandContext context{.source = source, .native_target = Target()};
        CHECK(registry.Execute(SelectAllId(), context).status == CommandRegistry::ExecutionStatus::Executed);
        CHECK(registry.Execute(InvertSelectionId(), context).status == CommandRegistry::ExecutionStatus::Executed);
    }
    REQUIRE(executed.size() == sources.size() * 2);
    for( std::size_t index = 0; index < executed.size(); index += 2 ) {
        CHECK(executed[index] == PaneSelectionIntent::SelectAll);
        CHECK(executed[index + 1] == PaneSelectionIntent::Invert);
    }

    live = PaneSelectionAvailability::Empty;
    const CommandContext context{.native_target = Target()};
    const auto empty_state = registry.QueryState(SelectAllId(), context).state;
    CHECK_FALSE(empty_state.enabled);
    REQUIRE(empty_state.disabled_reason);
    CHECK(empty_state.disabled_reason->code == "selection.noVisibleItems");
    CHECK(registry.Execute(InvertSelectionId(), context).status == CommandRegistry::ExecutionStatus::Disabled);
    CHECK(registry.QueryState(SelectAllId(), {}).state.disabled_reason->code == "context.paneTargetRequired");

    CommandRegistry missing;
    CHECK(missing.Register(MakeFileNewFolderCommand({}, {})) == CommandRegistry::RegisterResult::MissingHandler);
    CHECK(missing.Register(MakeFileNewFileCommand({}, {})) == CommandRegistry::RegisterResult::MissingHandler);
    CHECK(missing.Register(MakePaneSelectAllCommand({}, {})) == CommandRegistry::RegisterResult::MissingHandler);
    CHECK(missing.Register(MakePaneInvertSelectionCommand({}, {})) == CommandRegistry::RegisterResult::MissingHandler);
}

TEST_CASE(PREFIX "defines stable archive-create duplicate and copy-path metadata")
{
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeArchiveCreateCommand(
                [](void *, std::span<const ListingItem>) { return ArchiveCreateAvailability::Available; },
                [](void *, std::span<const ListingItem>, const void *) {
                    return ArchiveCreateAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakeFileDuplicateCommand(
                [](void *, std::span<const ListingItem>) { return FileDuplicateAvailability::Available; },
                [](void *, std::span<const ListingItem>, const void *) {
                    return FileDuplicateAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakeFileCopyPathCommand(
                [](void *, std::span<const ListingItem>) { return FileCopyPathAvailability::Available; },
                [](void *, std::span<const ListingItem>, const void *) {
                    return FileCopyPathAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);

    const auto *const archive = registry.Find(ArchiveCreateId());
    const auto *const duplicate = registry.Find(DuplicateId());
    const auto *const copy_path = registry.Find(CopyPathId());
    REQUIRE(archive);
    REQUIRE(duplicate);
    REQUIRE(copy_path);
    CHECK(archive->category == nc::core::CommandCategory::Archive);
    CHECK(archive->legacy->selector_name == "onCompressItemsHere:");
    CHECK(archive->legacy->shortcut_action_names == std::vector<std::string>{"menu.command.compress_here"});
    CHECK(archive->legacy->shortcut_tag == 15'100);
    CHECK_FALSE(archive->requires_operation_plan);
    CHECK(duplicate->legacy->selector_name == "OnDuplicate:");
    CHECK(duplicate->legacy->shortcut_action_names == std::vector<std::string>{"menu.file.duplicate"});
    CHECK(duplicate->legacy->shortcut_tag == 11'150);
    CHECK_FALSE(duplicate->requires_operation_plan);
    CHECK(copy_path->category == nc::core::CommandCategory::Edit);
    CHECK(copy_path->legacy->selector_name == "OnCopyCurrentFilePath:");
    CHECK(copy_path->legacy->shortcut_tag == 15'040);
}

TEST_CASE(PREFIX "archive extract exposes one stable Extract Here definition")
{
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeArchiveExtractCommand(
                [](void *, std::span<const ListingItem>) { return ArchiveExtractAvailability::Available; },
                [](void *, std::span<const ListingItem>, const void *) {
                    return ArchiveExtractAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);

    const auto *const extract = registry.Find(ArchiveExtractId());
    REQUIRE(extract);
    CHECK(extract->title_key == "commands.archive.extract.title");
    CHECK(extract->description_key == "commands.archive.extract.description");
    CHECK(extract->category == nc::core::CommandCategory::Archive);
    CHECK_FALSE(extract->requires_operation_plan);
    CHECK_FALSE(extract->supports_undo);
    REQUIRE(extract->legacy);
    CHECK(extract->legacy->selector_name == "onExtractArchiveHere:");
    CHECK(extract->legacy->shortcut_action_names ==
          std::vector<std::string>{"menu.command.extract_archive_here"});
    CHECK(extract->legacy->shortcut_tag == 15'250);
}

TEST_CASE(PREFIX "archive extract preserves exact payload and typed rejection across every source")
{
    const auto host = std::make_shared<MutationTestHost>(true, true, true, true);
    const auto items = Items(host, {"archive.zip"});
    ArchiveExtractAvailability live = ArchiveExtractAvailability::Available;
    int executions = 0;
    const void *received_sender = nullptr;
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeArchiveExtractCommand(
                [&](void *_target, std::span<const ListingItem> _items) {
                    CHECK(_target == Target());
                    CHECK(_items.data() == items.data());
                    CHECK(_items.size() == 1);
                    return live;
                },
                [&](void *_target, std::span<const ListingItem> _items, const void *_sender) {
                    CHECK(_target == Target());
                    CHECK(_items.data() == items.data());
                    CHECK(_items.size() == 1);
                    ++executions;
                    received_sender = _sender;
                    return executions == 4 ? ArchiveExtractAvailability::StaleContext
                                           : ArchiveExtractAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);

    int sender = 0;
    constexpr std::array sources{CommandInvocationSource::Menu,
                                 CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut};
    for( size_t index = 0; index < sources.size(); ++index ) {
        const CommandContext context{
            .source = sources[index],
            .native_sender = &sender,
            .native_target = Target(),
            .items = items,
        };
        const auto result = registry.Execute(ArchiveExtractId(), context);
        CHECK(result.status == (index + 1 == sources.size() ? CommandRegistry::ExecutionStatus::Rejected
                                                            : CommandRegistry::ExecutionStatus::Executed));
        if( result.status == CommandRegistry::ExecutionStatus::Rejected ) {
            REQUIRE(result.disabled_reason);
            CHECK(result.disabled_reason->code == "context.stale");
        }
    }
    CHECK(executions == 4);
    CHECK(received_sender == &sender);

    constexpr std::array unavailable{
        ArchiveExtractAvailability::WindowUnavailable,
        ArchiveExtractAvailability::Loading,
        ArchiveExtractAvailability::SelectionUnavailable,
        ArchiveExtractAvailability::SourceUnsupported,
        ArchiveExtractAvailability::SourceUnreadable,
        ArchiveExtractAvailability::DestinationReadOnly,
        ArchiveExtractAvailability::ProviderUnsupported,
        ArchiveExtractAvailability::CaseSensitivityUnavailable,
    };
    for( const auto availability : unavailable ) {
        live = availability;
        const CommandContext context{.native_target = Target(), .items = items};
        const auto state = registry.QueryState(ArchiveExtractId(), context).state;
        CHECK_FALSE(state.enabled);
        REQUIRE(state.disabled_reason);
        CHECK_FALSE(state.disabled_reason->code.empty());
        CHECK(registry.Execute(ArchiveExtractId(), context).status ==
              CommandRegistry::ExecutionStatus::Disabled);
    }

    CHECK(registry.QueryState(ArchiveExtractId(), {}).state.disabled_reason->code ==
          "context.paneTargetRequired");
    CommandRegistry missing;
    CHECK(missing.Register(MakeArchiveExtractCommand({}, {})) ==
          CommandRegistry::RegisterResult::MissingHandler);
}

TEST_CASE(PREFIX "archive-create duplicate and copy-path preserve exact payloads across mounted sources")
{
    const auto host = std::make_shared<MutationTestHost>(true, true, true, true);
    const auto items = Items(host, {"one", "two"});
    int executions = 0;
    std::span<const ListingItem> received;
    const void *received_sender = nullptr;
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeArchiveCreateCommand(
                [](void *_target, std::span<const ListingItem> _items) {
                    CHECK(_target == Target());
                    CHECK(_items.size() == 2);
                    return ArchiveCreateAvailability::Available;
                },
                [&](void *_target, std::span<const ListingItem> _items, const void *_sender) {
                    CHECK(_target == Target());
                    ++executions;
                    received = _items;
                    received_sender = _sender;
                    return ArchiveCreateAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakeFileDuplicateCommand(
                [](void *, std::span<const ListingItem>) { return FileDuplicateAvailability::Available; },
                [&](void *, std::span<const ListingItem> _items, const void *) {
                    ++executions;
                    received = _items;
                    return FileDuplicateAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakeFileCopyPathCommand(
                [](void *, std::span<const ListingItem>) { return FileCopyPathAvailability::Available; },
                [&](void *, std::span<const ListingItem> _items, const void *) {
                    ++executions;
                    received = _items;
                    return FileCopyPathAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);

    int sender = 0;
    const CommandContext context{
        .source = CommandInvocationSource::ContextMenu,
        .native_sender = &sender,
        .native_target = Target(),
        .items = items,
    };
    CHECK(registry.Execute(ArchiveCreateId(), context).status == CommandRegistry::ExecutionStatus::Executed);
    CHECK(registry.Execute(DuplicateId(), context).status == CommandRegistry::ExecutionStatus::Executed);
    CHECK(registry.Execute(CopyPathId(), context).status == CommandRegistry::ExecutionStatus::Executed);
    CHECK(executions == 3);
    CHECK(received.data() == items.data());
    CHECK(received.size() == items.size());
    CHECK(received_sender == &sender);
}

TEST_CASE(PREFIX "new payload commands fail closed for selection capability and stale execution")
{
    const auto host = std::make_shared<MutationTestHost>(true, true, true, true);
    const auto items = Items(host, {"one"});
    const auto parent = Items(host, {".."});

    constexpr std::array archive_unavailable{
        ArchiveCreateAvailability::WindowUnavailable,
        ArchiveCreateAvailability::Loading,
        ArchiveCreateAvailability::SourceUnreadable,
        ArchiveCreateAvailability::SourceNameCollision,
        ArchiveCreateAvailability::DestinationReadOnly,
        ArchiveCreateAvailability::ProviderUnsupported,
    };
    for( const auto unavailable : archive_unavailable ) {
        CommandRegistry registry;
        int executions = 0;
        REQUIRE(registry.Register(MakeArchiveCreateCommand(
                    [unavailable](void *, std::span<const ListingItem>) { return unavailable; },
                    [&](void *, std::span<const ListingItem>, const void *) {
                        ++executions;
                        return ArchiveCreateAvailability::Available;
                    })) == CommandRegistry::RegisterResult::Registered);
        const CommandContext context{.native_target = Target(), .items = items};
        CHECK_FALSE(registry.QueryState(ArchiveCreateId(), context).state.enabled);
        CHECK(registry.Execute(ArchiveCreateId(), context).status == CommandRegistry::ExecutionStatus::Disabled);
        CHECK(executions == 0);
    }

    CommandRegistry registry;
    REQUIRE(registry.Register(MakeFileDuplicateCommand(
                [](void *, std::span<const ListingItem>) { return FileDuplicateAvailability::Available; },
                [](void *, std::span<const ListingItem>, const void *) {
                    return FileDuplicateAvailability::StaleContext;
                })) == CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakeFileCopyPathCommand(
                [](void *, std::span<const ListingItem>) { return FileCopyPathAvailability::Available; },
                [](void *, std::span<const ListingItem>, const void *) {
                    return FileCopyPathAvailability::ClipboardUnavailable;
                })) == CommandRegistry::RegisterResult::Registered);

    for( const CommandId &id : std::array{DuplicateId(), CopyPathId()} ) {
        CHECK_FALSE(registry.QueryState(id, {}).state.enabled);
        CHECK_FALSE(registry.QueryState(id, CommandContext{.native_target = Target()}).state.enabled);
        CHECK_FALSE(registry.QueryState(id, CommandContext{.native_target = Target(), .items = parent}).state.enabled);
    }
    const CommandContext context{.native_target = Target(), .items = items};
    const auto duplicate = registry.Execute(DuplicateId(), context);
    CHECK(duplicate.status == CommandRegistry::ExecutionStatus::Rejected);
    REQUIRE(duplicate.disabled_reason);
    CHECK(duplicate.disabled_reason->code == "context.stale");
    const auto copy_path = registry.Execute(CopyPathId(), context);
    CHECK(copy_path.status == CommandRegistry::ExecutionStatus::Rejected);
    REQUIRE(copy_path.disabled_reason);
    CHECK(copy_path.disabled_reason->code == "clipboard.unavailable");

    CommandRegistry missing;
    CHECK(missing.Register(MakeArchiveCreateCommand({}, {})) == CommandRegistry::RegisterResult::MissingHandler);
    CHECK(missing.Register(MakeFileDuplicateCommand({}, {})) == CommandRegistry::RegisterResult::MissingHandler);
    CHECK(missing.Register(MakeFileCopyPathCommand({}, {})) == CommandRegistry::RegisterResult::MissingHandler);
}

TEST_CASE(PREFIX "defines stable calculate-sizes and batch-rename metadata")
{
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeFileCalculateSizesCommand(
                [](void *, std::span<const ListingItem>) { return FileCalculateSizesAvailability::Available; },
                [](void *, std::span<const ListingItem>, const void *) {
                    return FileCalculateSizesAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakeFileBatchRenameCommand(
                [](void *, std::span<const ListingItem>) { return FileBatchRenameAvailability::Available; },
                [](void *, std::span<const ListingItem>, const void *) {
                    return FileBatchRenameAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);

    const auto *const calculate = registry.Find(CalculateSizesId());
    const auto *const batch = registry.Find(BatchRenameId());
    REQUIRE(calculate);
    REQUIRE(batch);
    CHECK(calculate->title_key == "commands.file.calculateSizes.title");
    CHECK(calculate->category == nc::core::CommandCategory::File);
    REQUIRE(calculate->legacy);
    CHECK(calculate->legacy->selector_name == "OnCalculateSizes:");
    CHECK(calculate->legacy->shortcut_action_names == std::vector<std::string>{"menu.file.calculate_sizes"});
    CHECK(calculate->legacy->shortcut_tag == 11'030);
    CHECK(batch->title_key == "commands.file.batchRename.title");
    CHECK(batch->category == nc::core::CommandCategory::File);
    CHECK_FALSE(batch->requires_operation_plan);
    CHECK_FALSE(batch->supports_undo);
    REQUIRE(batch->legacy);
    CHECK(batch->legacy->selector_name == "OnBatchRename:");
    CHECK(batch->legacy->shortcut_action_names == std::vector<std::string>{"menu.command.batch_rename"});
    CHECK(batch->legacy->shortcut_tag == 15'220);
}

TEST_CASE(PREFIX "calculate-sizes and batch-rename preserve exact payloads for every invocation source")
{
    const auto host = std::make_shared<MutationTestHost>(true, true, true, true);
    const auto items = Items(host, {"one", "two"});
    int calculate_executions = 0;
    int batch_executions = 0;
    std::span<const ListingItem> received;
    const void *received_sender = nullptr;
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeFileCalculateSizesCommand(
                [](void *_target, std::span<const ListingItem> _items) {
                    CHECK(_target == Target());
                    CHECK(_items.size() == 2);
                    return FileCalculateSizesAvailability::Available;
                },
                [&](void *_target, std::span<const ListingItem> _items, const void *_sender) {
                    CHECK(_target == Target());
                    ++calculate_executions;
                    received = _items;
                    received_sender = _sender;
                    return FileCalculateSizesAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakeFileBatchRenameCommand(
                [](void *_target, std::span<const ListingItem> _items) {
                    CHECK(_target == Target());
                    CHECK(_items.size() == 2);
                    return FileBatchRenameAvailability::Available;
                },
                [&](void *_target, std::span<const ListingItem> _items, const void *_sender) {
                    CHECK(_target == Target());
                    ++batch_executions;
                    received = _items;
                    received_sender = _sender;
                    return FileBatchRenameAvailability::Available;
                })) == CommandRegistry::RegisterResult::Registered);

    int sender = 0;
    constexpr std::array sources{CommandInvocationSource::Menu,
                                 CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut};
    for( const auto source : sources ) {
        const CommandContext context{
            .source = source,
            .native_sender = &sender,
            .native_target = Target(),
            .items = items,
        };
        CHECK(registry.Execute(CalculateSizesId(), context).status == CommandRegistry::ExecutionStatus::Executed);
        CHECK(registry.Execute(BatchRenameId(), context).status == CommandRegistry::ExecutionStatus::Executed);
        CHECK(received.data() == items.data());
        CHECK(received.size() == items.size());
        CHECK(received_sender == &sender);
    }
    CHECK(calculate_executions == static_cast<int>(sources.size()));
    CHECK(batch_executions == static_cast<int>(sources.size()));
}

TEST_CASE(PREFIX "calculate-sizes fails closed for non-directory and stale or busy submission")
{
    const auto host = std::make_shared<MutationTestHost>(true, true, true, true);
    const auto items = Items(host, {"folder"});
    const auto parent = Items(host, {".."});
    constexpr std::array unavailable{
        FileCalculateSizesAvailability::Loading,
        FileCalculateSizesAvailability::DirectoryRequired,
        FileCalculateSizesAvailability::SourceUnreadable,
        FileCalculateSizesAvailability::Busy,
        FileCalculateSizesAvailability::StaleContext,
    };
    for( const auto availability : unavailable ) {
        int executions = 0;
        CommandRegistry registry;
        REQUIRE(registry.Register(MakeFileCalculateSizesCommand(
                    [availability](void *, std::span<const ListingItem>) { return availability; },
                    [&](void *, std::span<const ListingItem>, const void *) {
                        ++executions;
                        return FileCalculateSizesAvailability::Available;
                    })) == CommandRegistry::RegisterResult::Registered);
        const CommandContext context{.native_target = Target(), .items = items};
        const auto state = registry.QueryState(CalculateSizesId(), context).state;
        CHECK_FALSE(state.enabled);
        REQUIRE(state.disabled_reason);
        CHECK_FALSE(state.disabled_reason->user_message_key.empty());
        CHECK(registry.Execute(CalculateSizesId(), context).status == CommandRegistry::ExecutionStatus::Disabled);
        CHECK(executions == 0);
    }

    CommandRegistry stale;
    REQUIRE(stale.Register(MakeFileCalculateSizesCommand(
                [](void *, std::span<const ListingItem>) { return FileCalculateSizesAvailability::Available; },
                [](void *, std::span<const ListingItem>, const void *) {
                    return FileCalculateSizesAvailability::StaleContext;
                })) == CommandRegistry::RegisterResult::Registered);
    const CommandContext context{.native_target = Target(), .items = items};
    const auto rejected = stale.Execute(CalculateSizesId(), context);
    CHECK(rejected.status == CommandRegistry::ExecutionStatus::Rejected);
    REQUIRE(rejected.disabled_reason);
    CHECK(rejected.disabled_reason->code == "context.stale");
    CHECK_FALSE(stale.QueryState(CalculateSizesId(), {}).state.enabled);
    CHECK_FALSE(stale.QueryState(CalculateSizesId(), CommandContext{.native_target = Target()}).state.enabled);
    CHECK_FALSE(stale.QueryState(CalculateSizesId(),
                                 CommandContext{.native_target = Target(), .items = parent})
                    .state.enabled);
}

TEST_CASE(PREFIX "batch-rename maps reviewed failures and never executes a disabled intent")
{
    const auto host = std::make_shared<MutationTestHost>(true, true, true, true);
    const auto items = Items(host, {"one"});
    const auto parent = Items(host, {".."});
    constexpr std::array unavailable{
        FileBatchRenameAvailability::WindowUnavailable,
        FileBatchRenameAvailability::Loading,
        FileBatchRenameAvailability::ProviderUnavailable,
        FileBatchRenameAvailability::MixedProviders,
        FileBatchRenameAvailability::ProviderUnsupported,
        FileBatchRenameAvailability::DestinationConflict,
        FileBatchRenameAvailability::StaleContext,
    };
    for( const auto availability : unavailable ) {
        int executions = 0;
        CommandRegistry registry;
        REQUIRE(registry.Register(MakeFileBatchRenameCommand(
                    [availability](void *, std::span<const ListingItem>) { return availability; },
                    [&](void *, std::span<const ListingItem>, const void *) {
                        ++executions;
                        return FileBatchRenameAvailability::Available;
                    })) == CommandRegistry::RegisterResult::Registered);
        const CommandContext context{.native_target = Target(), .items = items};
        CHECK_FALSE(registry.QueryState(BatchRenameId(), context).state.enabled);
        CHECK(registry.Execute(BatchRenameId(), context).status == CommandRegistry::ExecutionStatus::Disabled);
        CHECK(executions == 0);
    }

    CommandRegistry invalid;
    REQUIRE(invalid.Register(MakeFileBatchRenameCommand(
                [](void *, std::span<const ListingItem>) { return FileBatchRenameAvailability::Available; },
                [](void *, std::span<const ListingItem>, const void *) {
                    return FileBatchRenameAvailability::InvalidPlan;
                })) == CommandRegistry::RegisterResult::Registered);
    const CommandContext context{.native_target = Target(), .items = items};
    const auto rejected = invalid.Execute(BatchRenameId(), context);
    CHECK(rejected.status == CommandRegistry::ExecutionStatus::Rejected);
    REQUIRE(rejected.disabled_reason);
    CHECK(rejected.disabled_reason->code == "plan.invalid");
    CHECK_FALSE(invalid.QueryState(BatchRenameId(), {}).state.enabled);
    CHECK_FALSE(invalid.QueryState(BatchRenameId(), CommandContext{.native_target = Target()}).state.enabled);
    CHECK_FALSE(invalid.QueryState(BatchRenameId(),
                                   CommandContext{.native_target = Target(), .items = parent})
                    .state.enabled);

    CommandRegistry missing;
    CHECK(missing.Register(MakeFileCalculateSizesCommand({}, {})) == CommandRegistry::RegisterResult::MissingHandler);
    CHECK(missing.Register(MakeFileBatchRenameCommand({}, {})) == CommandRegistry::RegisterResult::MissingHandler);
}

#undef PREFIX
