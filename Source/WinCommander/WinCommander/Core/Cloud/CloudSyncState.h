// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstdint>

namespace nc::core {

/** Sync state of one item in a cloud-managed location (spec §11 badge vocabulary). */
enum class CloudSyncState : uint8_t {
    /** Not in a cloud container at all. */
    NotCloud,
    /** Present locally and in step with the provider. The expected state. */
    Synced,
    /** A placeholder: the provider knows the file, the bytes are not here. */
    CloudOnly,
    Downloading,
    Uploading,
    /** The provider reports divergent versions. Nothing else outranks this. */
    Conflicted,
    /** Deliberately kept out of sync by the user or a rule. */
    Excluded
};

/**
 * What a provider can actually answer about one item. Deliberately booleans rather than a provider
 * enum, because every service words its states differently and the mapping belongs at its adapter.
 */
struct CloudItemFacts {
    bool in_cloud_container = false;
    /** The bytes are on this disk. False means a placeholder. */
    bool has_local_copy = true;
    bool download_in_progress = false;
    bool upload_in_progress = false;
    bool has_conflict = false;
    /** Excluded from syncing on purpose - not a failure. */
    bool excluded_from_sync = false;

    friend bool operator==(const CloudItemFacts &, const CloudItemFacts &) = default;
};

/**
 * Classifies one item.
 *
 * Ordering is by what the user most needs told: a conflict outranks everything, because it is the
 * only state where doing nothing loses data. Transfers outrank placeholder status, since a
 * downloading placeholder is on its way and a stalled one is not - showing both as `CloudOnly`
 * would hide the difference.
 */
[[nodiscard]] CloudSyncState ClassifyCloudSyncState(const CloudItemFacts &_facts) noexcept;

/**
 * Whether this state earns a badge.
 *
 * `Synced` deliberately gets none. Inside a cloud folder, synced is what everything is supposed to
 * be; badging it decorates every row identically and so conveys nothing, while making the rows that
 * genuinely differ harder to spot. A badge is worth its pixels only when it says something the
 * surrounding rows do not - which is the spec's "badges only where they are appropriate", read as a
 * rule rather than a preference.
 */
[[nodiscard]] constexpr bool ShouldBadgeCloudSyncState(const CloudSyncState _state) noexcept
{
    return _state != CloudSyncState::NotCloud && _state != CloudSyncState::Synced;
}

} // namespace nc::core
