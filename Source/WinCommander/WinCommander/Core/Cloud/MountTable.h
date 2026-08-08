// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "NetworkVolumeState.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nc::core {

/** One mounted filesystem, as the mount table describes it - without touching the filesystem. */
struct MountedVolume {
    std::string mount_point;
    /** As reported by the kernel: `apfs`, `smbfs`, `nfs`, `afpfs`, and so on. */
    std::string filesystem_type;
    /**
     * The kernel says this volume is not local.
     *
     * Taken from the mount flag rather than matched against a list of network filesystem names. A
     * name list goes stale the moment a new network filesystem appears, and the failure is silent
     * and in the worst direction: an unrecognised network volume would be treated as local and then
     * probed on the drawing thread, which is precisely the beachball this whole model exists to
     * prevent.
     */
    bool is_network = false;
    bool read_only = false;

    friend bool operator==(const MountedVolume &, const MountedVolume &) = default;
};

/**
 * Reads the mount table.
 *
 * Asks the kernel **not** to refresh each filesystem's statistics. Requesting fresh statistics makes
 * the call wait on every mounted filesystem in turn, and a network mount whose server has gone away
 * will not answer - so the very call meant to discover unresponsive volumes would hang on one.
 */
[[nodiscard]] std::vector<MountedVolume> ReadMountTable();

/**
 * The volume a path lives on: the longest mount point that contains it.
 *
 * Containment is by path component, never by string prefix. `/Volumes/data` does not contain
 * `/Volumes/database`, and treating it as though it did would attribute one volume's state to
 * another - reporting a live local disk as an unresponsive network mount, or the reverse.
 */
[[nodiscard]] std::optional<MountedVolume> VolumeForPath(const std::filesystem::path &_path,
                                                          std::span<const MountedVolume> _volumes);

/** Extra knowledge the mount table cannot supply, because answering it means touching the volume. */
struct NetworkVolumeProbeResult {
    bool answered = true;
    bool export_rejected = false;

    friend bool operator==(const NetworkVolumeProbeResult &, const NetworkVolumeProbeResult &) = default;
};

/**
 * What is known about the volume under a path, from the mount table plus an earlier probe. Nothing
 * when the path is on no known volume.
 *
 * The probe is a parameter rather than something taken here on purpose: finding out whether a server
 * still answers means touching it, and the entire point of `NetworkVolumeState` is to know the state
 * *before* the access rather than by making it.
 *
 * **The absence is reported rather than invented**, because neither available answer is honest for a
 * path we cannot place. Calling it local would invite a synchronous touch of exactly the path we
 * could not account for - the beachball this model exists to prevent. Calling it unmounted would be
 * worse in a different direction: if the mount table could not be read at all, every path would
 * become unmounted and every operation would be refused up front. The caller knows which of those
 * risks applies to it; this does not.
 */
[[nodiscard]] std::optional<NetworkVolumeFacts> NetworkVolumeFactsForPath(const std::filesystem::path &_path,
                                                                           std::span<const MountedVolume> _volumes,
                                                                           const NetworkVolumeProbeResult &_probe = {});

} // namespace nc::core
