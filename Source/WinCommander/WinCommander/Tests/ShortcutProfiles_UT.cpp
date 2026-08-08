// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Commands/ShortcutProfiles.h>

#include <string>
#include <vector>

namespace {

using nc::core::AllShortcutProfiles;
using nc::core::DetectShortcutConflicts;
using nc::core::FindShortcutProfile;
using nc::core::ShortcutBinding;
using nc::core::ShortcutConflict;
using nc::core::ShortcutProfile;
using nc::core::ShortcutProfileKind;

ShortcutBinding Bind(std::string _action, std::string _shortcut)
{
    return {.action = std::move(_action), .shortcut = std::move(_shortcut)};
}

} // namespace

#define PREFIX "nc::core::DetectShortcutConflicts "

TEST_CASE(PREFIX "reports two actions in one domain claiming the same key")
{
    const std::vector<ShortcutBinding> bindings{
        Bind("menu.go.documents", "⇧⌘o"),
        Bind("menu.view.command_palette", "⇧⌘o"),
        Bind("menu.file.copy", "⌘c"),
    };

    const auto conflicts = DetectShortcutConflicts(bindings);
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].domain == "menu");
    CHECK(conflicts[0].shortcut == "⇧⌘o");
    CHECK(conflicts[0].actions == std::vector<std::string>{"menu.go.documents", "menu.view.command_palette"});
}

TEST_CASE(PREFIX "does not report a key shared across different domains")
{
    // Shortcut lookup is domain-filtered, so these coexist legitimately; reporting them would make
    // the detector noise, and a detector nobody trusts is worse than none.
    const std::vector<ShortcutBinding> bindings{
        Bind("menu.file.copy", "⌘c"),
        Bind("panel.copy", "⌘c"),
        Bind("viewer.copy", "⌘c"),
    };
    CHECK(DetectShortcutConflicts(bindings).empty());
}

TEST_CASE(PREFIX "treats unbound actions as colliding with nothing")
{
    const std::vector<ShortcutBinding> bindings{
        Bind("menu.a", ""),
        Bind("menu.b", ""),
        Bind("menu.c", ""),
        Bind("", "⌘c"), // a nameless binding cannot be reported against
    };
    CHECK(DetectShortcutConflicts(bindings).empty());
}

TEST_CASE(PREFIX "groups every colliding action and preserves first-appearance order")
{
    const std::vector<ShortcutBinding> bindings{
        Bind("menu.z", "⌘k"),
        Bind("menu.a", "⌘j"),
        Bind("menu.y", "⌘k"),
        Bind("menu.b", "⌘j"),
        Bind("menu.x", "⌘k"),
    };

    const auto conflicts = DetectShortcutConflicts(bindings);
    REQUIRE(conflicts.size() == 2);
    // ⌘k appeared first, so its group is reported first, with all three of its actions.
    CHECK(conflicts[0].shortcut == "⌘k");
    CHECK(conflicts[0].actions == std::vector<std::string>{"menu.z", "menu.y", "menu.x"});
    CHECK(conflicts[1].shortcut == "⌘j");
    CHECK(conflicts[1].actions == std::vector<std::string>{"menu.a", "menu.b"});

    // Repeated runs over the same input must be identical, not merely equivalent.
    CHECK(DetectShortcutConflicts(bindings) == conflicts);
}

TEST_CASE(PREFIX "treats a dotless action name as its own domain")
{
    const std::vector<ShortcutBinding> bindings{Bind("standalone", "⌘c"), Bind("menu.file.copy", "⌘c")};
    CHECK(DetectShortcutConflicts(bindings).empty());

    const std::vector<ShortcutBinding> same_domain{Bind("standalone", "⌘c"), Bind("standalone2", "⌘c")};
    CHECK(DetectShortcutConflicts(same_domain).empty()); // different dotless names => different domains
}

TEST_CASE(PREFIX "reports nothing for an empty binding set")
{
    CHECK(DetectShortcutConflicts({}).empty());
}

#undef PREFIX
#define PREFIX "nc::core::AllShortcutProfiles "

TEST_CASE(PREFIX "offers Default first and every profile exactly once")
{
    const auto profiles = AllShortcutProfiles();
    REQUIRE(profiles.size() == 4);
    CHECK(profiles[0].kind == ShortcutProfileKind::Default);
    CHECK(profiles[0].id == "default");
    // Default carries no overrides: selecting it is how a user returns to the app's own bindings.
    CHECK(profiles[0].bindings.empty());

    std::vector<std::string> ids;
    for( const ShortcutProfile &profile : profiles )
        ids.push_back(profile.id);
    CHECK(ids == std::vector<std::string>{"default", "macos", "windows", "commander"});
}

TEST_CASE(PREFIX "ships no profile that conflicts with itself")
{
    // This is the detector earning its keep on our own data: the first draft of the Commander
    // profile bound F5 to both copy and rename, which this case would have caught.
    for( const ShortcutProfile &profile : AllShortcutProfiles() ) {
        const auto conflicts = DetectShortcutConflicts(profile.bindings);
        INFO("profile: " << profile.id);
        CHECK(conflicts.empty());
    }
}

TEST_CASE(PREFIX "ships no profile that binds an action twice")
{
    for( const ShortcutProfile &profile : AllShortcutProfiles() ) {
        std::vector<std::string> actions;
        for( const ShortcutBinding &binding : profile.bindings )
            actions.push_back(binding.action);
        const size_t before = actions.size();
        std::ranges::sort(actions);
        const auto duplicates = std::ranges::unique(actions);
        actions.erase(duplicates.begin(), duplicates.end());
        INFO("profile: " << profile.id);
        CHECK(actions.size() == before);
    }
}

TEST_CASE(PREFIX "resolves a persisted id and rejects an unknown one")
{
    const auto profiles = AllShortcutProfiles();
    const auto windows = FindShortcutProfile(profiles, "windows");
    REQUIRE(windows != profiles.end());
    CHECK(windows->kind == ShortcutProfileKind::WindowsExplorer);
    CHECK_FALSE(windows->bindings.empty());

    CHECK(FindShortcutProfile(profiles, "nonexistent") == profiles.end());
    CHECK(FindShortcutProfile(profiles, "") == profiles.end());
}
