// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "RemoteConnectionState.h"
#include "RemoteHostTrust.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace nc::core {

/** Link quality, derived from the latest latency sample. */
enum class RemoteLinkQuality : uint8_t {
    /** No sample - not connected, or the last attempt failed. */
    Unknown,
    Good,
    Fair,
    Poor
};

/** Whether a connection can authenticate itself without asking the user again. */
enum class RemoteCredentialsState : uint8_t {
    /** A credential is stored and has not been rejected. */
    Stored,
    /** Nothing stored; connecting will have to prompt. */
    Missing,
    /** Stored, but the server rejected it - it will keep failing until replaced. */
    Rejected
};

/**
 * Thresholds for `ClassifyRemoteLinkQuality`. Defaults are chosen for interactive browsing rather
 * than throughput: past roughly a quarter second, typing a path and waiting for a listing stops
 * feeling direct.
 */
struct RemoteLinkQualityThresholds {
    std::chrono::milliseconds good_below{120};
    std::chrono::milliseconds fair_below{400};

    friend bool operator==(const RemoteLinkQualityThresholds &, const RemoteLinkQualityThresholds &) = default;
};

[[nodiscard]] RemoteLinkQuality ClassifyRemoteLinkQuality(const std::optional<std::chrono::milliseconds> &_latency,
                                                          const RemoteLinkQualityThresholds &_thresholds = {});

/**
 * Everything the Connection Manager row shows for one connection (spec §46), derived from state.
 *
 * Carries no credential material, exactly like `RemoteConnectionState` - this value is what gets
 * copied into the UI, so giving it somewhere to put a password is how one ends up in a log.
 */
struct RemoteConnectionPresentation {
    RemoteConnectionStatus status = RemoteConnectionStatus::Disconnected;
    RemoteCredentialsState credentials = RemoteCredentialsState::Missing;
    RemoteLinkQuality quality = RemoteLinkQuality::Unknown;
    RemoteHostTrustVerdict trust = RemoteHostTrustVerdict::UnknownFirstUse;
    std::optional<int64_t> last_successful_connection;
    bool read_only = false;
    /** The user is expected to act; the row should say so rather than look merely idle. */
    bool needs_attention = false;
    /** How many failures the history holds, for a "N errors" affordance. */
    size_t recorded_failures = 0;

    friend bool operator==(const RemoteConnectionPresentation &, const RemoteConnectionPresentation &) = default;
};

/**
 * Projects one connection for display.
 *
 * `needs_attention` is the single flag a row acts on, and it is deliberately **not** just
 * "status == Blocked": a mismatched host pin needs the user even while the connection is sitting
 * idle and has never failed, so trust is folded in here rather than left for each surface to
 * remember separately.
 */
[[nodiscard]] RemoteConnectionPresentation
PresentRemoteConnection(const RemoteConnectionState &_state,
                        bool _has_stored_credential,
                        RemoteHostTrustVerdict _trust,
                        const RemoteLinkQualityThresholds &_thresholds = {});

} // namespace nc::core
