// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/NavigationHistoryCommand.h>
#include <array>
#include <optional>
#include <string>
#include <utility>

namespace {

using nc::core::CommandContext;
using nc::core::CommandId;
using nc::core::CommandInvocationSource;
using nc::core::CommandRegistry;
using nc::core::MakeNavigationBackCommand;
using nc::core::MakeNavigationForwardCommand;
using nc::core::NavigationHistoryDirection;
using nc::core::NavigationHistoryExecutionError;
using nc::core::NavigationHistoryExecutor;

CommandId BackId()
{
    return CommandId{nc::core::command_ids::NavigationBack};
}

CommandId ForwardId()
{
    return CommandId{nc::core::command_ids::NavigationForward};
}

CommandRegistry RegistryWithExecutor(const NavigationHistoryExecutor &_executor)
{
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeNavigationBackCommand(_executor)) ==
            CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakeNavigationForwardCommand(_executor)) ==
            CommandRegistry::RegisterResult::Registered);
    return registry;
}

void *Target()
{
    static int target;
    return &target;
}

CommandContext Context(const bool _can_go_back,
                       const bool _can_go_forward,
                       const CommandInvocationSource _source = CommandInvocationSource::Programmatic,
                       void *_target = Target())
{
    return CommandContext{
        .source = _source,
        .native_target = _target,
        .can_go_back = _can_go_back,
        .can_go_forward = _can_go_forward,
    };
}

} // namespace

#define PREFIX "nc::core::NavigationHistoryCommand "

