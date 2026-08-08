// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Compare/FolderComparison.h>

#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using nc::core::CompareFolders;
using nc::core::FolderCompareEntry;
using nc::core::FolderCompareFailure;
using nc::core::FolderCompareItem;
using nc::core::FolderCompareNewerSide;
using nc::core::FolderCompareOptions;
using nc::core::FolderCompareStatus;
using nc::core::FolderCompareSummary;
using nc::core::FolderComparison;

FolderCompareItem File(std::string _name, const uint64_t _size, const int64_t _time)
{
    return {.name = std::move(_name), .size = _size, .modification_time = _time, .is_directory = false};
}

FolderCompareItem Directory(std::string _name)
{
    return {.name = std::move(_name), .size = 0, .modification_time = 0, .is_directory = true};
}

const FolderCompareEntry *Find(const FolderComparison &_comparison, const std::string_view _name)
{
    for( const FolderCompareEntry &entry : _comparison.entries )
        if( entry.name == _name )
            return &entry;
    return nullptr;
}

FolderComparison Compare(const std::vector<FolderCompareItem> &_left,
                         const std::vector<FolderCompareItem> &_right,
                         const FolderCompareOptions &_options = {})
{
    auto result = CompareFolders(_left, _right, _options);
    REQUIRE(result);
    return std::move(*result);
}

} // namespace

#define PREFIX "nc::core::CompareFolders "

TEST_CASE(PREFIX "classifies every P0 status in one pass")
{
    const std::vector<FolderCompareItem> left{
        File("same.txt", 10, 1000),
        File("changed-size.txt", 10, 1000),
        File("changed-time.txt", 10, 1000),
        File("left-only.txt", 1, 1),
        Directory("shared-folder"),
        Directory("type-clash"),
    };
    const std::vector<FolderCompareItem> right{
        File("same.txt", 10, 1000),
        File("changed-size.txt", 11, 1000),
        File("changed-time.txt", 10, 2000),
        File("right-only.txt", 1, 1),
        Directory("shared-folder"),
        File("type-clash", 5, 5),
    };

    const FolderComparison comparison = Compare(left, right);

    REQUIRE(comparison.entries.size() == 7);
    CHECK(Find(comparison, "same.txt")->status == FolderCompareStatus::Same);
    CHECK(Find(comparison, "changed-size.txt")->status == FolderCompareStatus::Changed);
    CHECK(Find(comparison, "changed-time.txt")->status == FolderCompareStatus::Changed);
    CHECK(Find(comparison, "left-only.txt")->status == FolderCompareStatus::LeftOnly);
    CHECK(Find(comparison, "right-only.txt")->status == FolderCompareStatus::RightOnly);
    CHECK(Find(comparison, "shared-folder")->status == FolderCompareStatus::Same);
    CHECK(Find(comparison, "type-clash")->status == FolderCompareStatus::Conflict);

    CHECK(comparison.Summarize() ==
          FolderCompareSummary{.same = 2, .left_only = 1, .right_only = 1, .changed = 2, .conflict = 1});
    CHECK_FALSE(comparison.Identical());
}

TEST_CASE(PREFIX "orders left input first and appends right-only names in right order")
{
    const std::vector<FolderCompareItem> left{File("b", 1, 1), File("a", 1, 1)};
    const std::vector<FolderCompareItem> right{File("z", 1, 1), File("a", 1, 1), File("y", 1, 1)};

    const FolderComparison comparison = Compare(left, right);

    REQUIRE(comparison.entries.size() == 4);
    CHECK(comparison.entries[0].name == "b");
    CHECK(comparison.entries[1].name == "a");
    CHECK(comparison.entries[2].name == "z");
    CHECK(comparison.entries[3].name == "y");

    // Indices address the caller's own listings, so a consumer can mark the exact rows it passed in.
    CHECK(comparison.entries[0].left_index == 0);
    CHECK_FALSE(comparison.entries[0].right_index);
    CHECK(comparison.entries[1].left_index == 1);
    CHECK(comparison.entries[1].right_index == 1);
    CHECK_FALSE(comparison.entries[2].left_index);
    CHECK(comparison.entries[2].right_index == 0);
    CHECK(comparison.entries[3].right_index == 2);
}

