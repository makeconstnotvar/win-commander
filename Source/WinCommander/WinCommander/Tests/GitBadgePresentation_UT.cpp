// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Git/GitBadgePresentation.h>

#include <set>
#include <string_view>
#include <vector>

namespace {

using nc::core::GitFileState;
using nc::core::PresentGitBadge;
using nc::core::ShouldBadgeGitFileState;

const std::vector<GitFileState> &AllStates()
{
    [[clang::no_destroy]] static const std::vector<GitFileState> states{
        GitFileState::Unmodified, GitFileState::Modified,  GitFileState::Added,      GitFileState::Deleted,
        GitFileState::Renamed,    GitFileState::Untracked, GitFileState::Ignored,    GitFileState::Conflicted};
    return states;
}

} // namespace

#define PREFIX "nc::core::PresentGitBadge "

TEST_CASE(PREFIX "draws exactly the states the badge rule admits")
{
    // Deciding differently here would let a drawing site decorate rows the rule says carry no news -
    // and the two would disagree about which rows are decorated, with only one of them right.
    for( const GitFileState state : AllStates() )
        CHECK(PresentGitBadge(state).has_value() == ShouldBadgeGitFileState(state));
}

TEST_CASE(PREFIX "gives every badged state its own symbol")
{
    // Two states sharing one would make the badge say less than the status it came from, which is
    // the only thing it is for.
    std::set<std::string_view> symbols;
    size_t badged = 0;
    for( const GitFileState state : AllStates() ) {
        const auto badge = PresentGitBadge(state);
        if( !badge )
            continue;
        ++badged;
        CHECK_FALSE(badge->symbol.empty());
        symbols.insert(badge->symbol);
    }
    CHECK(badged == 6);
    CHECK(symbols.size() == badged);
}

TEST_CASE(PREFIX "emphasises only the state where doing nothing loses work")
{
    // If everything is emphasised, nothing is.
    size_t warnings = 0;
    for( const GitFileState state : AllStates() )
        if( const auto badge = PresentGitBadge(state); badge && badge->is_warning )
            ++warnings;
    CHECK(warnings == 1);

    const auto conflicted = PresentGitBadge(GitFileState::Conflicted);
    REQUIRE(conflicted);
    CHECK(conflicted->is_warning);
    const auto modified = PresentGitBadge(GitFileState::Modified);
    REQUIRE(modified);
    CHECK_FALSE(modified->is_warning);
}

TEST_CASE(PREFIX "says out loud what a coloured dot cannot")
{
    // A badge that is only a mark conveys nothing to someone who cannot see it, which would make git
    // status a sighted-only feature.
    std::set<std::string_view> phrases;
    for( const GitFileState state : AllStates() ) {
        const auto badge = PresentGitBadge(state);
        if( !badge )
            continue;
        CHECK_FALSE(badge->accessibility_phrase.empty());
        phrases.insert(badge->accessibility_phrase);
    }
    // Distinct as well as present: two states read aloud the same way are two states a screen-reader
    // user cannot tell apart.
    CHECK(phrases.size() == 6);
}

#undef PREFIX
