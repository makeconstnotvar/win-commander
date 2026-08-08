// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Bootstrap/Actions.h>
#include <WinCommander/Core/Commands/ShortcutProfiles.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using nc::bootstrap::g_ActionsTags;
using nc::bootstrap::g_DefaultActionShortcuts;
using nc::core::AllShortcutProfiles;
using nc::core::DetectShortcutConflicts;
using nc::core::ShortcutBinding;
using nc::core::ShortcutConflict;
using nc::core::ShortcutProfile;
using nc::core::ShortcutProfileKind;

/** The application's own defaults, in the form the conflict detector reads. */
std::vector<ShortcutBinding> ShippedBindings()
{
    std::vector<ShortcutBinding> bindings;
    bindings.reserve(std::size(g_DefaultActionShortcuts));
    for( const auto &[action, shortcut] : g_DefaultActionShortcuts )
        bindings.push_back(ShortcutBinding{.action = action, .shortcut = shortcut == nullptr ? "" : shortcut});
    return bindings;
}

std::set<std::string_view> ShippedActionNames()
{
    std::set<std::string_view> names;
    for( const auto &[action, tag] : g_ActionsTags )
        names.insert(action);
    return names;
}

std::string Describe(const ShortcutConflict &_conflict)
{
    std::string text = _conflict.domain + " '" + _conflict.shortcut + "':";
    for( const std::string &action : _conflict.actions )
        text += " " + action;
    return text;
}

} // namespace

#define PREFIX "nc::core::ShippedShortcutTables "

TEST_CASE(PREFIX "ships no two actions in one domain claiming the same key")
{
    // The detector was built to find this; running it on the tables the application actually ships
    // is what turns it from a utility into a guard. A conflict here means one of two menu items
    // silently never fires, and which one depends on lookup order.
    const std::vector<ShortcutConflict> conflicts = DetectShortcutConflicts(ShippedBindings());

    std::string report;
    for( const ShortcutConflict &conflict : conflicts )
        report += Describe(conflict) + "\n";
    INFO("conflicting default shortcuts:\n" << report);
    CHECK(conflicts.empty());
}

TEST_CASE(PREFIX "gives every default shortcut to an action that exists")
{
    // A default naming an action the tag table does not have is dead weight that reads as a working
    // binding: it survives every lookup, and the key it claims simply does nothing.
    const std::set<std::string_view> known = ShippedActionNames();

    std::vector<std::string> unknown;
    for( const auto &[action, shortcut] : g_DefaultActionShortcuts )
        if( !known.contains(action) )
            unknown.emplace_back(action);

    std::string report;
    for( const std::string &action : unknown )
        report += action + "\n";
    INFO("default shortcuts naming unknown actions:\n" << report);
    CHECK(unknown.empty());
}

TEST_CASE(PREFIX "gives every action a distinct name and a distinct tag")
{
    // A duplicated name or tag makes one of the two unreachable, and the survivor depends on which
    // side of the lookup you come from.
    std::map<std::string_view, int> by_name;
    std::map<int, std::string_view> by_tag;
    std::vector<std::string> duplicates;

    for( const auto &[action, tag] : g_ActionsTags ) {
        if( const auto found = by_name.find(action); found != by_name.end() )
            duplicates.push_back(std::string{"name "} + action);
        else
            by_name.emplace(action, tag);

        if( const auto found = by_tag.find(tag); found != by_tag.end() )
            duplicates.push_back("tag " + std::to_string(tag) + " (" + std::string{action} + " and " +
                                 std::string{found->second} + ")");
        else
            by_tag.emplace(tag, action);
    }

    std::string report;
    for( const std::string &duplicate : duplicates )
        report += duplicate + "\n";
    INFO("duplicated actions:\n" << report);
    CHECK(duplicates.empty());
}

TEST_CASE(PREFIX "every profile overrides actions the application actually has")
{
    // A profile naming an action that does not exist does nothing at all, and does it silently: the
    // user picks "Windows Explorer", presses the key it promised, and nothing happens.
    const std::set<std::string_view> known = ShippedActionNames();

    std::vector<std::string> unknown;
    for( const ShortcutProfile &profile : AllShortcutProfiles() )
        for( const ShortcutBinding &binding : profile.bindings )
            if( !known.contains(binding.action) )
                unknown.push_back(profile.id + ": " + binding.action);

    std::string report;
    for( const std::string &entry : unknown )
        report += entry + "\n";
    INFO("profile bindings naming unknown actions:\n" << report);
    CHECK(unknown.empty());
}

TEST_CASE(PREFIX "no profile introduces a conflict once applied over the defaults")
{
    // A profile is applied *on top of* the defaults, so a binding can be conflict-free on its own
    // and still collide with a default it does not replace. Checking the profile alone would miss
    // exactly the collisions a user would meet.
    for( const ShortcutProfile &profile : AllShortcutProfiles() ) {
        if( profile.kind == ShortcutProfileKind::Default ) {
            CHECK(profile.bindings.empty());
            continue;
        }

        std::vector<ShortcutBinding> applied = ShippedBindings();
        for( const ShortcutBinding &override_binding : profile.bindings ) {
            const auto existing = std::ranges::find_if(
                applied, [&](const ShortcutBinding &_b) { return _b.action == override_binding.action; });
            if( existing != applied.end() )
                existing->shortcut = override_binding.shortcut;
            else
                applied.push_back(override_binding);
        }

        const std::vector<ShortcutConflict> conflicts = DetectShortcutConflicts(applied);
        std::string report;
        for( const ShortcutConflict &conflict : conflicts )
            report += Describe(conflict) + "\n";
        INFO("profile '" << profile.id << "' conflicts once applied:\n" << report);
        CHECK(conflicts.empty());
    }
}

#undef PREFIX
