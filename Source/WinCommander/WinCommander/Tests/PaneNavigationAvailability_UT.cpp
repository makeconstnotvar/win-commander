// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Pane/PaneNavigationAvailability.h>
#include <WinCommander/Core/Pane/PaneSnapshot.h>
#include <VFS/Host.h>
#include <VFS/VFSListing.h>
#include <VFS/VFSListingInput.h>
#include <array>
#include <dirent.h>
#include <sys/stat.h>

namespace {

using namespace nc::core;

VFSListingPtr UniformListing(const VFSHostPtr &_host, std::string _directory)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = std::move(_directory);
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = _host;
    input.filenames.emplace_back("entry");
    input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
    input.unix_types.emplace_back(DT_REG);
    return VFSListing::Build(std::move(input));
}

VFSListingPtr NonUniformListing()
{
    nc::vfs::ListingInput input;
    input.title = "Search results";
    input.directories.reset(nc::base::variable_container<>::type::dense);
    input.hosts.reset(nc::base::variable_container<>::type::dense);
    for( size_t index = 0; index != 2; ++index ) {
        input.directories.insert(index, index == 0 ? "/first/" : "/second/");
        input.hosts.insert(index, VFSHost::DummyHost());
        input.filenames.emplace_back(index == 0 ? "first" : "second");
        input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
        input.unix_types.emplace_back(DT_REG);
    }
    return VFSListing::Build(std::move(input));
}

PaneState UniformState(const PaneLoadPhase _phase,
                       const VFSHostPtr &_host,
                       std::string _path)
{
    PaneState state;
    state.load_phase = _phase;
    state.is_uniform = true;
    state.path = _path;
    state.host = _host;
    state.listing = UniformListing(_host, std::move(_path));
    return state;
}

PaneState NonUniformState(const PaneLoadPhase _phase)
{
    PaneState state;
    state.load_phase = _phase;
    state.listing = NonUniformListing();
    return state;
}

} // namespace

#define PREFIX "nc::core::PaneNavigationAvailability "

TEST_CASE(PREFIX "maps loading and authoritative empty state without inventing content")
{
    PaneState loading;
    loading.load_phase = PaneLoadPhase::Loading;
    CHECK(MapPaneNavigationAvailability(loading) ==
          PaneNavigationAvailability{NavigationUpAvailability::Busy,
                                     NavigationRefreshAvailability::Busy});

    CHECK(MapPaneNavigationAvailability(PaneState{}) ==
          PaneNavigationAvailability{NavigationUpAvailability::HierarchyUnavailable,
                                     NavigationRefreshAvailability::NoCommittedContent});

    PaneState empty_sentinel;
    empty_sentinel.listing = VFSListing::EmptyListing();
    CHECK(MapPaneNavigationAvailability(empty_sentinel) ==
          PaneNavigationAvailability{NavigationUpAvailability::HierarchyUnavailable,
                                     NavigationRefreshAvailability::NoCommittedContent});
}

TEST_CASE(PREFIX "maps uniform child hosted root and top root independently from refresh")
{
    const VFSHostPtr top_host = VFSHost::DummyHost();
    const VFSHostPtr hosted = std::make_shared<VFSHost>("/archive.zip", top_host, "pane_availability_hosted");

    CHECK(MapPaneNavigationAvailability(UniformState(PaneLoadPhase::Loaded, top_host, "/child/")) ==
          PaneNavigationAvailability{NavigationUpAvailability::Available,
                                     NavigationRefreshAvailability::Available});
    CHECK(MapPaneNavigationAvailability(UniformState(PaneLoadPhase::Loaded, hosted, "/")) ==
          PaneNavigationAvailability{NavigationUpAvailability::Available,
                                     NavigationRefreshAvailability::Available});
    CHECK(MapPaneNavigationAvailability(UniformState(PaneLoadPhase::Loaded, top_host, "/")) ==
          PaneNavigationAvailability{NavigationUpAvailability::AtTop,
                                     NavigationRefreshAvailability::Available});

    const VFSHostPtr malformed_hosted =
        std::make_shared<VFSHost>("", top_host, "pane_availability_malformed_hosted");
    CHECK(MapPaneNavigationAvailability(UniformState(PaneLoadPhase::Loaded, malformed_hosted, "/")) ==
          PaneNavigationAvailability{NavigationUpAvailability::PaneUnavailable,
                                     NavigationRefreshAvailability::Available});
}

