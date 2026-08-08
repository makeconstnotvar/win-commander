// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ShortcutProfiles.h"

#include <algorithm>
#include <unordered_map>

namespace nc::core {

namespace {

/** The action name's first dot-separated segment; a name with no dot is its own domain. */
std::string_view DomainOf(const std::string_view _action) noexcept
{
    const size_t dot = _action.find('.');
    return dot == std::string_view::npos ? _action : _action.substr(0, dot);
}

ShortcutBinding Bind(std::string _action, std::string _shortcut)
{
    return {.action = std::move(_action), .shortcut = std::move(_shortcut)};
}

} // namespace

std::vector<ShortcutConflict> DetectShortcutConflicts(const std::span<const ShortcutBinding> _bindings)
{
    // Insertion order is kept explicitly rather than relying on a hash map's iteration order, so a
    // report reads in the same order as the table it came from and two runs never differ.
    std::vector<ShortcutConflict> groups;
    std::unordered_map<std::string, size_t> index_of;

    for( const ShortcutBinding &binding : _bindings ) {
        // An unbound action collides with nothing: any number of actions may have no shortcut.
        if( binding.shortcut.empty() || binding.action.empty() )
            continue;
        const std::string_view domain = DomainOf(binding.action);
        std::string key{domain};
        key.push_back('\n'); // A separator that cannot occur in a domain or a key equivalent.
        key.append(binding.shortcut);

        const auto existing = index_of.find(key);
        if( existing == index_of.end() ) {
            index_of.emplace(std::move(key), groups.size());
            groups.push_back({.domain = std::string{domain}, .shortcut = binding.shortcut, .actions = {binding.action}});
            continue;
        }
        groups[existing->second].actions.push_back(binding.action);
    }

    std::vector<ShortcutConflict> conflicts;
    for( ShortcutConflict &group : groups )
        if( group.actions.size() > 1 )
            conflicts.push_back(std::move(group));
    return conflicts;
}

// Every action named below must exist in the application's own tag table: a profile binding an
// action nobody defined does nothing at all, and does it silently - the user picks the profile,
// presses the key it promised, and gets no response and no explanation. `ShippedShortcutTables_UT`
// checks that, and checks that no profile collides with a default it does not itself replace.
std::vector<ShortcutProfile> AllShortcutProfiles()
{
    std::vector<ShortcutProfile> profiles;

    profiles.push_back({.kind = ShortcutProfileKind::Default, .id = "default", .bindings = {}});

    // Finder-like: the bindings a macOS user reaches for first. Rename on Return, Cmd-Down to open,
    // Cmd-Up to go to the enclosing folder, Cmd-Delete to trash.
    profiles.push_back({.kind = ShortcutProfileKind::MacOSNative,
                        .id = "macos",
                        .bindings = {
                            Bind("menu.command.rename_in_place", "\\r"),
                            // Return renames under Finder rules, so whatever else held it has to let
                            // go. Leaving `enter` there would make Return ambiguous and one of the two
                            // silently never fire - the profile would have promised a rename key and
                            // then, half the time, not delivered it.
                            Bind("menu.file.enter", ""),
                            Bind("menu.file.open", "⌘↓"),
                            Bind("menu.go.enclosing_folder", "⌘↑"),
                            Bind("menu.command.move_to_trash", "⌘⌫"),
                            Bind("menu.file.get_info", "⌘i"),
                            Bind("menu.file.new_folder", "⇧⌘n"),
                        }});

    // Windows Explorer-like: F2 renames, Delete trashes, Shift-Delete deletes permanently, and the
    // history moves on Alt-arrows.
    profiles.push_back({.kind = ShortcutProfileKind::WindowsExplorer,
                        .id = "windows",
                        .bindings = {
                            Bind("menu.command.rename_in_place", "\\uF705"),
                            Bind("menu.command.move_to_trash", "\\u007F"),
                            Bind("menu.command.delete_permanently", "⇧\\u007F"),
                            Bind("menu.go.back", "⌥←"),
                            Bind("menu.go.forward", "⌥→"),
                            Bind("menu.go.enclosing_folder", "⌥↑"),
                            Bind("menu.file.new_folder", "⇧⌘n"),
                        }});

    // Total Commander-like: the function-key row carries the core operations. F6 moves and
    // Shift-F6 renames in place, which is why rename is not on F6 itself.
    profiles.push_back({.kind = ShortcutProfileKind::Commander,
                        .id = "commander",
                        .bindings = {
                            Bind("menu.command.copy_to", "\\uF708"),
                            Bind("menu.command.move_to", "\\uF709"),
                            Bind("menu.command.rename_in_place", "⇧\\uF709"),
                            Bind("menu.file.new_folder", "\\uF70A"),
                            Bind("menu.command.move_to_trash", "\\uF70B"),
                            Bind("menu.view.swap_panels", "⌘u"),
                        }});
    return profiles;
}

std::vector<ShortcutProfile>::const_iterator FindShortcutProfile(const std::vector<ShortcutProfile> &_profiles,
                                                                 const std::string_view _id)
{
    return std::ranges::find_if(_profiles,
                                [_id](const ShortcutProfile &_profile) { return _profile.id == _id; });
}

} // namespace nc::core
