// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "GitRepositoryDiscovery.h"

#include <sys/stat.h>

namespace nc::core {

GitDiscoveryProbe NativeGitDiscoveryProbe()
{
    return GitDiscoveryProbe{
        .has_git_entry =
            [](const std::filesystem::path &_directory) {
                struct stat entry{};
                // Not `std::filesystem::exists`: a `.git` that is a dangling symlink is not a usable
                // repository marker, and lstat lets that be told apart from a real entry.
                return ::lstat((_directory / ".git").c_str(), &entry) == 0 &&
                       (S_ISDIR(entry.st_mode) || S_ISREG(entry.st_mode));
            },
        .device_id =
            [](const std::filesystem::path &_directory) -> std::optional<uint64_t> {
                struct stat entry{};
                if( ::stat(_directory.c_str(), &entry) != 0 )
                    return std::nullopt;
                return static_cast<uint64_t>(entry.st_dev);
            },
    };
}

std::optional<std::filesystem::path> FindGitRepositoryRoot(const std::filesystem::path &_directory,
                                                           const GitDiscoveryProbe &_probe)
{
    if( !_probe.has_git_entry || !_probe.device_id )
        return std::nullopt;
    // Refused rather than resolved: resolving against the process working directory would answer
    // about somewhere the caller never asked about.
    if( _directory.empty() || !_directory.is_absolute() )
        return std::nullopt;

    const std::optional<uint64_t> origin_device = _probe.device_id(_directory);
    if( !origin_device )
        return std::nullopt;

    std::filesystem::path current = _directory.lexically_normal();
    while( true ) {
        if( _probe.has_git_entry(current) )
            return current;

        const std::filesystem::path parent = current.parent_path();
        // `/`'s parent is itself, which is how the walk ends rather than by counting.
        if( parent.empty() || parent == current )
            return std::nullopt;

        const std::optional<uint64_t> parent_device = _probe.device_id(parent);
        // An unreadable parent ends the walk. Continuing would mean crossing a boundary we failed to
        // check for, which is exactly what the boundary rule exists to prevent.
        if( !parent_device || *parent_device != *origin_device )
            return std::nullopt;

        current = parent;
    }
}

std::optional<std::filesystem::path> FindGitRepositoryRoot(const std::filesystem::path &_directory)
{
    return FindGitRepositoryRoot(_directory, NativeGitDiscoveryProbe());
}

} // namespace nc::core
