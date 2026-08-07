// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/States/Explorer/NCExplorerInspectorModel.h>
#include <cerrno>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <type_traits>
#include <utility>

namespace {

using nc::core::PaneId;
using nc::core::PaneLoadPhase;
using nc::core::PaneSelectedItems;
using nc::core::PaneSnapshot;
using nc::core::FileMetadataSnapshot;
using nc::explorer::InspectorModel;
using nc::explorer::InspectorState;

static_assert(std::is_same_v<decltype(std::declval<const InspectorModel &>().Items()),
                             std::span<const FileMetadataSnapshot>>);

VFSListingPtr Listing()
{
    nc::vfs::ListingInput input;
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = VFSHost::DummyHost();
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/fixture/";
    input.filenames = {"..", "alpha.txt", "Beta", "shortcut.link"};
    input.unix_modes = {S_IFDIR | 0755, S_IFREG | 0640, S_IFDIR | 0750, S_IFREG | 0444};
    input.unix_types = {DT_DIR, DT_REG, DT_DIR, DT_LNK};

    input.display_filenames.reset(nc::base::variable_container<>::type::sparse);
    input.display_filenames.insert(1, "Alpha Document");
    input.sizes.reset(nc::base::variable_container<>::type::sparse);
    input.sizes.insert(1, 1234);
    input.inodes.reset(nc::base::variable_container<>::type::sparse);
    input.inodes.insert(1, 5678);
    input.atimes.reset(nc::base::variable_container<>::type::sparse);
    input.atimes.insert(1, 101);
    input.mtimes.reset(nc::base::variable_container<>::type::sparse);
    input.mtimes.insert(1, 102);
    input.ctimes.reset(nc::base::variable_container<>::type::sparse);
    input.ctimes.insert(1, 103);
    input.btimes.reset(nc::base::variable_container<>::type::sparse);
    input.btimes.insert(1, 104);
    input.add_times.reset(nc::base::variable_container<>::type::sparse);
    input.add_times.insert(1, 105);
    input.unix_flags.reset(nc::base::variable_container<>::type::sparse);
    input.unix_flags.insert(1, 0x20);
    input.uids.reset(nc::base::variable_container<>::type::sparse);
    input.uids.insert(1, 501);
    input.gids.reset(nc::base::variable_container<>::type::sparse);
    input.gids.insert(1, 20);
    input.symlinks.reset(nc::base::variable_container<>::type::sparse);
    input.symlinks.insert(3, "alpha.txt");
    input.tags.emplace(
        1,
        std::vector<nc::utility::Tags::Tag>{
            {nc::utility::Tags::Tag::Internalize("Important"), nc::utility::Tags::Color::Red}});
    return VFSListing::Build(std::move(input));
}

PaneSnapshot Snapshot(const PaneId _pane_id,
                      const uint64_t _revision,
                      const PaneLoadPhase _phase,
                      const VFSListingPtr &_listing = {})
{
    PaneSnapshot snapshot;
    snapshot.pane_id = _pane_id;
    snapshot.revision = _revision;
    snapshot.state.load_phase = _phase;
    snapshot.state.listing = _listing;
    return snapshot;
}

nc::core::FileManagerError PermissionFailure()
{
    return nc::core::FileManagerError{
        .code = {.domain = "ExplorerInspectorModelTest", .value = EACCES},
        .category = nc::core::FileManagerErrorCategory::PermissionError,
        .severity = nc::core::FileManagerErrorSeverity::BlockingError,
        .user_message_key = "errors.permission",
        .user_message = "Access denied.",
        .technical_message = "Test permission failure.",
        .original_error = nc::Error{nc::Error::POSIX, EACCES},
    };
}

} // namespace

#define PREFIX "nc::explorer::InspectorModel "

