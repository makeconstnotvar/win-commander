// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/VFSListing.h>
#include <VFS/VFSPath.h>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace nc::explorer {

struct SearchResultListingBuildOptions {
    unsigned long fetch_flags = 0;
    size_t maximum_results = 50'000;
    size_t maximum_path_bytes = 64 * 1024 * 1024;
    std::string title = "Search Results";
};

enum class SearchResultListingBuildStatus {
    Completed,
    Cancelled,
    Failed,
};

struct SearchResultListingBuildResult {
    SearchResultListingBuildStatus status = SearchResultListingBuildStatus::Failed;
    VFSListingPtr listing;
    size_t accepted_count = 0;
    size_t unavailable_count = 0;
    size_t permission_denied_count = 0;
    size_t missing_count = 0;
    size_t failed_count = 0;
    std::optional<Error> first_failure;
    size_t path_bytes = 0;
    bool limit_reached = false;
};

/**
 * Produces one bounded temporary non-uniform listing from exact provider/path pairs.
 * Duplicate pairs are removed before limits are applied. Cancellation never returns a listing.
 */
SearchResultListingBuildResult BuildSearchResultListing(std::vector<nc::vfs::VFSPath> _paths,
                                                        const SearchResultListingBuildOptions &_options,
                                                        const VFSCancelChecker &_cancel_checker = {});

} // namespace nc::explorer
