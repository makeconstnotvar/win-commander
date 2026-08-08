// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Compare/RecursiveFolderComparison.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using nc::core::CompareFoldersRecursively;
using nc::core::FolderCompareItem;
using nc::core::FolderCompareStatus;
using nc::core::RecursiveFolderCompareEntry;
using nc::core::RecursiveFolderCompareFailure;
using nc::core::RecursiveFolderCompareLimits;
using nc::core::RecursiveFolderCompareLister;

/** A tree described by relative directory path, with the directories that cannot be read named. */
struct Tree {
    std::map<std::string, std::vector<FolderCompareItem>> directories;
    std::set<std::string> unreadable;

    [[nodiscard]] RecursiveFolderCompareLister Lister() const
    {
        return [this](const std::string &_path) -> std::optional<std::vector<FolderCompareItem>> {
            if( unreadable.contains(_path) )
                return std::nullopt;
            const auto found = directories.find(_path);
            return found == directories.end() ? std::vector<FolderCompareItem>{} : found->second;
        };
    }
};

FolderCompareItem File(const std::string &_name, const uint64_t _size = 1, const int64_t _time = 100)
{
    return FolderCompareItem{.name = _name, .size = _size, .modification_time = _time, .is_directory = false};
}

FolderCompareItem Dir(const std::string &_name)
{
    return FolderCompareItem{.name = _name, .size = 0, .modification_time = 0, .is_directory = true};
}

[[nodiscard]] const RecursiveFolderCompareEntry *Find(const nc::core::RecursiveFolderComparison &_comparison,
                                                      const std::string &_path)
{
    for( const auto &entry : _comparison.entries )
        if( entry.relative_path == _path )
            return &entry;
    return nullptr;
}

} // namespace

#define PREFIX "nc::core::CompareFoldersRecursively "

TEST_CASE(PREFIX "reports a directory by what its subtree says, not by its own presence")
{
    // The gap this closes. One level alone sees a directory on both sides and reports Same, claiming
    // nothing about its contents - and a sync reading that as "nothing to do" skips a subtree that
    // is not identical.
    Tree left, right;
    left.directories[""] = {Dir("shared")};
    right.directories[""] = {Dir("shared")};
    left.directories["shared"] = {File("a.txt", 1)};
    right.directories["shared"] = {File("a.txt", 2)};

    const auto comparison = CompareFoldersRecursively(left.Lister(), right.Lister());
    REQUIRE(comparison);

    const auto *directory = Find(*comparison, "shared");
    REQUIRE(directory != nullptr);
    CHECK(directory->status == FolderCompareStatus::Changed);
    CHECK(directory->is_directory);
    CHECK(directory->depth == 0);

    const auto *file = Find(*comparison, "shared/a.txt");
    REQUIRE(file != nullptr);
    CHECK(file->status == FolderCompareStatus::Changed);
    CHECK(file->depth == 1);
    CHECK_FALSE(comparison->Identical());
}

TEST_CASE(PREFIX "leaves an identical tree identical, all the way down")
{
    Tree left, right;
    for( Tree *tree : {&left, &right} ) {
        tree->directories[""] = {Dir("one")};
        tree->directories["one"] = {Dir("two"), File("a.txt")};
        tree->directories["one/two"] = {File("b.txt")};
    }

    const auto comparison = CompareFoldersRecursively(left.Lister(), right.Lister());
    REQUIRE(comparison);
    CHECK(comparison->Identical());
    CHECK(comparison->entries.size() == 4);
    CHECK(comparison->Summarize().same == 4);

    // Depth-first with parents before their children, so a display can indent by depth alone.
    CHECK(comparison->entries[0].relative_path == "one");
    CHECK(comparison->entries[1].relative_path == "one/two");
    CHECK(comparison->entries[2].relative_path == "one/two/b.txt");
    CHECK(comparison->entries[3].relative_path == "one/a.txt");
}

TEST_CASE(PREFIX "reports a one-sided directory once, whole")
{
    // Enumerating what is inside something the other side does not have at all adds nothing a sync
    // can act on separately, and would bury the one entry that matters.
    Tree left, right;
    left.directories[""] = {Dir("only-left")};
    left.directories["only-left"] = {File("a.txt"), File("b.txt")};
    right.directories[""] = {};

    const auto comparison = CompareFoldersRecursively(left.Lister(), right.Lister());
    REQUIRE(comparison);
    REQUIRE(comparison->entries.size() == 1);
    CHECK(comparison->entries[0].relative_path == "only-left");
    CHECK(comparison->entries[0].status == FolderCompareStatus::LeftOnly);
}

