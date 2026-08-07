// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/FileGetInfoCommand.h>
#include <array>
#include <memory>
#include <optional>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <type_traits>
#include <vector>

namespace {

using nc::core::CommandContext;
using nc::core::CommandId;
using nc::core::CommandInvocationSource;
using nc::core::CommandRegistry;
using nc::core::FileGetInfoPresentation;
using nc::core::MakeFileGetInfoCommand;
using nc::vfs::ListingItem;

static_assert(std::is_copy_constructible_v<FileGetInfoPresentation>);

class MetadataOnlyTestHost final : public nc::vfs::Host
{
public:
    explicit MetadataOnlyTestHost(const char *_tag) : Host("/", nullptr, _tag) {}
};

VFSListingPtr DetailedListing(const bool _mixed_providers)
{
    nc::vfs::ListingInput input;
    if( _mixed_providers ) {
        input.hosts.reset(nc::base::variable_container<>::type::dense);
        input.hosts.insert(0, std::make_shared<MetadataOnlyTestHost>("get_info_first"));
        input.hosts.insert(1, std::make_shared<MetadataOnlyTestHost>("get_info_second"));
        input.directories.reset(nc::base::variable_container<>::type::dense);
        input.directories.insert(0, "/first/");
        input.directories.insert(1, "/second/");
    }
    else {
        input.hosts.reset(nc::base::variable_container<>::type::common);
        input.hosts[0] = std::make_shared<MetadataOnlyTestHost>("get_info_metadata");
        input.directories.reset(nc::base::variable_container<>::type::common);
        input.directories[0] = "/fixture/";
    }
    input.filenames = {"alpha.txt", ".shortcut.link"};
    input.unix_modes = {S_IFREG | 0640, S_IFREG | 0444};
    input.unix_types = {DT_REG, DT_LNK};

    input.display_filenames.reset(nc::base::variable_container<>::type::sparse);
    input.display_filenames.insert(0, "Alpha Document");
    input.sizes.reset(nc::base::variable_container<>::type::sparse);
    input.sizes.insert(0, 1234);
    input.inodes.reset(nc::base::variable_container<>::type::sparse);
    input.inodes.insert(0, 77);
    input.atimes.reset(nc::base::variable_container<>::type::sparse);
    input.atimes.insert(0, 101);
    input.mtimes.reset(nc::base::variable_container<>::type::sparse);
    input.mtimes.insert(0, 102);
    input.ctimes.reset(nc::base::variable_container<>::type::sparse);
    input.ctimes.insert(0, 103);
    input.btimes.reset(nc::base::variable_container<>::type::sparse);
    input.btimes.insert(0, 104);
    input.add_times.reset(nc::base::variable_container<>::type::sparse);
    input.add_times.insert(0, 105);
    input.unix_flags.reset(nc::base::variable_container<>::type::sparse);
    input.unix_flags.insert(0, 0x20);
    input.uids.reset(nc::base::variable_container<>::type::sparse);
    input.uids.insert(0, 501);
    input.gids.reset(nc::base::variable_container<>::type::sparse);
    input.gids.insert(0, 20);
    input.symlinks.reset(nc::base::variable_container<>::type::sparse);
    input.symlinks.insert(1, "alpha.txt");
    input.tags.emplace(
        0,
        std::vector<nc::utility::Tags::Tag>{
            {nc::utility::Tags::Tag::Internalize("Important"), nc::utility::Tags::Color::Red}});
    return VFSListing::Build(std::move(input));
}

std::vector<ListingItem> Items(const VFSListingPtr &_listing)
{
    std::vector<ListingItem> items;
    items.reserve(_listing->Count());
    for( unsigned index = 0; index < _listing->Count(); ++index )
        items.emplace_back(_listing->Item(index));
    return items;
}

ListingItem ParentItem()
{
    nc::vfs::ListingInput input;
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = std::make_shared<MetadataOnlyTestHost>("get_info_parent");
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/fixture/";
    input.filenames = {".."};
    input.unix_modes = {S_IFDIR | 0755};
    input.unix_types = {DT_DIR};
    return VFSListing::Build(std::move(input))->Item(0);
}

CommandId FileGetInfoId()
{
    return CommandId{nc::core::command_ids::FileGetInfo};
}

CommandRegistry RegistryWithPresenter(nc::core::FileGetInfoPresenter _presenter)
{
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeFileGetInfoCommand(std::move(_presenter))) ==
            CommandRegistry::RegisterResult::Registered);
    return registry;
}

