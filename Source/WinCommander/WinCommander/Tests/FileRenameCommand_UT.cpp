// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/FileRenameCommand.h>
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
using nc::core::FileRenameInitiationError;
using nc::core::MakeFileRenameCommand;
using nc::vfs::ListingItem;

class FileRenameTestHost final : public nc::vfs::Host
{
public:
    explicit FileRenameTestHost(const bool _supports_rename)
        : Host("/", nullptr, _supports_rename ? "file_rename_writable_test" : "file_rename_read_only_test")
    {
        if( _supports_rename )
            AddFeatures(nc::vfs::HostFeatures::Read | nc::vfs::HostFeatures::Rename);
    }

    bool IsWritable() const override { return true; }
};

std::vector<ListingItem> Items(const bool _supports_rename,
                               const std::initializer_list<std::string_view> _names)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/";
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = std::make_shared<FileRenameTestHost>(_supports_rename);
    for( const std::string_view name : _names ) {
        const bool is_directory = name == "..";
        input.filenames.emplace_back(name);
        input.unix_modes.emplace_back((is_directory ? S_IFDIR : S_IFREG) | S_IRUSR);
        input.unix_types.emplace_back(is_directory ? DT_DIR : DT_REG);
    }

    const auto listing = VFSListing::Build(std::move(input));
    std::vector<ListingItem> items;
    items.reserve(_names.size());
    for( std::size_t index = 0; index < _names.size(); ++index )
        items.emplace_back(listing->Item(static_cast<unsigned>(index)));
    return items;
}

CommandId FileRenameId()
{
    return CommandId{nc::core::command_ids::FileRename};
}

CommandRegistry RegistryWithInitiator(nc::core::FileRenameInitiator _initiator)
{
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeFileRenameCommand(std::move(_initiator))) ==
            CommandRegistry::RegisterResult::Registered);
    return registry;
}

void *Target()
{
    static int target;
    return &target;
}

CommandContext Context(std::span<const ListingItem> _items,
                       CommandInvocationSource _source = CommandInvocationSource::Programmatic,
                       void *_target = Target())
{
    return CommandContext{
        .source = _source,
        .native_target = _target,
        .items = _items,
    };
}

} // namespace

#define PREFIX "nc::core::FileRenameCommand "

TEST_CASE(PREFIX "defines stable initiation metadata")
{
    const auto registry = RegistryWithInitiator([](void *, const ListingItem &) { return true; });
    const auto *const descriptor = registry.Find(FileRenameId());

    REQUIRE(descriptor);
    CHECK(descriptor->id.Value() == "file.rename");
    CHECK(descriptor->title_key == "commands.file.rename.title");
    CHECK(descriptor->description_key == "commands.file.rename.description");
    CHECK(descriptor->category == nc::core::CommandCategory::File);
    CHECK(descriptor->icon_name == "pencil");
    CHECK_FALSE(descriptor->is_destructive);
    CHECK_FALSE(descriptor->requires_operation_plan);
    CHECK_FALSE(descriptor->supports_undo);
    CHECK(descriptor->analytics_name == "file.rename");
    REQUIRE(descriptor->legacy);
    CHECK(descriptor->legacy->selector_name == "OnRenameFileInPlace:");
    CHECK(descriptor->legacy->shortcut_action_names == std::vector<std::string>{"menu.command.rename_in_place"});
    CHECK(descriptor->legacy->shortcut_tag == 15'141);
}

TEST_CASE(PREFIX "initiates exactly once for every supported invocation source")
{
    int calls = 0;
    const auto items = Items(true, {"document.txt"});
    void *received_target = nullptr;
    bool received_exact_item = false;
    const auto registry = RegistryWithInitiator([&](void *_target, const ListingItem &_item) {
        ++calls;
        received_target = _target;
        received_exact_item = &_item == &items.front();
        return true;
    });
    constexpr std::array sources{CommandInvocationSource::Menu,
                                 CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut,
                                 CommandInvocationSource::Palette,
                                 CommandInvocationSource::Programmatic};

    for( std::size_t index = 0; index < sources.size(); ++index ) {
        CHECK(registry.Execute(FileRenameId(), Context(items, sources[index])).status ==
              CommandRegistry::ExecutionStatus::Executed);
        CHECK(calls == static_cast<int>(index + 1));
        CHECK(received_target == Target());
        CHECK(received_exact_item);
    }
}

TEST_CASE(PREFIX "returns structured disabled reasons for invalid context")
{
    const auto registry = RegistryWithInitiator([](void *, const ListingItem &) { return true; });
    const std::vector<ListingItem> empty;
    const auto writable = Items(true, {"first", "second"});
    const auto parent = Items(true, {".."});
    const auto unsupported = Items(false, {"document.txt"});

    const auto check = [&](const CommandContext &_context, const std::string_view _code) {
        const auto state = registry.QueryState(FileRenameId(), _context).state;
        CHECK_FALSE(state.enabled);
        REQUIRE(state.disabled_reason);
        CHECK(state.disabled_reason->code == _code);
        CHECK(registry.Execute(FileRenameId(), _context).status == CommandRegistry::ExecutionStatus::Disabled);
    };

    check(Context(empty), "selection.empty");
    check(Context(writable), "selection.singleItemRequired");
    check(Context(parent), "selection.parentEntryUnsupported");
    check(Context(unsupported), "provider.renameUnsupported");
    check(Context(std::span<const ListingItem>{writable.data(), 1}, CommandInvocationSource::Menu, nullptr),
          "context.paneTargetRequired");
}

TEST_CASE(PREFIX "reports a failed pane initiation instead of execution success")
{
    const auto registry = RegistryWithInitiator([](void *, const ListingItem &) { return false; });
    const auto items = Items(true, {"document.txt"});

    CHECK_THROWS_AS(registry.Execute(FileRenameId(), Context(items)), FileRenameInitiationError);
}

TEST_CASE(PREFIX "rejects registration without an initiator")
{
    CommandRegistry registry;
    CHECK(registry.Register(MakeFileRenameCommand({})) == CommandRegistry::RegisterResult::MissingHandler);
}

#undef PREFIX