TEST_CASE(PREFIX "does not walk into a directory facing a file")
{
    // There is no shared structure to walk, and pairing a directory's children against a file's
    // non-existent ones would be inventing a comparison.
    Tree left, right;
    left.directories[""] = {Dir("name")};
    left.directories["name"] = {File("inside.txt")};
    right.directories[""] = {File("name")};

    const auto comparison = CompareFoldersRecursively(left.Lister(), right.Lister());
    REQUIRE(comparison);
    REQUIRE(comparison->entries.size() == 1);
    CHECK(comparison->entries[0].status == FolderCompareStatus::Conflict);
}

TEST_CASE(PREFIX "fails rather than reporting an unreadable directory as empty")
{
    // The two are indistinguishable in the result, and the difference is everything: one means
    // "nothing inside", the other "we do not know" - and a sync acting on the first would delete a
    // subtree it never saw.
    Tree left, right;
    left.directories[""] = {Dir("locked")};
    right.directories[""] = {Dir("locked")};
    left.directories["locked"] = {File("a.txt")};
    right.unreadable.insert("locked");

    const auto comparison = CompareFoldersRecursively(left.Lister(), right.Lister());
    REQUIRE_FALSE(comparison);
    CHECK(comparison.error() == RecursiveFolderCompareFailure::Unreadable);

    // Including when it is a root that cannot be read.
    Tree unreadable_root;
    unreadable_root.unreadable.insert("");
    CHECK_FALSE(CompareFoldersRecursively(unreadable_root.Lister(), right.Lister()).has_value());
    // And when there is no lister at all.
    CHECK_FALSE(CompareFoldersRecursively({}, right.Lister()).has_value());
}

TEST_CASE(PREFIX "fails on its own budgets rather than answering with less")
{
    // A truncated comparison is indistinguishable from a complete one that found less, which is
    // exactly how a sync deletes what it never looked at.
    Tree left, right;
    for( Tree *tree : {&left, &right} ) {
        tree->directories[""] = {Dir("a")};
        tree->directories["a"] = {Dir("b")};
        tree->directories["a/b"] = {Dir("c")};
        tree->directories["a/b/c"] = {File("deep.txt")};
    }

    const auto too_deep = CompareFoldersRecursively(left.Lister(), right.Lister(), {},
                                                     RecursiveFolderCompareLimits{.maximum_depth = 1});
    REQUIRE_FALSE(too_deep);
    CHECK(too_deep.error() == RecursiveFolderCompareFailure::TooDeep);

    const auto too_large = CompareFoldersRecursively(left.Lister(), right.Lister(), {},
                                                      RecursiveFolderCompareLimits{.maximum_entries = 2});
    REQUIRE_FALSE(too_large);
    CHECK(too_large.error() == RecursiveFolderCompareFailure::TooLarge);

    // A budget that fits answers in full.
    CHECK(CompareFoldersRecursively(left.Lister(), right.Lister()).has_value());
}

TEST_CASE(PREFIX "stops when cancelled, and when the cancel checker itself is broken")
{
    Tree left, right;
    for( Tree *tree : {&left, &right} ) {
        tree->directories[""] = {Dir("a")};
        tree->directories["a"] = {File("x.txt")};
    }

    const auto cancelled = CompareFoldersRecursively(left.Lister(), right.Lister(), {}, {}, [] { return true; });
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error() == RecursiveFolderCompareFailure::Cancelled);

    // A checker that threw told us nothing, and continuing on a broken predicate is how a cancelled
    // walk keeps going.
    const auto broken = CompareFoldersRecursively(left.Lister(), right.Lister(), {}, {},
                                                   []() -> bool { throw std::runtime_error{"boom"}; });
    REQUIRE_FALSE(broken);
    CHECK(broken.error() == RecursiveFolderCompareFailure::Cancelled);
}

TEST_CASE(PREFIX "carries a nested difference all the way up to the root's own child")
{
    // The property a sync depends on: no directory above a difference may read as Same, however deep
    // the difference is.
    Tree left, right;
    for( Tree *tree : {&left, &right} ) {
        tree->directories[""] = {Dir("a")};
        tree->directories["a"] = {Dir("b")};
        tree->directories["a/b"] = {Dir("c")};
    }
    left.directories["a/b/c"] = {File("deep.txt", 1)};
    right.directories["a/b/c"] = {File("deep.txt", 999)};

    const auto comparison = CompareFoldersRecursively(left.Lister(), right.Lister());
    REQUIRE(comparison);
    for( const std::string &path : {"a", "a/b", "a/b/c", "a/b/c/deep.txt"} ) {
        const auto *entry = Find(*comparison, path);
        REQUIRE(entry != nullptr);
        CHECK(entry->status == FolderCompareStatus::Changed);
    }
    CHECK_FALSE(comparison->Identical());
}

#undef PREFIX
