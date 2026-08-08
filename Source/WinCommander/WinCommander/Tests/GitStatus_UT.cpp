// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Git/GitStatus.h>

#include <string>
#include <vector>

namespace {

using nc::core::ClassifyGitStatusCode;
using nc::core::GitFileState;
using nc::core::GitStatusEntry;
using nc::core::ParseGitPorcelainV1;
using nc::core::ShouldBadgeGitFileState;

/** Builds NUL-separated porcelain output from records. */
std::string Porcelain(const std::vector<std::string> &_records)
{
    std::string output;
    for( const auto &record : _records ) {
        output += record;
        output.push_back('\0');
    }
    return output;
}

} // namespace

#define PREFIX "nc::core::GitStatus "

TEST_CASE(PREFIX "lets an unresolved merge outrank every other reading of the same pair")
{
    // AA and DD would otherwise look like an ordinary add or delete, and a conflict is the one
    // state where doing nothing is wrong.
    CHECK(ClassifyGitStatusCode('A', 'A') == GitFileState::Conflicted);
    CHECK(ClassifyGitStatusCode('D', 'D') == GitFileState::Conflicted);
    CHECK(ClassifyGitStatusCode('U', 'U') == GitFileState::Conflicted);
    CHECK(ClassifyGitStatusCode('U', 'D') == GitFileState::Conflicted);
    CHECK(ClassifyGitStatusCode('A', 'U') == GitFileState::Conflicted);
}

TEST_CASE(PREFIX "lets the worktree column decide when the two disagree")
{
    // The worktree is what the row on screen actually shows, so a file staged as added but since
    // edited reads as Modified rather than Added.
    CHECK(ClassifyGitStatusCode('A', 'M') == GitFileState::Modified);
    CHECK(ClassifyGitStatusCode('M', 'D') == GitFileState::Deleted);
    CHECK(ClassifyGitStatusCode('R', 'M') == GitFileState::Modified);

    // With a clean worktree the index decides.
    CHECK(ClassifyGitStatusCode('M', ' ') == GitFileState::Modified);
    CHECK(ClassifyGitStatusCode('A', ' ') == GitFileState::Added);
    CHECK(ClassifyGitStatusCode('D', ' ') == GitFileState::Deleted);
    CHECK(ClassifyGitStatusCode('R', ' ') == GitFileState::Renamed);
    CHECK(ClassifyGitStatusCode('C', ' ') == GitFileState::Added);
    CHECK(ClassifyGitStatusCode(' ', ' ') == GitFileState::Unmodified);
}

TEST_CASE(PREFIX "recognises untracked and ignored")
{
    CHECK(ClassifyGitStatusCode('?', '?') == GitFileState::Untracked);
    CHECK(ClassifyGitStatusCode('!', '!') == GitFileState::Ignored);
}

TEST_CASE(PREFIX "parses records and keeps a rename's original path")
{
    const std::string output =
        Porcelain({" M src/main.cpp", "A  docs/new.md", "?? scratch.txt", "R  dst.txt", "src.txt", "!! build/out.o"});

    const auto entries = ParseGitPorcelainV1(output);
    REQUIRE(entries);
    REQUIRE(entries->size() == 5);

    CHECK((*entries)[0] == GitStatusEntry{.path = "src/main.cpp", .state = GitFileState::Modified});
    CHECK((*entries)[1] == GitStatusEntry{.path = "docs/new.md", .state = GitFileState::Added});
    CHECK((*entries)[2] == GitStatusEntry{.path = "scratch.txt", .state = GitFileState::Untracked});
    CHECK((*entries)[3].state == GitFileState::Renamed);
    CHECK((*entries)[3].path == "dst.txt");
    CHECK((*entries)[3].original_path == std::optional<std::string>{"src.txt"});
    CHECK((*entries)[4].state == GitFileState::Ignored);
}

TEST_CASE(PREFIX "keeps a path containing a newline in one piece")
{
    // A newline is legal in a path. A line-based parser would split this into two entries and
    // mis-badge whatever the second half collided with - which is why the -z form is parsed.
    const std::string output = Porcelain({" M weird\nname.txt", "?? plain.txt"});

    const auto entries = ParseGitPorcelainV1(output);
    REQUIRE(entries);
    REQUIRE(entries->size() == 2);
    CHECK((*entries)[0].path == "weird\nname.txt");
    CHECK((*entries)[0].state == GitFileState::Modified);
    CHECK((*entries)[1].path == "plain.txt");
}

TEST_CASE(PREFIX "refuses malformed output instead of salvaging a prefix")
{
    // A partial status is indistinguishable from a clean tree for the paths it omits, which would
    // badge a modified file as unmodified - a silent wrong answer rather than a visible failure.
    CHECK_FALSE(ParseGitPorcelainV1(std::string{" M unterminated.txt"}));   // no NUL
    CHECK_FALSE(ParseGitPorcelainV1(Porcelain({"XY"})));                    // no path
    CHECK_FALSE(ParseGitPorcelainV1(Porcelain({" Mnospace.txt"})));         // missing separator
    CHECK_FALSE(ParseGitPorcelainV1(Porcelain({"R  dst.txt"})));            // rename with no origin

    // Empty output is a clean tree, not a malformed one.
    const auto clean = ParseGitPorcelainV1(std::string_view{});
    REQUIRE(clean);
    CHECK(clean->empty());
}

TEST_CASE(PREFIX "badges only what carries news")
{
    // In a repository most files are unmodified and ignored paths can number in the thousands.
    // Badging either decorates rows that say nothing and buries the ones that do.
    CHECK_FALSE(ShouldBadgeGitFileState(GitFileState::Unmodified));
    CHECK_FALSE(ShouldBadgeGitFileState(GitFileState::Ignored));

    CHECK(ShouldBadgeGitFileState(GitFileState::Modified));
    CHECK(ShouldBadgeGitFileState(GitFileState::Added));
    CHECK(ShouldBadgeGitFileState(GitFileState::Deleted));
    CHECK(ShouldBadgeGitFileState(GitFileState::Renamed));
    CHECK(ShouldBadgeGitFileState(GitFileState::Untracked));
    CHECK(ShouldBadgeGitFileState(GitFileState::Conflicted));
}
