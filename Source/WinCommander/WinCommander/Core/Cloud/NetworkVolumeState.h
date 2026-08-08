// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstdint>

namespace nc::core {

/** State of a mounted network volume, as a file list needs to reason about it. */
enum class NetworkVolumeState : uint8_t {
    /** Not a network volume at all. */
    Local,
    /** Mounted and answering. */
    Responsive,
    /**
     * Mounted, but the server stopped answering. Every path under it will block until the kernel's
     * own timeout - this is the beachball case.
     */
    Unresponsive,
    /** Mounted, answering, but the export is gone or changed identity: operations fail, fast. */
    Stale,
    /** Was mounted and is not any more. */
    Unmounted
};

/** What the mount table and a cheap non-blocking probe can tell us. */
struct NetworkVolumeFacts {
    bool is_network_mount = false;
    bool is_mounted = true;
    /** A previous access returned within the responsiveness budget. */
    bool last_probe_answered = true;
    /** The server answered, but rejected the export - identity changed or share removed. */
    bool export_rejected = false;

    friend bool operator==(const NetworkVolumeFacts &, const NetworkVolumeFacts &) = default;
};

[[nodiscard]] NetworkVolumeState ClassifyNetworkVolume(const NetworkVolumeFacts &_facts) noexcept;

/**
 * Whether it is safe to touch this volume from the thread that draws.
 *
 * The rule this type exists for: an unresponsive network mount **must not be probed synchronously
 * on the main thread**. A `stat()` under a dead NFS or SMB mount does not fail - it blocks until the
 * kernel's own timeout, which is measured in tens of seconds. Doing that while drawing is exactly
 * how a file manager becomes the beachball, and it is why the state has to be known *before* the
 * access rather than discovered by making it.
 *
 * `Local` and `Responsive` are safe. Everything else must be handled off the main thread, or from
 * cached knowledge.
 */
[[nodiscard]] constexpr bool MayTouchSynchronously(const NetworkVolumeState _state) noexcept
{
    return _state == NetworkVolumeState::Local || _state == NetworkVolumeState::Responsive;
}

/**
 * Whether an operation targeting this volume should be refused up front rather than attempted.
 *
 * A stale mount is the case worth naming: it looks mounted and answers immediately, so an
 * optimistic attempt fails per item, potentially thousands of times, each with its own error. One
 * refusal naming the volume is more useful than a thousand naming files.
 */
[[nodiscard]] constexpr bool ShouldRefuseBeforeAttempting(const NetworkVolumeState _state) noexcept
{
    return _state == NetworkVolumeState::Stale || _state == NetworkVolumeState::Unmounted;
}

} // namespace nc::core
