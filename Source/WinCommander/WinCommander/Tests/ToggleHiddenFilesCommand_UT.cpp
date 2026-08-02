// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/ToggleHiddenFilesCommand.h>
#include <array>
#include <optional>
#include <string>
#include <utility>

namespace {

using nc::core::CommandCheckState;
using nc::core::CommandContext;
using nc::core::CommandId;
using nc::core::CommandInvocationSource;
using nc::core::CommandRegistry;
using nc::core::HiddenFilesVisibilitySetter;
using nc::core::MakeViewToggleHiddenFilesCommand;
using nc::core::ViewToggleHiddenFilesError;

CommandId ViewToggleHiddenFilesId()
{
    return CommandId{nc::core::command_ids::ViewToggleHiddenFiles};
}

CommandRegistry RegistryWithSetter(HiddenFilesVisibilitySetter _setter)
{
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeViewToggleHiddenFilesCommand(std::move(_setter))) ==
            CommandRegistry::RegisterResult::Registered);
    return registry;
}

void *Target()
{
    static int target;
    return &target;
}

CommandContext Context(const bool _shows_hidden_files,
                       const CommandInvocationSource _source = CommandInvocationSource::Programmatic,
                       void *_target = Target())
{
    return CommandContext{
        .source = _source,
        .native_target = _target,
        .shows_hidden_files = _shows_hidden_files,
    };
}

} // namespace

#define PREFIX "nc::core::ToggleHiddenFilesCommand "

TEST_CASE(PREFIX "defines stable non-mutating view metadata")
{
    const auto registry = RegistryWithSetter([](void *, bool) { return true; });
    const auto *const descriptor = registry.Find(ViewToggleHiddenFilesId());

    REQUIRE(descriptor);
    CHECK(descriptor->id.Value() == "view.toggleHiddenFiles");
    CHECK(descriptor->title_key == "commands.view.toggleHiddenFiles.title");
    CHECK(descriptor->description_key == "commands.view.toggleHiddenFiles.description");
    CHECK(descriptor->category == nc::core::CommandCategory::View);
    CHECK(descriptor->icon_name == "eye");
    CHECK_FALSE(descriptor->is_destructive);
    CHECK_FALSE(descriptor->requires_operation_plan);
    CHECK_FALSE(descriptor->supports_undo);
    CHECK(descriptor->analytics_name == "view.toggleHiddenFiles");
    REQUIRE(descriptor->legacy);
    CHECK(descriptor->legacy->selector_name == "ToggleViewHiddenFiles:");
    CHECK(descriptor->legacy->shortcut_action_names ==
          std::vector<std::string>{"menu.view.sorting_view_hidden"});
    CHECK(descriptor->legacy->shortcut_tag == 13'140);
}

TEST_CASE(PREFIX "reflects the current pane visibility as check state")
{
    const auto registry = RegistryWithSetter([](void *, bool) { return true; });

    const auto hidden = registry.QueryState(ViewToggleHiddenFilesId(), Context(false));
    REQUIRE(hidden.status == CommandRegistry::LookupStatus::Found);
    CHECK(hidden.state.enabled);
    CHECK(hidden.state.check_state == CommandCheckState::Off);

    const auto visible = registry.QueryState(ViewToggleHiddenFilesId(), Context(true));
    REQUIRE(visible.status == CommandRegistry::LookupStatus::Found);
    CHECK(visible.state.enabled);
    CHECK(visible.state.check_state == CommandCheckState::On);
}

TEST_CASE(PREFIX "updates exactly once for every supported invocation source")
{
    int calls = 0;
    void *received_target = nullptr;
    std::optional<bool> received_visibility;
    const auto registry = RegistryWithSetter([&](void *_target, const bool _shows_hidden_files) {
        ++calls;
        received_target = _target;
        received_visibility = _shows_hidden_files;
        return true;
    });
    constexpr std::array sources{CommandInvocationSource::Menu,
                                 CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut,
                                 CommandInvocationSource::Palette,
                                 CommandInvocationSource::Programmatic};

    for( std::size_t index = 0; index < sources.size(); ++index ) {
        const bool currently_visible = index % 2 != 0;
        CHECK(registry.Execute(ViewToggleHiddenFilesId(), Context(currently_visible, sources[index])).status ==
              CommandRegistry::ExecutionStatus::Executed);
        CHECK(calls == static_cast<int>(index + 1));
        CHECK(received_target == Target());
        CHECK(received_visibility == !currently_visible);
    }
}

TEST_CASE(PREFIX "returns structured disabled reasons for missing pane context")
{
    int calls = 0;
    const auto registry = RegistryWithSetter([&](void *, bool) {
        ++calls;
        return true;
    });

    const auto check = [&](const CommandContext &_context,
                           const std::string_view _code,
                           const CommandCheckState _check_state) {
        const auto state = registry.QueryState(ViewToggleHiddenFilesId(), _context).state;
        CHECK_FALSE(state.enabled);
        CHECK(state.check_state == _check_state);
        REQUIRE(state.disabled_reason);
        CHECK(state.disabled_reason->code == _code);
        CHECK(registry.Execute(ViewToggleHiddenFilesId(), _context).status ==
              CommandRegistry::ExecutionStatus::Disabled);
    };

    check(Context(true, CommandInvocationSource::Menu, nullptr),
          "context.paneTargetRequired",
          CommandCheckState::On);

    CommandContext missing_snapshot;
    missing_snapshot.native_target = Target();
    check(missing_snapshot, "context.hiddenFilesStateRequired", CommandCheckState::Off);
    CHECK(calls == 0);
}

TEST_CASE(PREFIX "reports a failed pane update instead of execution success")
{
    const auto registry = RegistryWithSetter([](void *, bool) { return false; });

    CHECK_THROWS_AS(registry.Execute(ViewToggleHiddenFilesId(), Context(false)),
                    ViewToggleHiddenFilesError);
}

TEST_CASE(PREFIX "rejects registration without a visibility setter")
{
    CommandRegistry registry;
    CHECK(registry.Register(MakeViewToggleHiddenFilesCommand({})) ==
          CommandRegistry::RegisterResult::MissingHandler);
}

#undef PREFIX
