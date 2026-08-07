// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "SearchResultListingBuilder.h"

#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <algorithm>
#include <cerrno>

namespace nc::explorer {

namespace {

bool IsPermissionDenied(const Error &_error) noexcept
{
    return _error.Domain() == Error::POSIX && (_error.Code() == EACCES || _error.Code() == EPERM);
}

bool IsMissing(const Error &_error) noexcept
{
    return _error.Domain() == Error::POSIX &&
           (_error.Code() == ENOENT || _error.Code() == ENOTDIR || _error.Code() == ESTALE);
}

void RecordUnavailable(SearchResultListingBuildResult &_result, const Error &_error)
{
    ++_result.unavailable_count;
    if( IsPermissionDenied(_error) )
        ++_result.permission_denied_count;
    else if( IsMissing(_error) )
        ++_result.missing_count;
    else {
        ++_result.failed_count;
        if( !_result.first_failure )
            _result.first_failure = _error;
    }
}

} // namespace

SearchResultListingBuildResult BuildSearchResultListing(std::vector<nc::vfs::VFSPath> _paths,
                                                        const SearchResultListingBuildOptions &_options,
                                                        const VFSCancelChecker &_cancel_checker)
{
    SearchResultListingBuildResult result;
    if( _options.maximum_results == 0 || _options.maximum_path_bytes == 0 || _options.title.empty() )
        return result;
    if( _cancel_checker && _cancel_checker() ) {
        result.status = SearchResultListingBuildStatus::Cancelled;
        return result;
    }

    std::erase_if(_paths, [](const nc::vfs::VFSPath &_path) {
        return !_path || _path.Path().empty() || _path.Path().front() != '/';
    });
    std::ranges::sort(_paths);
    _paths.erase(std::ranges::unique(_paths).begin(), _paths.end());

    std::vector<VFSListingPtr> listings;
    listings.reserve(std::min(_paths.size(), _options.maximum_results));
    for( const nc::vfs::VFSPath &path : _paths ) {
        if( _cancel_checker && _cancel_checker() ) {
            result.status = SearchResultListingBuildStatus::Cancelled;
            result.listing.reset();
            return result;
        }
        if( listings.size() >= _options.maximum_results ||
            path.Path().size() > _options.maximum_path_bytes - result.path_bytes ) {
            result.limit_reached = true;
            break;
        }

        result.path_bytes += path.Path().size();
        try {
            const std::expected<VFSListingPtr, Error> listing =
                path.Host()->FetchSingleItemListing(path.Path(), _options.fetch_flags, _cancel_checker);
            if( listing && *listing && (*listing)->Count() == 1 )
                listings.emplace_back(*listing);
            else if( !listing )
                RecordUnavailable(result, listing.error());
            else {
                ++result.unavailable_count;
                ++result.failed_count;
            }
        } catch( ... ) {
            ++result.unavailable_count;
            ++result.failed_count;
        }
    }
    if( listings.size() < _paths.size() - result.unavailable_count )
        result.limit_reached = true;
    if( _cancel_checker && _cancel_checker() ) {
        result.status = SearchResultListingBuildStatus::Cancelled;
        return result;
    }

    try {
        nc::vfs::ListingInput input = VFSListing::Compose(listings);
        input.title = _options.title;
        result.listing = VFSListing::Build(std::move(input));
        result.accepted_count = listings.size();
        result.status = SearchResultListingBuildStatus::Completed;
    } catch( ... ) {
        result.listing.reset();
        result.status = SearchResultListingBuildStatus::Failed;
    }
    return result;
}

} // namespace nc::explorer
