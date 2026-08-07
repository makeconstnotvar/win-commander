// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/FilePreviewCommand.h>
#include <array>
#include <memory>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <type_traits>
#include <vector>

namespace {

using nc::core::CommandContext;
using nc::core::CommandId;
using nc::core::CommandInvocationSource;
using nc::core::CommandRegistry;
using nc::core::FilePreviewIntent;
using nc::core::MakeFilePreviewCommand;
using nc::vfs::ListingItem;

static_assert(std::is_trivially_copyable_v<FilePreviewIntent>);
static_assert(std::is_standard_layout_v<FilePreviewIntent>);

class PreviewTestHost final : public nc::vfs::Host
{
public:
    PreviewTestHost() : Host("/", nullptr, "file_preview") {}
};

ListingItem Item(const char *_filename, const mode_t _mode, const uint8_t _type)
{
    nc::vfs::ListingInput input;
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = std::make_shared<PreviewTestHost>();
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/fixture/";
    input.filenames = {_filename};
    input.unix_modes = {_mode};
    input.unix_types = {_type};
    return VFSListing::Build(std::move(input))->Item(0);
}

CommandId FilePreviewId()
{
    return CommandId{nc::core::command_ids::FilePreview};
}

CommandRegistry RegistryWithHandler(nc::core::FilePreviewHandler _handler)
{
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeFilePreviewCommand(std::move(_handler))) ==
            CommandRegistry::RegisterResult::Registered);
    return registry;
}

CommandContext Context(const std::span<const ListingItem> _items,
                       void *_target,
                       const CommandInvocationSource _source = CommandInvocationSource::Programmatic,
                       const void *_sender = nullptr)
{
    return CommandContext{
        .source = _source,
        .native_sender = _sender,
        .native_target = _target,
        .items = _items,
    };
}

} // namespace

#define PREFIX "nc::core::FilePreviewCommand "

TEST_CASE(PREFIX "defines the stable read-only Quick Look descriptor and exact legacy route")
{
    const auto registry = RegistryWithHandler([](void *, const ListingItem &, FilePreviewIntent) { return true; });
    const auto *const descriptor = registry.Find(FilePreviewId());

    REQUIRE(descriptor);
    CHECK(descriptor->id.Value() == "file.preview");
    CHECK(descriptor->title_key == "commands.file.preview.title");
    CHECK(descriptor->description_key == "commands.file.preview.description");
    CHECK(descriptor->category == nc::core::CommandCategory::File);
    CHECK(descriptor->icon_name == "eye");
    CHECK_FALSE(descriptor->is_destructive);
    CHECK_FALSE(descriptor->requires_operation_plan);
    CHECK_FALSE(descriptor->supports_undo);
    CHECK(descriptor->analytics_name == "file.preview");
    REQUIRE(descriptor->legacy);
    CHECK(descriptor->legacy->selector_name == "OnFileViewCommand:");
    CHECK(descriptor->legacy->shortcut_action_names ==
          std::vector<std::string>{"menu.command.quick_look", "panel.show_preview"});
    CHECK(descriptor->legacy->shortcut_tag == 15'070);
}

TEST_CASE(PREFIX "requires a live target and exactly one valid non-parent item")
{
    int calls = 0;
    const auto registry = RegistryWithHandler([&](void *, const ListingItem &, FilePreviewIntent) {
        ++calls;
        return true;
    });
    int target = 0;
    const ListingItem regular = Item("document.pdf", S_IFREG | 0644, DT_REG);
    const ListingItem directory = Item("Folder", S_IFDIR | 0755, DT_DIR);
    const ListingItem parent = Item("..", S_IFDIR | 0755, DT_DIR);
    const std::array one{regular};
    const std::array multiple{regular, directory};
    const std::array invalid{ListingItem{}};
    const std::array parent_only{parent};

    const auto check = [&](const CommandContext &_context, const std::string_view _reason) {
        const auto state = registry.QueryState(FilePreviewId(), _context);
        REQUIRE(state.status == CommandRegistry::LookupStatus::Found);
        CHECK_FALSE(state.state.enabled);
        REQUIRE(state.state.disabled_reason);
        CHECK(state.state.disabled_reason->code == _reason);

        const auto execution = registry.Execute(FilePreviewId(), _context);
        CHECK(execution.status == CommandRegistry::ExecutionStatus::Disabled);
        REQUIRE(execution.disabled_reason);
        CHECK(execution.disabled_reason->code == _reason);
    };

    check(Context(one, nullptr), "context.paneTargetRequired");
    check(Context({}, &target), "selection.empty");
    check(Context(multiple, &target), "selection.singleItemRequired");
    check(Context(invalid, &target), "selection.invalidItem");
    check(Context(parent_only, &target), "selection.parentEntryUnsupported");
    CHECK(calls == 0);
}

TEST_CASE(PREFIX "passes the borrowed target, exact item and value intent for every invocation source")
{
    int target = 0;
    void *handled_target = nullptr;
    ListingItem handled_item;
    std::vector<CommandInvocationSource> handled_sources;
    std::vector<const void *> handled_senders;
    const auto registry = RegistryWithHandler([&](void *_target,
                                                  const ListingItem &_item,
                                                  const FilePreviewIntent _intent) {
        handled_target = _target;
        handled_item = _item;
        handled_sources.emplace_back(_intent.source);
        handled_senders.emplace_back(_intent.native_sender);
        return true;
    });
    const std::array item{Item("movie.mov", S_IFREG | 0644, DT_REG)};
    int sender = 0;
    constexpr std::array sources{CommandInvocationSource::Menu,
                                 CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut,
                                 CommandInvocationSource::Palette,
                                 CommandInvocationSource::Programmatic};

    for( const CommandInvocationSource source : sources ) {
        const CommandContext context = Context(item, &target, source, &sender);
        CHECK(registry.QueryState(FilePreviewId(), context).state.enabled);
        CHECK(registry.Execute(FilePreviewId(), context).status == CommandRegistry::ExecutionStatus::Executed);
        CHECK(handled_target == &target);
        CHECK(handled_item.Listing() == item.front().Listing());
        CHECK(handled_item.Index() == item.front().Index());
    }
    CHECK(handled_sources == std::vector<CommandInvocationSource>(sources.begin(), sources.end()));
    CHECK(handled_senders == std::vector<const void *>(sources.size(), &sender));
}

TEST_CASE(PREFIX "reports live target rejection without executing stale preview intent")
{
    int target = 0;
    const std::array item{Item("image.png", S_IFREG | 0644, DT_REG)};
    const auto registry =
        RegistryWithHandler([](void *, const ListingItem &, FilePreviewIntent) { return false; });

    const auto execution = registry.Execute(FilePreviewId(), Context(item, &target));
    CHECK(execution.status == CommandRegistry::ExecutionStatus::Rejected);
    REQUIRE(execution.disabled_reason);
    CHECK(execution.disabled_reason->code == "preview.unavailable");
}

TEST_CASE(PREFIX "does not register without a live preview handler")
{
    CommandRegistry registry;
    CHECK(registry.Register(MakeFilePreviewCommand({})) == CommandRegistry::RegisterResult::MissingHandler);
    CHECK(registry.Find(FilePreviewId()) == nullptr);
}

#undef PREFIX
