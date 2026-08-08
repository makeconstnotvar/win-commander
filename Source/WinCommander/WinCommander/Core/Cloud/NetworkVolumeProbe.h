// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "MountTable.h"

#include <Base/UnorderedUtil.h>

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nc::core {

/**
 * Asks a mount point whether it is still there, giving up after a budget.
 *
 * The call is made on a thread of its own and abandoned if it does not return in time. There is no
 * way to interrupt a blocked `stat()` under a dead network mount - the kernel holds it for tens of
 * seconds - so the only way to have a deadline at all is to stop waiting for it. The abandoned
 * thread touches nothing the caller owns, so its eventual return is harmless and unread.
 *
 * **A late answer is not an answer.** Once the budget is spent the volume is reported unresponsive,
 * and a result arriving afterwards does not retract that: something that takes half a minute to
 * reply is exactly what the caller must not put on the drawing thread.
 */
[[nodiscard]] NetworkVolumeProbeResult ProbeNetworkVolume(const std::string &_mount_point,
                                                           std::chrono::milliseconds _timeout);

/**
 * Remembers what each mount point last said.
 *
 * The point of the whole model is that a volume's state is known *before* an access rather than
 * discovered by making one. That only works if the answer is remembered - probing to draw a row
 * would be the very synchronous touch being avoided.
 *
 * Internally synchronized: refreshes run on a worker while the drawing thread reads.
 */
class NetworkVolumeProbeCache final
{
public:
    using Instant = std::chrono::milliseconds;
    /** Answers for one mount point. May block for a long time; never called on the drawing thread. */
    using Prober = std::function<NetworkVolumeProbeResult(const std::string &_mount_point)>;

    explicit NetworkVolumeProbeCache(Prober _prober, std::chrono::milliseconds _freshness = std::chrono::seconds{10});

    /**
     * What this mount point last said, or nothing when it has never been asked or the answer has
     * aged out. **Never blocks and never probes** - this is the call the drawing thread may make.
     *
     * An aged-out answer is withheld rather than returned: a `Responsive` from a minute ago would
     * hide a mount that died since, and hiding it is how the synchronous touch happens anyway.
     */
    [[nodiscard]] std::optional<NetworkVolumeProbeResult> Known(std::string_view _mount_point, Instant _now) const;

    /** True when a refresh is worth starting: nothing known, or what is known has aged out. */
    [[nodiscard]] bool NeedsRefresh(std::string_view _mount_point, Instant _now) const;

    /** Runs the prober and records what it said. Call off the drawing thread. */
    void Refresh(const std::string &_mount_point, Instant _now);

    /** Drops what is remembered about a mount point - for one that went away. */
    void Forget(std::string_view _mount_point);

private:
    struct Record {
        NetworkVolumeProbeResult result;
        Instant taken_at{};
    };

    mutable std::mutex m_Lock;
    Prober m_Prober;
    std::chrono::milliseconds m_Freshness;
    std::unordered_map<std::string, Record, UnorderedStringHashEqual, UnorderedStringHashEqual> m_Records;
};

} // namespace nc::core
