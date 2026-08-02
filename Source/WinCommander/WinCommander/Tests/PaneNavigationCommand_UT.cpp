// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/PaneNavigationCommand.h>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace {

using nc::core::CommandContext;
using nc::core::CommandId;
using nc::core::CommandInvocationSource;
using nc::core::CommandRegistry;
using nc::core::MakeNavigationRefreshCommand;
using nc::core::MakeNavigationUpCommand;
using nc::core::NavigationRefreshAvailability;
using nc::core::NavigationRefreshExecutionError;
using nc::core::NavigationRefreshExecutor;
using nc::core::NavigationUpAvailability;
using nc::core::NavigationUpExecutionError;
using nc::core::NavigationUpExecutor;

CommandId UpId()
{
    return CommandId{nc::core::command_ids::NavigationUp};
}

CommandId RefreshId()
{
    return CommandId{nc::core::command_ids::NavigationRefresh};
}

CommandRegistry RegistryWithExecutors(NavigationUpExecutor _up,
                                      NavigationRefreshExecutor _refresh)
{
    CommandRegistry registry;
    REQUIRE(registry.Register(MakeNavigationUpCommand(std::move(_up))) ==
            CommandRegistry::RegisterResult::Registered);
    REQUIRE(registry.Register(MakeNavigationRefreshCommand(std::move(_refresh))) ==
            CommandRegistry::RegisterResult::Registered);
    return registry;
}

void *Target()
{
    static int target;
    return &target;
}

CommandContext Context(const NavigationUpAvailability _up,
                       const NavigationRefreshAvailability _refresh,
                       const CommandInvocationSource _source = CommandInvocationSource::Programmatic,
                       void *_target = Target())
{
    CommandContext context;
    context.source = _source;
    context.native_target = _target;
    context.navigation_up_availability = _up;
    context.navigation_refresh_availability = _refresh;
    return context;
}

} // namespace

#define PREFIX "nc::core::PaneNavigationCommand "

