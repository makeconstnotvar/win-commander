// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Cloud/GalleryThumbnails.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace {

using nc::core::GalleryEligibility;
using nc::core::GalleryRow;
using nc::core::GalleryThumbnailCache;
using nc::core::GalleryThumbnailState;

GalleryRow Row(const std::string &_name, const GalleryEligibility _eligibility)
{
    return GalleryRow{.filename = _name, .eligibility = _eligibility, .listing_index = 0};
}

/** Hands back a non-null token, or nothing for the paths a test names as unreadable. */
struct ScriptedGenerator {
    std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::set<std::string>> unreadable = std::make_shared<std::set<std::string>>();

    GalleryThumbnailCache::Generator Handler() const
    {
        auto calls_copy = calls;
        auto unreadable_copy = unreadable;
        return [calls_copy, unreadable_copy](const std::string &_path) -> GalleryThumbnailCache::Thumbnail {
            ++*calls_copy;
            if( unreadable_copy->contains(_path) )
                return {};
            return std::make_shared<int>(1);
        };
    }
};

} // namespace

#define PREFIX "nc::core::GalleryThumbnailCache "

TEST_CASE(PREFIX "answers from memory and never generates while being asked")
{
    ScriptedGenerator generator;
    GalleryThumbnailCache cache{generator.Handler()};
    const auto row = Row("a.jpg", GalleryEligibility::Thumbnail);

    CHECK(cache.State("/a.jpg") == GalleryThumbnailState::Unknown);
    CHECK(cache.Known("/a.jpg") == nullptr);
    CHECK(cache.NeedsGeneration(row, "/a.jpg"));
    CHECK(generator.calls->load() == 0); // asking must never generate

    cache.Generate(row, "/a.jpg");
    CHECK(generator.calls->load() == 1);
    CHECK(cache.State("/a.jpg") == GalleryThumbnailState::Ready);
    CHECK(cache.Known("/a.jpg") != nullptr);
    CHECK_FALSE(cache.NeedsGeneration(row, "/a.jpg"));

    for( int i = 0; i < 5; ++i )
        CHECK(cache.Known("/a.jpg") != nullptr);
    CHECK(generator.calls->load() == 1);
}

TEST_CASE(PREFIX "never generates for a file whose bytes are not local")
{
    // The rule GL-1 exists for: generating is what would fetch them, and switching to Gallery is not
    // consent to a download.
    ScriptedGenerator generator;
    GalleryThumbnailCache cache{generator.Handler()};
    const auto placeholder = Row("cloud.jpg", GalleryEligibility::PlaceholderOnly);

    CHECK_FALSE(cache.NeedsGeneration(placeholder, "/cloud.jpg"));
    cache.Generate(placeholder, "/cloud.jpg");
    CHECK(generator.calls->load() == 0);
    // Recorded as withheld rather than left unknown, so nothing keeps reconsidering it and a surface
    // can say why the tile has no picture.
    CHECK(cache.State("/cloud.jpg") == GalleryThumbnailState::Withheld);
    CHECK(cache.Known("/cloud.jpg") == nullptr);
    CHECK_FALSE(cache.NeedsGeneration(placeholder, "/cloud.jpg"));
}

TEST_CASE(PREFIX "remembers a failure instead of retrying it on every redraw")
{
    // A folder holding one file the generator cannot read would otherwise re-attempt it forever.
    ScriptedGenerator generator;
    generator.unreadable->insert("/broken.jpg");
    GalleryThumbnailCache cache{generator.Handler()};
    const auto row = Row("broken.jpg", GalleryEligibility::Thumbnail);

    cache.Generate(row, "/broken.jpg");
    CHECK(cache.State("/broken.jpg") == GalleryThumbnailState::Failed);
    CHECK(cache.Known("/broken.jpg") == nullptr);
    CHECK_FALSE(cache.NeedsGeneration(row, "/broken.jpg"));
    CHECK(generator.calls->load() == 1);
}

TEST_CASE(PREFIX "treats a generator that threw as a failure, not a crash")
{
    GalleryThumbnailCache cache{
        [](const std::string &) -> GalleryThumbnailCache::Thumbnail { throw std::runtime_error{"boom"}; }};
    const auto row = Row("a.jpg", GalleryEligibility::Thumbnail);
    cache.Generate(row, "/a.jpg");
    CHECK(cache.State("/a.jpg") == GalleryThumbnailState::Failed);
}

TEST_CASE(PREFIX "holds only as many as it was told to, dropping what was asked for longest ago")
{
    // A folder can hold fifty thousand photographs, and an unbounded cache of them is a memory
    // problem the user did not ask for.
    ScriptedGenerator generator;
    GalleryThumbnailCache cache{generator.Handler(), 2};
    const auto row = Row("x.jpg", GalleryEligibility::Thumbnail);

    cache.Generate(row, "/1.jpg");
    cache.Generate(row, "/2.jpg");
    CHECK(cache.HeldCount() == 2);

    // Asking for the oldest again makes it the newest, so the other one goes instead.
    CHECK(cache.Known("/1.jpg") != nullptr);
    cache.Generate(row, "/3.jpg");
    CHECK(cache.HeldCount() == 2);
    CHECK(cache.State("/3.jpg") == GalleryThumbnailState::Ready);
    CHECK(cache.State("/2.jpg") == GalleryThumbnailState::Unknown);

    // And a dropped entry is generated again if it comes back into view.
    CHECK(cache.NeedsGeneration(row, "/2.jpg"));
}

TEST_CASE(PREFIX "forgets everything when the folder changes")
{
    // None of it applies any more, and holding it would spend memory on a folder nobody is looking
    // at.
    ScriptedGenerator generator;
    GalleryThumbnailCache cache{generator.Handler()};
    const auto row = Row("a.jpg", GalleryEligibility::Thumbnail);
    cache.Generate(row, "/a.jpg");
    REQUIRE(cache.HeldCount() == 1);

    cache.Clear();
    CHECK(cache.HeldCount() == 0);
    CHECK(cache.State("/a.jpg") == GalleryThumbnailState::Unknown);
    CHECK(cache.NeedsGeneration(row, "/a.jpg"));
}

TEST_CASE(PREFIX "does not generate for a folder row")
{
    ScriptedGenerator generator;
    GalleryThumbnailCache cache{generator.Handler()};
    const auto folder = Row("pictures", GalleryEligibility::Folder);
    CHECK_FALSE(cache.NeedsGeneration(folder, "/pictures"));
    cache.Generate(folder, "/pictures");
    CHECK(generator.calls->load() == 0);
    CHECK(cache.State("/pictures") == GalleryThumbnailState::Unknown);
}

#undef PREFIX
