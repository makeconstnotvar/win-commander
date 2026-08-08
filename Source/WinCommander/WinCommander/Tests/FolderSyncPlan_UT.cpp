// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Compare/FolderSyncPlan.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using nc::core::CompareFolders;
using nc::core::FolderCompareItem;
using nc::core::FolderComparison;
using nc::core::FolderSyncAction;
using nc::core::FolderSyncActionKind;
using nc::core::FolderSyncDirection;
using nc::core::FolderSyncOptions;
using nc::core::FolderSyncPlan;
using nc::core::FolderSyncSkipReason;
using nc::core::FolderSyncSummary;
using nc::core::PlanOneWaySync;

FolderCompareItem File(std::string _name, const uint64_t _size, const int64_t _time)
{
    return {.name = std::move(_name), .size = _size, .modification_time = _time, .is_directory = false};
}

FolderCompareItem Directory(std::string _name)
{
    return {.name = std::move(_name), .size = 0, .modification_time = 0, .is_directory = true};
}

FolderComparison Compare(const std::vector<FolderCompareItem> &_left, const std::vector<FolderCompareItem> &_right)
{
    auto result = CompareFolders(_left, _right);
    REQUIRE(result);
    return std::move(*result);
}

const FolderSyncAction *Find(const FolderSyncPlan &_plan, const std::string_view _name)
{
    for( const FolderSyncAction &action : _plan.actions )
        if( action.name == _name )
            return &action;
    return nullptr;
}

/** left: source-only, shared-changed, shared-same, dir-on-both, clash. right adds its own extra. */
FolderComparison MixedComparison()
{
    return Compare(
        {
            File("source-only", 1, 1000),
            File("changed", 1, 3000),
            File("identical", 1, 1000),
            Directory("shared-dir"),
            Directory("clash"),
        },
        {
            File("changed", 2, 1000),
            File("identical", 1, 1000),
            Directory("shared-dir"),
            File("clash", 1, 1),
            File("destination-only", 1, 1),
        });
}

} // namespace

#define PREFIX "nc::core::PlanOneWaySync "

TEST_CASE(PREFIX "produces exactly one action per compared name so a preview omits nothing")
{
    const FolderComparison comparison = MixedComparison();
    const FolderSyncPlan plan = PlanOneWaySync(comparison, FolderSyncDirection::LeftToRight);
    CHECK(plan.actions.size() == comparison.entries.size());
    for( const auto &entry : comparison.entries )
        CHECK(Find(plan, entry.name) != nullptr);
}

TEST_CASE(PREFIX "maps each comparison status onto its one-way action")
{
    const FolderSyncPlan plan = PlanOneWaySync(MixedComparison(), FolderSyncDirection::LeftToRight);

    CHECK(Find(plan, "source-only")->kind == FolderSyncActionKind::Create);
    CHECK(Find(plan, "changed")->kind == FolderSyncActionKind::Overwrite);
    CHECK(Find(plan, "identical")->kind == FolderSyncActionKind::Skip);
    CHECK(Find(plan, "identical")->skip_reason == FolderSyncSkipReason::Identical);

    // A directory pair is Same by presence only, so the plan must not claim its contents match.
    CHECK(Find(plan, "shared-dir")->kind == FolderSyncActionKind::Skip);
    CHECK(Find(plan, "shared-dir")->skip_reason == FolderSyncSkipReason::DirectoryContentsNotCompared);

    // A directory facing a file has no safe direction and is never resolved silently.
    CHECK(Find(plan, "clash")->kind == FolderSyncActionKind::Skip);
    CHECK(Find(plan, "clash")->skip_reason == FolderSyncSkipReason::TypeConflict);

    // Deletion is off by default, so an extraneous destination entry is reported, not removed.
    CHECK(Find(plan, "destination-only")->kind == FolderSyncActionKind::Skip);
    CHECK(Find(plan, "destination-only")->skip_reason == FolderSyncSkipReason::DeletionNotRequested);
    CHECK_FALSE(plan.HasDeletions());
    CHECK_FALSE(plan.IsEmpty());
    CHECK(plan.Summarize() == FolderSyncSummary{.create = 1, .overwrite = 1, .skip = 4});
}

