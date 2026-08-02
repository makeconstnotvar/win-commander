// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "LegacyShortcutBindingAdapter.h"
#include <algorithm>
#include <map>
#include <utility>

namespace nc::core {

namespace {

template <class T>
void AppendUnique(std::vector<T> &_target, T _value)
{
    if( std::ranges::find(_target, _value) == _target.end() )
        _target.emplace_back(std::move(_value));
}

void AppendShortcuts(LegacyShortcutBindingAdapter::Shortcuts &_target,
                     const std::optional<LegacyShortcutBindingAdapter::Shortcuts> &_source)
{
    if( !_source )
        return;

    for( const LegacyShortcutBindingAdapter::Shortcut &shortcut : *_source )
        if( shortcut && std::ranges::find(_target, shortcut) == _target.end() )
            _target.emplace_back(shortcut);
}

} // namespace

LegacyShortcutBindingAdapter::LegacyShortcutBindingAdapter(const std::span<const CommandDescriptor> _descriptors,
                                                           const utility::ActionsShortcutsManager &_shortcuts_manager)
    : m_ShortcutsManager(_shortcuts_manager)
{
    // std::map both merges duplicate descriptors for the same stable id and gives deterministic ordering.
    std::map<std::string, Binding> bindings;
    for( const CommandDescriptor &descriptor : _descriptors ) {
        if( !descriptor.id.IsValid() || !descriptor.legacy )
            continue;

        const std::string stable_id{descriptor.id.Value()};
        auto [iterator, inserted] = bindings.try_emplace(stable_id);
        Binding &binding = iterator->second;
        if( inserted )
            binding.command_id = descriptor.id;

        for( const std::string &action_name : descriptor.legacy->shortcut_action_names )
            if( !action_name.empty() )
                AppendUnique(binding.action_names, action_name);

        if( descriptor.legacy->shortcut_tag ) {
            const int action_tag = *descriptor.legacy->shortcut_tag;
            AppendUnique(binding.action_tags, action_tag);
            if( const auto action_name = m_ShortcutsManager.ActionFromTag(action_tag);
                action_name && !action_name->empty() )
                AppendUnique(binding.action_names, std::string{*action_name});
        }
    }

    m_Bindings.reserve(bindings.size());
    m_Index.reserve(bindings.size());
    for( auto &[stable_id, binding] : bindings ) {
        m_Index.emplace(stable_id, m_Bindings.size());
        m_Bindings.emplace_back(std::move(binding));
    }
}

LegacyShortcutBindingAdapter::ResolveResult LegacyShortcutBindingAdapter::Resolve(const Shortcut &_shortcut) const
{
    if( !_shortcut )
        return {};

    std::vector<CommandId> matches;
    for( const Binding &binding : m_Bindings ) {
        const Shortcuts shortcuts = CurrentShortcuts(binding.command_id);
        if( std::ranges::find(shortcuts, _shortcut) != shortcuts.end() )
            matches.emplace_back(binding.command_id);
    }

    if( matches.empty() )
        return {};
    if( matches.size() == 1 ) {
        return ResolveResult{
            .status = ResolveStatus::Resolved,
            .command_id = std::move(matches.front()),
        };
    }
    return ResolveResult{
        .status = ResolveStatus::Ambiguous,
        .ambiguous_command_ids = std::move(matches),
    };
}

LegacyShortcutBindingAdapter::Shortcuts
LegacyShortcutBindingAdapter::CurrentShortcuts(const CommandId &_command_id) const
{
    const Binding *const binding = FindBinding(_command_id);
    if( !binding )
        return {};

    Shortcuts shortcuts;
    for( const std::string &action_name : binding->action_names )
        AppendShortcuts(shortcuts, m_ShortcutsManager.ShortcutsFromAction(action_name));
    for( const int action_tag : binding->action_tags )
        AppendShortcuts(shortcuts, m_ShortcutsManager.ShortcutsFromTag(action_tag));
    return shortcuts;
}

std::span<const std::string>
LegacyShortcutBindingAdapter::LegacyActionNames(const CommandId &_command_id) const noexcept
{
    if( const Binding *const binding = FindBinding(_command_id) )
        return binding->action_names;
    return {};
}

const LegacyShortcutBindingAdapter::Binding *
LegacyShortcutBindingAdapter::FindBinding(const CommandId &_command_id) const noexcept
{
    const auto iterator = m_Index.find(std::string{_command_id.Value()});
    return iterator == m_Index.end() ? nullptr : &m_Bindings[iterator->second];
}

} // namespace nc::core
