// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Commands/CommandPaletteFilter.h>

#include <string>
#include <vector>

namespace {

using nc::core::CommandPaletteEntry;
using nc::core::CommandPaletteMatch;
using nc::core::CommandPaletteMatchField;
using nc::core::CommandPaletteOptions;
using nc::core::FilterCommandPalette;

CommandPaletteEntry Entry(std::string _id, std::string _title, std::string _subtitle = {}, const bool _enabled = true)
{
    return {.id = std::move(_id),
            .title = std::move(_title),
            .subtitle = std::move(_subtitle),
            .enabled = _enabled};
}

/** Ids of the results, in ranked order - the only ordering the palette's users ever perceive. */
std::vector<std::string> RankedIds(const std::vector<CommandPaletteEntry> &_entries,
                                   const std::string_view _query,
                                   const CommandPaletteOptions &_options = {})
{
    std::vector<std::string> ids;
    for( const CommandPaletteMatch &match : FilterCommandPalette(_entries, _query, _options) )
        ids.push_back(_entries[match.entry_index].id);
    return ids;
}

std::vector<CommandPaletteEntry> Roster()
{
    return {
        Entry("file.copy", "Copy"),
        Entry("file.copyPath", "Copy Item Path"),
        Entry("file.paste", "Paste"),
        Entry("view.toggleHiddenFiles", "Show Hidden Files", "View"),
        Entry("archive.create", "Compress Selection", "File · Archive"),
        Entry("operationCenter.open", "Open Operation Center", "Window"),
    };
}

} // namespace

#define PREFIX "nc::core::FilterCommandPalette "

TEST_CASE(PREFIX "returns the whole roster in its own order for an empty query")
{
    const auto matches = FilterCommandPalette(Roster(), "");
    REQUIRE(matches.size() == Roster().size());
    for( size_t index = 0; index < matches.size(); ++index ) {
        CHECK(matches[index].entry_index == index);
        CHECK(matches[index].score == 0);
        CHECK(matches[index].offsets.empty());
    }
}

TEST_CASE(PREFIX "ranks a verbatim title hit above a subsequence one")
{
    const std::vector<CommandPaletteEntry> entries{
        Entry("subsequence", "Create OtherProject Yield"), // c-o-p-y scattered
        Entry("verbatim", "Copy"),
    };
    CHECK(RankedIds(entries, "copy") == std::vector<std::string>{"verbatim", "subsequence"});
}

TEST_CASE(PREFIX "prefers a hit at the start, then at a word boundary, then anywhere")
{
    const std::vector<CommandPaletteEntry> entries{
        Entry("anywhere", "Recompress Archive"),
        Entry("boundary", "Archive Compress"),
        Entry("start", "Compress Selection"),
    };
    CHECK(RankedIds(entries, "compress") == std::vector<std::string>{"start", "boundary", "anywhere"});
}

TEST_CASE(PREFIX "prefers the tighter of two titles containing the query")
{
    const std::vector<CommandPaletteEntry> entries{
        Entry("long", "Copy Item Path to the System Clipboard"),
        Entry("short", "Copy"),
    };
    CHECK(RankedIds(entries, "copy") == std::vector<std::string>{"short", "long"});
}

TEST_CASE(PREFIX "never lets a subtitle hit overtake a title hit")
{
    const std::vector<CommandPaletteEntry> entries{
        // An exact, start-anchored subtitle hit against a merely scattered title hit.
        Entry("subtitle-exact", "Show Hidden Files", "view"),
        Entry("title-scattered", "Verify Integrity of Every Winning archive"),
    };
    const auto matches = FilterCommandPalette(entries, "view");
    REQUIRE(matches.size() == 2);
    CHECK(entries[matches[0].entry_index].id == "title-scattered");
    CHECK(matches[0].field == CommandPaletteMatchField::Title);
    CHECK(entries[matches[1].entry_index].id == "subtitle-exact");
    CHECK(matches[1].field == CommandPaletteMatchField::Subtitle);
}

TEST_CASE(PREFIX "matches case-insensitively and reports the offsets a row would highlight")
{
    const std::vector<CommandPaletteEntry> entries{Entry("id", "Copy Item Path")};

    SECTION("verbatim offsets are contiguous")
    {
        const auto matches = FilterCommandPalette(entries, "ITEM");
        REQUIRE(matches.size() == 1);
        CHECK(matches[0].offsets == std::vector<size_t>{5, 6, 7, 8});
    }
    SECTION("subsequence offsets are the matched positions, ascending")
    {
        const auto matches = FilterCommandPalette(entries, "cip");
        REQUIRE(matches.size() == 1);
        // C(0) ... I(5) ... P(10)
        CHECK(matches[0].offsets == std::vector<size_t>{0, 5, 10});
        CHECK(matches[0].field == CommandPaletteMatchField::Title);
    }
}

