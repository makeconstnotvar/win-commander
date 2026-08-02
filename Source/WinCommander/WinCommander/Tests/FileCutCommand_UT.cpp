// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/FileCutCommand.h>
#include <array>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string_view>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

using nc::core::CommandContext;
using nc::core::CommandId;
using nc::core::CommandInvocationSource;
using nc::core::CommandRegistry;
using nc::core::FileCutIntent;
using nc::core::FileCutWriteError;
using nc::core::MakeFileCutCommand;
using nc::vfs::ListingItem;

class FileCutTestHost final : public nc::vfs::Host
{
public:
    explicit FileCutTestHost(const bool _native)
        : Host("/", nullptr, _native ? "file_cut_native_test" : "file_cut_remote_test"), m_Native(_native)
    {
    }

    bool IsNativeFS() const noexcept override { return m_Native; }

private:
    bool m_Native;
};

std::vector<ListingItem> Items(const bool _native, const std::initializer_list<std::string_view> _names)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/";
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = std::make_shared<FileCutTestHost>(_native);
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

CommandId FileCutId()
{
    return CommandId{nc::core::command_ids::FileCut};
}

CommandRegistry RegistryWithWriter(nc::core::FileCutWriter _writer)
{
    CommandRegistry registry;
    const auto result = registry.Register(MakeFileCutCommand(std::move(_writer)));
    REQUIRE(result == CommandRegistry::RegisterResult::Registered);
    return registry;
}

} // namespace

#define PREFIX "nc::core::FileCutCommand "

TEST_CASE(PREFIX "defines stable non-mutating staging metadata")
{
    const CommandRegistry registry =
        RegistryWithWriter([](std::span<const ListingItem>, FileCutIntent) { return true; });
    const auto *const descriptor = registry.Find(FileCutId());

    REQUIRE(descriptor);
    CHECK(descriptor->id.Value() == "file.cut");
    CHECK(descriptor->title_key == "commands.file.cut.title");
    CHECK(descriptor->description_key == "commands.file.cut.description");
    CHECK(descriptor->category == nc::core::CommandCategory::File);
    CHECK(descriptor->icon_name == "scissors");
    CHECK(descriptor->analytics_name == "file.cut");
    CHECK_FALSE(descriptor->is_destructive);
    CHECK_FALSE(descriptor->requires_operation_plan);
    CHECK_FALSE(descriptor->supports_undo);
    REQUIRE(descriptor->legacy);
    CHECK(descriptor->legacy->selector_name == "cut:");
    CHECK(descriptor->legacy->shortcut_action_names.empty());
    CHECK_FALSE(descriptor->legacy->shortcut_tag);
}

TEST_CASE(PREFIX "uses one borrowed payload and move intent for four invocation sources")
{
    int writes = 0;
    const ListingItem *written_data = nullptr;
    std::size_t written_size = 0;
    std::optional<FileCutIntent> written_intent;
    const auto registry = RegistryWithWriter(
        [&](const std::span<const ListingItem> _items, const FileCutIntent _intent) {
            ++writes;
            written_data = _items.data();
            written_size = _items.size();
            written_intent = _intent;
            return true;
        });
    const std::vector<ListingItem> items = Items(true, {"first", "second"});
    constexpr std::array sources{CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::Menu,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut};

    for( std::size_t index = 0; index < sources.size(); ++index ) {
        const CommandContext context{.source = sources[index], .items = items};
        CHECK(registry.Execute(FileCutId(), context).status == CommandRegistry::ExecutionStatus::Executed);
        CHECK(writes == static_cast<int>(index + 1));
        CHECK(written_data == items.data());
        CHECK(written_size == items.size());
        CHECK(written_intent == FileCutIntent::Move);
    }
}

TEST_CASE(PREFIX "uses the explicit context snapshot instead of a current pane snapshot")
{
    const ListingItem *written_data = nullptr;
    std::size_t written_size = 0;
    const auto registry = RegistryWithWriter(
        [&](const std::span<const ListingItem> _items, FileCutIntent) {
            written_data = _items.data();
            written_size = _items.size();
            return true;
        });
    const std::vector<ListingItem> pane_items = Items(true, {"pane-selection"});
    const std::vector<ListingItem> context_items = Items(true, {"context-first", "context-second"});

    const CommandContext context{
        .source = CommandInvocationSource::ContextMenu,
        .items = context_items,
    };
    CHECK(registry.Execute(FileCutId(), context).status == CommandRegistry::ExecutionStatus::Executed);
    CHECK(written_data == context_items.data());
    CHECK(written_data != pane_items.data());
    CHECK(written_size == context_items.size());
}

