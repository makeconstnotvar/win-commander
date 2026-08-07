// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/States/Explorer/SearchResultListingBuilder.h>
#include <map>
#include <set>
#include <string>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <vector>

namespace {

using nc::explorer::BuildSearchResultListing;
using nc::explorer::SearchResultListingBuildOptions;
using nc::explorer::SearchResultListingBuildStatus;
using nc::vfs::VFSPath;

class SearchResultListingTestHost final : public nc::vfs::Host
{
public:
    SearchResultListingTestHost() : Host("/", nullptr, "search-result-listing-tests") {}

    void MakeUnavailable(std::string _path, const int _error = ENOENT)
    {
        m_Unavailable.insert_or_assign(std::move(_path), _error);
    }

    [[nodiscard]] const std::vector<std::string> &FetchedPaths() const noexcept { return m_FetchedPaths; }

    std::expected<VFSListingPtr, nc::Error>
    FetchSingleItemListing(std::string_view _path,
                           unsigned long _flags,
                           [[maybe_unused]] const VFSCancelChecker &_cancel_checker) override
    {
        m_FetchedPaths.emplace_back(_path);
        m_FetchedFlags.emplace_back(_flags);
        if( const auto unavailable = m_Unavailable.find(_path); unavailable != m_Unavailable.end() )
            return std::unexpected(nc::Error{nc::Error::POSIX, unavailable->second});

        const size_t slash = _path.rfind('/');
        if( slash == std::string_view::npos || slash + 1 >= _path.size() )
            return std::unexpected(nc::Error{nc::Error::POSIX, EINVAL});

        nc::vfs::ListingInput input;
        input.hosts[0] = SharedPtr();
        input.directories[0] = std::string{_path.substr(0, slash + 1)};
        input.filenames.emplace_back(_path.substr(slash + 1));
        input.unix_modes.emplace_back(S_IFREG | 0644);
        input.unix_types.emplace_back(DT_REG);
        return VFSListing::Build(std::move(input));
    }

private:
    std::map<std::string, int, std::less<>> m_Unavailable;
    std::vector<std::string> m_FetchedPaths;
    std::vector<unsigned long> m_FetchedFlags;
};

std::set<std::pair<const VFSHost *, std::string>> ListingIdentities(const VFSListing &_listing)
{
    std::set<std::pair<const VFSHost *, std::string>> identities;
    for( unsigned index = 0; index != _listing.Count(); ++index )
        identities.emplace(_listing.Host(index).get(), _listing.Path(index));
    return identities;
}

} // namespace

#define PREFIX "nc::explorer::SearchResultListingBuilder "

TEST_CASE(PREFIX "sorts and deduplicates exact provider-path identities into a titled non-uniform listing")
{
    auto first = std::make_shared<SearchResultListingTestHost>();
    auto second = std::make_shared<SearchResultListingTestHost>();
    std::vector<VFSPath> paths{
        {first, "/zeta.txt"},
        {second, "/alpha.txt"},
        {first, "/alpha.txt"},
        {first, "/alpha.txt"},
    };
    SearchResultListingBuildOptions options;
    options.fetch_flags = 0x1234;
    options.title = "Exact Search Results";

    const auto result = BuildSearchResultListing(std::move(paths), options);

    REQUIRE(result.status == SearchResultListingBuildStatus::Completed);
    REQUIRE(result.listing);
    CHECK(result.accepted_count == 3);
    CHECK(result.unavailable_count == 0);
    CHECK_FALSE(result.limit_reached);
    CHECK(result.listing->Title() == "Exact Search Results");
    CHECK_FALSE(result.listing->IsUniform());
    CHECK(ListingIdentities(*result.listing) ==
          std::set<std::pair<const VFSHost *, std::string>>{
              {first.get(), "/alpha.txt"}, {first.get(), "/zeta.txt"}, {second.get(), "/alpha.txt"}});
    CHECK(first->FetchedPaths() == std::vector<std::string>{"/alpha.txt", "/zeta.txt"});
    CHECK(second->FetchedPaths() == std::vector<std::string>{"/alpha.txt"});
}

TEST_CASE(PREFIX "counts unavailable exact paths while retaining partial results")
{
    auto host = std::make_shared<SearchResultListingTestHost>();
    host->MakeUnavailable("/missing.txt");
    const auto result = BuildSearchResultListing({{host, "/ok.txt"}, {host, "/missing.txt"}}, {});

    REQUIRE(result.status == SearchResultListingBuildStatus::Completed);
    REQUIRE(result.listing);
    CHECK(result.accepted_count == 1);
    CHECK(result.unavailable_count == 1);
    CHECK(result.missing_count == 1);
    CHECK(result.permission_denied_count == 0);
    CHECK(result.failed_count == 0);
    CHECK_FALSE(result.first_failure);
    CHECK_FALSE(result.limit_reached);
    REQUIRE(result.listing->Count() == 1);
    CHECK(result.listing->Path(0) == "/ok.txt");
}

