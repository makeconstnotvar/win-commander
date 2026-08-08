// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Git/GitDirectoryBadge.h>

#include <string>
#include <vector>

namespace {

using nc::core::AggregateGitDirectoryBadge;
using nc::core::BadgeGitDirectoryChildren;
using nc::core::GitBadgeAssignment;
using nc::core::GitFileState;
using nc::core::GitStatusEntry;

GitStatusEntry Entry(const std::string &_path, const GitFileState _state)
{
    return GitStatusEntry{.path = _path, .state = _state, .original_path = std::nullopt};
}

} // namespace

#define PREFIX "nc::core::AggregateGitDirectoryBadge "

TEST_CASE(PREFIX "lets an unresolved merge outrank everything below it")
{
    // The one state where doing nothing is wrong. Burying it under a milder summary is how it gets
    // missed.
    CHECK(AggregateGitDirectoryBadge(std::vector{GitFileState::Modified, GitFileState::Conflicted}) ==
          GitFileState::Conflicted);
    CHECK(AggregateGitDirectoryBadge(std::vector{GitFileState::Conflicted, GitFileState::Untracked}) ==
          GitFileState::Conflicted);
    CHECK(AggregateGitDirectoryBadge(std::vector{GitFileState::Ignored, GitFileState::Conflicted}) ==
          GitFileState::Conflicted);
}

TEST_CASE(PREFIX "summarises everything else as modified, whatever it was")
{
    // A directory holding untracked files is not itself untracked - git may well be tracking its
    // other contents - and reporting it so would invite "add this whole folder" on something already
    // half-tracked. Added and Deleted are false for the same reason.
    for( const auto state : {GitFileState::Modified,
                             GitFileState::Added,
                             GitFileState::Deleted,
                             GitFileState::Renamed,
                             GitFileState::Untracked} ) {
        CHECK(AggregateGitDirectoryBadge(std::vector{state}) == GitFileState::Modified);
    }
}

TEST_CASE(PREFIX "gives nothing to a directory with nothing to say")
{
    // In a repository most directories are unchanged, and badging them all buries the ones that are
    // not. Ignored paths can number in the thousands, and their contents are ignored too.
    CHECK(AggregateGitDirectoryBadge({}) == std::nullopt);
    CHECK(AggregateGitDirectoryBadge(std::vector{GitFileState::Unmodified}) == std::nullopt);
    CHECK(AggregateGitDirectoryBadge(std::vector{GitFileState::Ignored, GitFileState::Ignored}) == std::nullopt);
    CHECK(AggregateGitDirectoryBadge(std::vector{GitFileState::Unmodified, GitFileState::Ignored}) == std::nullopt);
}

#undef PREFIX
#define PREFIX "nc::core::BadgeGitDirectoryChildren "

TEST_CASE(PREFIX "badges a folder for what is inside it, which git never says itself")
{
    // git status reports files, not directories - so without this a folder full of modified files
    // looks exactly like an untouched one, and the only way to find a change is to open every folder.
    const std::vector<GitStatusEntry> status{
        Entry("src/deep/a.txt", GitFileState::Modified),
        Entry("README.md", GitFileState::Untracked),
    };

    const auto root = BadgeGitDirectoryChildren("", status);
    REQUIRE(root.size() == 2);
    CHECK(root[0] == GitBadgeAssignment{.path = "src", .state = GitFileState::Modified});
    CHECK(root[1] == GitBadgeAssignment{.path = "README.md", .state = GitFileState::Untracked});

    // And one level down, the same folder is summarised again from what it holds.
    const auto inside = BadgeGitDirectoryChildren("src", status);
    REQUIRE(inside.size() == 1);
    CHECK(inside[0] == GitBadgeAssignment{.path = "deep", .state = GitFileState::Modified});

    // At the bottom, the file's own state is what shows.
    const auto bottom = BadgeGitDirectoryChildren("src/deep", status);
    REQUIRE(bottom.size() == 1);
    CHECK(bottom[0] == GitBadgeAssignment{.path = "a.txt", .state = GitFileState::Modified});
}

TEST_CASE(PREFIX "does not let one folder collect another's changes")
{
    // `src` must not gather what belongs to `src-vendor`, which would badge a folder for changes it
    // does not contain - the same component-versus-prefix rule the mount table and the tool launcher
    // both need.
    const std::vector<GitStatusEntry> status{
        Entry("src-vendor/lib.txt", GitFileState::Modified),
    };

    CHECK(BadgeGitDirectoryChildren("src", status).empty());

    const auto root = BadgeGitDirectoryChildren("", status);
    REQUIRE(root.size() == 1);
    CHECK(root[0].path == "src-vendor");
}

TEST_CASE(PREFIX "leaves unmodified and ignored children unbadged")
{
    const std::vector<GitStatusEntry> status{
        Entry("kept.txt", GitFileState::Unmodified),
        Entry("build/output.o", GitFileState::Ignored),
        Entry("noise.log", GitFileState::Ignored),
        Entry("real.txt", GitFileState::Modified),
    };

    const auto root = BadgeGitDirectoryChildren("", status);
    REQUIRE(root.size() == 1);
    CHECK(root[0] == GitBadgeAssignment{.path = "real.txt", .state = GitFileState::Modified});
}

TEST_CASE(PREFIX "carries a conflict up through every folder above it")
{
    const std::vector<GitStatusEntry> status{
        Entry("a/b/merge.txt", GitFileState::Conflicted),
        Entry("a/other.txt", GitFileState::Modified),
    };

    const auto root = BadgeGitDirectoryChildren("", status);
    REQUIRE(root.size() == 1);
    CHECK(root[0] == GitBadgeAssignment{.path = "a", .state = GitFileState::Conflicted});

    const auto inside = BadgeGitDirectoryChildren("a", status);
    REQUIRE(inside.size() == 2);
    CHECK(inside[0] == GitBadgeAssignment{.path = "b", .state = GitFileState::Conflicted});
    CHECK(inside[1] == GitBadgeAssignment{.path = "other.txt", .state = GitFileState::Modified});
}

TEST_CASE(PREFIX "answers an empty status and an unrelated directory with nothing")
{
    CHECK(BadgeGitDirectoryChildren("", {}).empty());
    CHECK(BadgeGitDirectoryChildren("elsewhere", std::vector{Entry("src/a.txt", GitFileState::Modified)}).empty());
}

#undef PREFIX
