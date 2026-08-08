// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nc::core {

/** What a presented host fingerprint means against what was pinned for that host. */
enum class RemoteHostTrustVerdict : uint8_t {
    /** The presented fingerprint equals the pinned one. */
    TrustedPinned,
    /** Nothing is pinned yet. The user must decide; this is never an automatic yes. */
    UnknownFirstUse,
    /**
     * A fingerprint is pinned and the presented one differs. This is the signature of an
     * interception and must never be resolved automatically.
     */
    Mismatch,
    /** The presented fingerprint is absent or malformed, so nothing was verified at all. */
    Unusable
};

/**
 * Normalizes a host fingerprint for comparison: lowercased hex with separators removed.
 *
 * Providers spell the same fingerprint several ways - `AA:BB:CC`, `aa bb cc`, `aabbcc` - and
 * comparing those literally would report a mismatch for a host that never changed, training users
 * to click through the one warning that must never be routine.
 *
 * Returns nothing for anything that is not an even-length run of hex digits once separators are
 * removed. A malformed value is rejected rather than coerced, because coercing it could make two
 * different inputs compare equal.
 */
[[nodiscard]] std::optional<std::string> NormalizeHostFingerprint(std::string_view _fingerprint);

/**
 * Classifies a presented fingerprint against the pin held for that host.
 *
 * Deliberately total and side-effect free: it neither stores a pin nor upgrades a `Mismatch`. The
 * decision to pin a first-use host, and the refusal to ever silently re-pin a mismatched one,
 * belong to the caller that can actually ask the user.
 */
[[nodiscard]] RemoteHostTrustVerdict ClassifyRemoteHost(const std::optional<std::string> &_pinned_fingerprint,
                                                        std::string_view _presented_fingerprint);

/** True only for the verdict that may proceed without asking anyone. */
[[nodiscard]] constexpr bool MayConnectWithoutPrompt(const RemoteHostTrustVerdict _verdict) noexcept
{
    return _verdict == RemoteHostTrustVerdict::TrustedPinned;
}

/**
 * True when the verdict may be resolved by asking the user to accept the host.
 *
 * `Mismatch` is excluded on purpose: an established pin that suddenly disagrees is not a question
 * to put to the user as a routine accept/decline, because the honest answer requires knowing why it
 * changed. Replacing such a pin is a separate, explicit action.
 */
[[nodiscard]] constexpr bool MayPromptToTrust(const RemoteHostTrustVerdict _verdict) noexcept
{
    return _verdict == RemoteHostTrustVerdict::UnknownFirstUse;
}

} // namespace nc::core
