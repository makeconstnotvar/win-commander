// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Cloud/GalleryListingSource.h>

#include <string>
#include <vector>

namespace {

using nc::core::BuildGalleryListing;
using nc::core::BuildGalleryContents;
using nc::core::CloudSyncState;
using nc::core::GalleryEligibility;
using nc::core::NativeCloudProbe;
using nc::core::NativeListingEntry;

NativeListingEntry File(const std::string &_name)
{
    return NativeListingEntry{.filename = _name, .is_directory = false};
}

NativeListingEntry Dir(const std::string &_name)
{
    return NativeListingEntry{.filename = _name, .is_directory = true};
}

/** Reports the named paths as not-yet-downloaded placeholders, and counts what it was asked. */
struct ScriptedProber {
    std::shared_ptr<std::vector<std::string>> asked = std::make_shared<std::vector<std::string>>();
    std::shared_ptr<std::set<std::string>> placeholders = std::make_shared<std::set<std::string>>();

    std::function<NativeCloudProbe(const std::string &)> Handler() const
    {
        auto asked_copy = asked;
        auto placeholders_copy = placeholders;
        return [asked_copy, placeholders_copy](const std::string &_path) {
            asked_copy->push_back(_path);
            NativeCloudProbe probe;
            if( placeholders_copy->contains(_path) ) {
                probe.in_cloud_container = true;
                probe.is_dataless_placeholder = true;
            }
            return probe;
        };
    }
};

} // namespace

#define PREFIX "nc::core::BuildGalleryListing "

TEST_CASE(PREFIX "unmasks a placeholder before anything reads its extension")
{
    // A not-yet-downloaded photograph is on disk as `.holiday.jpg.icloud`. Taken at face value its
    // extension is `icloud`, Gallery decides it is not media, and the one row the user most wants to
    // see silently vanishes from the view.
    ScriptedProber prober;
    prober.placeholders->insert("/photos/.holiday.jpg.icloud");

    const auto source = BuildGalleryListing("/photos", std::vector{File(".holiday.jpg.icloud")}, prober.Handler());
    REQUIRE(source.Size() == 1);
    CHECK(source.DisplayName(0) == "holiday.jpg");
    CHECK(source.Items()[0].filename == "holiday.jpg");
    CHECK(source.Items()[0].facts.extension == "jpg");
    CHECK(source.Items()[0].facts.cloud_state == CloudSyncState::CloudOnly);

    // Probed under the name on disk, which is what actually exists there.
    REQUIRE(prober.asked->size() == 1);
    CHECK(prober.asked->front() == "/photos/.holiday.jpg.icloud");

    // And it survives all the way to a Gallery row - as a placeholder, not as a missing entry.
    const auto contents = BuildGalleryContents(source.Items());
    REQUIRE(contents.rows.size() == 1);
    CHECK(contents.rows[0].filename == "holiday.jpg");
    CHECK(contents.rows[0].eligibility == GalleryEligibility::PlaceholderOnly);
}

TEST_CASE(PREFIX "does not probe directories")
{
    // A folder is a folder to Gallery whatever its sync state, so asking would spend a filesystem
    // call per row to learn something nothing reads.
    ScriptedProber prober;
    const auto source =
        BuildGalleryListing("/d", std::vector{Dir("sub"), File("a.jpg"), Dir("other")}, prober.Handler());
    REQUIRE(source.Size() == 3);
    CHECK(prober.asked->size() == 1);
    CHECK(prober.asked->front() == "/d/a.jpg");
    CHECK(source.Items()[0].facts.is_directory);
    CHECK(source.Items()[0].facts.cloud_state == CloudSyncState::NotCloud);
}

TEST_CASE(PREFIX "reads extensions the way the rest of the application does")
{
    const auto source = BuildGalleryListing(
        "/d", std::vector{File("a.jpg"), File("archive.tar.gz"), File("plain"), File(".profile"), File("trailing.")},
        {});
    REQUIRE(source.Size() == 5);
    CHECK(source.Items()[0].facts.extension == "jpg");
    CHECK(source.Items()[1].facts.extension == "gz");
    CHECK(source.Items()[2].facts.extension.empty());
    // A leading dot starts a hidden name, not an extension: ".profile" has none, and treating
    // "profile" as one would put dotfiles in front of every extension-driven rule.
    CHECK(source.Items()[3].facts.extension.empty());
    CHECK(source.Items()[4].facts.extension.empty());
}

TEST_CASE(PREFIX "marks the parent entry so nothing counts it as content")
{
    const auto source = BuildGalleryListing("/d", std::vector{Dir(".."), File("a.jpg")}, {});
    REQUIRE(source.Size() == 2);
    CHECK(source.Items()[0].is_dot_dot);
    CHECK_FALSE(source.Items()[1].is_dot_dot);

    const auto contents = BuildGalleryContents(source.Items());
    REQUIRE(contents.rows.size() == 1);
    CHECK(contents.rows[0].filename == "a.jpg");
}

TEST_CASE(PREFIX "keeps its views valid across a move")
{
    // The items hold views into the source's own strings, so a move must keep them pointing
    // somewhere real - which is why copying is deleted and moving is not.
    auto original = BuildGalleryListing("/d", std::vector{File("a.jpg"), File("b.png")}, {});
    const auto moved = std::move(original);
    REQUIRE(moved.Size() == 2);
    CHECK(moved.Items()[0].filename == "a.jpg");
    CHECK(moved.Items()[1].facts.extension == "png");
}

TEST_CASE(PREFIX "answers an empty listing and a missing prober")
{
    const auto empty = BuildGalleryListing("/d", {}, {});
    CHECK(empty.Size() == 0);

    // No prober means no cloud state - which is the honest answer, not an invented one.
    const auto no_prober = BuildGalleryListing("/d", std::vector{File("a.jpg")}, {});
    REQUIRE(no_prober.Size() == 1);
    CHECK(no_prober.Items()[0].facts.cloud_state == CloudSyncState::NotCloud);
}

#undef PREFIX