TEST_CASE(PREFIX "defines stable navigation metadata")
{
    const auto registry = RegistryWithExecutor([](void *, NavigationHistoryDirection) { return true; });

    struct Expected {
        CommandId id;
        std::string_view title;
        std::string_view description;
        std::string_view icon;
        std::string_view selector;
        std::string_view action;
        int tag;
    };
    const std::array expected{
        Expected{BackId(),
                 "commands.navigation.back.title",
                 "commands.navigation.back.description",
                 "chevron.left",
                 "OnGoBack:",
                 "menu.go.back",
                 14'000},
        Expected{ForwardId(),
                 "commands.navigation.forward.title",
                 "commands.navigation.forward.description",
                 "chevron.right",
                 "OnGoForward:",
                 "menu.go.forward",
                 14'010},
    };

    for( const Expected &value : expected ) {
        const auto *const descriptor = registry.Find(value.id);
        REQUIRE(descriptor);
        CHECK(descriptor->title_key == value.title);
        CHECK(descriptor->description_key == value.description);
        CHECK(descriptor->category == nc::core::CommandCategory::Navigation);
        CHECK(descriptor->icon_name == value.icon);
        CHECK_FALSE(descriptor->is_destructive);
        CHECK_FALSE(descriptor->requires_operation_plan);
        CHECK_FALSE(descriptor->supports_undo);
        CHECK(descriptor->analytics_name == value.id.Value());
        REQUIRE(descriptor->legacy);
        CHECK(descriptor->legacy->selector_name == value.selector);
        CHECK(descriptor->legacy->shortcut_action_names ==
              std::vector<std::string>{std::string{value.action}});
        CHECK(descriptor->legacy->shortcut_tag == value.tag);
    }
}

TEST_CASE(PREFIX "projects each direction from its own availability")
{
    const auto registry = RegistryWithExecutor([](void *, NavigationHistoryDirection) { return true; });

    for( const bool can_go_back : {false, true} ) {
        for( const bool can_go_forward : {false, true} ) {
            const CommandContext context = Context(can_go_back, can_go_forward);
            const auto back = registry.QueryState(BackId(), context);
            const auto forward = registry.QueryState(ForwardId(), context);
            REQUIRE(back.status == CommandRegistry::LookupStatus::Found);
            REQUIRE(forward.status == CommandRegistry::LookupStatus::Found);
            CHECK(back.state.enabled == can_go_back);
            CHECK(forward.state.enabled == can_go_forward);
            if( !can_go_back ) {
                REQUIRE(back.state.disabled_reason);
                CHECK(back.state.disabled_reason->code == "navigation.backUnavailable");
            }
            if( !can_go_forward ) {
                REQUIRE(forward.state.disabled_reason);
                CHECK(forward.state.disabled_reason->code == "navigation.forwardUnavailable");
            }
        }
    }
}

TEST_CASE(PREFIX "returns structured reasons for missing borrowed context")
{
    int calls = 0;
    const auto registry = RegistryWithExecutor([&](void *, NavigationHistoryDirection) {
        ++calls;
        return true;
    });

    const CommandContext missing_target = Context(true, true, CommandInvocationSource::Menu, nullptr);
    for( const CommandId &id : {BackId(), ForwardId()} ) {
        const auto state = registry.QueryState(id, missing_target).state;
        CHECK_FALSE(state.enabled);
        REQUIRE(state.disabled_reason);
        CHECK(state.disabled_reason->code == "context.paneTargetRequired");
        CHECK(registry.Execute(id, missing_target).status == CommandRegistry::ExecutionStatus::Disabled);
    }

    CommandContext missing_snapshot;
    missing_snapshot.native_target = Target();
    for( const CommandId &id : {BackId(), ForwardId()} ) {
        const auto state = registry.QueryState(id, missing_snapshot).state;
        CHECK_FALSE(state.enabled);
        REQUIRE(state.disabled_reason);
        CHECK(state.disabled_reason->code == "context.historyAvailabilityRequired");
        CHECK(registry.Execute(id, missing_snapshot).status == CommandRegistry::ExecutionStatus::Disabled);
    }
    CHECK(calls == 0);
}

TEST_CASE(PREFIX "executes each source exactly once through the narrow port")
{
    int calls = 0;
    void *received_target = nullptr;
    std::optional<NavigationHistoryDirection> received_direction;
    const auto registry = RegistryWithExecutor(
        [&](void *_target, const NavigationHistoryDirection _direction) {
            ++calls;
            received_target = _target;
            received_direction = _direction;
            return true;
        });
    constexpr std::array sources{CommandInvocationSource::Menu,
                                 CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut,
                                 CommandInvocationSource::Palette,
                                 CommandInvocationSource::Programmatic};

    for( std::size_t index = 0; index < sources.size(); ++index ) {
        const bool back = index % 2 == 0;
        const CommandId id = back ? BackId() : ForwardId();
        CHECK(registry.Execute(id, Context(true, true, sources[index])).status ==
              CommandRegistry::ExecutionStatus::Executed);
        CHECK(calls == static_cast<int>(index + 1));
        CHECK(received_target == Target());
        REQUIRE(received_direction);
        CHECK(*received_direction == (back ? NavigationHistoryDirection::Back
                                           : NavigationHistoryDirection::Forward));
    }
}

TEST_CASE(PREFIX "reports a failed live guard and does not run disabled directions")
{
    int calls = 0;
    const auto registry = RegistryWithExecutor([&](void *, NavigationHistoryDirection) {
        ++calls;
        return false;
    });

    CHECK(registry.Execute(BackId(), Context(false, true)).status ==
          CommandRegistry::ExecutionStatus::Disabled);
    CHECK(calls == 0);
    CHECK_THROWS_AS(registry.Execute(ForwardId(), Context(false, true)),
                    NavigationHistoryExecutionError);
    CHECK(calls == 1);
}

TEST_CASE(PREFIX "rejects registration without an executor")
{
    CommandRegistry registry;
    CHECK(registry.Register(MakeNavigationBackCommand({})) ==
          CommandRegistry::RegisterResult::MissingHandler);
    CHECK(registry.Register(MakeNavigationForwardCommand({})) ==
          CommandRegistry::RegisterResult::MissingHandler);
}

#undef PREFIX
