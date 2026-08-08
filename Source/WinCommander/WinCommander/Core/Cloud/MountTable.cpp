// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "MountTable.h"

#include <sys/mount.h>
#include <sys/param.h>

#include <algorithm>

namespace nc::core {

std::vector<MountedVolume> ReadMountTable()
{
    struct statfs *mounts = nullptr;
    // MNT_NOWAIT, never MNT_WAIT: asking for fresh statistics makes this wait on every mounted
    // filesystem in turn, and a network mount whose server has gone away will not answer. The call
    // meant to discover unresponsive volumes would then hang on one.
    const int count = ::getmntinfo(&mounts, MNT_NOWAIT);
    if( count <= 0 || mounts == nullptr )
        return {};

    std::vector<MountedVolume> volumes;
    volumes.reserve(static_cast<size_t>(count));
    for( int i = 0; i < count; ++i ) {
        volumes.push_back(MountedVolume{
            .mount_point = mounts[i].f_mntonname,
            .filesystem_type = mounts[i].f_fstypename,
            .is_network = (mounts[i].f_flags & MNT_LOCAL) == 0,
            .read_only = (mounts[i].f_flags & MNT_RDONLY) != 0,
        });
    }
    return volumes;
}

namespace {

/** True when `_mount_point` contains `_path`, matching whole components rather than characters. */
bool Contains(const std::filesystem::path &_mount_point, const std::filesystem::path &_path)
{
    auto mount_it = _mount_point.begin();
    auto path_it = _path.begin();
    for( ; mount_it != _mount_point.end(); ++mount_it, ++path_it ) {
        if( path_it == _path.end() || *mount_it != *path_it )
            return false;
    }
    return true;
}

size_t ComponentCount(const std::filesystem::path &_path)
{
    return static_cast<size_t>(std::distance(_path.begin(), _path.end()));
}

} // namespace

std::optional<MountedVolume> VolumeForPath(const std::filesystem::path &_path,
                                           const std::span<const MountedVolume> _volumes)
{
    if( _path.empty() || !_path.is_absolute() )
        return std::nullopt;
    const std::filesystem::path path = _path.lexically_normal();

    std::optional<MountedVolume> best;
    size_t best_depth = 0;
    for( const MountedVolume &volume : _volumes ) {
        const std::filesystem::path mount_point = std::filesystem::path{volume.mount_point}.lexically_normal();
        if( mount_point.empty() || !Contains(mount_point, path) )
            continue;
        // Longest wins: a volume mounted inside another is the one a path under it belongs to.
        const size_t depth = ComponentCount(mount_point);
        if( !best || depth > best_depth ) {
            best = volume;
            best_depth = depth;
        }
    }
    return best;
}

std::optional<NetworkVolumeFacts> NetworkVolumeFactsForPath(const std::filesystem::path &_path,
                                                            const std::span<const MountedVolume> _volumes,
                                                            const NetworkVolumeProbeResult &_probe)
{
    const std::optional<MountedVolume> volume = VolumeForPath(_path, _volumes);
    // Reported, not invented. Local would invite a synchronous touch of a path we could not account
    // for; unmounted would refuse every operation whenever the mount table could not be read.
    if( !volume )
        return std::nullopt;

    return NetworkVolumeFacts{
        .is_network_mount = volume->is_network,
        .is_mounted = true,
        // A local volume is answering by construction; carrying a stale probe result into it could
        // report a working disk as unresponsive.
        .last_probe_answered = volume->is_network ? _probe.answered : true,
        .export_rejected = volume->is_network && _probe.export_rejected,
    };
}

} // namespace nc::core