TEST_CASE(PREFIX "rewards word-boundary initials, which is how a palette is actually typed")
{
    const std::vector<CommandPaletteEntry> entries{
        Entry("noise", "Ocean Photograph Cropper"),
        Entry("initials", "Open Operation Center"),
    };
    // "ooc" is the three word initials of the second entry.
    CHECK(RankedIds(entries, "ooc").front() == "initials");
}

TEST_CASE(PREFIX "drops entries the query cannot reach at all")
{
    CHECK(RankedIds(Roster(), "zzz").empty());
    // Order matters for a subsequence: the characters must appear left to right.
    CHECK(RankedIds(std::vector{Entry("id", "Copy")}, "ypoc").empty());
}

TEST_CASE(PREFIX "keeps equal-scoring rows in a fixed roster order across queries")
{
    // Two identical titles can only be separated by roster position; that order must be stable, or
    // the selected row silently becomes a different command between keystrokes.
    const std::vector<CommandPaletteEntry> entries{
        Entry("first", "Duplicate"),
        Entry("second", "Duplicate"),
        Entry("third", "Duplicate"),
    };
    const std::vector<std::string> expected{"first", "second", "third"};
    CHECK(RankedIds(entries, "dup") == expected);
    CHECK(RankedIds(entries, "duplicate") == expected);
    CHECK(RankedIds(entries, "e") == expected);
}

TEST_CASE(PREFIX "honours the result bound and the disabled-entry filter")
{
    SECTION("result bound keeps the highest-ranked rows")
    {
        const auto limited = RankedIds(Roster(), "", {.maximum_results = 2});
        CHECK(limited == std::vector<std::string>{"file.copy", "file.copyPath"});
        CHECK(RankedIds(Roster(), "copy", {.maximum_results = 1}) == std::vector<std::string>{"file.copy"});
    }
    SECTION("a zero bound returns nothing rather than everything")
    {
        CHECK(FilterCommandPalette(Roster(), "copy", {.maximum_results = 0}).empty());
    }
    SECTION("disabled entries are shown by default and can be excluded")
    {
        const std::vector<CommandPaletteEntry> entries{
            Entry("available", "Copy"),
            Entry("blocked", "Copy Item Path", {}, false),
        };
        CHECK(RankedIds(entries, "copy").size() == 2);
        CHECK(RankedIds(entries, "copy", {.exclude_disabled = true}) == std::vector<std::string>{"available"});
    }
}

TEST_CASE("nc::core::BuildCommandPaletteRoster never offers a command the registry hides")
{
    using nc::core::BuildCommandPaletteRoster;
    using nc::core::CommandPaletteSource;

    const std::vector<CommandPaletteSource> sources{
        {.id = "file.copy", .title = "Copy", .category = "File", .visible = true, .enabled = true},
        // Hidden: the registry says this command does not apply here at all.
        {.id = "file.paste", .title = "Paste", .category = "File", .visible = false, .enabled = true},
        // Disabled but applicable: it belongs in the roster, greyed out.
        {.id = "file.trash", .title = "Move to Trash", .category = "File", .visible = true, .enabled = false},
        // Unusable rows: listable but never meaningfully choosable.
        {.id = "", .title = "No Identity", .category = "File"},
        {.id = "no.title", .title = "", .category = "File"},
    };

    const auto roster = BuildCommandPaletteRoster(sources);
    REQUIRE(roster.size() == 2);
    CHECK(roster[0].id == "file.copy");
    CHECK(roster[0].enabled);
    CHECK(roster[1].id == "file.trash");
    CHECK_FALSE(roster[1].enabled);
    // The category becomes the searchable subtitle.
    CHECK(roster[0].subtitle == "File");

    // A hidden command must not be reachable by querying for it either.
    CHECK(FilterCommandPalette(roster, "paste").empty());
    CHECK(BuildCommandPaletteRoster({}).empty());
}

TEST_CASE(PREFIX "handles an empty roster and empty entry text without matching on nothing")
{
    CHECK(FilterCommandPalette({}, "copy").empty());
    CHECK(FilterCommandPalette({}, "").empty());

    // An entry with no title must not be matched via its absent text.
    const std::vector<CommandPaletteEntry> blank{Entry("blank", "", "")};
    CHECK(FilterCommandPalette(blank, "a").empty());
    // ...but an empty query still lists it, because listing is not matching.
    CHECK(FilterCommandPalette(blank, "").size() == 1);
}