TEST_CASE(PREFIX "defines stable Up and Refresh metadata with all legacy aliases")
{
    const auto registry = RegistryWithExecutors([](void *) { return true; },
                                                [](void *) { return true; });

    struct Expected {
        CommandId id;
        std::string_view title;
        std::string_view description;
        std::string_view icon;
        std::string_view selector;
        std::vector<std::string> actions;
        int tag;
    };
    const std::array expected{
        Expected{UpId(),
                 "commands.navigation.up.title",
                 "commands.navigation.up.description",
                 "chevron.up",
                 "OnGoToUpperDirectory:",
                 {"menu.go.enclosing_folder", "panel.go_into_enclosing_folder"},
                 14'020},
        Expected{RefreshId(),
                 "commands.navigation.refresh.title",
                 "commands.navigation.refresh.description",
                 "arrow.clockwise",
                 "OnRefreshPanel:",
                 {"menu.view.refresh"},
                 13'040},
    };

    CHECK(UpId().Value() == "navigation.up");
    CHECK(RefreshId().Value() == "navigation.refresh");
    CHECK(UpId() != RefreshId());
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
        CHECK(descriptor->legacy->shortcut_action_names == value.actions);
        CHECK(descriptor->legacy->shortcut_tag == value.tag);
    }
}

TEST_CASE(PREFIX "maps every Up availability to a structured state")
{
    int calls = 0;
    const auto registry = RegistryWithExecutors(
        [&](void *) {
            ++calls;
            return true;
        },
        [](void *) { return true; });

    struct Expected {
        NavigationUpAvailability availability;
        bool enabled;
        std::string_view code;
        std::string_view key;
    };
    constexpr std::array expected{
        Expected{NavigationUpAvailability::PaneUnavailable,
                 false,
                 "pane.unavailable",
                 "commands.navigation.up.disabled.paneUnavailable"},
        Expected{NavigationUpAvailability::Busy,
                 false,
                 "pane.busy",
                 "commands.navigation.up.disabled.busy"},
        Expected{NavigationUpAvailability::AtTop,
                 false,
                 "navigation.up.atTop",
                 "commands.navigation.up.disabled.atTop"},
        Expected{NavigationUpAvailability::HierarchyUnavailable,
                 false,
                 "navigation.up.hierarchyUnavailable",
                 "commands.navigation.up.disabled.hierarchyUnavailable"},
        Expected{NavigationUpAvailability::Available, true, {}, {}},
    };

    for( const Expected &value : expected ) {
        const CommandContext context =
            Context(value.availability, NavigationRefreshAvailability::Available);
        const auto state = registry.QueryState(UpId(), context);
        REQUIRE(state.status == CommandRegistry::LookupStatus::Found);
        CHECK(state.state.enabled == value.enabled);
        if( value.enabled ) {
            CHECK_FALSE(state.state.disabled_reason);
        }
        else {
            REQUIRE(state.state.disabled_reason);
            CHECK(state.state.disabled_reason->code == value.code);
            CHECK(state.state.disabled_reason->user_message_key == value.key);
            CHECK_FALSE(state.state.disabled_reason->technical_message.empty());
            CHECK(registry.Execute(UpId(), context).status ==
                  CommandRegistry::ExecutionStatus::Disabled);
        }
    }
    CHECK(calls == 0);
}

TEST_CASE(PREFIX "maps every Refresh availability to a structured state")
{
    int calls = 0;
    const auto registry = RegistryWithExecutors(
        [](void *) { return true; },
        [&](void *) {
            ++calls;
            return true;
        });

    struct Expected {
        NavigationRefreshAvailability availability;
        bool enabled;
        std::string_view code;
        std::string_view key;
    };
    constexpr std::array expected{
        Expected{NavigationRefreshAvailability::PaneUnavailable,
                 false,
                 "pane.unavailable",
                 "commands.navigation.refresh.disabled.paneUnavailable"},
        Expected{NavigationRefreshAvailability::Busy,
                 false,
                 "pane.busy",
                 "commands.navigation.refresh.disabled.busy"},
        Expected{NavigationRefreshAvailability::NoCommittedContent,
                 false,
                 "navigation.refresh.noCommittedContent",
                 "commands.navigation.refresh.disabled.noCommittedContent"},
        Expected{NavigationRefreshAvailability::Available, true, {}, {}},
    };

    for( const Expected &value : expected ) {
        const CommandContext context = Context(NavigationUpAvailability::Available,
                                               value.availability);
        const auto state = registry.QueryState(RefreshId(), context);
        REQUIRE(state.status == CommandRegistry::LookupStatus::Found);
        CHECK(state.state.enabled == value.enabled);
        if( value.enabled ) {
            CHECK_FALSE(state.state.disabled_reason);
        }
        else {
            REQUIRE(state.state.disabled_reason);
            CHECK(state.state.disabled_reason->code == value.code);
            CHECK(state.state.disabled_reason->user_message_key == value.key);
            CHECK_FALSE(state.state.disabled_reason->technical_message.empty());
            CHECK(registry.Execute(RefreshId(), context).status ==
                  CommandRegistry::ExecutionStatus::Disabled);
        }
    }
    CHECK(calls == 0);
}

TEST_CASE(PREFIX "fails closed for missing, targetless, and malformed projections")
{
    int calls = 0;
    const auto registry = RegistryWithExecutors(
        [&](void *) {
            ++calls;
            return true;
        },
        [&](void *) {
            ++calls;
            return true;
        });

    const auto check = [&](const CommandId &_id,
                           const CommandContext &_context,
                           const std::string_view _code,
                           const std::string_view _key) {
        const auto state = registry.QueryState(_id, _context).state;
        CHECK_FALSE(state.enabled);
        REQUIRE(state.disabled_reason);
        CHECK(state.disabled_reason->code == _code);
        CHECK(state.disabled_reason->user_message_key == _key);
        CHECK(registry.Execute(_id, _context).status == CommandRegistry::ExecutionStatus::Disabled);
    };

    const CommandContext targetless = Context(NavigationUpAvailability::Available,
                                              NavigationRefreshAvailability::Available,
                                              CommandInvocationSource::Toolbar,
                                              nullptr);
    check(UpId(),
          targetless,
          "context.paneTargetRequired",
          "commands.navigation.up.disabled.paneUnavailable");
    check(RefreshId(),
          targetless,
          "context.paneTargetRequired",
          "commands.navigation.refresh.disabled.paneUnavailable");

    CommandContext missing_up = Context(NavigationUpAvailability::Available,
                                        NavigationRefreshAvailability::Available);
    missing_up.navigation_up_availability.reset();
    check(UpId(),
          missing_up,
          "context.navigationUpAvailabilityRequired",
          "commands.navigation.up.disabled.stateUnavailable");

    CommandContext missing_refresh = Context(NavigationUpAvailability::Available,
                                             NavigationRefreshAvailability::Available);
    missing_refresh.navigation_refresh_availability.reset();
    check(RefreshId(),
          missing_refresh,
          "context.navigationRefreshAvailabilityRequired",
          "commands.navigation.refresh.disabled.stateUnavailable");

    CommandContext malformed = Context(NavigationUpAvailability::Available,
                                       NavigationRefreshAvailability::Available);
    malformed.navigation_up_availability = static_cast<NavigationUpAvailability>(255);
    malformed.navigation_refresh_availability = static_cast<NavigationRefreshAvailability>(255);
    check(UpId(),
          malformed,
          "context.navigationUpAvailabilityInvalid",
          "commands.navigation.up.disabled.stateUnavailable");
    check(RefreshId(),
          malformed,
          "context.navigationRefreshAvailabilityInvalid",
          "commands.navigation.refresh.disabled.stateUnavailable");
    CHECK(calls == 0);
}

TEST_CASE(PREFIX "executes each command exactly once from every invocation source")
{
    int up_calls = 0;
    int refresh_calls = 0;
    void *up_target = nullptr;
    void *refresh_target = nullptr;
    const auto registry = RegistryWithExecutors(
        [&](void *_target) {
            ++up_calls;
            up_target = _target;
            return true;
        },
        [&](void *_target) {
            ++refresh_calls;
            refresh_target = _target;
            return true;
        });
    constexpr std::array sources{CommandInvocationSource::Menu,
                                 CommandInvocationSource::Toolbar,
                                 CommandInvocationSource::ContextMenu,
                                 CommandInvocationSource::Shortcut,
                                 CommandInvocationSource::Palette,
                                 CommandInvocationSource::Programmatic};

    for( std::size_t index = 0; index < sources.size(); ++index ) {
        const CommandContext context = Context(NavigationUpAvailability::Available,
                                               NavigationRefreshAvailability::Available,
                                               sources[index]);
        CHECK(registry.Execute(UpId(), context).status ==
              CommandRegistry::ExecutionStatus::Executed);
        CHECK(up_calls == static_cast<int>(index + 1));
        CHECK(refresh_calls == static_cast<int>(index));
        CHECK(up_target == Target());

        CHECK(registry.Execute(RefreshId(), context).status ==
              CommandRegistry::ExecutionStatus::Executed);
        CHECK(up_calls == static_cast<int>(index + 1));
        CHECK(refresh_calls == static_cast<int>(index + 1));
        CHECK(refresh_target == Target());
    }
}

TEST_CASE(PREFIX "throws command-specific errors when live execution declines")
{
    int up_calls = 0;
    int refresh_calls = 0;
    const auto registry = RegistryWithExecutors(
        [&](void *) {
            ++up_calls;
            return false;
        },
        [&](void *) {
            ++refresh_calls;
            return false;
        });
    const CommandContext context = Context(NavigationUpAvailability::Available,
                                           NavigationRefreshAvailability::Available);

    CHECK_THROWS_AS(registry.Execute(UpId(), context), NavigationUpExecutionError);
    CHECK(up_calls == 1);
    CHECK(refresh_calls == 0);
    CHECK_THROWS_AS(registry.Execute(RefreshId(), context), NavigationRefreshExecutionError);
    CHECK(up_calls == 1);
    CHECK(refresh_calls == 1);
}

TEST_CASE(PREFIX "rejects each registration without its executor")
{
    CommandRegistry registry;
    CHECK(registry.Register(MakeNavigationUpCommand({})) ==
          CommandRegistry::RegisterResult::MissingHandler);
    CHECK(registry.Register(MakeNavigationRefreshCommand({})) ==
          CommandRegistry::RegisterResult::MissingHandler);
}

#undef PREFIX
