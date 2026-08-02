// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/CommandRegistry.h>
#include <array>
#include <set>

using nc::core::CommandContext;
using nc::core::CommandDescriptor;
using nc::core::CommandId;
using nc::core::CommandInvocationSource;
using nc::core::CommandRegistry;
using nc::core::CommandState;
using nc::core::DisabledReason;

#define PREFIX "nc::core::CommandRegistry "

namespace {

CommandDescriptor Descriptor(std::string_view _id)
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{_id};
    descriptor.title_key = "commands.test.title";
    descriptor.description_key = "commands.test.description";
    return descriptor;
}

CommandRegistry::Registration Registration(std::string_view _id, CommandRegistry::Handler _handler)
{
    CommandRegistry::Registration registration;
    registration.descriptor = Descriptor(_id);
    registration.handler = std::move(_handler);
    return registration;
}

DisabledReason TestDisabledReason()
{
    return DisabledReason{
        .code = "selection.empty",
        .user_message_key = "commands.disabled.selectionEmpty",
        .technical_message = "No selected items.",
        .suggested_action = std::nullopt,
    };
}

} // namespace

TEST_CASE(PREFIX "stable command ids")
{
    using namespace nc::core::command_ids;

    CHECK(FileOpen == "file.open");
    CHECK(NavigationBack == "navigation.back");
    CHECK(NavigationForward == "navigation.forward");
    CHECK(NavigationUp == "navigation.up");
    CHECK(NavigationRefresh == "navigation.refresh");
    CHECK(FileCopy == "file.copy");
    CHECK(FileCut == "file.cut");
    CHECK(FileRename == "file.rename");
    CHECK(ViewToggleHiddenFiles == "view.toggleHiddenFiles");
    CHECK(OperationCancel == "operation.cancel");
    CHECK(OperationCenterOpen == "operationCenter.open");

    const std::set<std::string_view> ids{
        FileOpen,
        NavigationBack,
        NavigationForward,
        NavigationUp,
        NavigationRefresh,
        FileCopy,
        FileCut,
        FileRename,
        ViewToggleHiddenFiles,
        OperationCancel,
        OperationCenterOpen};
    CHECK(ids.size() == 11);
}

TEST_CASE(PREFIX "rejects invalid ids and missing handlers")
{
    CommandRegistry registry;

    CHECK(registry.Register(Registration("", [](const CommandContext &) {})) ==
          CommandRegistry::RegisterResult::InvalidCommandId);
    CHECK(registry.Register(Registration("file..copy", [](const CommandContext &) {})) ==
          CommandRegistry::RegisterResult::InvalidCommandId);
    CHECK(registry.Register(Registration("file.copy-now", [](const CommandContext &) {})) ==
          CommandRegistry::RegisterResult::InvalidCommandId);
    CHECK(registry.Register(Registration("file.copy", {})) == CommandRegistry::RegisterResult::MissingHandler);
    CHECK(registry.All().empty());
}

TEST_CASE(PREFIX "rejects duplicate ids without replacing the original")
{
    CommandRegistry registry;
    int first_calls = 0;
    int replacement_calls = 0;

    REQUIRE(registry.Register(Registration("file.copy", [&](const CommandContext &) { ++first_calls; })) ==
            CommandRegistry::RegisterResult::Registered);
    CHECK(registry.Register(Registration("file.copy", [&](const CommandContext &) { ++replacement_calls; })) ==
          CommandRegistry::RegisterResult::DuplicateCommandId);

    CHECK(registry.Execute(CommandId{"file.copy"}, {}).status == CommandRegistry::ExecutionStatus::Executed);
    CHECK(first_calls == 1);
    CHECK(replacement_calls == 0);
    CHECK(registry.All().size() == 1);
}

TEST_CASE(PREFIX "returns typed unknown results")
{
    CommandRegistry registry;
    const CommandId unknown{"file.unknown"};

    CHECK(registry.Find(unknown) == nullptr);
    const auto state = registry.QueryState(unknown, {});
    CHECK(state.status == CommandRegistry::LookupStatus::UnknownCommand);
    CHECK_FALSE(state.state.visible);
    CHECK_FALSE(state.state.enabled);
    CHECK(registry.Execute(unknown, {}).status == CommandRegistry::ExecutionStatus::UnknownCommand);
}

