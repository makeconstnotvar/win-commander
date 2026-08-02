// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/FileOpenCommand.h>
#include <array>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <vector>

namespace {

using nc::core::CommandContext;
using nc::core::CommandId;
using nc::core::CommandInvocationSource;
using nc::core::CommandRegistry;
using nc::core::FileOpenExecutionError;
using nc::core::MakeFileOpenCommand;
using nc::vfs::ListingItem;

class FileOpenTestHost final : public nc::vfs::Host
{
public:
    FileOpenTestHost(const bool _native, const bool _readable)
        : Host("/", nullptr, _native ? "file_open_native_test" : "file_open_remote_test"), m_Native{_native}
    {
        if( _readable )
            AddFeatures(nc::vfs::HostFeatures::Read);
    }

    bool IsNativeFS() const noexcept override { return m_Native; }

private:
    bool m_Native;
};

struct ItemSpec {
    std::shared_ptr<FileOpenTestHost> host;
    std::string_view name;
    mode_t mode = S_IFREG | S_IRUSR;
    uint8_t type = DT_REG;
    std::string_view directory = "/";
};

std::vector<ListingItem> Items(const std::initializer_list<ItemSpec> _specs)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::dense);
    input.hosts.reset(nc::base::variable_container<>::type::dense);
    std::size_t index = 0;
    for( const ItemSpec &spec : _specs ) {
        input.hosts.insert(index, spec.host);
        input.directories.insert(index, std::string{spec.directory});
        input.filenames.emplace_back(spec.name);
        input.unix_modes.emplace_back(spec.mode);
        input.unix_types.emplace_back(spec.type);
        ++index;
    }

    const auto listing = VFSListing::Build(std::move(input));
    std::vector<ListingItem> items;
    items.reserve(_specs.size());
    for( std::size_t item_index = 0; item_index < _specs.size(); ++item_index )
        items.emplace_back(listing->Item(static_cast<unsigned>(item_index)));
    return items;
}

CommandId FileOpenId()
{
    return CommandId{nc::core::command_ids::FileOpen};
}

CommandRegistry RegistryWithExecutor(nc::core::FileOpenExecutor _executor)
{
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeFileOpenCommand(std::move(_executor))) ==
            CommandRegistry::RegisterResult::Registered);
    return registry;
}

void *Target()
{
    static int target;
    return &target;
}

CommandContext Context(std::span<const ListingItem> _items,
                       const CommandInvocationSource _source = CommandInvocationSource::Programmatic,
                       void *_target = Target())
{
    return CommandContext{
        .source = _source,
        .native_target = _target,
        .items = _items,
    };
}

} // namespace

#define PREFIX "nc::core::FileOpenCommand "