TEST_CASE(PREFIX "reverses source and destination with the direction")
{
    const FolderComparison comparison = MixedComparison();
    const FolderSyncPlan plan = PlanOneWaySync(comparison, FolderSyncDirection::RightToLeft);

    CHECK(plan.direction == FolderSyncDirection::RightToLeft);
    // What was source-only left-to-right is now an extraneous destination entry, and vice versa.
    CHECK(Find(plan, "source-only")->skip_reason == FolderSyncSkipReason::DeletionNotRequested);
    CHECK(Find(plan, "destination-only")->kind == FolderSyncActionKind::Create);
    CHECK(Find(plan, "changed")->kind == FolderSyncActionKind::Overwrite);
    CHECK(Find(plan, "clash")->skip_reason == FolderSyncSkipReason::TypeConflict);

    // Indices follow the relabelling: the source index now addresses the right listing.
    CHECK(Find(plan, "destination-only")->source_index == 4);
    CHECK_FALSE(Find(plan, "destination-only")->destination_index);
}

TEST_CASE(PREFIX "treats deletion as opt-in and exposes it for the mandatory preview")
{
    const FolderComparison comparison = MixedComparison();
    const FolderSyncPlan plan =
        PlanOneWaySync(comparison, FolderSyncDirection::LeftToRight, {.delete_extraneous = true});

    const FolderSyncAction *const removed = Find(plan, "destination-only");
    CHECK(removed->kind == FolderSyncActionKind::Delete);
    CHECK(removed->destination_index == 4);
    CHECK_FALSE(removed->source_index);

    CHECK(plan.HasDeletions());
    const std::vector<const FolderSyncAction *> deletions = plan.Deletions();
    REQUIRE(deletions.size() == 1);
    CHECK(deletions[0]->name == "destination-only");
    CHECK(plan.Summarize() == FolderSyncSummary{.create = 1, .overwrite = 1, .remove = 1, .skip = 3});
}

TEST_CASE(PREFIX "flags an overwrite that would replace a newer destination copy")
{
    // The destination holds the more recent copy; a one-way sync still proceeds, but this is the
    // case a preview has to call out, because the user is about to lose the newer of the two.
    const FolderComparison comparison = Compare({File("a", 1, 1000), File("b", 1, 5000)},
                                                {File("a", 1, 5000), File("b", 1, 1000)});
    const FolderSyncPlan plan = PlanOneWaySync(comparison, FolderSyncDirection::LeftToRight);

    CHECK(Find(plan, "a")->kind == FolderSyncActionKind::Overwrite);
    CHECK(Find(plan, "a")->overwrites_newer_destination);
    CHECK(Find(plan, "b")->kind == FolderSyncActionKind::Overwrite);
    CHECK_FALSE(Find(plan, "b")->overwrites_newer_destination);
    CHECK(plan.Summarize() == FolderSyncSummary{.overwrite = 2, .overwrite_newer_destination = 1});

    const FolderSyncPlan reversed = PlanOneWaySync(comparison, FolderSyncDirection::RightToLeft);
    CHECK_FALSE(Find(reversed, "a")->overwrites_newer_destination);
    CHECK(Find(reversed, "b")->overwrites_newer_destination);
}

TEST_CASE(PREFIX "honours disabled overwrite and reports an inert plan as empty")
{
    const FolderComparison comparison = MixedComparison();

    const FolderSyncPlan no_overwrite =
        PlanOneWaySync(comparison, FolderSyncDirection::LeftToRight, {.overwrite_changed = false});
    CHECK(Find(no_overwrite, "changed")->kind == FolderSyncActionKind::Skip);
    CHECK(Find(no_overwrite, "changed")->skip_reason == FolderSyncSkipReason::OverwriteNotRequested);
    CHECK_FALSE(no_overwrite.IsEmpty()); // the Create is still real work

    const FolderSyncPlan nothing_to_do =
        PlanOneWaySync(Compare({File("a", 1, 1), Directory("d")}, {File("a", 1, 1), Directory("d")}),
                       FolderSyncDirection::LeftToRight,
                       {.overwrite_changed = true, .delete_extraneous = true});
    CHECK(nothing_to_do.IsEmpty());
    CHECK_FALSE(nothing_to_do.HasDeletions());
    CHECK(nothing_to_do.Deletions().empty());
    CHECK(nothing_to_do.Summarize() == FolderSyncSummary{.skip = 2});
}

