// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/FileCopyCommand.h>
#include <array>
#include <memory>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

using nc::core::CommandContext;
using nc::core::CommandId;
using nc::core::CommandInvocationSource;
using nc::core::CommandRegistry;
using nc::core::MakeFileCopyCommand;
using nc::vfs::ListingItem;

class FileCopyTestHost final : public nc::vfs::Host
{
public:
    explicit FileCopyTestHost(const bool _native)
        : Host("/", nullptr, _native ? "file_copy_native_test" : "file_copy_remote_test"), m_Native(_native)
    {
    }

    bool IsNativeFS() const noexcept override { return m_Native; }

private:
    bool m_Native;
};

std::vector<ListingItem> Items(const bool _native, const std::size_t _count)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/";
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = std::make_shared<FileCopyTestHost>(_native);
    for( std::size_t index = 0; index < _count; ++index ) {
        input.filenames.emplace_back("item-" + std::to_string(index));
        input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
        input.unix_types.emplace_back(DT_REG);
    }

    const auto listing = VFSListing::Build(std::move(input));
    std::vector<ListingItem> items;
    items.reserve(_count);
    for( std::size_t index = 0; index < _count; ++index )
        items.emplace_back(listing->Item(static_cast<unsigned>(index)));
    return items;
}

CommandId FileCopyId()
{
    return CommandId{nc::core::command_ids::FileCopy};
}

CommandRegistry RegistryWithWriter(nc::core::FileCopyWriter _writer)
{
    CommandRegistry registry;
    const auto result = registry.Register(MakeFileCopyCommand(std::move(_writer)));
    REQUIRE(result == CommandRegistry::RegisterResult::Registered);
    return registry;
}

} // namespace

#define PREFIX "nc::core::FileCopyCommand "

TEST_CASE(PREFIX "defines stable presentation and legacy responder metadata")
{
    const CommandRegistry registry = RegistryWithWriter([](std::span<const ListingItem>) {});
    const auto *const descriptor = registry.Find(FileCopyId());

    REQUIRE(descriptor);
    CHECK(descriptor->id.Value() == "file.copy");
    CHECK(descriptor->title_key == "commands.file.copy.title");
    CHECK(descriptor->description_key == "commands.file.copy.description");
    CHECK(descriptor->icon_name == "doc.on.doc");
    CHECK(descriptor->analytics_name == "file.copy");
    REQUIRE(descriptor->legacy);
    CHECK(descriptor->legacy->selector_name == "copy:");
    CHECK(descriptor->legacy->shortcut_action_names == std::vector<std::string>{"menu.edit.copy"});
    CHECK(descriptor->legacy->shortcut_tag == 12'000);
}

TEST_CASE(PREFIX "uses the same definition for four invocation sources and writes exactly once")
{
    int writes = 0;
    const ListingItem *written_data = nullptr;
    std::size_t written_size = 0;
    const auto registry = RegistryWithWriter([&](const std::span<const ListingItem> _items) {
        ++writes;
        written_data = _items.data();
        written_size = _items.size();
    });
    const std::vector<ListingItem> items = Items(true, 2);
    constexpr std::array sources{CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::Menu,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut};

    for( std::size_t index = 0; index < sources.size(); ++index ) {
        const CommandInvocationSource source = sources[index];
        const CommandContext context{.source = source, .items = items};
        CHECK(registry.Execute(FileCopyId(), context).status == CommandRegistry::ExecutionStatus::Executed);
        CHECK(writes == static_cast<int>(index + 1));
        CHECK(written_data == items.data());
        CHECK(written_size == items.size());
    }
}

TEST_CASE(PREFIX "returns the localized empty-selection reason and does not write")
{
    int writes = 0;
    const auto registry = RegistryWithWriter([&](std::span<const ListingItem>) { ++writes; });
    const CommandContext context{.source = CommandInvocationSource::Toolbar};

    const auto state = registry.QueryState(FileCopyId(), context);
    REQUIRE(state.status == CommandRegistry::LookupStatus::Found);
    CHECK_FALSE(state.state.enabled);
    REQUIRE(state.state.disabled_reason);
    CHECK(state.state.disabled_reason->code == "selection.empty");
    CHECK(state.state.disabled_reason->user_message_key == "commands.file.copy.disabled.selectionEmpty");

    const auto execution = registry.Execute(FileCopyId(), context);
    CHECK(execution.status == CommandRegistry::ExecutionStatus::Disabled);
    REQUIRE(execution.disabled_reason);
    CHECK(execution.disabled_reason->user_message_key == "commands.file.copy.disabled.selectionEmpty");
    CHECK(writes == 0);
}

TEST_CASE(PREFIX "passes the explicit context-menu item override instead of the pane selection")
{
    std::vector<std::pair<const ListingItem *, std::size_t>> writes;
    const auto registry = RegistryWithWriter(
        [&](const std::span<const ListingItem> _items) { writes.emplace_back(_items.data(), _items.size()); });
    const std::vector<ListingItem> selected_items = Items(true, 3);
    const std::span<const ListingItem> clicked_item{selected_items.data() + 1, 1};

    CHECK(
        registry
            .Execute(FileCopyId(), CommandContext{.source = CommandInvocationSource::Toolbar, .items = selected_items})
            .status == CommandRegistry::ExecutionStatus::Executed);
    CHECK(registry
              .Execute(FileCopyId(),
                       CommandContext{.source = CommandInvocationSource::ContextMenu, .items = clicked_item})
              .status == CommandRegistry::ExecutionStatus::Executed);

    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == std::pair{selected_items.data(), selected_items.size()});
    CHECK(writes[1] == std::pair{clicked_item.data(), clicked_item.size()});
}

TEST_CASE(PREFIX "rejects remote and mixed contexts before the native pasteboard writer")
{
    int writes = 0;
    const auto registry = RegistryWithWriter([&](std::span<const ListingItem>) { ++writes; });
    const std::vector<ListingItem> remote_items = Items(false, 2);
    std::vector<ListingItem> mixed_items = Items(true, 1);
    const std::vector<ListingItem> one_remote_item = Items(false, 1);
    mixed_items.emplace_back(one_remote_item.front());

    for( const std::span<const ListingItem> items : {std::span<const ListingItem>{remote_items},
                                                     std::span<const ListingItem>{mixed_items}} ) {
        const auto state = registry.QueryState(FileCopyId(), CommandContext{.items = items});
        CHECK_FALSE(state.state.enabled);
        REQUIRE(state.state.disabled_reason);
        CHECK(state.state.disabled_reason->code == "provider.nativeItemsRequired");
        CHECK(state.state.disabled_reason->user_message_key ==
              "commands.file.copy.disabled.nativeItemsRequired");

        const auto execution = registry.Execute(FileCopyId(), CommandContext{.items = items});
        CHECK(execution.status == CommandRegistry::ExecutionStatus::Disabled);
    }
    CHECK(writes == 0);
}

TEST_CASE(PREFIX "does not register a command without its composition writer")
{
    CommandRegistry registry;
    CHECK(registry.Register(MakeFileCopyCommand({})) == CommandRegistry::RegisterResult::MissingHandler);
    CHECK(registry.Find(FileCopyId()) == nullptr);
}