TEST_CASE(PREFIX "does not execute hidden or disabled commands")
{
    CommandRegistry registry;
    int hidden_calls = 0;
    int disabled_calls = 0;
    int missing_reason_calls = 0;

    auto hidden = Registration("view.hidden", [&](const CommandContext &) { ++hidden_calls; });
    hidden.state_provider = [](const CommandContext &) {
        CommandState state;
        state.visible = false;
        return state;
    };
    REQUIRE(registry.Register(std::move(hidden)) == CommandRegistry::RegisterResult::Registered);

    const auto reason = TestDisabledReason();
    auto disabled = Registration("file.copy", [&](const CommandContext &) { ++disabled_calls; });
    disabled.state_provider = [reason](const CommandContext &) {
        CommandState state;
        state.enabled = false;
        state.disabled_reason = reason;
        return state;
    };
    REQUIRE(registry.Register(std::move(disabled)) == CommandRegistry::RegisterResult::Registered);

    auto missing_reason = Registration("file.missingReason", [&](const CommandContext &) { ++missing_reason_calls; });
    missing_reason.state_provider = [](const CommandContext &) {
        CommandState state;
        state.enabled = false;
        return state;
    };
    REQUIRE(registry.Register(std::move(missing_reason)) == CommandRegistry::RegisterResult::Registered);

    CHECK(registry.Execute(CommandId{"view.hidden"}, {}).status == CommandRegistry::ExecutionStatus::Hidden);
    const auto disabled_result = registry.Execute(CommandId{"file.copy"}, {});
    CHECK(disabled_result.status == CommandRegistry::ExecutionStatus::Disabled);
    REQUIRE(disabled_result.disabled_reason);
    CHECK(*disabled_result.disabled_reason == reason);

    const auto fallback_state = registry.QueryState(CommandId{"file.missingReason"}, {});
    REQUIRE(fallback_state.state.disabled_reason);
    CHECK(fallback_state.state.disabled_reason->code == "command.disabled");
    CHECK(fallback_state.state.disabled_reason->user_message_key == "commands.disabled.generic");
    const auto fallback_execution = registry.Execute(CommandId{"file.missingReason"}, {});
    CHECK(fallback_execution.status == CommandRegistry::ExecutionStatus::Disabled);
    REQUIRE(fallback_execution.disabled_reason);
    CHECK(fallback_execution.disabled_reason->code == "command.disabled");

    CHECK(hidden_calls == 0);
    CHECK(disabled_calls == 0);
    CHECK(missing_reason_calls == 0);
}

TEST_CASE(PREFIX "recomputes state for every query and execution")
{
    CommandRegistry registry;
    int state_queries = 0;
    int handler_calls = 0;
    bool enabled = false;

    auto registration = Registration("file.open", [&](const CommandContext &) { ++handler_calls; });
    registration.state_provider = [&](const CommandContext &) {
        ++state_queries;
        CommandState state;
        state.enabled = enabled;
        if( !enabled )
            state.disabled_reason = TestDisabledReason();
        return state;
    };
    REQUIRE(registry.Register(std::move(registration)) == CommandRegistry::RegisterResult::Registered);

    CHECK_FALSE(registry.QueryState(CommandId{"file.open"}, {}).state.enabled);
    enabled = true;
    CHECK(registry.QueryState(CommandId{"file.open"}, {}).state.enabled);
    CHECK(registry.Execute(CommandId{"file.open"}, {}).status == CommandRegistry::ExecutionStatus::Executed);
    CHECK(state_queries == 3);
    CHECK(handler_calls == 1);
}

TEST_CASE(PREFIX "preserves registration order")
{
    CommandRegistry registry;
    const std::array ids{"file.open", "file.copy", "operation.cancel"};

    for( const auto *id : ids )
        REQUIRE(registry.Register(Registration(id, [](const CommandContext &) {})) ==
                CommandRegistry::RegisterResult::Registered);

    const auto descriptors = registry.All();
    REQUIRE(descriptors.size() == ids.size());
    for( std::size_t index = 0; index < ids.size(); ++index )
        CHECK(descriptors[index].id.Value() == ids[index]);
}

TEST_CASE(PREFIX "executes exactly once with the supplied context")
{
    CommandRegistry registry;
    int calls = 0;
    const int sender = 42;
    CommandInvocationSource observed_source = CommandInvocationSource::Programmatic;
    const void *observed_sender = nullptr;

    auto registration = Registration("file.copy", [&](const CommandContext &_context) {
        ++calls;
        observed_source = _context.source;
        observed_sender = _context.native_sender;
    });
    REQUIRE(registry.Register(std::move(registration)) == CommandRegistry::RegisterResult::Registered);

    CommandContext context;
    context.source = CommandInvocationSource::Shortcut;
    context.native_sender = &sender;
    CHECK(registry.Execute(CommandId{"file.copy"}, context).status == CommandRegistry::ExecutionStatus::Executed);
    CHECK(calls == 1);
    CHECK(observed_source == CommandInvocationSource::Shortcut);
    CHECK(observed_sender == &sender);
}
