// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "Command.h"
#include <Utility/ActionsShortcutsManager.h>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace nc::core {

/**
 * A read-only bridge between stable command identifiers and legacy persisted shortcut bindings.
 *
 * The adapter copies descriptor metadata but keeps shortcut values in ActionsShortcutsManager. This
 * preserves legacy action names as persistence keys and makes overrides immediately visible without
 * rebuilding the index.
 */
class LegacyShortcutBindingAdapter final
{
public:
    using Shortcut = utility::ActionsShortcutsManager::Shortcut;
    using Shortcuts = utility::ActionsShortcutsManager::Shortcuts;

    enum class ResolveStatus {
        NotFound,
        Resolved,
        Ambiguous
    };

    struct ResolveResult {
        ResolveStatus status = ResolveStatus::NotFound;
        /** Present only for Resolved. Ambiguous resolutions never select a command. */
        std::optional<CommandId> command_id;
        /** Sorted stable command identifiers, present only for Ambiguous. */
        std::vector<CommandId> ambiguous_command_ids;

        bool operator==(const ResolveResult &) const = default;
    };

    LegacyShortcutBindingAdapter(std::span<const CommandDescriptor> _descriptors,
                                 const utility::ActionsShortcutsManager &_shortcuts_manager);

    /** Resolves the manager's current shortcut values. Overrides take effect immediately. */
    [[nodiscard]] ResolveResult Resolve(const Shortcut &_shortcut) const;

    /** Returns current, de-duplicated shortcuts for a stable command identifier. */
    [[nodiscard]] Shortcuts CurrentShortcuts(const CommandId &_command_id) const;

    /** Returns copied legacy persistence keys for a stable command identifier. */
    [[nodiscard]] std::span<const std::string> LegacyActionNames(const CommandId &_command_id) const noexcept;

private:
    struct Binding {
        CommandId command_id;
        std::vector<std::string> action_names;
        std::vector<int> action_tags;
    };

    [[nodiscard]] const Binding *FindBinding(const CommandId &_command_id) const noexcept;

    const utility::ActionsShortcutsManager &m_ShortcutsManager;
    std::vector<Binding> m_Bindings;
    std::unordered_map<std::string, std::size_t> m_Index;
};

} // namespace nc::core