TEST_CASE(PREFIX "defines stable external-open metadata")
{
    const auto registry = RegistryWithExecutor([](void *, std::span<const ListingItem>) { return true; });
    const auto *const descriptor = registry.Find(FileOpenId());

    REQUIRE(descriptor);
    CHECK(descriptor->id.Value() == "file.open");
    CHECK(descriptor->title_key == "commands.file.open.title");
    CHECK(descriptor->description_key == "commands.file.open.description");
    CHECK(descriptor->category == nc::core::CommandCategory::File);
    CHECK(descriptor->icon_name == "arrow.up.forward.app");
    CHECK_FALSE(descriptor->is_destructive);
    CHECK_FALSE(descriptor->requires_operation_plan);
    CHECK_FALSE(descriptor->supports_undo);
    CHECK(descriptor->analytics_name == "file.open");
    REQUIRE(descriptor->legacy);
    CHECK(descriptor->legacy->selector_name == "OnOpenNatively:");
    CHECK(descriptor->legacy->shortcut_action_names == std::vector<std::string>{"menu.file.open"});
    CHECK(descriptor->legacy->shortcut_tag == 11'020);
}

TEST_CASE(PREFIX "submits the exact borrowed item for every invocation source")
{
    const auto host = std::make_shared<FileOpenTestHost>(false, true);
    const auto items = Items({ItemSpec{.host = host, .name = "document.txt"}});
    int calls = 0;
    void *received_target = nullptr;
    const ListingItem *received_data = nullptr;
    std::size_t received_size = 0;
    const auto registry = RegistryWithExecutor([&](void *_target, const std::span<const ListingItem> _items) {
        ++calls;
        received_target = _target;
        received_data = _items.data();
        received_size = _items.size();
        return true;
    });
    constexpr std::array sources{CommandInvocationSource::Menu,
                                 CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut,
                                 CommandInvocationSource::Palette,
                                 CommandInvocationSource::Programmatic};

    for( std::size_t index = 0; index < sources.size(); ++index ) {
        CHECK(registry.Execute(FileOpenId(), Context(items, sources[index])).status ==
              CommandRegistry::ExecutionStatus::Executed);
        CHECK(calls == static_cast<int>(index + 1));
        CHECK(received_target == Target());
        CHECK(received_data == items.data());
        CHECK(received_size == items.size());
    }
}

TEST_CASE(PREFIX "allows readable regular files on native and remote providers")
{
    const auto native = std::make_shared<FileOpenTestHost>(true, true);
    const auto remote = std::make_shared<FileOpenTestHost>(false, true);
    const auto registry = RegistryWithExecutor([](void *, std::span<const ListingItem>) { return true; });

    for( const auto &items : {Items({ItemSpec{.host = native, .name = "local.txt"}}),
                             Items({ItemSpec{.host = remote, .name = "remote.txt"}})} ) {
        const auto state = registry.QueryState(FileOpenId(), Context(items)).state;
        CHECK(state.enabled);
        CHECK_FALSE(state.disabled_reason);
    }
}

TEST_CASE(PREFIX "allows one native directory or special item through the existing workspace path")
{
    const auto native_without_provider_read = std::make_shared<FileOpenTestHost>(true, false);
    const auto directory = Items({ItemSpec{
        .host = native_without_provider_read,
        .name = "Folder",
        .mode = S_IFDIR | S_IRUSR,
        .type = DT_DIR,
    }});
    const auto special = Items({ItemSpec{
        .host = native_without_provider_read,
        .name = "Pipe",
        .mode = S_IFIFO | S_IRUSR,
        .type = DT_FIFO,
    }});
    const auto registry = RegistryWithExecutor([](void *, std::span<const ListingItem>) { return true; });

    CHECK(registry.QueryState(FileOpenId(), Context(directory)).state.enabled);
    CHECK(registry.QueryState(FileOpenId(), Context(special)).state.enabled);
}

TEST_CASE(PREFIX "allows a same-provider batch of readable regular files")
{
    const auto remote = std::make_shared<FileOpenTestHost>(false, true);
    const auto items = Items({ItemSpec{.host = remote, .name = "first.txt", .directory = "/one/"},
                              ItemSpec{.host = remote, .name = "second.txt", .directory = "/two/"}});
    std::size_t submitted = 0;
    const auto registry = RegistryWithExecutor([&](void *, const std::span<const ListingItem> _items) {
        submitted = _items.size();
        return true;
    });

    CHECK(registry.QueryState(FileOpenId(), Context(items)).state.enabled);
    CHECK(registry.Execute(FileOpenId(), Context(items)).status == CommandRegistry::ExecutionStatus::Executed);
    CHECK(submitted == 2);
}

TEST_CASE(PREFIX "returns structured disabled reasons for non-executable contexts")
{
    const auto native_readable = std::make_shared<FileOpenTestHost>(true, true);
    const auto remote_readable = std::make_shared<FileOpenTestHost>(false, true);
    const auto remote_unreadable = std::make_shared<FileOpenTestHost>(false, false);
    const auto other_remote = std::make_shared<FileOpenTestHost>(false, true);
    const std::vector<ListingItem> empty;
    const std::vector<ListingItem> invalid{ListingItem{}};
    const auto regular = Items({ItemSpec{.host = native_readable, .name = "local.txt"}});
    const auto parent = Items({ItemSpec{
        .host = native_readable,
        .name = "..",
        .mode = S_IFDIR | S_IRUSR,
        .type = DT_DIR,
    }});
    const auto unreadable = Items({ItemSpec{.host = remote_unreadable, .name = "remote.txt"}});
    const auto remote_directory = Items({ItemSpec{
        .host = remote_readable,
        .name = "Folder",
        .mode = S_IFDIR | S_IRUSR,
        .type = DT_DIR,
    }});
    const auto remote_special = Items({ItemSpec{
        .host = remote_readable,
        .name = "Pipe",
        .mode = S_IFIFO | S_IRUSR,
        .type = DT_FIFO,
    }});
    const auto mixed_hosts = Items({ItemSpec{.host = remote_readable, .name = "first.txt"},
                                    ItemSpec{.host = other_remote, .name = "second.txt"}});
    const auto mixed_types = Items({ItemSpec{.host = native_readable, .name = "first.txt"},
                                    ItemSpec{
                                        .host = native_readable,
                                        .name = "Folder",
                                        .mode = S_IFDIR | S_IRUSR,
                                        .type = DT_DIR,
                                    }});
    const auto unreadable_batch = Items({ItemSpec{.host = remote_unreadable, .name = "first.txt"},
                                         ItemSpec{.host = remote_unreadable, .name = "second.txt"}});
    const auto registry = RegistryWithExecutor([](void *, std::span<const ListingItem>) { return true; });

    const auto check = [&](const CommandContext &_context, const std::string_view _code) {
        const auto state = registry.QueryState(FileOpenId(), _context).state;
        CHECK_FALSE(state.enabled);
        REQUIRE(state.disabled_reason);
        CHECK(state.disabled_reason->code == _code);
        CHECK(registry.Execute(FileOpenId(), _context).status == CommandRegistry::ExecutionStatus::Disabled);
    };

    check(Context(regular, CommandInvocationSource::Menu, nullptr), "context.paneTargetRequired");
    check(Context(empty), "selection.empty");
    check(Context(invalid), "provider.unavailable");
    check(Context(parent), "selection.parentEntryUnsupported");
    check(Context(unreadable), "provider.readUnsupported");
    check(Context(remote_directory), "provider.remoteItemTypeUnsupported");
    check(Context(remote_special), "provider.remoteItemTypeUnsupported");
    check(Context(mixed_hosts), "selection.sameProviderRequired");
    check(Context(mixed_types), "selection.regularFilesRequired");
    check(Context(unreadable_batch), "provider.readUnsupported");
}

TEST_CASE(PREFIX "reports failed live submission instead of execution success")
{
    const auto host = std::make_shared<FileOpenTestHost>(false, true);
    const auto items = Items({ItemSpec{.host = host, .name = "document.txt"}});
    const auto registry = RegistryWithExecutor([](void *, std::span<const ListingItem>) { return false; });

    CHECK_THROWS_AS(registry.Execute(FileOpenId(), Context(items)), FileOpenExecutionError);
}

TEST_CASE(PREFIX "rejects registration without an executor")
{
    CommandRegistry registry;
    CHECK(registry.Register(MakeFileOpenCommand({})) == CommandRegistry::RegisterResult::MissingHandler);
}

#undef PREFIX
