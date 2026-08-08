// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "GitStatus.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nc::core {

/** A path with its state, as a listing row needs them. */
struct GitBadgeAssignment {
    std::string path;
    GitFileState state = GitFileState::Unmodified;

    friend bool operator==(const GitBadgeAssignment &, const GitBadgeAssignment &) = default;
};

/**
 * The badge a directory earns from what lies beneath it.
 *
 * `git status` reports files, not directories - so without this a folder full of modified files
 * looks exactly like an untouched one, and the only way to find a change is to open every folder.
 *
 * The precedence, and what each rule is for:
 *
 * - **`Conflicted` outranks everything.** An unresolved merge is the one state where doing nothing
 *   is wrong, and burying it under a milder summary is how it gets missed.
 * - **`Untracked` does not propagate as itself.** A directory holding untracked files is not itself
 *   untracked - git may well be tracking the directory's other contents - so it reports `Modified`.
 *   Reporting `Untracked` would invite "add this whole folder" on something already half-tracked.
 * - **`Deleted` and `Added` likewise summarise as `Modified`.** A directory whose contents changed
 *   in any of those ways has changed; claiming the directory itself was added or deleted would be
 *   false.
 * - **`Ignored` contributes nothing.** Ignored paths can number in the thousands and their contents
 *   are ignored too, so a directory does not become interesting for containing them. This is the
 *   same rule `ShouldBadgeGitFileState` applies to the paths themselves.
 * - **A directory with nothing to say gets no badge**, rather than an `Unmodified` one. In a
 *   repository most directories are unchanged, and badging them all buries the ones that are not.
 */
[[nodiscard]] std::optional<GitFileState> AggregateGitDirectoryBadge(std::span<const GitFileState> _below);

/**
 * Badges for one directory's immediate children, given the repository's whole status.
 *
 * `_directory` is relative to the repository root, empty for the root itself. A child directory is
 * given the badge its own subtree earns; a child file the state git reported for it.
 *
 * Paths outside `_directory` are ignored rather than matched loosely: `src` must not collect what
 * belongs to `src-vendor`, which would badge a folder for changes it does not contain.
 */
[[nodiscard]] std::vector<GitBadgeAssignment>
BadgeGitDirectoryChildren(std::string_view _directory, std::span<const GitStatusEntry> _status);

} // namespace nc::core
