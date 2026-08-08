// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "FolderComparison.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace nc::core {

/** One compared name, carrying where in the tree it sits. */
struct RecursiveFolderCompareEntry {
    /** Path relative to the two roots, with `/` separators and no leading slash. */
    std::string relative_path;
    FolderCompareStatus status = FolderCompareStatus::Same;
    FolderCompareNewerSide newer_side = FolderCompareNewerSide::Neither;
    bool is_directory = false;
    /** How deep below the roots this sits; the roots' own children are at 0. */
    size_t depth = 0;

    friend bool operator==(const RecursiveFolderCompareEntry &, const RecursiveFolderCompareEntry &) = default;
};

enum class RecursiveFolderCompareFailure : uint8_t {
    /** A listing was rejected by the level comparison - see `FolderCompareFailure`. */
    InvalidListing,
    /** A directory could not be listed, so nothing can be claimed about what is inside it. */
    Unreadable,
    /** The walk hit its depth budget, which means the answer would have been incomplete. */
    TooDeep,
    /** The walk hit its entry budget. */
    TooLarge,
    Cancelled
};

struct RecursiveFolderCompareLimits {
    /** Levels below the roots. Exists so a symlink loop or a pathological tree cannot run forever. */
    size_t maximum_depth = 64;
    /** Total entries reported, across the whole tree. */
    size_t maximum_entries = 200'000;

    friend bool operator==(const RecursiveFolderCompareLimits &, const RecursiveFolderCompareLimits &) = default;
};

/**
 * Lists one side's directory. Nothing when it cannot be read.
 *
 * Injected rather than taken here, because "read this directory" belongs to whatever holds the VFS
 * host, and because a comparison of two trees must be testable without either of them existing.
 */
using RecursiveFolderCompareLister = std::function<std::optional<std::vector<FolderCompareItem>>(
    const std::string &_relative_path)>;

struct RecursiveFolderComparison {
    /** Depth-first, parents before their children, in the order the level comparison produced. */
    std::vector<RecursiveFolderCompareEntry> entries;

    [[nodiscard]] FolderCompareSummary Summarize() const noexcept;
    /** True when nothing anywhere in either tree differs. */
    [[nodiscard]] bool Identical() const noexcept;
};

using RecursiveFolderComparisonResult = std::expected<RecursiveFolderComparison, RecursiveFolderCompareFailure>;

/**
 * Compares two directory trees, level by level, from their roots down.
 *
 * `CompareFolders` compares one level and reports a directory present on both sides as `Same`,
 * claiming nothing about its contents - which a consumer must not read as "nothing to do". This
 * closes that: a directory pair is descended into, and its own status becomes what its subtree says.
 *
 * Decisions worth naming, all of them about refusing rather than guessing:
 *
 * - **A directory pair that differs anywhere below reports `Changed`, not `Same`.** That is the
 *   whole point: a sync reading `Same` on a directory would skip a subtree that is not identical.
 * - **An unreadable directory fails the whole comparison** rather than being reported as empty. An
 *   empty listing and an unreadable one look identical in the result, and the difference is
 *   everything: one means "nothing inside", the other means "we do not know", and a sync acting on
 *   the first would delete a subtree it never saw.
 * - **Only a directory present on *both* sides is descended into.** A `LeftOnly` directory is
 *   reported once, whole. Enumerating what is inside something the other side does not have at all
 *   adds nothing a sync can act on separately, and would bury the one entry that matters.
 * - **A `Conflict` is never descended into.** A directory facing a file has no common structure to
 *   walk, and pairing its children against a file's non-existent ones would be inventing a
 *   comparison.
 * - **Both budgets are failures, not truncations.** A truncated comparison is indistinguishable
 *   from a complete one that found less, which is exactly how a sync deletes what it never looked
 *   at.
 */
[[nodiscard]] RecursiveFolderComparisonResult
CompareFoldersRecursively(const RecursiveFolderCompareLister &_left,
                          const RecursiveFolderCompareLister &_right,
                          const FolderCompareOptions &_options = {},
                          const RecursiveFolderCompareLimits &_limits = {},
                          const std::function<bool()> &_is_cancelled = {});

} // namespace nc::core
