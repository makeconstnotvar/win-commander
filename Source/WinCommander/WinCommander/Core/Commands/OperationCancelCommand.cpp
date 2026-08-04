// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "OperationCancelCommand.h"

#include "CommandIds.h"

namespace nc::core {

namespace {

DisabledReason Disabled(std::string _code, std::string _key, std::string _technical)
{
    return DisabledReason{
        .code = std::move(_code),
        .user_message_key = std::move(_key),
        .technical_message = std::move(_technical),
    };
}

DisabledReason MissingTargetReason()
{
    return Disabled("context.operationCancelTargetRequired",
                    "commands.operation.cancel.disabled.targetUnavailable",
                    "The operation.cancel command requires an immutable OperationCenter target.");
}

DisabledReason CancelUnavailableReason()
{
    return Disabled("operation.cancelUnavailable",
                    "commands.operation.cancel.disabled.unavailable",
                    "The immutable OperationCenter target does not allow cancellation.");
}

DisabledReason ControlUnavailableReason()
{
    return Disabled("operation.controlUnavailable",
                    "commands.operation.cancel.disabled.controlUnavailable",
                    "The OperationCenter cancellation control port is unavailable.");
}

CommandState ControlUnavailableState()
{
    CommandState state;
    state.enabled = false;
    state.disabled_reason = ControlUnavailableReason();
    return state;
}

DisabledReason RejectionReason(const ops::OperationCenterCancelResultCode _code)
{
    using enum ops::OperationCenterCancelResultCode;
    switch( _code ) {
        case OperationNotFound:
            return Disabled("operation.notFound",
                            "commands.operation.cancel.disabled.notFound",
                            "The operation no longer exists in the OperationCenter model.");
        case StaleRevision:
            return Disabled("operation.staleRevision",
                            "commands.operation.cancel.disabled.staleRevision",
                            "The operation changed after its cancellation target was projected.");
        case CancelUnavailable:
            return CancelUnavailableReason();
        case ResidencyUnavailable:
            return Disabled("operation.residencyUnavailable",
                            "commands.operation.cancel.disabled.residencyUnavailable",
                            "The operation has no matching live engine residency.");
        case CancelInProgress:
            return Disabled("operation.cancelInProgress",
                            "commands.operation.cancel.disabled.inProgress",
                            "A cancellation request is already being processed for this operation.");
        case StopRejected:
            return Disabled("operation.stopRejected",
                            "commands.operation.cancel.disabled.stopRejected",
                            "The operation executor rejected the cancellation request.");
        case Accepted:
            break;
    }
    return Disabled("operation.cancelRejected",
                    "commands.operation.cancel.disabled.rejected",
                    "The operation cancellation request was rejected without an actionable result.");
}

CommandState State(const CommandContext &_context)
{
    CommandState state;
    if( !_context.operation_cancel_target ) {
        state.enabled = false;
        state.disabled_reason = MissingTargetReason();
    }
    else if( !_context.operation_cancel_target->can_cancel ) {
        state.enabled = false;
        state.disabled_reason = CancelUnavailableReason();
    }
    return state;
}

} // namespace

OperationCancelTarget OperationCancelTargetFromRecord(const ops::OperationRecord &_record) noexcept
{
    return {
        .operation_id = _record.operation_id,
        .expected_revision = _record.revision,
        .can_cancel = _record.controls.can_cancel,
    };
}

CommandContext OperationCancelContextFromRecord(const ops::OperationRecord &_record,
                                                const CommandInvocationSource _source) noexcept
{
    CommandContext context;
    context.source = _source;
    context.operation_cancel_target = OperationCancelTargetFromRecord(_record);
    return context;
}

CommandState OperationCancelPresentationState(const CommandRegistry::StateResult &_registry_state)
{
    if( _registry_state.status == CommandRegistry::LookupStatus::Found )
        return _registry_state.state;
    return ControlUnavailableState();
}

CommandRegistry::Registration MakeOperationCancelCommand(OperationCancelExecutor _executor)
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::OperationCancel};
    descriptor.title_key = "commands.operation.cancel.title";
    descriptor.description_key = "commands.operation.cancel.description";
    descriptor.category = CommandCategory::Operation;
    descriptor.icon_name = "xmark.circle";
    descriptor.is_destructive = false;
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "operation.cancel";

    CommandRegistry::Registration registration;
    registration.descriptor = std::move(descriptor);
    registration.state_provider = State;
    registration.result_handler = [_executor = std::move(_executor)](const CommandContext &_context)
        -> std::optional<DisabledReason> {
        if( !_context.operation_cancel_target )
            return MissingTargetReason();
        if( !_context.operation_cancel_target->can_cancel )
            return CancelUnavailableReason();
        if( !_executor )
            return ControlUnavailableReason();

        const auto result = _executor(_context.operation_cancel_target->operation_id,
                                      _context.operation_cancel_target->expected_revision);
        if( result.code == ops::OperationCenterCancelResultCode::Accepted )
            return std::nullopt;
        return RejectionReason(result.code);
    };
    return registration;
}

} // namespace nc::core