TEST_CASE(PREFIX "reports the newer side of a matched file pair independently of the status")
{
    const std::vector<FolderCompareItem> left{File("older", 10, 1000), File("newer", 10, 3000), File("tie", 10, 1000)};
    const std::vector<FolderCompareItem> right{File("older", 10, 2000), File("newer", 10, 1000), File("tie", 10, 1000)};

    SECTION("time is one of the criteria")
    {
        const FolderComparison comparison = Compare(left, right);
        CHECK(Find(comparison, "older")->newer_side == FolderCompareNewerSide::Right);
        CHECK(Find(comparison, "newer")->newer_side == FolderCompareNewerSide::Left);
        CHECK(Find(comparison, "tie")->newer_side == FolderCompareNewerSide::Neither);
    }
    SECTION("comparing by size alone still records which side is newer")
    {
        // A one-way sync needs the direction even when the criteria call the pair Same.
        const FolderComparison comparison =
            Compare(left, right, {.compare_size = true, .compare_modification_time = false});
        CHECK(Find(comparison, "older")->status == FolderCompareStatus::Same);
        CHECK(Find(comparison, "older")->newer_side == FolderCompareNewerSide::Right);
        CHECK(comparison.Identical());
    }
    SECTION("an unmatched entry, a directory pair and a conflict carry no direction")
    {
        const FolderComparison comparison = Compare({File("gone", 1, 9000), Directory("dir"), Directory("clash")},
                                                    {Directory("dir"), File("clash", 1, 1)});
        CHECK(Find(comparison, "gone")->newer_side == FolderCompareNewerSide::Neither);
        CHECK(Find(comparison, "dir")->newer_side == FolderCompareNewerSide::Neither);
        CHECK(Find(comparison, "clash")->newer_side == FolderCompareNewerSide::Neither);
    }
}

TEST_CASE(PREFIX "honours the selected criteria and the timestamp tolerance")
{
    const std::vector<FolderCompareItem> left{File("a", 10, 1000)};
    const std::vector<FolderCompareItem> right{File("a", 20, 1002)};

    SECTION("size only")
    {
        const FolderComparison comparison =
            Compare(left, right, {.compare_size = true, .compare_modification_time = false});
        CHECK(comparison.entries[0].status == FolderCompareStatus::Changed);
    }
    SECTION("time only")
    {
        const FolderComparison comparison =
            Compare(left, right, {.compare_size = false, .compare_modification_time = true});
        CHECK(comparison.entries[0].status == FolderCompareStatus::Changed);
    }
    SECTION("neither criterion compares by presence alone")
    {
        const FolderComparison comparison =
            Compare(left, right, {.compare_size = false, .compare_modification_time = false});
        CHECK(comparison.entries[0].status == FolderCompareStatus::Same);
        CHECK(comparison.Identical());
    }
    SECTION("a tolerance absorbs a coarser filesystem's timestamp granularity")
    {
        const FolderCompareOptions within{
            .compare_size = false, .compare_modification_time = true, .modification_time_tolerance = 2};
        CHECK(Compare(left, right, within).entries[0].status == FolderCompareStatus::Same);
        CHECK(Compare(left, right, within).entries[0].newer_side == FolderCompareNewerSide::Neither);

        const FolderCompareOptions just_below{
            .compare_size = false, .compare_modification_time = true, .modification_time_tolerance = 1};
        CHECK(Compare(left, right, just_below).entries[0].status == FolderCompareStatus::Changed);
    }
}

TEST_CASE(PREFIX "matches names byte-exactly so an unresolved pairing stays visible")
{
    // Case and Unicode-normalization differences are deliberately reported as two distinct names
    // rather than silently paired - see the contract note on CompareFolders.
    const FolderComparison cased = Compare({File("README", 1, 1)}, {File("readme", 1, 1)});
    REQUIRE(cased.entries.size() == 2);
    CHECK(cased.entries[0].status == FolderCompareStatus::LeftOnly);
    CHECK(cased.entries[1].status == FolderCompareStatus::RightOnly);

    const std::string composed = "e\xCC\x81";  // NFD "e" + combining acute
    const std::string precomposed = "\xC3\xA9"; // NFC "é"
    const FolderComparison normalized = Compare({File(composed, 1, 1)}, {File(precomposed, 1, 1)});
    REQUIRE(normalized.entries.size() == 2);
    CHECK(normalized.entries[0].status == FolderCompareStatus::LeftOnly);
    CHECK(normalized.entries[1].status == FolderCompareStatus::RightOnly);
}

