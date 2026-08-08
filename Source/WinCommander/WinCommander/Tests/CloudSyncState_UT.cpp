// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Cloud/CloudSyncState.h>

namespace {

using nc::core::ClassifyCloudSyncState;
using nc::core::CloudItemFacts;
using nc::core::CloudSyncState;
using nc::core::ShouldBadgeCloudSyncState;

CloudItemFacts InCloud()
{
    return {.in_cloud_container = true, .has_local_copy = true};
}

} // namespace

#define PREFIX "nc::core::CloudSyncState "

TEST_CASE(PREFIX "ignores every other fact outside a cloud container")
{
    // A stray flag from a confused adapter must not turn an ordinary local file into a cloud one.
    CloudItemFacts facts{.in_cloud_container = false,
                         .has_local_copy = false,
                         .download_in_progress = true,
                         .upload_in_progress = true,
                         .has_conflict = true,
                         .excluded_from_sync = true};
    CHECK(ClassifyCloudSyncState(facts) == CloudSyncState::NotCloud);
    CHECK_FALSE(ShouldBadgeCloudSyncState(CloudSyncState::NotCloud));
}

TEST_CASE(PREFIX "lets a conflict outrank everything")
{
    // It is the one state where doing nothing loses data.
    CloudItemFacts facts = InCloud();
    facts.has_conflict = true;
    facts.download_in_progress = true;
    facts.upload_in_progress = true;
    facts.excluded_from_sync = true;
    facts.has_local_copy = false;
    CHECK(ClassifyCloudSyncState(facts) == CloudSyncState::Conflicted);
}

TEST_CASE(PREFIX "reports a deliberate exclusion rather than a pending transfer")
{
    // An excluded item is not waiting for anything; a stale transfer flag must not make it look
    // like sync is still coming.
    CloudItemFacts facts = InCloud();
    facts.excluded_from_sync = true;
    facts.download_in_progress = true;
    CHECK(ClassifyCloudSyncState(facts) == CloudSyncState::Excluded);
}

TEST_CASE(PREFIX "separates a placeholder on its way from one that is stalled")
{
    CloudItemFacts arriving = InCloud();
    arriving.has_local_copy = false;
    arriving.download_in_progress = true;
    CHECK(ClassifyCloudSyncState(arriving) == CloudSyncState::Downloading);

    CloudItemFacts stalled = InCloud();
    stalled.has_local_copy = false;
    CHECK(ClassifyCloudSyncState(stalled) == CloudSyncState::CloudOnly);

    // Reporting both as CloudOnly would hide exactly the difference the user cares about.
    CHECK(ClassifyCloudSyncState(arriving) != ClassifyCloudSyncState(stalled));
}

TEST_CASE(PREFIX "reports an upload of an item that is already local")
{
    CloudItemFacts facts = InCloud();
    facts.upload_in_progress = true;
    CHECK(ClassifyCloudSyncState(facts) == CloudSyncState::Uploading);

    // Download wins over upload when an adapter reports both, because the missing bytes are the
    // more consequential half for someone about to open the file.
    facts.download_in_progress = true;
    CHECK(ClassifyCloudSyncState(facts) == CloudSyncState::Downloading);
}

TEST_CASE(PREFIX "calls a present, quiet item synced")
{
    CHECK(ClassifyCloudSyncState(InCloud()) == CloudSyncState::Synced);
}

TEST_CASE(PREFIX "badges only what differs from the surrounding rows")
{
    // Inside a cloud folder, synced is what everything is supposed to be. Badging it decorates
    // every row identically and so says nothing, while making the rows that genuinely differ
    // harder to spot. This is the spec's "badges only where appropriate" as a rule.
    CHECK_FALSE(ShouldBadgeCloudSyncState(CloudSyncState::Synced));
    CHECK_FALSE(ShouldBadgeCloudSyncState(CloudSyncState::NotCloud));

    CHECK(ShouldBadgeCloudSyncState(CloudSyncState::CloudOnly));
    CHECK(ShouldBadgeCloudSyncState(CloudSyncState::Downloading));
    CHECK(ShouldBadgeCloudSyncState(CloudSyncState::Uploading));
    CHECK(ShouldBadgeCloudSyncState(CloudSyncState::Conflicted));
    CHECK(ShouldBadgeCloudSyncState(CloudSyncState::Excluded));
}