TEST_CASE(PREFIX "disables an empty context without invoking the writer")
{
    int writes = 0;
    const auto registry = RegistryWithWriter(
        [&](std::span<const ListingItem>, FileCutIntent) {
            ++writes;
            return true;
        });
    const CommandContext context{.source = CommandInvocationSource::Toolbar};

    const auto state = registry.QueryState(FileCutId(), context);
    REQUIRE(state.status == CommandRegistry::LookupStatus::Found);
    CHECK_FALSE(state.state.enabled);
    REQUIRE(state.state.disabled_reason);
    CHECK(state.state.disabled_reason->code == "selection.empty");
    CHECK(state.state.disabled_reason->user_message_key ==
          "commands.file.cut.disabled.selectionEmpty");

    const auto execution = registry.Execute(FileCutId(), context);
    CHECK(execution.status == CommandRegistry::ExecutionStatus::Disabled);
    CHECK(writes == 0);
}

TEST_CASE(PREFIX "rejects the parent entry as one invalid whole payload")
{
    int writes = 0;
    const auto registry = RegistryWithWriter(
        [&](std::span<const ListingItem>, FileCutIntent) {
            ++writes;
            return true;
        });
    const std::vector<ListingItem> items = Items(true, {"ordinary", ".."});
    const CommandContext context{.source = CommandInvocationSource::ContextMenu, .items = items};

    const auto state = registry.QueryState(FileCutId(), context);
    CHECK_FALSE(state.state.enabled);
    REQUIRE(state.state.disabled_reason);
    CHECK(state.state.disabled_reason->code == "selection.parentEntryUnsupported");
    CHECK(state.state.disabled_reason->user_message_key ==
          "commands.file.cut.disabled.parentEntryUnsupported");
    CHECK(registry.Execute(FileCutId(), context).status == CommandRegistry::ExecutionStatus::Disabled);
    CHECK(writes == 0);
}

TEST_CASE(PREFIX "rejects remote and mixed contexts before the writer")
{
    int writes = 0;
    const auto registry = RegistryWithWriter(
        [&](std::span<const ListingItem>, FileCutIntent) {
            ++writes;
            return true;
        });
    const std::vector<ListingItem> remote_items = Items(false, {"remote-a", "remote-b"});
    std::vector<ListingItem> mixed_items = Items(true, {"native"});
    const std::vector<ListingItem> one_remote_item = Items(false, {"remote"});
    mixed_items.emplace_back(one_remote_item.front());

    for( const std::span<const ListingItem> items : {std::span<const ListingItem>{remote_items},
                                                     std::span<const ListingItem>{mixed_items}} ) {
        const auto state = registry.QueryState(FileCutId(), CommandContext{.items = items});
        CHECK_FALSE(state.state.enabled);
        REQUIRE(state.state.disabled_reason);
        CHECK(state.state.disabled_reason->code == "provider.nativeItemsRequired");
        CHECK(state.state.disabled_reason->user_message_key ==
              "commands.file.cut.disabled.nativeItemsRequired");
        CHECK(registry.Execute(FileCutId(), CommandContext{.items = items}).status ==
              CommandRegistry::ExecutionStatus::Disabled);
    }
    CHECK(writes == 0);
}

TEST_CASE(PREFIX "propagates a writer failure instead of reporting successful execution")
{
    int writes = 0;
    const auto registry = RegistryWithWriter(
        [&](std::span<const ListingItem>, FileCutIntent) {
            ++writes;
            return false;
        });
    const std::vector<ListingItem> items = Items(true, {"unwritable"});

    CHECK_THROWS_AS(registry.Execute(FileCutId(), CommandContext{.items = items}), FileCutWriteError);
    CHECK(writes == 1);
}

TEST_CASE(PREFIX "does not register without the injected move-intent writer")
{
    CommandRegistry registry;
    CHECK(registry.Register(MakeFileCutCommand({})) == CommandRegistry::RegisterResult::MissingHandler);
    CHECK(registry.Find(FileCutId()) == nullptr);
}
