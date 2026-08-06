// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "OperationCenterOpenCommand.h"

#include "CommandIds.h"

#include <utility>

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
    return Disabled("context.operationCenterPresentationTargetRequired",
                    "commands.operationCenter.open.disabled.targetUnavailable",
                    "The operationCenter.open command requires a live synchronous presentation target.");
}

DisabledReason SnapshotUnavailableReason()
{
    return Disabled("operation.snapshotUnavailable",
                    "commands.operationCenter.open.disabled.snapshotUnavailable",
                    "The OperationCenter value snapshot is unavailable.");
}

DisabledReason PresenterUnavailableReason()
{
    return Disabled("operation.presenterUnavailable",
                    "commands.operationCenter.open.disabled.presenterUnavailable",
                    "The OperationCenter snapshot presenter is unavailable.");
}

DisabledReason CenterUnavailableReason()
{
    return Disabled("operation.centerUnavailable",
                    "commands.operationCenter.open.disabled.unavailable",
                    "The operationCenter.open Registry definition is unavailable.");
}

DisabledReason PresentationRejectedReason()
{
    return Disabled("operation.presentationRejected",
                    "commands.operationCenter.open.disabled.rejected",
                    "The OperationCenter snapshot presenter rejected the value-only snapshot.");
}

CommandState DisabledState(const DisabledReason &_reason)
{
    CommandState state;
    state.enabled = false;
    state.disabled_reason = _reason;
    return state;
}

CommandState State(const CommandContext &_context,
                   const bool _has_snapshot_provider,
                   const bool _has_presenter,
                   const OperationCenterOpenSnapshotAvailability &_snapshot_available)
{
    if( !_context.native_target )
        return DisabledState(MissingTargetReason());
    if( !_has_snapshot_provider )
        return DisabledState(SnapshotUnavailableReason());
    if( !_has_presenter )
        return DisabledState(PresenterUnavailableReason());
    if( _snapshot_available && !_snapshot_available() )
        return DisabledState(SnapshotUnavailableReason());
    return {};
}

} // namespace

CommandState OperationCenterOpenPresentationState(const CommandRegistry::StateResult &_registry_state)
{
    if( _registry_state.status == CommandRegistry::LookupStatus::Found )
        return _registry_state.state;
    return DisabledState(CenterUnavailableReason());
}

CommandRegistry::Registration MakeOperationCenterOpenCommand(OperationCenterOpenSnapshotProvider _snapshot_provider,
                                                              OperationCenterOpenPresenter _presenter,
                                                              OperationCenterOpenSnapshotAvailability _snapshot_available)
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{command_ids::OperationCenterOpen};
    descriptor.title_key = "commands.operationCenter.open.title";
    descriptor.description_key = "commands.operationCenter.open.description";
    descriptor.category = CommandCategory::Operation;
    descriptor.icon_name = "list.bullet.rectangle";
    descriptor.is_destructive = false;
    descriptor.requires_operation_plan = false;
    descriptor.supports_undo = false;
    descriptor.analytics_name = "operationCenter.open";

    const bool has_snapshot_provider = static_cast<bool>(_snapshot_provider);
    const bool has_presenter = static_cast<bool>(_presenter);

    CommandRegistry::Registration registration;
    registration.descriptor = std::move(descriptor);
    registration.state_provider = [has_snapshot_provider,
                                   has_presenter,
                                   _snapshot_available](const CommandContext &_context) {
        return State(_context, has_snapshot_provider, has_presenter, _snapshot_available);
    };
    registration.result_handler = [_snapshot_provider = std::move(_snapshot_provider),
                                   _presenter = std::move(_presenter),
                                   _snapshot_available = std::move(_snapshot_available)](const CommandContext &_context)
        -> std::optional<DisabledReason> {
        if( !_context.native_target )
            return MissingTargetReason();
        if( !_snapshot_provider )
            return SnapshotUnavailableReason();
        if( !_presenter )
            return PresenterUnavailableReason();
        if( _snapshot_available && !_snapshot_available() )
            return SnapshotUnavailableReason();

        const auto snapshot = _snapshot_provider();
        if( !snapshot )
            return SnapshotUnavailableReason();

        // Copy again at the Registry boundary. The UI receives only this independent value vector,
        // never model storage or an OperationCenterCoordinator reference.
        std::vector<ops::OperationRecord> presentation_snapshot{*snapshot};
        if( !_presenter(_context.native_target, std::move(presentation_snapshot)) )
            return PresentationRejectedReason();
        return std::nullopt;
    };
    return registration;
}

} // namespace nc::core
