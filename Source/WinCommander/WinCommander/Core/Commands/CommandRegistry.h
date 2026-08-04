// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "Command.h"
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace nc::core {

class CommandRegistry final
{
public:
    // Registration is expected during application composition. QueryState and Execute run on the
    // UI thread because state providers and handlers may access AppKit-owned state. The registry
    // intentionally provides no internal synchronization.
    using StateProvider = std::function<CommandState(const CommandContext &)>;
    using Handler = std::function<void(const CommandContext &)>;
    // A result handler can reject an already-admitted command after its live execution port
    // revalidates a value target. This is distinct from a stale presentation snapshot.
    using ResultHandler = std::function<std::optional<DisabledReason>(const CommandContext &)>;

    struct Registration {
        CommandDescriptor descriptor;
        StateProvider state_provider;
        Handler handler;
        ResultHandler result_handler;
    };

    enum class RegisterResult {
        Registered,
        InvalidCommandId,
        DuplicateCommandId,
        MissingHandler,
        ConflictingHandlers
    };

    enum class LookupStatus {
        Found,
        UnknownCommand
    };

    struct StateResult {
        LookupStatus status = LookupStatus::UnknownCommand;
        CommandState state;
    };

    enum class ExecutionStatus {
        Executed,
        UnknownCommand,
        Hidden,
        Disabled,
        Rejected
    };

    struct ExecutionResult {
        ExecutionStatus status = ExecutionStatus::UnknownCommand;
        std::optional<DisabledReason> disabled_reason;
    };

    [[nodiscard]] RegisterResult Register(Registration _registration);

    // Returned pointers remain valid until the next successful registration.
    [[nodiscard]] const CommandDescriptor *Find(const CommandId &_id) const noexcept;
    [[nodiscard]] StateResult QueryState(const CommandId &_id, const CommandContext &_context) const;
    [[nodiscard]] ExecutionResult Execute(const CommandId &_id, const CommandContext &_context) const;

    // Descriptors retain registration order for deterministic menu and palette projection.
    [[nodiscard]] std::span<const CommandDescriptor> All() const noexcept { return m_Descriptors; }

private:
    [[nodiscard]] std::optional<std::size_t> IndexOf(const CommandId &_id) const noexcept;
    [[nodiscard]] CommandState EvaluateState(std::size_t _index, const CommandContext &_context) const;

    std::vector<CommandDescriptor> m_Descriptors;
    std::vector<StateProvider> m_StateProviders;
    std::vector<Handler> m_Handlers;
    std::vector<ResultHandler> m_ResultHandlers;
    std::unordered_map<std::string, std::size_t> m_Index;
};

} // namespace nc::core