TEST_CASE(PREFIX "starts hidden and preserves matching presentation against a foreign pane")
{
    const PaneId pane{41};
    InspectorModel model{pane};
    CHECK(model.State() == InspectorState::Hidden);
    CHECK(model.Items().empty());
    CHECK_FALSE(model.LastAcceptedRevision());

    const auto listing = Listing();
    auto matching = Snapshot(pane, 3, PaneLoadPhase::Loaded, listing);
    matching.state.focused_item = listing->Item(1);
    REQUIRE(model.Apply(matching));
    REQUIRE(model.State() == InspectorState::Single);

    auto foreign = Snapshot(PaneId{42}, 100, PaneLoadPhase::Loaded, listing);
    foreign.state.focused_item = listing->Item(2);
    CHECK_FALSE(model.Apply(foreign));
    CHECK(model.State() == InspectorState::Single);
    REQUIRE(model.Items().size() == 1);
    CHECK(model.Items()[0].filename == "alpha.txt");
    CHECK(model.PreviewItem() == listing->Item(1));
    CHECK_FALSE(model.Error());
    REQUIRE(model.LastAcceptedRevision());
    CHECK(*model.LastAcceptedRevision() == 3);
}

TEST_CASE(PREFIX "copies complete immediate metadata without filesystem IO")
{
    const PaneId pane{51};
    const auto listing = Listing();
    auto snapshot = Snapshot(pane, 1, PaneLoadPhase::Loaded, listing);
    snapshot.state.focused_item = listing->Item(1);
    InspectorModel model{pane};

    REQUIRE(model.Apply(snapshot));
    CHECK(model.State() == InspectorState::Single);
    CHECK_FALSE(model.IsRefreshing());
    REQUIRE(model.Items().size() == 1);
    CHECK(model.PreviewItem() == listing->Item(1));
    const FileMetadataSnapshot &metadata = model.Items().front();
    CHECK(metadata.path == "/fixture/alpha.txt");
    CHECK(metadata.filename == "alpha.txt");
    CHECK(metadata.display_name == "Alpha Document");
    CHECK(metadata.extension == "txt");
    CHECK(metadata.unix_mode == (S_IFREG | 0640));
    CHECK(metadata.unix_type == DT_REG);
    CHECK_FALSE(metadata.is_directory);
    CHECK(metadata.is_regular);
    CHECK_FALSE(metadata.is_symlink);
    CHECK_FALSE(metadata.is_hidden);
    CHECK(metadata.size == 1234);
    CHECK(metadata.inode == 5678);
    CHECK(metadata.accessed_time == 101);
    CHECK(metadata.modified_time == 102);
    CHECK(metadata.status_changed_time == 103);
    CHECK(metadata.created_time == 104);
    CHECK(metadata.added_time == 105);
    CHECK(metadata.unix_flags == 0x20);
    CHECK(metadata.unix_uid == 501);
    CHECK(metadata.unix_gid == 20);
    CHECK_FALSE(metadata.symlink_target);
    REQUIRE(metadata.tags.size() == 1);
    CHECK(metadata.tags[0].label == "Important");
    CHECK(metadata.tags[0].color == nc::utility::Tags::Color::Red);
}

TEST_CASE(PREFIX "uses exact selected order instead of the focused item")
{
    const PaneId pane{61};
    const auto listing = Listing();
    auto snapshot = Snapshot(pane, 2, PaneLoadPhase::Loaded, listing);
    snapshot.state.focused_item = listing->Item(1);
    snapshot.state.selected_items = PaneSelectedItems{listing->Item(2), listing->Item(3)};
    InspectorModel model{pane};

    REQUIRE(model.Apply(snapshot));
    CHECK(model.State() == InspectorState::Multiple);
    REQUIRE(model.Items().size() == 2);
    CHECK_FALSE(model.PreviewItem());
    CHECK(model.Items()[0].filename == "Beta");
    CHECK(model.Items()[0].is_directory);
    CHECK(model.Items()[1].filename == "shortcut.link");
    CHECK(model.Items()[1].is_symlink);
    CHECK(model.Items()[1].symlink_target == "alpha.txt");
}

