// Copyright (C) 2017-2024 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CalculateSizes.h"
#include "../PanelController.h"
#include <Panel/PanelData.h>
#include "../PanelView.h"
#include <VFS/VFS.h>

#include <algorithm>
#include <iterator>
#include <tuple>

namespace nc::panel::actions {

CalculateSizesSubmissionResult EvaluateCalculateSizesSubmission(const std::span<const VFSListingItem> _items,
                                                                PanelController *_target)
{
    if( !_target )
        return CalculateSizesSubmissionResult::PaneUnavailable;
    if( _target.isDoingBackgroundLoading )
        return CalculateSizesSubmissionResult::Loading;

    const auto listing = _target.data.ListingPtr();
    if( !_target.data.IsLoaded() || !listing || listing == VFSListing::EmptyListing() )
        return CalculateSizesSubmissionResult::ListingUnavailable;
    if( _items.empty() )
        return CalculateSizesSubmissionResult::SelectionUnavailable;
    if( std::ranges::any_of(_items, [](const VFSListingItem &_item) { return _item.IsDotDot(); }) )
        return CalculateSizesSubmissionResult::ParentEntryUnsupported;
    if( std::ranges::any_of(_items, [&](const VFSListingItem &_item) {
            return !_item || !_item.Host() || _item.Listing().get() != listing.get();
        }) ) {
        return CalculateSizesSubmissionResult::StaleContext;
    }

    bool has_directories = false;
    try {
        for( const VFSListingItem &item : _items ) {
            if( !item.IsDir() )
                continue;
            has_directories = true;
            if( !vfs::ProviderCapabilitiesResolver::Resolve(*item.Host(), item.Directory()).can_read )
                return CalculateSizesSubmissionResult::SourceUnreadable;
        }
    } catch( ... ) {
        return CalculateSizesSubmissionResult::SourceUnreadable;
    }
    if( !has_directories )
        return CalculateSizesSubmissionResult::NoDirectories;
    if( _target.isDirectorySizeCalculationBusy )
        return CalculateSizesSubmissionResult::CalculationBusy;
    return CalculateSizesSubmissionResult::Submitted;
}

CalculateSizesSubmissionResult SubmitCalculateSizes(const std::span<const VFSListingItem> _items,
                                                     PanelController *_target)
{
    const CalculateSizesSubmissionResult live = EvaluateCalculateSizesSubmission(_items, _target);
    if( live != CalculateSizesSubmissionResult::Submitted )
        return live;

    std::vector<VFSListingItem> directories;
    directories.reserve(_items.size());
    std::ranges::copy_if(_items, std::back_inserter(directories), [](const VFSListingItem &_item) {
        return _item.IsDir();
    });
    return [_target calculateSizesOfItems:directories] ? CalculateSizesSubmissionResult::Submitted
                                                       : CalculateSizesSubmissionResult::CalculationBusy;
}

bool CalculateSizes::Predicate(PanelController *_target) const
{
    if( !_target )
        return false;
    const auto items = _target.selectedEntriesOrFocusedEntryWithDotDot;
    return EvaluateCalculateSizesSubmission(items, _target) == CalculateSizesSubmissionResult::Submitted;
}

void CalculateSizes::Perform(PanelController *_target, id /*_sender*/) const
{
    auto selected = _target.selectedEntriesOrFocusedEntryWithDotDot;
    std::ignore = SubmitCalculateSizes(selected, _target);
}

void CalculateAllSizes::Perform(PanelController *_target, id /*_sender*/) const
{
    std::vector<VFSListingItem> items;
    auto &data = _target.data;
    for( auto ind : data.SortedDirectoryEntries() )
        if( auto e = data.EntryAtRawPosition(ind) )
            if( e.IsDir() )
                items.emplace_back(std::move(e));

    std::ignore = [_target calculateSizesOfItems:items];
}

} // namespace nc::panel::actions
