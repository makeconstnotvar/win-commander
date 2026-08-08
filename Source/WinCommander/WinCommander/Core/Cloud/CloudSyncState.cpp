// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CloudSyncState.h"

namespace nc::core {

CloudSyncState ClassifyCloudSyncState(const CloudItemFacts &_facts) noexcept
{
    // Outside a cloud container none of the other facts mean anything - a stray "downloading" flag
    // from a confused adapter must not turn a local file into a cloud one.
    if( !_facts.in_cloud_container )
        return CloudSyncState::NotCloud;

    // A conflict is the only state where doing nothing loses data, so nothing outranks it.
    if( _facts.has_conflict )
        return CloudSyncState::Conflicted;

    // Exclusion is a deliberate choice, and an excluded item is not "waiting" for anything; it
    // outranks transfer flags so a stale one cannot make it look like sync is still coming.
    if( _facts.excluded_from_sync )
        return CloudSyncState::Excluded;

    // Transfers outrank placeholder status: a downloading placeholder is on its way, a stalled one
    // is not, and reporting both as CloudOnly would hide exactly that difference.
    if( _facts.download_in_progress )
        return CloudSyncState::Downloading;
    if( _facts.upload_in_progress )
        return CloudSyncState::Uploading;

    return _facts.has_local_copy ? CloudSyncState::Synced : CloudSyncState::CloudOnly;
}

} // namespace nc::core