CommandContext Context(const std::span<const ListingItem> _items,
                       void *_target,
                       const CommandInvocationSource _source = CommandInvocationSource::Programmatic)
{
    return CommandContext{.source = _source, .native_target = _target, .items = _items};
}

} // namespace

#define PREFIX "nc::core::FileGetInfoCommand "

TEST_CASE(PREFIX "defines a stable read-only Properties descriptor with the admitted menu route")
{
    const auto registry = RegistryWithPresenter([](void *, FileGetInfoPresentation) { return true; });
    const auto *const descriptor = registry.Find(FileGetInfoId());

    REQUIRE(descriptor);
    CHECK(descriptor->id.Value() == "file.getInfo");
    CHECK(descriptor->title_key == "commands.file.getInfo.title");
    CHECK(descriptor->description_key == "commands.file.getInfo.description");
    CHECK(descriptor->category == nc::core::CommandCategory::File);
    CHECK(descriptor->icon_name == "info.circle");
    CHECK_FALSE(descriptor->is_destructive);
    CHECK_FALSE(descriptor->requires_operation_plan);
    CHECK_FALSE(descriptor->supports_undo);
    CHECK(descriptor->analytics_name == "file.getInfo");
    REQUIRE(descriptor->legacy);
    CHECK(descriptor->legacy->selector_name == "OnFileGetInfo:");
    CHECK(descriptor->legacy->shortcut_action_names == std::vector<std::string>{"menu.file.get_info"});
    CHECK(descriptor->legacy->shortcut_tag == 11'190);
}

TEST_CASE(PREFIX "requires one live pane target and one complete non-parent exact payload")
{
    int presentations = 0;
    const auto registry = RegistryWithPresenter([&](void *, FileGetInfoPresentation) {
        ++presentations;
        return true;
    });
    int target = 0;
    const auto listing = DetailedListing(false);
    const auto items = Items(listing);
    const std::array invalid_payload{items.front(), ListingItem{}};
    const std::array parent_payload{items.front(), ParentItem()};

    const auto check = [&](const CommandContext &_context, const std::string_view _reason) {
        const auto state = registry.QueryState(FileGetInfoId(), _context);
        REQUIRE(state.status == CommandRegistry::LookupStatus::Found);
        CHECK_FALSE(state.state.enabled);
        REQUIRE(state.state.disabled_reason);
        CHECK(state.state.disabled_reason->code == _reason);

        const auto execution = registry.Execute(FileGetInfoId(), _context);
        CHECK(execution.status == CommandRegistry::ExecutionStatus::Disabled);
        REQUIRE(execution.disabled_reason);
        CHECK(execution.disabled_reason->code == _reason);
    };

    check(Context(items, nullptr), "context.paneTargetRequired");
    check(Context({}, &target), "selection.empty");
    check(Context(invalid_payload, &target), "selection.invalidItem");
    check(Context(parent_payload, &target), "selection.parentEntryUnsupported");
    CHECK(presentations == 0);
}

TEST_CASE(PREFIX "accepts mixed metadata-only providers for every invocation source")
{
    int target = 0;
    void *presented_target = nullptr;
    std::vector<CommandInvocationSource> presented_sources;
    const auto registry = RegistryWithPresenter([&](void *_target, FileGetInfoPresentation _presentation) {
        presented_target = _target;
        presented_sources.emplace_back(_presentation.source);
        REQUIRE(_presentation.items.size() == 2);
        CHECK(_presentation.items[0].path == "/first/alpha.txt");
        CHECK(_presentation.items[1].path == "/second/.shortcut.link");
        return true;
    });
    const auto listing = DetailedListing(true);
    const auto items = Items(listing);
    constexpr std::array sources{CommandInvocationSource::Menu,
                                 CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut,
                                 CommandInvocationSource::Palette,
                                 CommandInvocationSource::Programmatic};

    for( const CommandInvocationSource source : sources ) {
        const CommandContext context = Context(items, &target, source);
        CHECK(registry.QueryState(FileGetInfoId(), context).state.enabled);
        CHECK(registry.Execute(FileGetInfoId(), context).status == CommandRegistry::ExecutionStatus::Executed);
        CHECK(presented_target == &target);
    }
    CHECK(presented_sources == std::vector<CommandInvocationSource>(sources.begin(), sources.end()));
}

TEST_CASE(PREFIX "copies complete listing metadata into an authority-free owning payload")
{
    int target = 0;
    std::optional<FileGetInfoPresentation> captured;
    const auto registry = RegistryWithPresenter(
        [&](void *_target, FileGetInfoPresentation _presentation) {
            CHECK(_target == &target);
            captured = std::move(_presentation);
            return true;
        });

    {
        const auto listing = DetailedListing(false);
        const auto items = Items(listing);
        REQUIRE(registry.Execute(FileGetInfoId(), Context(items, &target, CommandInvocationSource::Toolbar)).status ==
                CommandRegistry::ExecutionStatus::Executed);
    }

    REQUIRE(captured);
    CHECK(captured->source == CommandInvocationSource::Toolbar);
    REQUIRE(captured->items.size() == 2);
    const auto &metadata = captured->items[0];
    CHECK(metadata.path == "/fixture/alpha.txt");
    CHECK(metadata.filename == "alpha.txt");
    CHECK(metadata.display_name == "Alpha Document");
    CHECK(metadata.extension == "txt");
    CHECK(metadata.unix_mode == (S_IFREG | 0640));
    CHECK(metadata.unix_type == DT_REG);
    CHECK_FALSE(metadata.is_directory);
    CHECK(metadata.is_regular);
    CHECK_FALSE(metadata.is_symlink);
    CHECK_FALSE(metadata.is_hidden);
    CHECK(metadata.size == 1234);
    CHECK(metadata.inode == 77);
    CHECK(metadata.accessed_time == 101);
    CHECK(metadata.modified_time == 102);
    CHECK(metadata.status_changed_time == 103);
    CHECK(metadata.created_time == 104);
    CHECK(metadata.added_time == 105);
    CHECK(metadata.unix_flags == 0x20);
    CHECK(metadata.unix_uid == 501);
    CHECK(metadata.unix_gid == 20);
    CHECK_FALSE(metadata.symlink_target);
    REQUIRE(metadata.tags.size() == 1);
    CHECK(metadata.tags[0].label == "Important");
    CHECK(metadata.tags[0].color == nc::utility::Tags::Color::Red);

    const auto &link = captured->items[1];
    CHECK(link.is_symlink);
    CHECK(link.is_hidden);
    CHECK(link.symlink_target == "alpha.txt");
    CHECK_FALSE(link.size);
    CHECK(link.tags.empty());
}

TEST_CASE(PREFIX "reports a live admitted-pane rejection without claiming execution")
{
    int target = 0;
    const auto registry = RegistryWithPresenter([](void *, FileGetInfoPresentation) { return false; });
    const auto listing = DetailedListing(false);
    const auto items = Items(listing);

    const auto execution = registry.Execute(FileGetInfoId(), Context(items, &target));

    CHECK(execution.status == CommandRegistry::ExecutionStatus::Rejected);
    REQUIRE(execution.disabled_reason);
    CHECK(execution.disabled_reason->code == "context.fileGetInfoPresentationRejected");
    CHECK(execution.disabled_reason->user_message_key ==
          "commands.file.getInfo.disabled.presentationRejected");
}

TEST_CASE(PREFIX "does not register without a Properties presenter")
{
    CommandRegistry registry;
    CHECK(registry.Register(MakeFileGetInfoCommand({})) == CommandRegistry::RegisterResult::MissingHandler);
    CHECK(registry.Find(FileGetInfoId()) == nullptr);
}

#undef PREFIX
