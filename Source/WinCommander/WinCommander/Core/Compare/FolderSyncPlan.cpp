// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "FolderSyncPlan.h"

#include <utility>

namespace nc::core {

namespace {

/** Re-labels a comparison entry's two sides as source and destination for the chosen direction. */
struct DirectedEntry {
    std::optional<size_t> source_index;
    std::optional<size_t> destination_index;
    bool source_only = false;
    bool destination_only = false;
    /** For a Changed pair: the destination holds the newer copy. */
    bool destination_newer = false;
};

DirectedEntry Direct(const FolderCompareEntry &_entry, const FolderSyncDirection _direction) noexcept
{
    const bool left_to_right = _direction == FolderSyncDirection::LeftToRight;
    DirectedEntry directed;
    directed.source_index = left_to_right ? _entry.left_index : _entry.right_index;
    directed.destination_index = left_to_right ? _entry.right_index : _entry.left_index;
    directed.source_only = _entry.status ==
                           (left_to_right ? FolderCompareStatus::LeftOnly : FolderCompareStatus::RightOnly);
    directed.destination_only = _entry.status ==
                                (left_to_right ? FolderCompareStatus::RightOnly : FolderCompareStatus::LeftOnly);
    directed.destination_newer =
        _entry.newer_side == (left_to_right ? FolderCompareNewerSide::Right : FolderCompareNewerSide::Left);
    return directed;
}

FolderSyncAction Skipped(const FolderCompareEntry &_entry, const DirectedEntry &_directed, const FolderSyncSkipReason _reason)
{
    return {.name = _entry.name,
            .kind = FolderSyncActionKind::Skip,
            .skip_reason = _reason,
            .is_directory = _entry.is_directory,
            .source_index = _directed.source_index,
            .destination_index = _directed.destination_index};
}

} // namespace

FolderSyncSummary FolderSyncPlan::Summarize() const noexcept
{
    FolderSyncSummary summary;
    for( const FolderSyncAction &action : actions ) {
        switch( action.kind ) {
            case FolderSyncActionKind::Create:
                ++summary.create;
                break;
            case FolderSyncActionKind::Overwrite:
                ++summary.overwrite;
                if( action.overwrites_newer_destination )
                    ++summary.overwrite_newer_destination;
                break;
            case FolderSyncActionKind::Delete:
                ++summary.remove;
                break;
            case FolderSyncActionKind::Skip:
                ++summary.skip;
                break;
        }
    }
    return summary;
}

bool FolderSyncPlan::HasDeletions() const noexcept
{
    for( const FolderSyncAction &action : actions )
        if( action.kind == FolderSyncActionKind::Delete )
            return true;
    return false;
}

bool FolderSyncPlan::IsEmpty() const noexcept
{
    for( const FolderSyncAction &action : actions )
        if( action.kind != FolderSyncActionKind::Skip )
            return false;
    return true;
}

std::vector<const FolderSyncAction *> FolderSyncPlan::Deletions() const
{
    std::vector<const FolderSyncAction *> deletions;
    for( const FolderSyncAction &action : actions )
        if( action.kind == FolderSyncActionKind::Delete )
            deletions.push_back(&action);
    return deletions;
}

FolderSyncPlan PlanOneWaySync(const FolderComparison &_comparison,
                              const FolderSyncDirection _direction,
                              const FolderSyncOptions &_options)
{
    FolderSyncPlan plan;
    plan.direction = _direction;
    plan.actions.reserve(_comparison.entries.size());

    for( const FolderCompareEntry &entry : _comparison.entries ) {
        const DirectedEntry directed = Direct(entry, _direction);

        if( entry.status == FolderCompareStatus::Conflict ) {
            // A directory facing a file has no safe direction at all; it must reach the user.
            plan.actions.emplace_back(Skipped(entry, directed, FolderSyncSkipReason::TypeConflict));
            continue;
        }
        if( entry.status == FolderCompareStatus::Same ) {
            // CompareFolders judges a directory pair by presence only, so "Same" says nothing about
            // its contents. Reporting that as Identical would be the exact bug CompareFolders'
            // contract note warns consumers about, so it gets its own reason instead.
            plan.actions.emplace_back(Skipped(entry,
                                              directed,
                                              entry.is_directory ? FolderSyncSkipReason::DirectoryContentsNotCompared
                                                                 : FolderSyncSkipReason::Identical));
            continue;
        }
        if( directed.source_only ) {
            plan.actions.emplace_back(FolderSyncAction{.name = entry.name,
                                                       .kind = FolderSyncActionKind::Create,
                                                       .is_directory = entry.is_directory,
                                                       .source_index = directed.source_index,
                                                       .destination_index = directed.destination_index});
            continue;
        }
        if( directed.destination_only ) {
            if( !_options.delete_extraneous ) {
                plan.actions.emplace_back(Skipped(entry, directed, FolderSyncSkipReason::DeletionNotRequested));
                continue;
            }
            plan.actions.emplace_back(FolderSyncAction{.name = entry.name,
                                                       .kind = FolderSyncActionKind::Delete,
                                                       .is_directory = entry.is_directory,
                                                       .source_index = directed.source_index,
                                                       .destination_index = directed.destination_index});
            continue;
        }

        // The remaining case is a Changed pair present on both sides.
        if( !_options.overwrite_changed ) {
            plan.actions.emplace_back(Skipped(entry, directed, FolderSyncSkipReason::OverwriteNotRequested));
            continue;
        }
        plan.actions.emplace_back(FolderSyncAction{.name = entry.name,
                                                   .kind = FolderSyncActionKind::Overwrite,
                                                   .is_directory = entry.is_directory,
                                                   .source_index = directed.source_index,
                                                   .destination_index = directed.destination_index,
                                                   .overwrites_newer_destination = directed.destination_newer});
    }
    return plan;
}

} // namespace nc::core
