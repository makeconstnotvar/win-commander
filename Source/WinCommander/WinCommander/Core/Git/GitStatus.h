// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nc::core {

/** Per-path badge state (spec §11 vocabulary, plus the states a badge must not omit). */
enum class GitFileState : uint8_t {
    /** Tracked and identical to HEAD. Carries no badge - see ShouldBadgeGitFileState. */
    Unmodified,
    Modified,
    Added,
    Deleted,
    Renamed,
    /** Not tracked and not ignored. */
    Untracked,
    /** Ignored by a rule; a badge would be noise on potentially thousands of paths. */
    Ignored,
    /** An unresolved merge. Nothing outranks it. */
    Conflicted
};

struct GitStatusEntry {
    std::string path;
    GitFileState state = GitFileState::Unmodified;
    /** For a rename, where it came from. */
    std::optional<std::string> original_path;

    friend bool operator==(const GitStatusEntry &, const GitStatusEntry &) = default;
};

/**
 * Parses `git status --porcelain=v1 -z` output.
 *
 * NUL-separated rather than line-based on purpose: a newline is a legal character in a path, and a
 * line-based parser would split one such path into two entries and mis-badge whatever the second
 * half collided with. The `-z` form also stops git from quoting and escaping unusual paths, so what
 * arrives is the exact bytes on disk.
 *
 * Returns nothing for input that is not well-formed porcelain-v1, rather than salvaging a prefix:
 * a partial status is indistinguishable from a clean tree for the paths it omits, which would
 * badge a modified file as unmodified.
 */
[[nodiscard]] std::optional<std::vector<GitStatusEntry>> ParseGitPorcelainV1(std::string_view _output);

/** Maps one porcelain XY pair to a state. */
[[nodiscard]] GitFileState ClassifyGitStatusCode(char _index, char _worktree) noexcept;

/**
 * Whether the state earns a badge.
 *
 * `Unmodified` and `Ignored` get none, for the same reason a synced cloud file does not: in a
 * repository most files are unmodified, and ignored paths can number in the thousands. Badging
 * either decorates rows that carry no news and buries the ones that do.
 */
[[nodiscard]] constexpr bool ShouldBadgeGitFileState(const GitFileState _state) noexcept
{
    return _state != GitFileState::Unmodified && _state != GitFileState::Ignored;
}

} // namespace nc::core
