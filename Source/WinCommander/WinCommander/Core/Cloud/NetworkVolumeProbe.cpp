// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NetworkVolumeProbe.h"

#include <sys/mount.h>
#include <sys/param.h>

#include <cerrno>
#include <future>
#include <memory>
#include <thread>
#include <utility>

namespace nc::core {

NetworkVolumeProbeResult ProbeNetworkVolume(const std::string &_mount_point, const std::chrono::milliseconds _timeout)
{
    // Heap-allocated and shared with the worker, so the worker owns everything it touches. If the
    // wait below gives up, this object outlives the call and the abandoned thread still writes
    // somewhere valid - into state nobody reads.
    struct SharedState {
        std::promise<NetworkVolumeProbeResult> promise;
        std::string mount_point;
    };
    auto state = std::make_shared<SharedState>();
    state->mount_point = _mount_point;
    std::future<NetworkVolumeProbeResult> answer = state->promise.get_future();

    std::thread{[state] {
        struct statfs info{};
        // This is the call that blocks under a dead mount. There is no timeout on it, which is why
        // the wait rather than the call is what has a deadline.
        const bool answered = ::statfs(state->mount_point.c_str(), &info) == 0;
        const int error = errno;
        state->promise.set_value(NetworkVolumeProbeResult{
            .answered = answered,
            // The server replied and refused: the export is gone or changed identity. Distinct from
            // silence, and the caller treats them differently.
            .export_rejected = !answered && (error == ENOENT || error == ESTALE || error == ENOTCONN),
        });
    }}.detach();

    if( answer.wait_for(_timeout) != std::future_status::ready ) {
        // A late answer is not an answer. Something that takes this long to reply is precisely what
        // must not be put on the drawing thread, whatever it eventually says.
        return NetworkVolumeProbeResult{.answered = false, .export_rejected = false};
    }
    return answer.get();
}

NetworkVolumeProbeCache::NetworkVolumeProbeCache(Prober _prober, const std::chrono::milliseconds _freshness)
    : m_Prober{std::move(_prober)}, m_Freshness{_freshness}
{
}

std::optional<NetworkVolumeProbeResult> NetworkVolumeProbeCache::Known(const std::string_view _mount_point,
                                                                      const Instant _now) const
{
    const auto lock = std::lock_guard{m_Lock};
    const auto found = m_Records.find(_mount_point);
    if( found == m_Records.end() )
        return std::nullopt;
    // Withheld rather than returned once it has aged out: a `Responsive` from a minute ago would
    // hide a mount that has died since, and hiding it is how the synchronous touch happens anyway.
    if( _now - found->second.taken_at > m_Freshness )
        return std::nullopt;
    return found->second.result;
}

bool NetworkVolumeProbeCache::NeedsRefresh(const std::string_view _mount_point, const Instant _now) const
{
    return !Known(_mount_point, _now).has_value();
}

void NetworkVolumeProbeCache::Refresh(const std::string &_mount_point, const Instant _now)
{
    if( !m_Prober )
        return;
    // Deliberately outside the lock. The prober is the part that can take tens of seconds, and
    // holding the lock across it would block every drawing-thread read for exactly that long -
    // reintroducing the stall this class exists to prevent, one indirection further away.
    const NetworkVolumeProbeResult result = m_Prober(_mount_point);

    const auto lock = std::lock_guard{m_Lock};
    Record &record = m_Records[_mount_point];
    // A slower probe that started earlier must not overwrite a fresher answer.
    if( record.taken_at <= _now ) {
        record.result = result;
        record.taken_at = _now;
    }
}

void NetworkVolumeProbeCache::Forget(const std::string_view _mount_point)
{
    const auto lock = std::lock_guard{m_Lock};
    m_Records.erase(std::string{_mount_point});
}

} // namespace nc::core