TEST_CASE(PREFIX "retains typed permission missing and unexpected materialization failures")
{
    auto host = std::make_shared<SearchResultListingTestHost>();
    host->MakeUnavailable("/denied.txt", EACCES);
    host->MakeUnavailable("/missing.txt", ENOENT);
    host->MakeUnavailable("/failed.txt", EIO);

    const auto result = BuildSearchResultListing(
        {{host, "/ok.txt"}, {host, "/denied.txt"}, {host, "/missing.txt"}, {host, "/failed.txt"}}, {});

    REQUIRE(result.status == SearchResultListingBuildStatus::Completed);
    CHECK(result.accepted_count == 1);
    CHECK(result.unavailable_count == 3);
    CHECK(result.permission_denied_count == 1);
    CHECK(result.missing_count == 1);
    CHECK(result.failed_count == 1);
    REQUIRE(result.first_failure);
    CHECK(*result.first_failure == nc::Error{nc::Error::POSIX, EIO});
}

TEST_CASE(PREFIX "builds an empty titled listing")
{
    SearchResultListingBuildOptions options;
    options.title = "Empty Search Results";
    const auto result = BuildSearchResultListing({}, options);

    REQUIRE(result.status == SearchResultListingBuildStatus::Completed);
    REQUIRE(result.listing);
    CHECK(result.listing->Count() == 0);
    CHECK(result.listing->Title() == "Empty Search Results");
    CHECK(result.accepted_count == 0);
    CHECK(result.unavailable_count == 0);
    CHECK(result.path_bytes == 0);
    CHECK_FALSE(result.limit_reached);
}

TEST_CASE(PREFIX "enforces result and path-byte caps")
{
    auto host = std::make_shared<SearchResultListingTestHost>();

    SECTION("result count")
    {
        SearchResultListingBuildOptions options;
        options.maximum_results = 2;
        const auto result = BuildSearchResultListing(
            {{host, "/charlie.txt"}, {host, "/alpha.txt"}, {host, "/bravo.txt"}}, options);

        REQUIRE(result.status == SearchResultListingBuildStatus::Completed);
        REQUIRE(result.listing);
        CHECK(result.accepted_count == 2);
        CHECK(result.listing->Count() == 2);
        CHECK(result.limit_reached);
    }

    SECTION("path bytes")
    {
        SearchResultListingBuildOptions options;
        options.maximum_path_bytes = std::string_view{"/a"}.size();
        const auto result = BuildSearchResultListing({{host, "/long-name.txt"}, {host, "/a"}}, options);

        REQUIRE(result.status == SearchResultListingBuildStatus::Completed);
        REQUIRE(result.listing);
        CHECK(result.accepted_count == 1);
        CHECK(result.listing->Count() == 1);
        CHECK(result.path_bytes == std::string_view{"/a"}.size());
        CHECK(result.limit_reached);
    }
}

TEST_CASE(PREFIX "cancellation never returns a listing")
{
    auto host = std::make_shared<SearchResultListingTestHost>();

    SECTION("before fetching")
    {
        const auto result = BuildSearchResultListing({{host, "/alpha.txt"}}, {}, [] { return true; });
        CHECK(result.status == SearchResultListingBuildStatus::Cancelled);
        CHECK_FALSE(result.listing);
        CHECK(result.accepted_count == 0);
        CHECK(host->FetchedPaths().empty());
    }

    SECTION("after fetching")
    {
        size_t checks = 0;
        const auto result = BuildSearchResultListing(
            {{host, "/alpha.txt"}}, {}, [&] { return ++checks >= 3; });
        CHECK(result.status == SearchResultListingBuildStatus::Cancelled);
        CHECK_FALSE(result.listing);
        CHECK(result.accepted_count == 0);
        CHECK(host->FetchedPaths() == std::vector<std::string>{"/alpha.txt"});
    }
}

TEST_CASE(PREFIX "rejects invalid options")
{
    const auto check_invalid = [](SearchResultListingBuildOptions _options) {
        const auto result = BuildSearchResultListing({}, _options);
        CHECK(result.status == SearchResultListingBuildStatus::Failed);
        CHECK_FALSE(result.listing);
        CHECK(result.accepted_count == 0);
    };

    SearchResultListingBuildOptions no_results;
    no_results.maximum_results = 0;
    check_invalid(no_results);

    SearchResultListingBuildOptions no_path_budget;
    no_path_budget.maximum_path_bytes = 0;
    check_invalid(no_path_budget);

    SearchResultListingBuildOptions no_title;
    no_title.title.clear();
    check_invalid(no_title);
}

#undef PREFIX