TEST_CASE(PREFIX "rejects an input that is not a valid listing instead of guessing")
{
    const std::vector<FolderCompareItem> valid{File("a", 1, 1)};

    SECTION("duplicate names on either side")
    {
        const std::vector<FolderCompareItem> duplicated{File("a", 1, 1), File("a", 2, 2)};
        CHECK(CompareFolders(duplicated, valid).error() == FolderCompareFailure::DuplicateName);
        CHECK(CompareFolders(valid, duplicated).error() == FolderCompareFailure::DuplicateName);
    }
    SECTION("an empty name")
    {
        const std::vector<FolderCompareItem> empty_name{File("", 1, 1)};
        CHECK(CompareFolders(empty_name, valid).error() == FolderCompareFailure::EmptyName);
        CHECK(CompareFolders(valid, empty_name).error() == FolderCompareFailure::EmptyName);
    }
    SECTION("navigation entries are never comparison subjects")
    {
        CHECK(CompareFolders(std::vector{File("..", 0, 0)}, valid).error() == FolderCompareFailure::ReservedName);
        CHECK(CompareFolders(valid, std::vector{File(".", 0, 0)}).error() == FolderCompareFailure::ReservedName);
    }
    SECTION("a negative tolerance is not a usable criterion")
    {
        CHECK(CompareFolders(valid, valid, {.modification_time_tolerance = -1}).error() ==
              FolderCompareFailure::NegativeTolerance);
    }
}

TEST_CASE(PREFIX "handles empty and identical listings without inventing work")
{
    SECTION("both sides empty")
    {
        const FolderComparison comparison = Compare({}, {});
        CHECK(comparison.entries.empty());
        CHECK(comparison.Summarize() == FolderCompareSummary{});
        // An empty comparison has nothing that differs, so there is nothing for a sync to do.
        CHECK(comparison.Identical());
    }
    SECTION("one side empty")
    {
        const FolderComparison from_left = Compare({File("a", 1, 1), Directory("d")}, {});
        CHECK(from_left.Summarize() == FolderCompareSummary{.left_only = 2});
        const FolderComparison from_right = Compare({}, {File("a", 1, 1), Directory("d")});
        CHECK(from_right.Summarize() == FolderCompareSummary{.right_only = 2});
    }
    SECTION("identical listings")
    {
        const std::vector<FolderCompareItem> items{File("a", 1, 1), Directory("d"), File("b", 2, 2)};
        const FolderComparison comparison = Compare(items, items);
        CHECK(comparison.entries.size() == 3);
        CHECK(comparison.Identical());
        CHECK(comparison.Summarize() == FolderCompareSummary{.same = 3});
    }
}

TEST_CASE(PREFIX "keeps extreme timestamps from overflowing into the opposite verdict")
{
    const int64_t minimum = std::numeric_limits<int64_t>::min();
    const int64_t maximum = std::numeric_limits<int64_t>::max();

    const FolderComparison left_newer = Compare({File("a", 1, maximum)}, {File("a", 1, minimum)});
    CHECK(left_newer.entries[0].newer_side == FolderCompareNewerSide::Left);
    CHECK(left_newer.entries[0].status == FolderCompareStatus::Changed);

    const FolderComparison right_newer = Compare({File("a", 1, minimum)}, {File("a", 1, maximum)});
    CHECK(right_newer.entries[0].newer_side == FolderCompareNewerSide::Right);

    // The full span between the extremes is 2^64-1 seconds, genuinely wider than the largest
    // expressible tolerance, so even that tolerance must still report the pair as different.
    const FolderComparison beyond_tolerance =
        Compare({File("a", 1, maximum)},
                {File("a", 1, minimum)},
                {.compare_size = false, .compare_modification_time = true, .modification_time_tolerance = maximum});
    CHECK(beyond_tolerance.entries[0].status == FolderCompareStatus::Changed);

    // A difference that exactly fills a maximal tolerance is absorbed, without the comparison
    // wrapping around in either direction.
    const FolderComparison within_tolerance =
        Compare({File("a", 1, 0)},
                {File("a", 1, maximum)},
                {.compare_size = false, .compare_modification_time = true, .modification_time_tolerance = maximum});
    CHECK(within_tolerance.entries[0].status == FolderCompareStatus::Same);
    CHECK(within_tolerance.entries[0].newer_side == FolderCompareNewerSide::Neither);
}