TEST_CASE(PREFIX "never projects the synthetic parent entry")
{
    const PaneId pane{71};
    const auto listing = Listing();
    InspectorModel model{pane};

    auto focused_parent = Snapshot(pane, 1, PaneLoadPhase::Loaded, listing);
    focused_parent.state.focused_item = listing->Item(0);
    REQUIRE(model.Apply(focused_parent));
    CHECK(model.State() == InspectorState::Empty);
    CHECK(model.Items().empty());
    CHECK_FALSE(model.PreviewItem());

    auto selected_parent = Snapshot(pane, 2, PaneLoadPhase::Loaded, listing);
    selected_parent.state.focused_item = listing->Item(1);
    selected_parent.state.selected_items = PaneSelectedItems{listing->Item(0)};
    REQUIRE(model.Apply(selected_parent));
    CHECK(model.State() == InspectorState::Empty);
    CHECK(model.Items().empty());
    CHECK_FALSE(model.PreviewItem());
}

TEST_CASE(PREFIX "maps empty loading refreshing and failed pane states")
{
    const PaneId pane{81};
    const auto listing = Listing();
    InspectorModel model{pane};

    REQUIRE(model.Apply(Snapshot(pane, 1, PaneLoadPhase::Empty)));
    CHECK(model.State() == InspectorState::Empty);

    auto loading = Snapshot(pane, 2, PaneLoadPhase::Loading, listing);
    loading.state.focused_item = listing->Item(1);
    REQUIRE(model.Apply(loading));
    CHECK(model.State() == InspectorState::PaneLoading);
    CHECK_FALSE(model.IsRefreshing());
    CHECK(model.Items().empty());
    CHECK_FALSE(model.PreviewItem());

    auto refreshing_single = Snapshot(pane, 3, PaneLoadPhase::Refreshing, listing);
    refreshing_single.state.focused_item = listing->Item(1);
    REQUIRE(model.Apply(refreshing_single));
    CHECK(model.State() == InspectorState::Single);
    CHECK(model.IsRefreshing());
    REQUIRE(model.Items().size() == 1);
    CHECK(model.Items()[0].filename == "alpha.txt");
    CHECK(model.PreviewItem() == listing->Item(1));

    auto refreshing_multiple = Snapshot(pane, 4, PaneLoadPhase::Refreshing, listing);
    refreshing_multiple.state.selected_items = PaneSelectedItems{listing->Item(1), listing->Item(2)};
    refreshing_multiple.state.visible_error = PermissionFailure();
    REQUIRE(model.Apply(refreshing_multiple));
    CHECK(model.State() == InspectorState::Multiple);
    CHECK(model.IsRefreshing());
    REQUIRE(model.Items().size() == 2);
    CHECK_FALSE(model.PreviewItem());
    REQUIRE(model.Error());
    CHECK(model.Error()->category == nc::core::FileManagerErrorCategory::PermissionError);

    auto failed = Snapshot(pane, 5, PaneLoadPhase::Failed, listing);
    failed.state.visible_error = PermissionFailure();
    REQUIRE(model.Apply(failed));
    CHECK(model.State() == InspectorState::PaneError);
    CHECK_FALSE(model.IsRefreshing());
    CHECK(model.Items().empty());
    CHECK_FALSE(model.PreviewItem());
    REQUIRE(model.Error());
    CHECK(model.Error()->category == nc::core::FileManagerErrorCategory::PermissionError);
}

TEST_CASE(PREFIX "preserves the last accepted presentation against stale matching snapshots")
{
    const PaneId pane{91};
    const auto listing = Listing();
    InspectorModel model{pane};
    auto current = Snapshot(pane, 9, PaneLoadPhase::Loaded, listing);
    current.state.focused_item = listing->Item(1);
    REQUIRE(model.Apply(current));
    REQUIRE(model.State() == InspectorState::Single);
    REQUIRE(model.Items().size() == 1);

    auto stale = Snapshot(pane, 8, PaneLoadPhase::Failed, listing);
    stale.state.visible_error = PermissionFailure();
    CHECK_FALSE(model.Apply(stale));
    CHECK(model.State() == InspectorState::Single);
    REQUIRE(model.Items().size() == 1);
    CHECK(model.Items()[0].filename == "alpha.txt");
    CHECK(model.PreviewItem() == listing->Item(1));
    CHECK_FALSE(model.Error());
    REQUIRE(model.LastAcceptedRevision());
    CHECK(*model.LastAcceptedRevision() == 9);
}

#undef PREFIX
