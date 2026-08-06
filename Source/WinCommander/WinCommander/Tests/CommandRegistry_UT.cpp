// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/Core/Commands/CommandIds.h>
#include <WinCommander/Core/Commands/OperationCancelCommand.h>
#include <WinCommander/Core/Commands/OperationCenterOpenCommand.h>
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
using nc::core::OperationCancelTarget;
using nc::core::OperationCancelExecutor;
using nc::core::OperationCancelContextFromRecord;
using nc::core::OperationCancelPresentationState;
using nc::core::MakeOperationCancelCommand;
using nc::core::OperationCenterOpenPresentationState;
using nc::core::MakeOperationCenterOpenCommand;
using nc::ops::OperationCenterCancelResult;
using nc::ops::OperationCenterCancelResultCode;
using nc::ops::OperationId;

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

CommandContext OperationCancelContext(const bool _can_cancel = true, const uint64_t _revision = 7)
{
    const auto operation_id = OperationId::Parse("op-41");
    REQUIRE(operation_id);
    CommandContext context;
    context.operation_cancel_target = OperationCancelTarget{
        .operation_id = *operation_id,
        .expected_revision = _revision,
        .can_cancel = _can_cancel,
    };
    return context;
}

nc::ops::OperationRecord OperationCancelRecord(const bool _can_cancel = true, const uint64_t _revision = 7)
{
    const auto operation_id = OperationId::Parse("op-41");
    REQUIRE(operation_id);
    auto plan = nc::ops::OperationPlan::Create({
        .plan_id = "command-registry-operation-cancel",
        .type = nc::ops::OperationPlanType::Copy,
        .sources = {nc::ops::OperationPlanSourceInput{"native", "/source"}},
        .destination = nc::ops::OperationPlanDestinationInput{
            "native", "/destination", nc::ops::OperationPlanDestinationKind::Directory},
        .conflict_policy = nc::ops::OperationPlanConflictPolicy{nc::ops::OperationPlanConflictDecision::Ask,
                                                                 nc::ops::OperationPlanConflictScope::ThisItem},
        .created_at = nc::ops::OperationPlan::TimePoint{},
    });
    REQUIRE(plan);
    return {
        .operation_id = *operation_id,
        .plan_id = (*plan).Id(),
        .operation_type = nc::ops::OperationPlanType::Copy,
        .state = nc::ops::OperationRecordState::Running,
        .revision = _revision,
        .created_at = nc::ops::OperationPlan::TimePoint{},
        .controls = {.can_cancel = _can_cancel},
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

TEST_CASE(PREFIX "operation.cancel context is a value-only projection of one immutable record")
{
    const auto record = OperationCancelRecord(true, 9);
    const auto context = OperationCancelContextFromRecord(record, CommandInvocationSource::Menu);

    CHECK(context.source == CommandInvocationSource::Menu);
    REQUIRE(context.operation_cancel_target);
    CHECK(context.operation_cancel_target->operation_id == record.operation_id);
    CHECK(context.operation_cancel_target->expected_revision == record.revision);
    CHECK(context.operation_cancel_target->can_cancel == record.controls.can_cancel);
}

TEST_CASE(PREFIX "operation.cancel missing Registry definition is visibly disabled with a typed reason")
{
    CommandRegistry registry;
    const auto context = OperationCancelContextFromRecord(OperationCancelRecord(), CommandInvocationSource::Menu);
    const auto missing = registry.QueryState(CommandId{nc::core::command_ids::OperationCancel}, context);

    CHECK(missing.status == CommandRegistry::LookupStatus::UnknownCommand);
    const auto presentation = OperationCancelPresentationState(missing);
    CHECK(presentation.visible);
    CHECK_FALSE(presentation.enabled);
    REQUIRE(presentation.disabled_reason);
    CHECK(presentation.disabled_reason->code == "operation.controlUnavailable");
    CHECK(presentation.disabled_reason->user_message_key == "commands.operation.cancel.disabled.controlUnavailable");
}

TEST_CASE(PREFIX "operationCenter.open missing Registry definition is visibly disabled with a typed reason")
{
    CommandRegistry registry;
    int native_target = 0;
    CommandContext context;
    context.native_target = &native_target;
    const auto missing = registry.QueryState(CommandId{nc::core::command_ids::OperationCenterOpen}, context);

    CHECK(missing.status == CommandRegistry::LookupStatus::UnknownCommand);
    const auto presentation = OperationCenterOpenPresentationState(missing);
    CHECK(presentation.visible);
    CHECK_FALSE(presentation.enabled);
    REQUIRE(presentation.disabled_reason);
    CHECK(presentation.disabled_reason->code == "operation.centerUnavailable");
    CHECK(presentation.disabled_reason->user_message_key == "commands.operationCenter.open.disabled.unavailable");
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

TEST_CASE(PREFIX "result handlers reject after live execution revalidation")
{
    CommandRegistry registry;
    bool ordinary_handler_called = false;
    auto registration = Registration("operation.cancel", [&](const CommandContext &) { ordinary_handler_called = true; });
    registration.result_handler = [](const CommandContext &) -> std::optional<DisabledReason> {
        return DisabledReason{
            .code = "operation.staleRevision",
            .user_message_key = "commands.operation.cancel.disabled.staleRevision",
            .technical_message = "The operation changed.",
        };
    };
    CHECK(registry.Register(std::move(registration)) == CommandRegistry::RegisterResult::ConflictingHandlers);
    CHECK_FALSE(ordinary_handler_called);
}

TEST_CASE(PREFIX "operation.cancel accepts only an enabled value target and maps live rejection")
{
    int executor_calls = 0;
    std::optional<OperationId> observed_id;
    uint64_t observed_revision = 0;
    OperationCenterCancelResultCode next_result = OperationCenterCancelResultCode::Accepted;
    OperationCancelExecutor executor = [&](const OperationId _operation_id, const uint64_t _expected_revision) {
        ++executor_calls;
        observed_id = _operation_id;
        observed_revision = _expected_revision;
        return OperationCenterCancelResult{.code = next_result};
    };

    CommandRegistry registry;
    REQUIRE(registry.Register(MakeOperationCancelCommand(std::move(executor))) ==
            CommandRegistry::RegisterResult::Registered);

    const auto missing_target = registry.QueryState(CommandId{nc::core::command_ids::OperationCancel}, {});
    CHECK_FALSE(missing_target.state.enabled);
    REQUIRE(missing_target.state.disabled_reason);
    CHECK(missing_target.state.disabled_reason->code == "context.operationCancelTargetRequired");

    const auto unavailable_context = OperationCancelContext(false);
    const auto unavailable = registry.Execute(CommandId{nc::core::command_ids::OperationCancel}, unavailable_context);
    CHECK(unavailable.status == CommandRegistry::ExecutionStatus::Disabled);
    REQUIRE(unavailable.disabled_reason);
    CHECK(unavailable.disabled_reason->code == "operation.cancelUnavailable");
    CHECK(executor_calls == 0);

    const auto record = OperationCancelRecord(true, 17);
    const auto context = OperationCancelContextFromRecord(record, CommandInvocationSource::Menu);
    CHECK(context.source == CommandInvocationSource::Menu);
    const auto accepted = registry.Execute(CommandId{nc::core::command_ids::OperationCancel}, context);
    CHECK(accepted.status == CommandRegistry::ExecutionStatus::Executed);
    CHECK(executor_calls == 1);
    REQUIRE(observed_id);
    CHECK(*observed_id == record.operation_id);
    CHECK(observed_revision == record.revision);

    struct RejectionCase {
        OperationCenterCancelResultCode code;
        std::string_view disabled_code;
    };
    const std::array rejection_cases{
        RejectionCase{OperationCenterCancelResultCode::OperationNotFound, "operation.notFound"},
        RejectionCase{OperationCenterCancelResultCode::StaleRevision, "operation.staleRevision"},
        RejectionCase{OperationCenterCancelResultCode::CancelUnavailable, "operation.cancelUnavailable"},
        RejectionCase{OperationCenterCancelResultCode::ResidencyUnavailable, "operation.residencyUnavailable"},
        RejectionCase{OperationCenterCancelResultCode::CancelInProgress, "operation.cancelInProgress"},
        RejectionCase{OperationCenterCancelResultCode::StopRejected, "operation.stopRejected"},
    };
    for( const auto rejection : rejection_cases ) {
        next_result = rejection.code;
        const auto rejected = registry.Execute(CommandId{nc::core::command_ids::OperationCancel}, context);
        CHECK(rejected.status == CommandRegistry::ExecutionStatus::Rejected);
        REQUIRE(rejected.disabled_reason);
        CHECK(rejected.disabled_reason->code == rejection.disabled_code);
    }
    CHECK(executor_calls == 1 + static_cast<int>(rejection_cases.size()));
}

TEST_CASE(PREFIX "operationCenter.open presents one copied immutable value snapshot")
{
    const std::vector<nc::ops::OperationRecord> source_records{OperationCancelRecord(true, 23)};
    int snapshot_calls = 0;
    int presenter_calls = 0;
    int native_target = 0;
    void *observed_target = nullptr;
    std::optional<uint64_t> observed_revision;

    CommandRegistry registry;
    REQUIRE(registry.Register(MakeOperationCenterOpenCommand(
                [&]() -> std::optional<std::vector<nc::ops::OperationRecord>> {
                    ++snapshot_calls;
                    return source_records;
                },
                [&](void *const _native_target, std::vector<nc::ops::OperationRecord> _snapshot) {
                    ++presenter_calls;
                    observed_target = _native_target;
                    REQUIRE(_snapshot.size() == 1);
                    observed_revision = _snapshot.front().revision;
                    _snapshot.front().revision = 99;
                    return true;
                })) == CommandRegistry::RegisterResult::Registered);

    const CommandDescriptor *const descriptor = registry.Find(CommandId{nc::core::command_ids::OperationCenterOpen});
    REQUIRE(descriptor);
    CHECK(descriptor->title_key == "commands.operationCenter.open.title");
    CHECK(descriptor->description_key == "commands.operationCenter.open.description");
    CHECK(descriptor->category == nc::core::CommandCategory::Operation);
    CHECK(descriptor->icon_name == "list.bullet.rectangle");
    CHECK_FALSE(descriptor->is_destructive);
    CHECK_FALSE(descriptor->requires_operation_plan);
    CHECK_FALSE(descriptor->supports_undo);
    CHECK(descriptor->analytics_name == "operationCenter.open");

    const auto missing_target = registry.QueryState(CommandId{nc::core::command_ids::OperationCenterOpen}, {});
    CHECK_FALSE(missing_target.state.enabled);
    REQUIRE(missing_target.state.disabled_reason);
    CHECK(missing_target.state.disabled_reason->code == "context.operationCenterPresentationTargetRequired");
    CHECK(snapshot_calls == 0);

    CommandContext context;
    context.source = CommandInvocationSource::Menu;
    context.native_target = &native_target;
    const auto state = registry.QueryState(CommandId{nc::core::command_ids::OperationCenterOpen}, context);
    CHECK(state.status == CommandRegistry::LookupStatus::Found);
    CHECK(state.state.visible);
    CHECK(state.state.enabled);
    CHECK_FALSE(state.state.disabled_reason);
    CHECK(snapshot_calls == 0);

    const auto execution = registry.Execute(CommandId{nc::core::command_ids::OperationCenterOpen}, context);
    CHECK(execution.status == CommandRegistry::ExecutionStatus::Executed);
    CHECK(snapshot_calls == 1);
    CHECK(presenter_calls == 1);
    CHECK(observed_target == &native_target);
    REQUIRE(observed_revision);
    CHECK(*observed_revision == 23);
    CHECK(source_records.front().revision == 23);
}

TEST_CASE(PREFIX "operationCenter.open fails closed without its snapshot presenter")
{
    int native_target = 0;
    int snapshot_calls = 0;
    CommandContext context;
    context.native_target = &native_target;

    CommandRegistry registry;
    REQUIRE(registry.Register(MakeOperationCenterOpenCommand(
                [&]() -> std::optional<std::vector<nc::ops::OperationRecord>> {
                    ++snapshot_calls;
                    return std::vector<nc::ops::OperationRecord>{OperationCancelRecord()};
                },
                {})) == CommandRegistry::RegisterResult::Registered);

    const auto state = registry.QueryState(CommandId{nc::core::command_ids::OperationCenterOpen}, context);
    CHECK_FALSE(state.state.enabled);
    REQUIRE(state.state.disabled_reason);
    CHECK(state.state.disabled_reason->code == "operation.presenterUnavailable");

    const auto execution = registry.Execute(CommandId{nc::core::command_ids::OperationCenterOpen}, context);
    CHECK(execution.status == CommandRegistry::ExecutionStatus::Disabled);
    REQUIRE(execution.disabled_reason);
    CHECK(execution.disabled_reason->code == "operation.presenterUnavailable");
    CHECK(snapshot_calls == 0);
}

TEST_CASE(PREFIX "operationCenter.open disables an unavailable snapshot provider before invocation")
{
    bool snapshot_available = true;
    int snapshot_calls = 0;
    int presenter_calls = 0;
    int native_target = 0;
    CommandContext context;
    context.native_target = &native_target;

    CommandRegistry registry;
    REQUIRE(registry.Register(MakeOperationCenterOpenCommand(
                [&]() -> std::optional<std::vector<nc::ops::OperationRecord>> {
                    ++snapshot_calls;
                    return std::vector<nc::ops::OperationRecord>{OperationCancelRecord()};
                },
                [&](void *, std::vector<nc::ops::OperationRecord>) {
                    ++presenter_calls;
                    return true;
                },
                [&] { return snapshot_available; })) == CommandRegistry::RegisterResult::Registered);

    CHECK(registry.QueryState(CommandId{nc::core::command_ids::OperationCenterOpen}, context).state.enabled);
    snapshot_available = false;
    const auto state = registry.QueryState(CommandId{nc::core::command_ids::OperationCenterOpen}, context);
    CHECK_FALSE(state.state.enabled);
    REQUIRE(state.state.disabled_reason);
    CHECK(state.state.disabled_reason->code == "operation.snapshotUnavailable");

    const auto execution = registry.Execute(CommandId{nc::core::command_ids::OperationCenterOpen}, context);
    CHECK(execution.status == CommandRegistry::ExecutionStatus::Disabled);
    REQUIRE(execution.disabled_reason);
    CHECK(execution.disabled_reason->code == "operation.snapshotUnavailable");
    CHECK(snapshot_calls == 0);
    CHECK(presenter_calls == 0);
}

TEST_CASE(PREFIX "operationCenter.open reports runtime snapshot and presentation rejection")
{
    int native_target = 0;
    CommandContext context;
    context.native_target = &native_target;

    SECTION("snapshot disappears after state projection")
    {
        int snapshot_calls = 0;
        int presenter_calls = 0;
        CommandRegistry registry;
        REQUIRE(registry.Register(MakeOperationCenterOpenCommand(
                    [&]() -> std::optional<std::vector<nc::ops::OperationRecord>> {
                        ++snapshot_calls;
                        return std::nullopt;
                    },
                    [&](void *, std::vector<nc::ops::OperationRecord>) {
                        ++presenter_calls;
                        return true;
                    })) == CommandRegistry::RegisterResult::Registered);

        CHECK(registry.QueryState(CommandId{nc::core::command_ids::OperationCenterOpen}, context).state.enabled);
        const auto execution = registry.Execute(CommandId{nc::core::command_ids::OperationCenterOpen}, context);
        CHECK(execution.status == CommandRegistry::ExecutionStatus::Rejected);
        REQUIRE(execution.disabled_reason);
        CHECK(execution.disabled_reason->code == "operation.snapshotUnavailable");
        CHECK(snapshot_calls == 1);
        CHECK(presenter_calls == 0);
    }

    SECTION("presenter rejects the copied snapshot")
    {
        int snapshot_calls = 0;
        int presenter_calls = 0;
        CommandRegistry registry;
        REQUIRE(registry.Register(MakeOperationCenterOpenCommand(
                    [&]() -> std::optional<std::vector<nc::ops::OperationRecord>> {
                        ++snapshot_calls;
                        return std::vector<nc::ops::OperationRecord>{OperationCancelRecord()};
                    },
                    [&](void *, std::vector<nc::ops::OperationRecord>) {
                        ++presenter_calls;
                        return false;
                    })) == CommandRegistry::RegisterResult::Registered);

        const auto execution = registry.Execute(CommandId{nc::core::command_ids::OperationCenterOpen}, context);
        CHECK(execution.status == CommandRegistry::ExecutionStatus::Rejected);
        REQUIRE(execution.disabled_reason);
        CHECK(execution.disabled_reason->code == "operation.presentationRejected");
        CHECK(snapshot_calls == 1);
        CHECK(presenter_calls == 1);
    }
}
