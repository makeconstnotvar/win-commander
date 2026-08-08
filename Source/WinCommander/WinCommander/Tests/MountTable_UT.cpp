// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Cloud/MountTable.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using nc::core::ClassifyNetworkVolume;
using nc::core::MayTouchSynchronously;
using nc::core::MountedVolume;
using nc::core::NetworkVolumeFactsForPath;
using nc::core::NetworkVolumeProbeResult;
using nc::core::NetworkVolumeState;
using nc::core::ReadMountTable;
using nc::core::VolumeForPath;

std::vector<MountedVolume> Table()
{
    return {
        MountedVolume{.mount_point = "/", .filesystem_type = "apfs", .is_network = false, .read_only = false},
        MountedVolume{
            .mount_point = "/Volumes/data", .filesystem_type = "apfs", .is_network = false, .read_only = false},
        // Chosen to collide with the one above under a naive string-prefix match.
        MountedVolume{
            .mount_point = "/Volumes/database", .filesystem_type = "smbfs", .is_network = true, .read_only = false},
        MountedVolume{
            .mount_point = "/Volumes/data/inner", .filesystem_type = "nfs", .is_network = true, .read_only = true},
    };
}

} // namespace

#define PREFIX "nc::core::MountTable "

TEST_CASE(PREFIX "matches a path to its volume by component, never by string prefix")
{
    const auto table = Table();

    // `/Volumes/data` is not `/Volumes/database`. Treating one as containing the other would report a
    // live local disk as a network mount, or the reverse - and the reverse is the one that gets
    // probed on the drawing thread.
    const auto database = VolumeForPath("/Volumes/database/file.txt", table);
    REQUIRE(database);
    CHECK(database->mount_point == "/Volumes/database");
    CHECK(database->is_network);

    const auto data = VolumeForPath("/Volumes/data/file.txt", table);
    REQUIRE(data);
    CHECK(data->mount_point == "/Volumes/data");
    CHECK_FALSE(data->is_network);
}

TEST_CASE(PREFIX "gives a path to the innermost volume containing it")
{
    const auto table = Table();

    const auto inner = VolumeForPath("/Volumes/data/inner/deep/file.txt", table);
    REQUIRE(inner);
    // A volume mounted inside another is the one a path under it belongs to.
    CHECK(inner->mount_point == "/Volumes/data/inner");
    CHECK(inner->filesystem_type == "nfs");

    const auto outer = VolumeForPath("/Volumes/data/other/file.txt", table);
    REQUIRE(outer);
    CHECK(outer->mount_point == "/Volumes/data");

    // The mount point itself belongs to its own volume, not to its parent.
    const auto at_mount = VolumeForPath("/Volumes/data/inner", table);
    REQUIRE(at_mount);
    CHECK(at_mount->mount_point == "/Volumes/data/inner");
}

TEST_CASE(PREFIX "falls back to the root volume and refuses a path it cannot place")
{
    const auto table = Table();

    const auto root = VolumeForPath("/Users/me/file.txt", table);
    REQUIRE(root);
    CHECK(root->mount_point == "/");

    CHECK(VolumeForPath("relative/path", table) == std::nullopt);
    CHECK(VolumeForPath("", table) == std::nullopt);
    CHECK(VolumeForPath("/Users/me", {}) == std::nullopt);
}

TEST_CASE(PREFIX "normalizes before matching")
{
    const auto table = Table();

    const auto normalized = VolumeForPath("/Volumes/data/inner/../inner/file.txt", table);
    REQUIRE(normalized);
    CHECK(normalized->mount_point == "/Volumes/data/inner");

    const auto dotted = VolumeForPath("/Volumes/./database/file.txt", table);
    REQUIRE(dotted);
    CHECK(dotted->mount_point == "/Volumes/database");
}

TEST_CASE(PREFIX "a local volume is answering by construction")
{
    const auto table = Table();

    // Carrying a stale probe result into a local volume could report a working disk as
    // unresponsive, and everything downstream would then refuse to touch it.
    const auto facts = NetworkVolumeFactsForPath(
        "/Volumes/data/file.txt", table, NetworkVolumeProbeResult{.answered = false, .export_rejected = true});
    REQUIRE(facts);
    CHECK_FALSE(facts->is_network_mount);
    CHECK(facts->is_mounted);
    CHECK(facts->last_probe_answered);
    CHECK_FALSE(facts->export_rejected);
    CHECK(ClassifyNetworkVolume(*facts) == NetworkVolumeState::Local);
    CHECK(MayTouchSynchronously(ClassifyNetworkVolume(*facts)));
}

TEST_CASE(PREFIX "carries a probe result through for a network volume")
{
    const auto table = Table();

    const auto responsive = NetworkVolumeFactsForPath("/Volumes/database/file.txt", table, {});
    REQUIRE(responsive);
    CHECK(responsive->is_network_mount);
    CHECK(ClassifyNetworkVolume(*responsive) == NetworkVolumeState::Responsive);

    const auto unresponsive = NetworkVolumeFactsForPath(
        "/Volumes/database/file.txt", table, NetworkVolumeProbeResult{.answered = false, .export_rejected = false});
    REQUIRE(unresponsive);
    CHECK(ClassifyNetworkVolume(*unresponsive) == NetworkVolumeState::Unresponsive);
    // The rule the whole model exists for.
    CHECK_FALSE(MayTouchSynchronously(ClassifyNetworkVolume(*unresponsive)));

    const auto stale = NetworkVolumeFactsForPath(
        "/Volumes/database/file.txt", table, NetworkVolumeProbeResult{.answered = true, .export_rejected = true});
    REQUIRE(stale);
    CHECK(ClassifyNetworkVolume(*stale) == NetworkVolumeState::Stale);
}

TEST_CASE(PREFIX "reports that it cannot place a path rather than inventing an answer")
{
    // Neither available answer is honest here. `Local` means "safe to touch on the drawing thread",
    // for exactly the path we could not account for. `Unmounted` refuses operations up front - which
    // would refuse *everything* on a machine where the mount table could not be read at all. The
    // caller knows which risk applies to it; this does not.
    CHECK(NetworkVolumeFactsForPath("/Volumes/gone/file.txt", {}) == std::nullopt);
    CHECK(NetworkVolumeFactsForPath("relative/path", Table()) == std::nullopt);

    // And a path that *can* be placed is still answered.
    CHECK(NetworkVolumeFactsForPath("/Users/me/file.txt", Table()).has_value());
}

TEST_CASE(PREFIX "reads the real mount table and finds the root volume in it")
{
    const std::vector<MountedVolume> table = ReadMountTable();
    REQUIRE_FALSE(table.empty());

    const auto root = std::ranges::find_if(table, [](const MountedVolume &_v) { return _v.mount_point == "/"; });
    REQUIRE(root != table.end());
    CHECK_FALSE(root->is_network);
    CHECK_FALSE(root->filesystem_type.empty());

    // Every entry the kernel gave us is usable as a volume for something.
    for( const MountedVolume &volume : table )
        CHECK_FALSE(volume.mount_point.empty());

    // And the machine's own root directory places against it.
    const auto placed = VolumeForPath("/", table);
    REQUIRE(placed);
    CHECK(placed->mount_point == "/");
}

#undef PREFIX
