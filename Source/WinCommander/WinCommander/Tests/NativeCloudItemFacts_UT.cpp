// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Cloud/NativeCloudItemFacts.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

using nc::core::CloudItemFactsFromProbe;
using nc::core::CloudSyncState;
using nc::core::ClassifyCloudSyncState;
using nc::core::NativeCloudProbe;
using nc::core::ProbeNativeCloudItem;
using nc::core::UnmaskedCloudPlaceholderName;

} // namespace

#define PREFIX "nc::core::UnmaskedCloudPlaceholderName "

TEST_CASE(PREFIX "gives back the name the user is actually looking for")
{
    // A not-yet-downloaded file is stored as `.name.ext.icloud`. Shown verbatim it is a hidden file
    // with the wrong extension in place of the photograph - and every extension-driven decision
    // above, Gallery eligibility included, would then be made about `.icloud` rather than `.jpg`.
    CHECK(UnmaskedCloudPlaceholderName(".holiday.jpg.icloud") == "holiday.jpg");
    CHECK(UnmaskedCloudPlaceholderName(".notes.txt.icloud") == "notes.txt");
    CHECK(UnmaskedCloudPlaceholderName(".a.icloud") == "a");
}

TEST_CASE(PREFIX "does not invent a file that is not there")
{
    // The leading dot is as much a part of the convention as the suffix: a file genuinely named
    // "notes.icloud" is an ordinary file, and unmasking it would report one that does not exist.
    CHECK(UnmaskedCloudPlaceholderName("notes.icloud") == std::nullopt);
    CHECK(UnmaskedCloudPlaceholderName("holiday.jpg") == std::nullopt);
    CHECK(UnmaskedCloudPlaceholderName(".hidden") == std::nullopt);
    CHECK(UnmaskedCloudPlaceholderName(".icloud") == std::nullopt);
    CHECK(UnmaskedCloudPlaceholderName("..icloud") == std::nullopt);
    CHECK(UnmaskedCloudPlaceholderName("") == std::nullopt);
}

#undef PREFIX
#define PREFIX "nc::core::CloudItemFactsFromProbe "

TEST_CASE(PREFIX "reads a placeholder as exactly what it is")
{
    // "The provider knows this file and its bytes are not here" - which is what stops anything above
    // asking for a thumbnail and thereby fetching it.
    NativeCloudProbe probe;
    probe.in_cloud_container = true;
    probe.is_dataless_placeholder = true;

    const auto facts = CloudItemFactsFromProbe(probe);
    CHECK(facts.in_cloud_container);
    CHECK_FALSE(facts.has_local_copy);
    CHECK(ClassifyCloudSyncState(facts) == CloudSyncState::CloudOnly);
}

TEST_CASE(PREFIX "carries every state the provider can report")
{
    NativeCloudProbe probe;
    probe.in_cloud_container = true;

    CHECK(ClassifyCloudSyncState(CloudItemFactsFromProbe(probe)) == CloudSyncState::Synced);

    probe.download_in_progress = true;
    CHECK(ClassifyCloudSyncState(CloudItemFactsFromProbe(probe)) == CloudSyncState::Downloading);
    probe.download_in_progress = false;

    probe.upload_in_progress = true;
    CHECK(ClassifyCloudSyncState(CloudItemFactsFromProbe(probe)) == CloudSyncState::Uploading);
    probe.upload_in_progress = false;

    probe.excluded_from_sync = true;
    CHECK(ClassifyCloudSyncState(CloudItemFactsFromProbe(probe)) == CloudSyncState::Excluded);
    probe.excluded_from_sync = false;

    // A conflict outranks everything, because it is the only state where doing nothing loses data.
    probe.has_conflict = true;
    probe.is_dataless_placeholder = true;
    probe.download_in_progress = true;
    CHECK(ClassifyCloudSyncState(CloudItemFactsFromProbe(probe)) == CloudSyncState::Conflicted);
}

TEST_CASE(PREFIX "reports an item outside any container as not cloud at all")
{
    // Whatever else a probe carries, a file that is not in a provider's folder has no sync state,
    // and inventing one would badge ordinary files.
    NativeCloudProbe probe;
    probe.in_cloud_container = false;
    probe.is_dataless_placeholder = true;
    probe.has_conflict = true;
    CHECK(ClassifyCloudSyncState(CloudItemFactsFromProbe(probe)) == CloudSyncState::NotCloud);
}

#undef PREFIX

#define PREFIX "nc::core::ProbeNativeCloudItem "

TEST_CASE(PREFIX "reports an ordinary local file as not cloud at all")
{
    const TempTestDir tmp_dir;
    const std::filesystem::path file = std::filesystem::path{tmp_dir.directory} / "ordinary.txt";
    std::ofstream(file) << "contents";

    const NativeCloudProbe probe = ProbeNativeCloudItem(file.native());
    CHECK_FALSE(probe.in_cloud_container);
    // Everything else is meaningless outside a container, and must not come back half-filled.
    CHECK_FALSE(probe.is_dataless_placeholder);
    CHECK_FALSE(probe.download_in_progress);
    CHECK_FALSE(probe.upload_in_progress);
    CHECK_FALSE(probe.has_conflict);
    CHECK_FALSE(probe.excluded_from_sync);
    CHECK(ClassifyCloudSyncState(CloudItemFactsFromProbe(probe)) == CloudSyncState::NotCloud);
}

TEST_CASE(PREFIX "answers safely for a path it cannot read")
{
    // Reporting an unreadable item as a placeholder would badge it and tell every surface above that
    // its bytes are elsewhere, when in fact nobody knows.
    for( const std::string &path : {std::string{}, std::string{"/nonexistent-for-tests/x.jpg"}} ) {
        const NativeCloudProbe probe = ProbeNativeCloudItem(path);
        CHECK_FALSE(probe.in_cloud_container);
        CHECK_FALSE(probe.is_dataless_placeholder);
    }

    // A directory is answered too, rather than refused.
    const TempTestDir tmp_dir;
    CHECK_FALSE(ProbeNativeCloudItem(tmp_dir.directory).in_cloud_container);
}

#undef PREFIX