TEST_CASE("nc::core::BindSyncPlan resolves a reviewed plan onto the listings it decided about")
{
    const std::vector<std::string> left{"source-only", "changed", "identical", "shared-dir", "clash"};
    const std::vector<std::string> right{"changed", "identical", "shared-dir", "clash", "destination-only"};
    const FolderSyncPlan plan =
        PlanOneWaySync(MixedComparison(), FolderSyncDirection::LeftToRight, {.delete_extraneous = true});

    const auto submission = nc::core::BindSyncPlan(plan, left, right);
    REQUIRE(submission);
    // Create("source-only") at 0 and Overwrite("changed") at 1 on the source side.
    CHECK(submission->copy_source_indices == std::vector<size_t>{0, 1});
    // Delete("destination-only") at 4 on the destination side.
    CHECK(submission->delete_destination_indices == std::vector<size_t>{4});
}

TEST_CASE("nc::core::BindSyncPlan fails atomically when a listing no longer matches the reviewed plan")
{
    const std::vector<std::string> left{"source-only", "changed", "identical", "shared-dir", "clash"};
    const std::vector<std::string> right{"changed", "identical", "shared-dir", "clash", "destination-only"};
    const FolderSyncPlan plan =
        PlanOneWaySync(MixedComparison(), FolderSyncDirection::LeftToRight, {.delete_extraneous = true});

    SECTION("a different name now sits at a copy position")
    {
        std::vector<std::string> moved = left;
        moved[0] = "something-else";
        // The unrelated Overwrite would still resolve, but binding must not submit that subset.
        CHECK_FALSE(nc::core::BindSyncPlan(plan, moved, right));
    }
    SECTION("a different name now sits at a delete position")
    {
        std::vector<std::string> moved = right;
        moved[4] = "someone-elses-file";
        CHECK_FALSE(nc::core::BindSyncPlan(plan, left, moved));
    }
    SECTION("a listing shrank below a referenced position")
    {
        CHECK_FALSE(nc::core::BindSyncPlan(plan, std::vector<std::string>{"source-only"}, right));
        CHECK_FALSE(nc::core::BindSyncPlan(plan, left, std::vector<std::string>{}));
    }
    SECTION("both listings emptied")
    {
        CHECK_FALSE(nc::core::BindSyncPlan(plan, std::vector<std::string>{}, std::vector<std::string>{}));
    }
}

TEST_CASE("nc::core::BindSyncPlan binds an inert plan to no work at all")
{
    const std::vector<std::string> names{"a", "d"};
    const FolderSyncPlan plan = PlanOneWaySync(Compare({File("a", 1, 1), Directory("d")},
                                                       {File("a", 1, 1), Directory("d")}),
                                               FolderSyncDirection::LeftToRight,
                                               {.overwrite_changed = true, .delete_extraneous = true});
    REQUIRE(plan.IsEmpty());

    const auto submission = nc::core::BindSyncPlan(plan, names, names);
    REQUIRE(submission);
    CHECK(submission->copy_source_indices.empty());
    CHECK(submission->delete_destination_indices.empty());
    // A plan that touches nothing binds cleanly even against listings that changed entirely.
    CHECK(nc::core::BindSyncPlan(plan, std::vector<std::string>{}, std::vector<std::string>{}));
}

TEST_CASE(PREFIX "plans nothing for an empty comparison")
{
    const FolderSyncPlan plan = PlanOneWaySync(Compare({}, {}), FolderSyncDirection::LeftToRight,
                                               {.overwrite_changed = true, .delete_extraneous = true});
    CHECK(plan.actions.empty());
    CHECK(plan.IsEmpty());
    CHECK_FALSE(plan.HasDeletions());
    CHECK(plan.Summarize() == FolderSyncSummary{});
}
