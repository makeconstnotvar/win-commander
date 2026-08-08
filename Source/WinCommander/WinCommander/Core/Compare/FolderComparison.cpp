// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "FolderComparison.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace nc::core {

namespace {

bool IsReservedName(const std::string_view _name) noexcept
{
    return _name == "." || _name == "..";
}

/**
 * Indexes one listing by exact name, rejecting anything that would make matching ambiguous or that
 * is not a comparison subject in the first place.
 */
std::expected<std::unordered_map<std::string_view, size_t>, FolderCompareFailure>
IndexListing(const std::span<const FolderCompareItem> _items)
{
    std::unordered_map<std::string_view, size_t> index;
    index.reserve(_items.size());
    for( size_t position = 0; position < _items.size(); ++position ) {
        const std::string_view name = _items[position].name;
        if( name.empty() )
            return std::unexpected(FolderCompareFailure::EmptyName);
        if( IsReservedName(name) )
            return std::unexpected(FolderCompareFailure::ReservedName);
        if( !index.emplace(name, position).second )
            return std::unexpected(FolderCompareFailure::DuplicateName);
    }
    return index;
}

FolderCompareNewerSide NewerSide(const int64_t _left_time, const int64_t _right_time, const int64_t _tolerance) noexcept
{
    if( _left_time == _right_time )
        return FolderCompareNewerSide::Neither;

    // Timestamps come from filesystem metadata and may legitimately sit at the extremes of the type,
    // where a signed subtraction would overflow - and signed overflow is undefined, so the verdict
    // could invert. The magnitude is therefore taken in unsigned arithmetic, which is defined for
    // every input pair and, on two's-complement, yields the exact difference for the whole range.
    const bool left_newer = _left_time > _right_time;
    const uint64_t larger = static_cast<uint64_t>(left_newer ? _left_time : _right_time);
    const uint64_t smaller = static_cast<uint64_t>(left_newer ? _right_time : _left_time);
    // Callers never reach here with a negative tolerance; CompareFolders rejects one up front.
    if( larger - smaller <= static_cast<uint64_t>(_tolerance) )
        return FolderCompareNewerSide::Neither;
    return left_newer ? FolderCompareNewerSide::Left : FolderCompareNewerSide::Right;
}

/** Judges a matched pair of regular files against the active criteria. */
FolderCompareStatus CompareFiles(const FolderCompareItem &_left,
                                 const FolderCompareItem &_right,
                                 const FolderCompareOptions &_options,
                                 const FolderCompareNewerSide _newer_side) noexcept
{
    if( _options.compare_size && _left.size != _right.size )
        return FolderCompareStatus::Changed;
    if( _options.compare_modification_time && _newer_side != FolderCompareNewerSide::Neither )
        return FolderCompareStatus::Changed;
    return FolderCompareStatus::Same;
}

} // namespace

FolderCompareSummary FolderComparison::Summarize() const noexcept
{
    FolderCompareSummary summary;
    for( const FolderCompareEntry &entry : entries ) {
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

bool FolderComparison::Identical() const noexcept
{
    return std::ranges::all_of(entries, [](const FolderCompareEntry &_entry) noexcept {
        return _entry.status == FolderCompareStatus::Same;
    });
}

FolderComparisonResult CompareFolders(const std::span<const FolderCompareItem> _left,
                                      const std::span<const FolderCompareItem> _right,
                                      const FolderCompareOptions &_options)
{
    if( _options.modification_time_tolerance < 0 )
        return std::unexpected(FolderCompareFailure::NegativeTolerance);

    const auto left_index = IndexListing(_left);
    if( !left_index )
        return std::unexpected(left_index.error());
    const auto right_index = IndexListing(_right);
    if( !right_index )
        return std::unexpected(right_index.error());

    FolderComparison comparison;
    comparison.entries.reserve(_left.size() + _right.size());

    for( size_t position = 0; position < _left.size(); ++position ) {
        const FolderCompareItem &left_item = _left[position];
        FolderCompareEntry entry;
        entry.name = left_item.name;
        entry.left_index = position;

        const auto match = right_index->find(std::string_view{left_item.name});
        if( match == right_index->end() ) {
            entry.status = FolderCompareStatus::LeftOnly;
            entry.is_directory = left_item.is_directory;
            comparison.entries.emplace_back(std::move(entry));
            continue;
        }

        const FolderCompareItem &right_item = _right[match->second];
        entry.right_index = match->second;
        if( left_item.is_directory != right_item.is_directory ) {
            entry.status = FolderCompareStatus::Conflict;
        }
        else if( left_item.is_directory ) {
            // Presence-only agreement: this level says nothing about the directory's contents.
            entry.status = FolderCompareStatus::Same;
            entry.is_directory = true;
        }
        else {
            entry.newer_side = NewerSide(
                left_item.modification_time, right_item.modification_time, _options.modification_time_tolerance);
            entry.status = CompareFiles(left_item, right_item, _options, entry.newer_side);
        }
        comparison.entries.emplace_back(std::move(entry));
    }

    for( size_t position = 0; position < _right.size(); ++position ) {
        const FolderCompareItem &right_item = _right[position];
        if( left_index->contains(std::string_view{right_item.name}) )
            continue;
        FolderCompareEntry entry;
        entry.name = right_item.name;
        entry.status = FolderCompareStatus::RightOnly;
        entry.right_index = position;
        entry.is_directory = right_item.is_directory;
        comparison.entries.emplace_back(std::move(entry));
    }

    return comparison;
}

} // namespace nc::core
