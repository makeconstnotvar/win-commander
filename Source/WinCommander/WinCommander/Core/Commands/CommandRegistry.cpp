// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CommandRegistry.h"

namespace nc::core {

namespace {

DisabledReason MissingDisabledReason()
{
    return DisabledReason{
        .code = "command.disabled",
        .user_message_key = "commands.disabled.generic",
        .technical_message = "The command state provider did not supply a disabled reason.",
        .suggested_action = std::nullopt,
    };
}

CommandState UnknownCommandState()
{
    return CommandState{
        .visible = false,
        .enabled = false,
        .disabled_reason = std::nullopt,
        .title_key_override = std::nullopt,
        .check_state = CommandCheckState::Off,
    };
}

} // namespace

CommandRegistry::RegisterResult CommandRegistry::Register(Registration _registration)
{
    if( !_registration.descriptor.id.IsValid() )
        return RegisterResult::InvalidCommandId;
    if( !_registration.handler && !_registration.result_handler )
        return RegisterResult::MissingHandler;
    if( _registration.handler && _registration.result_handler )
        return RegisterResult::ConflictingHandlers;

    const std::string id{_registration.descriptor.id.Value()};
    if( m_Index.contains(id) )
        return RegisterResult::DuplicateCommandId;

    const auto index = m_Descriptors.size();
    m_Descriptors.emplace_back(std::move(_registration.descriptor));
    m_StateProviders.emplace_back(std::move(_registration.state_provider));
    m_Handlers.emplace_back(std::move(_registration.handler));
    m_ResultHandlers.emplace_back(std::move(_registration.result_handler));
    m_Index.emplace(id, index);
    return RegisterResult::Registered;
}

const CommandDescriptor *CommandRegistry::Find(const CommandId &_id) const noexcept
{
    const auto index = IndexOf(_id);
    return index ? &m_Descriptors[*index] : nullptr;
}

CommandRegistry::StateResult CommandRegistry::QueryState(const CommandId &_id,
                                                         const CommandContext &_context) const
{
    const auto index = IndexOf(_id);
    if( !index )
        return StateResult{.status = LookupStatus::UnknownCommand, .state = UnknownCommandState()};

    return StateResult{.status = LookupStatus::Found, .state = EvaluateState(*index, _context)};
}

CommandRegistry::ExecutionResult CommandRegistry::Execute(const CommandId &_id,
                                                          const CommandContext &_context) const
{
    const auto index = IndexOf(_id);
    if( !index )
        return ExecutionResult{.status = ExecutionStatus::UnknownCommand};

    CommandState state = EvaluateState(*index, _context);
    if( !state.visible )
        return ExecutionResult{.status = ExecutionStatus::Hidden, .disabled_reason = std::move(state.disabled_reason)};
    if( !state.enabled )
        return ExecutionResult{.status = ExecutionStatus::Disabled, .disabled_reason = std::move(state.disabled_reason)};

    if( const auto &result_handler = m_ResultHandlers[*index] ) {
        if( auto reason = result_handler(_context) )
            return ExecutionResult{.status = ExecutionStatus::Rejected, .disabled_reason = std::move(reason)};
    }
    else {
        m_Handlers[*index](_context);
    }
    return ExecutionResult{.status = ExecutionStatus::Executed};
}

std::optional<std::size_t> CommandRegistry::IndexOf(const CommandId &_id) const noexcept
{
    const auto iterator = m_Index.find(std::string{_id.Value()});
    if( iterator == m_Index.end() )
        return std::nullopt;
    return iterator->second;
}

CommandState CommandRegistry::EvaluateState(const std::size_t _index, const CommandContext &_context) const
{
    CommandState state = m_StateProviders[_index] ? m_StateProviders[_index](_context) : CommandState{};
    if( !state.enabled && !state.disabled_reason )
        state.disabled_reason = MissingDisabledReason();
    return state;
}

} // namespace nc::core
