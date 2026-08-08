// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "RecursiveFolderComparison.h"

#include <algorithm>
#include <utility>

namespace nc::core {

FolderCompareSummary RecursiveFolderComparison::Summarize() const noexcept
{
    FolderCompareSummary summary;
    for( const RecursiveFolderCompareEntry &entry : entries ) {
        switch( entry.status ) {
            case FolderCompareStatus::Same:
                ++summary.same;
                break;
            case FolderCompareStatus::LeftOnly:
                ++summary.left_only;
                break;
            case FolderCompareStatus::RightOnly:
                ++summary.right_only;
                break;
            case FolderCompareStatus::Changed:
                ++summary.changed;
                break;
            case FolderCompareStatus::Conflict:
                ++summary.conflict;
                break;
        }
    }
    return summary;
}

bool RecursiveFolderComparison::Identical() const noexcept
{
    return std::ranges::all_of(entries, [](const RecursiveFolderCompareEntry &_entry) {
        return _entry.status == FolderCompareStatus::Same;
    });
}

namespace {

std::string JoinRelative(const std::string &_parent, const std::string &_name)
{
    return _parent.empty() ? _name : _parent + "/" + _name;
}

struct Walk {
    const RecursiveFolderCompareLister &left;
    const RecursiveFolderCompareLister &right;
    const FolderCompareOptions &options;
    const RecursiveFolderCompareLimits &limits;
    const std::function<bool()> &is_cancelled;
    RecursiveFolderComparison result;

    [[nodiscard]] bool Cancelled() const
    {
        if( !is_cancelled )
            return false;
        try {
            return is_cancelled();
        } catch( ... ) {
            // A cancel checker that threw told us nothing; stopping is the safe reading, since
            // continuing on the strength of a broken predicate is how a cancelled walk keeps going.
            return true;
        }
    }

    /**
     * Compares one level and appends it, descending where a pair is a directory on both sides.
     *
     * Returns the strongest status found at or below this level, so a parent can report what its
     * subtree says instead of the "present on both sides" that one level alone can see.
     */
    [[nodiscard]] std::expected<FolderCompareStatus, RecursiveFolderCompareFailure>
    Descend(const std::string &_relative_path, const size_t _depth)
    {
        if( Cancelled() )
            return std::unexpected(RecursiveFolderCompareFailure::Cancelled);
        if( _depth > limits.maximum_depth )
            return std::unexpected(RecursiveFolderCompareFailure::TooDeep);

        // An unreadable directory is not an empty one. The two are indistinguishable in the result,
        // and the difference is everything: one means "nothing inside", the other "we do not know".
        const auto left_items = left(_relative_path);
        if( !left_items )
            return std::unexpected(RecursiveFolderCompareFailure::Unreadable);
        const auto right_items = right(_relative_path);
        if( !right_items )
            return std::unexpected(RecursiveFolderCompareFailure::Unreadable);

        const FolderComparisonResult level = CompareFolders(*left_items, *right_items, options);
        if( !level )
            return std::unexpected(RecursiveFolderCompareFailure::InvalidListing);

        FolderCompareStatus strongest = FolderCompareStatus::Same;
        for( const FolderCompareEntry &entry : level->entries ) {
            if( result.entries.size() >= limits.maximum_entries )
                return std::unexpected(RecursiveFolderCompareFailure::TooLarge);

            const std::string child_path = JoinRelative(_relative_path, entry.name);
            const size_t index = result.entries.size();
            result.entries.push_back(RecursiveFolderCompareEntry{
                .relative_path = child_path,
                .status = entry.status,
                .newer_side = entry.newer_side,
                .is_directory = entry.is_directory,
                .depth = _depth,
            });

            // Only a directory the two sides agree on has a common structure to walk. A LeftOnly
            // directory is reported once, whole - enumerating what is inside something the other
            // side does not have adds nothing a sync can act on separately, and would bury the one
            // entry that matters. A Conflict has no shared structure at all.
            if( entry.is_directory && entry.status == FolderCompareStatus::Same ) {
                const auto below = Descend(child_path, _depth + 1);
                if( !below )
                    return std::unexpected(below.error());
                if( *below != FolderCompareStatus::Same ) {
                    // The directory itself becomes what its subtree says. A sync reading Same here
                    // would skip a subtree that is not identical.
                    result.entries[index].status = FolderCompareStatus::Changed;
                }
            }

            if( result.entries[index].status != FolderCompareStatus::Same )
                strongest = FolderCompareStatus::Changed;
        }
        return strongest;
    }
};

} // namespace

RecursiveFolderComparisonResult CompareFoldersRecursively(const RecursiveFolderCompareLister &_left,
                                                          const RecursiveFolderCompareLister &_right,
                                                          const FolderCompareOptions &_options,
                                                          const RecursiveFolderCompareLimits &_limits,
                                                          const std::function<bool()> &_is_cancelled)
{
    if( !_left || !_right )
        return std::unexpected(RecursiveFolderCompareFailure::Unreadable);

    Walk walk{.left = _left,
              .right = _right,
              .options = _options,
              .limits = _limits,
              .is_cancelled = _is_cancelled,
              .result = {}};
    const auto root = walk.Descend({}, 0);
    if( !root )
        return std::unexpected(root.error());
    return std::move(walk.result);
}

} // namespace nc::core
