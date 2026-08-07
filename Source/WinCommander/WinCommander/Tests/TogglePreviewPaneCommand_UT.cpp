// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/TogglePreviewPaneCommand.h>
#include <array>
#include <optional>
#include <utility>

namespace {

using nc::core::CommandCheckState;
using nc::core::CommandContext;
using nc::core::CommandId;
using nc::core::CommandInvocationSource;
using nc::core::CommandRegistry;
using nc::core::MakeViewTogglePreviewPaneCommand;
using nc::core::PreviewPaneVisibilitySetter;

CommandId ViewTogglePreviewPaneId()
{
    return CommandId{nc::core::command_ids::ViewTogglePreviewPane};
}

CommandRegistry RegistryWithSetter(PreviewPaneVisibilitySetter _setter)
{
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeViewTogglePreviewPaneCommand(std::move(_setter))) ==
            CommandRegistry::RegisterResult::Registered);
    return registry;
}

void *Target()
{
    static int target;
    return &target;
}

CommandContext Context(const bool _visible,
                       const CommandInvocationSource _source = CommandInvocationSource::Programmatic,
                       void *_target = Target())
{
    return CommandContext{
        .source = _source,
        .native_target = _target,
        .preview_pane_visible = _visible,
    };
}

} // namespace

#define PREFIX "nc::core::TogglePreviewPaneCommand "

TEST_CASE(PREFIX "defines stable read-only view metadata and the exact menu route")
{
    const auto registry = RegistryWithSetter([](void *, bool, bool) { return true; });
    const auto *const descriptor = registry.Find(ViewTogglePreviewPaneId());

    REQUIRE(descriptor);
    CHECK(descriptor->id.Value() == "view.togglePreviewPane");
    CHECK(descriptor->title_key == "commands.view.togglePreviewPane.title");
    CHECK(descriptor->description_key == "commands.view.togglePreviewPane.description");
    CHECK(descriptor->category == nc::core::CommandCategory::View);
    CHECK(descriptor->icon_name == "sidebar.right");
    CHECK_FALSE(descriptor->is_destructive);
    CHECK_FALSE(descriptor->requires_operation_plan);
    CHECK_FALSE(descriptor->supports_undo);
    CHECK(descriptor->analytics_name == "view.togglePreviewPane");
    REQUIRE(descriptor->legacy);
    CHECK(descriptor->legacy->selector_name == "OnTogglePreviewPane:");
    CHECK(descriptor->legacy->shortcut_action_names ==
          std::vector<std::string>{"menu.view.toggle_preview_pane"});
    CHECK(descriptor->legacy->shortcut_tag == 13'280);
}

TEST_CASE(PREFIX "reflects the borrowed pane visibility as check state")
{
    const auto registry = RegistryWithSetter([](void *, bool, bool) { return true; });

    const auto hidden = registry.QueryState(ViewTogglePreviewPaneId(), Context(false));
    REQUIRE(hidden.status == CommandRegistry::LookupStatus::Found);
    CHECK(hidden.state.enabled);
    CHECK(hidden.state.check_state == CommandCheckState::Off);

    const auto visible = registry.QueryState(ViewTogglePreviewPaneId(), Context(true));
    REQUIRE(visible.status == CommandRegistry::LookupStatus::Found);
    CHECK(visible.state.enabled);
    CHECK(visible.state.check_state == CommandCheckState::On);
}

TEST_CASE(PREFIX "passes expected and desired visibility to the live setter for every source")
{
    int calls = 0;
    void *received_target = nullptr;
    std::optional<bool> received_expected;
    std::optional<bool> received_desired;
    const auto registry = RegistryWithSetter([&](void *_target, const bool _expected, const bool _desired) {
        ++calls;
        received_target = _target;
        received_expected = _expected;
        received_desired = _desired;
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
        CHECK(registry.Execute(ViewTogglePreviewPaneId(), Context(currently_visible, sources[index])).status ==
              CommandRegistry::ExecutionStatus::Executed);
        CHECK(calls == static_cast<int>(index + 1));
        CHECK(received_target == Target());
        CHECK(received_expected == currently_visible);
        CHECK(received_desired == !currently_visible);
    }
}

TEST_CASE(PREFIX "returns structured disabled reasons for missing live pane context")
{
    int calls = 0;
    const auto registry = RegistryWithSetter([&](void *, bool, bool) {
        ++calls;
        return true;
    });

    const auto check = [&](const CommandContext &_context,
                           const std::string_view _code,
                           const CommandCheckState _check_state) {
        const auto state = registry.QueryState(ViewTogglePreviewPaneId(), _context).state;
        CHECK_FALSE(state.enabled);
        CHECK(state.check_state == _check_state);
        REQUIRE(state.disabled_reason);
        CHECK(state.disabled_reason->code == _code);
        CHECK(registry.Execute(ViewTogglePreviewPaneId(), _context).status ==
              CommandRegistry::ExecutionStatus::Disabled);
    };

    check(Context(true, CommandInvocationSource::Menu, nullptr),
          "context.paneTargetRequired",
          CommandCheckState::On);

    CommandContext missing_snapshot;
    missing_snapshot.native_target = Target();
    check(missing_snapshot, "context.previewPaneStateRequired", CommandCheckState::Off);
    CHECK(calls == 0);
}

TEST_CASE(PREFIX "rejects an update when live visibility no longer matches the snapshot")
{
    bool received_expected = true;
    bool received_desired = false;
    const auto registry = RegistryWithSetter([&](void *, const bool _expected, const bool _desired) {
        received_expected = _expected;
        received_desired = _desired;
        return false;
    });

    const auto execution = registry.Execute(ViewTogglePreviewPaneId(), Context(false));
    CHECK(received_expected == false);
    CHECK(received_desired == true);
    CHECK(execution.status == CommandRegistry::ExecutionStatus::Rejected);
    REQUIRE(execution.disabled_reason);
    CHECK(execution.disabled_reason->code == "context.previewPaneStateChanged");
}

TEST_CASE(PREFIX "does not register without a live visibility setter")
{
    CommandRegistry registry;
    CHECK(registry.Register(MakeViewTogglePreviewPaneCommand({})) ==
          CommandRegistry::RegisterResult::MissingHandler);
}

#undef PREFIX