TEST_CASE(PREFIX "keeps non-uniform and malformed hierarchy fail closed while retaining refresh")
{
    PaneState non_uniform = NonUniformState(PaneLoadPhase::Loaded);
    non_uniform.history_availability.can_go_back = true;
    CHECK(MapPaneNavigationAvailability(non_uniform) ==
          PaneNavigationAvailability{NavigationUpAvailability::HierarchyUnavailable,
                                     NavigationRefreshAvailability::Available});

    PaneState missing_host = UniformState(PaneLoadPhase::Loaded, VFSHost::DummyHost(), "/child/");
    missing_host.host.reset();
    CHECK(MapPaneNavigationAvailability(missing_host) ==
          PaneNavigationAvailability{NavigationUpAvailability::PaneUnavailable,
                                     NavigationRefreshAvailability::Available});

    PaneState relative_path = UniformState(PaneLoadPhase::Loaded, VFSHost::DummyHost(), "/child/");
    relative_path.path = "child/";
    CHECK(MapPaneNavigationAvailability(relative_path) ==
          PaneNavigationAvailability{NavigationUpAvailability::PaneUnavailable,
                                     NavigationRefreshAvailability::Available});

    PaneState non_canonical_path =
        UniformState(PaneLoadPhase::Loaded, VFSHost::DummyHost(), "/child/");
    non_canonical_path.path = "/child/../";
    CHECK(MapPaneNavigationAvailability(non_canonical_path) ==
          PaneNavigationAvailability{NavigationUpAvailability::PaneUnavailable,
                                     NavigationRefreshAvailability::Available});
}

TEST_CASE(PREFIX "refreshes loaded refreshing and retained failed content")
{
    const std::array phases{PaneLoadPhase::Loaded, PaneLoadPhase::Refreshing, PaneLoadPhase::Failed};
    for( const PaneLoadPhase phase : phases ) {
        CAPTURE(phase);
        CHECK(MapPaneNavigationAvailability(UniformState(phase, VFSHost::DummyHost(), "/child/")) ==
              PaneNavigationAvailability{NavigationUpAvailability::Available,
                                         NavigationRefreshAvailability::Available});

        PaneState no_content;
        no_content.load_phase = phase;
        no_content.listing = VFSListing::EmptyListing();
        CHECK(MapPaneNavigationAvailability(no_content) ==
              PaneNavigationAvailability{NavigationUpAvailability::HierarchyUnavailable,
                                         NavigationRefreshAvailability::NoCommittedContent});
    }
}

TEST_CASE(PREFIX "returns PaneUnavailable for malformed or unknown pane state")
{
    PaneState contradictory_empty;
    contradictory_empty.is_uniform = true;
    contradictory_empty.host = VFSHost::DummyHost();
    contradictory_empty.path = "/";
    contradictory_empty.listing = UniformListing(contradictory_empty.host, contradictory_empty.path);
    CHECK(MapPaneNavigationAvailability(contradictory_empty) == PaneNavigationAvailability{});

    PaneState unknown = contradictory_empty;
    unknown.load_phase = static_cast<PaneLoadPhase>(255);
    CHECK(MapPaneNavigationAvailability(unknown) == PaneNavigationAvailability{});
}

TEST_CASE(PREFIX "accepts only an exact matching pane snapshot at the consumer boundary")
{
    constexpr PaneId pane{71};
    PaneSnapshot snapshot;
    snapshot.pane_id = pane;

    const auto empty = MapMatchingPaneNavigationAvailability(pane, snapshot);
    REQUIRE(empty);
    CHECK(*empty == PaneNavigationAvailability{NavigationUpAvailability::HierarchyUnavailable,
                                               NavigationRefreshAvailability::NoCommittedContent});

    snapshot.state = UniformState(PaneLoadPhase::Refreshing, VFSHost::DummyHost(), "/child/");
    const auto matching = MapMatchingPaneNavigationAvailability(pane, snapshot);
    REQUIRE(matching);
    CHECK(*matching == PaneNavigationAvailability{NavigationUpAvailability::Available,
                                                  NavigationRefreshAvailability::Available});

    snapshot.pane_id = PaneId{72};
    CHECK_FALSE(MapMatchingPaneNavigationAvailability(pane, snapshot));
    CHECK_FALSE(MapMatchingPaneNavigationAvailability(PaneId{}, snapshot));
}

#undef PREFIX
