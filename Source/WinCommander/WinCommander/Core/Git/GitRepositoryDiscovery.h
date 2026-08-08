// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace nc::core {

/** How discovery looks at a candidate directory. Injected so the walk is testable without a disk. */
struct GitDiscoveryProbe {
    /**
     * Whether `<directory>/.git` exists, as a directory **or** a file.
     *
     * A linked worktree and a submodule both have a `.git` *file* pointing elsewhere. A probe that
     * only accepted a directory would report those as not repositories at all.
     */
    std::function<bool(const std::filesystem::path &_directory)> has_git_entry;
    /** The device the directory lives on, or nothing when it cannot be read. */
    std::function<std::optional<uint64_t>(const std::filesystem::path &_directory)> device_id;
};

/** Probes the real filesystem. */
[[nodiscard]] GitDiscoveryProbe NativeGitDiscoveryProbe();

/**
 * Walks up from a directory to the root of the repository containing it.
 *
 * Three refusals, each for a reason:
 *
 * - **A relative path is refused, not resolved.** The caller's "here" is a panel's directory, and
 *   resolving it against the process working directory would answer confidently about a completely
 *   different place.
 * - **The walk stops at a filesystem boundary**, which is what git itself does by default. A volume
 *   mounted inside a checkout is not part of that checkout, and letting it inherit the repository
 *   would badge files git knows nothing about.
 * - **A directory whose device cannot be read ends the walk.** Continuing would mean crossing a
 *   boundary we failed to check for, which is the thing the boundary rule exists to prevent.
 */
[[nodiscard]] std::optional<std::filesystem::path> FindGitRepositoryRoot(const std::filesystem::path &_directory,
                                                                         const GitDiscoveryProbe &_probe);

/** Same, against the real filesystem. */
[[nodiscard]] std::optional<std::filesystem::path> FindGitRepositoryRoot(const std::filesystem::path &_directory);

} // namespace nc::core
