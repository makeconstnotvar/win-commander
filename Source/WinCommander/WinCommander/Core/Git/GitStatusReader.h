// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "GitStatus.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <vector>

namespace nc::core {

enum class GitStatusReadError : uint8_t {
    /** The directory is not inside a repository. */
    NotARepository,
    /** git could not be started at all. */
    LaunchFailed,
    /** git was still running when the budget ran out and was stopped. */
    TimedOut,
    /** git produced more than the budget allows and was stopped. */
    OutputTooLarge,
    /** git ran and refused. */
    GitFailed,
    /** git succeeded but its output is not well-formed porcelain v1. */
    Unparsable
};

/**
 * What a single status refresh may spend.
 *
 * Both bounds exist because this runs to draw badges. A repository can be arbitrarily large, on an
 * arbitrarily slow disk, and a refresh that runs without limit turns one unlucky folder into a
 * permanently stuck listing.
 */
struct GitStatusReadLimits {
    std::chrono::milliseconds timeout{5'000};
    /** Roughly 100k paths at typical lengths. */
    size_t maximum_output_bytes = 8u * 1024u * 1024u;

    friend bool operator==(const GitStatusReadLimits &, const GitStatusReadLimits &) = default;
};

struct GitStatusSnapshot {
    std::filesystem::path root;
    std::vector<GitStatusEntry> entries;

    friend bool operator==(const GitStatusSnapshot &, const GitStatusSnapshot &) = default;
};

/**
 * Finds the repository containing a directory and reads its status.
 *
 * Runs `git` directly with an explicit argument vector - never through a shell, so a repository path
 * containing spaces, quotes or newlines cannot be re-read as syntax.
 *
 * The child's environment has every `GIT_*` variable removed. A stray `GIT_DIR` or `GIT_WORK_TREE`
 * inherited from whoever launched the application would silently point git at a different
 * repository, and the badges would then describe someone else's working tree.
 */
[[nodiscard]] std::expected<GitStatusSnapshot, GitStatusReadError>
ReadGitStatus(const std::filesystem::path &_directory, const GitStatusReadLimits &_limits = {});

} // namespace nc::core
