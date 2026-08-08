// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/States/FilePanels/PanelControllerPaneStoreAdapter.h>

#include <WinCommander/States/FilePanels/PanelController.h>
#include <WinCommander/States/FilePanels/PanelHistory.h>
#include <WinCommander/States/FilePanels/PanelView.h>
#include <Base/dispatch_cpp.h>
#include <Config/ConfigImpl.h>
#include <Config/NonPersistentOverwritesStorage.h>
#include <Panel/PanelData.h>
#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <CoreFoundation/CoreFoundation.h>
#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <sys/dirent.h>
#include <tuple>
#include <vector>

namespace {

using namespace std::chrono_literals;
using nc::core::PaneId;
using nc::core::PaneLoadPhase;
using nc::core::PaneGroupingKey;
using nc::core::PaneGroupingState;
using nc::core::PaneHistoryAvailability;
using nc::core::PaneSortDirection;
using nc::core::PaneSortKey;
using nc::core::PaneTextCollation;
using nc::core::PaneViewMode;
using nc::core::PaneViewState;
using nc::core::PaneLifecycleCancelled;
using nc::core::PaneLifecycleCommitted;
using nc::core::PaneLifecycleEvent;
using nc::core::PaneLifecycleFailed;
using nc::core::PaneLifecycleProducer;
using nc::core::PaneLifecycleRejected;
using nc::core::PaneRejectionReason;
using nc::core::PaneRequestDescriptor;
using nc::core::PaneRequestKind;
using nc::panel::PanelControllerPaneStoreAdapter;
using nc::panel::ProjectPaneState;
using nc::panel::data::Model;

bool RunMainLoopUntil(const std::function<bool()> &_predicate, const std::chrono::milliseconds _timeout = 1s)
{
    const auto deadline = std::chrono::steady_clock::now() + _timeout;
    while( !_predicate() && std::chrono::steady_clock::now() < deadline )
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    return _predicate();
}

VFSListingPtr UniformListing(std::string _directory = "/fixture/")
{
    nc::vfs::ListingInput input;
    input.title = "ignored uniform title";
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = std::move(_directory);
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = VFSHost::DummyHost();

    const std::vector<std::tuple<std::string, mode_t, uint8_t, uint64_t>> entries = {
        {"..", S_IFDIR | S_IRUSR, DT_DIR, 0}, {"alpha", S_IFREG | S_IRUSR, DT_REG, 11},
        {"beta", S_IFREG | S_IRUSR, DT_REG, 31}};
    for( const auto &[name, mode, type, size] : entries ) {
        const size_t index = input.filenames.size();
        input.filenames.emplace_back(name);
        input.unix_modes.emplace_back(mode);
        input.unix_types.emplace_back(type);
        input.sizes.insert(index, size);
    }
    return VFSListing::Build(std::move(input));
}

VFSListingPtr NonUniformListing()
{
    nc::vfs::ListingInput input;
    input.title = "Search results";
    input.directories.reset(nc::base::variable_container<>::type::dense);
    input.hosts.reset(nc::base::variable_container<>::type::dense);

    const std::vector<std::tuple<std::string, std::string, uint64_t>> entries = {
        {"/first/", "alpha", 11}, {"/second/", "beta", 31}};
    for( size_t index = 0; index < entries.size(); ++index ) {
        input.directories.insert(index, std::get<0>(entries[index]));
        input.hosts.insert(index, VFSHost::DummyHost());
        input.filenames.emplace_back(std::get<1>(entries[index]));
        input.unix_modes.emplace_back(S_IFREG | S_IRUSR);
        input.unix_types.emplace_back(DT_REG);
        input.sizes.insert(index, std::get<2>(entries[index]));
    }
    return VFSListing::Build(std::move(input));
}

PaneRequestDescriptor NavigationDescriptor(const std::string &_path = "/fixture/")
{
    return PaneRequestDescriptor{
        .kind = PaneRequestKind::Navigation,
        .target = nc::core::PaneRequestLocation{.host = VFSHost::DummyHost(), .path = _path},
        .initiated_by_user = true,
    };
}

PaneRequestDescriptor RefreshDescriptor()
{
    return PaneRequestDescriptor{.kind = PaneRequestKind::Refresh};
}

nc::core::FileManagerError NavigationFailure()
{
    return nc::core::FileManagerError{
        .code = {.domain = "PanelControllerPaneStoreAdapterTest", .value = 1},
        .category = nc::core::FileManagerErrorCategory::PathNotFoundError,
        .severity = nc::core::FileManagerErrorSeverity::BlockingError,
        .user_message_key = "errors.path_not_found",
        .user_message = "The folder could not be found.",
        .technical_message = "Test navigation failure.",
        .original_error = nc::Error{nc::Error::POSIX, ENOENT},
    };
}

} // namespace

@interface PaneStoreTestPanelView : NSObject
- (VFSListingItem)item;
- (void)setTestItem:(VFSListingItem)_item;
- (bool)explorerDetailsGroupingEnabled;
- (void)setExplorerDetailsGroupingEnabled:(bool)_enabled;
- (nc::panel::PanelViewLayout::LayoutVariant)presentationLayout;
- (void)setTestPresentationLayout:(const nc::panel::PanelViewLayout &)_layout;
@end

@implementation PaneStoreTestPanelView {
    VFSListingItem m_TestItem;
    bool m_TestGroupingEnabled;
    nc::panel::PanelViewLayout::LayoutVariant m_TestPresentationLayout;
}

- (VFSListingItem)item
{
    return m_TestItem;
}

- (void)setTestItem:(VFSListingItem)_item
{
    m_TestItem = std::move(_item);
}

- (bool)explorerDetailsGroupingEnabled
{
    return m_TestGroupingEnabled;
}

- (void)setExplorerDetailsGroupingEnabled:(const bool)_enabled
{
    if( m_TestGroupingEnabled == _enabled )
        return;
    m_TestGroupingEnabled = _enabled;
    [NSNotificationCenter.defaultCenter postNotificationName:NCPanelViewContextDidChangeNotification object:self];
}

- (nc::panel::PanelViewLayout::LayoutVariant)presentationLayout
{
    return m_TestPresentationLayout;
}

- (void)setTestPresentationLayout:(const nc::panel::PanelViewLayout &)_layout
{
    m_TestPresentationLayout = _layout.layout;
}

@end

@interface PaneStoreTestPanelController : PanelController
- (instancetype)initWithGeneration:(unsigned long)_generation paneId:(nc::core::PaneId)_pane_id;
- (nc::panel::data::Model &)mutableTestData;
- (void)setTestGeneration:(unsigned long)_generation;
- (void)setTestFocusedItem:(VFSListingItem)_item;
- (void)publishTestFallbackAtDisabledLayoutIndex;
- (void)clearTestContext;
- (void)postContextChange;
- (nc::core::PaneLifecycleProducer &)testLifecycle;
- (std::shared_ptr<size_t>)testLifecycleDeliveries;
@end

@implementation PaneStoreTestPanelController {
    nc::panel::data::Model m_TestData;
    nc::core::PaneId m_TestPaneId;
    unsigned long m_TestGeneration;
    PaneStoreTestPanelView *m_TestContext;
    std::unique_ptr<nc::config::ConfigImpl> m_TestConfig;
    std::shared_ptr<nc::panel::PanelViewLayoutsStorage> m_TestLayouts;
    int m_TestLayoutIndex;
    std::unique_ptr<PaneLifecycleProducer> m_TestLifecycle;
    std::shared_ptr<size_t> m_TestLifecycleDeliveries;
}

- (instancetype)initWithGeneration:(const unsigned long)_generation paneId:(const nc::core::PaneId)_pane_id
{
    self = [super init];
    if( self ) {
        m_TestPaneId = _pane_id;
        m_TestGeneration = _generation;
        m_TestContext = [PaneStoreTestPanelView new];
        __weak PaneStoreTestPanelView *weak_context = m_TestContext;
        self.history.SetNavigationStateChangeCallback([weak_context]() noexcept {
            dispatch_assert_main_queue();
            if( PaneStoreTestPanelView *const context = weak_context )
                [NSNotificationCenter.defaultCenter
                    postNotificationName:NCPanelViewContextDidChangeNotification
                                  object:context];
        });
        m_TestConfig = std::make_unique<nc::config::ConfigImpl>(
            R"json({
                "tests": {
                    "pane_store": {
                        "layouts": [
                            {"title": "Icons", "brief": {}},
                            {"title": "Details", "list": {"columns": [], "icon_scale": 1}},
                            {"title": "Gallery", "gallery": {"icon_scale": 1, "text_lines": 2}},
                            {"title": "Disabled", "disabled": null}
                        ]
                    }
                }
            })json",
            std::make_shared<nc::config::NonPersistentOverwritesStorage>(""));
        m_TestLayouts = std::make_shared<nc::panel::PanelViewLayoutsStorage>(
            "tests.pane_store.layouts", *m_TestConfig);
        m_TestLayoutIndex = 0;
        if( const auto layout = m_TestLayouts->GetLayout(m_TestLayoutIndex) )
            [m_TestContext setTestPresentationLayout:*layout];
        m_TestLifecycle = std::make_unique<PaneLifecycleProducer>(_pane_id);
        m_TestLifecycleDeliveries = std::make_shared<size_t>(0);
    }
    return self;
}

- (nc::core::PaneId)paneId
{
    return m_TestPaneId;
}

- (PanelView *)view
{
    return reinterpret_cast<PanelView *>(m_TestContext);
}

- (const nc::panel::data::Model &)data
{
    return m_TestData;
}

- (bool)isDisplayingCommittedData
{
    return true;
}

- (unsigned long)dataGeneration
{
    return m_TestGeneration;
}

- (int)layoutIndex
{
    return m_TestLayoutIndex;
}

- (void)setLayoutIndex:(const int)_layout_index
{
    if( m_TestLayoutIndex == _layout_index )
        return;
    const auto layout = m_TestLayouts->GetLayout(_layout_index);
    if( !layout || layout->is_disabled() )
        return;
    m_TestLayoutIndex = _layout_index;
    [m_TestContext setTestPresentationLayout:*layout];
    [NSNotificationCenter.defaultCenter postNotificationName:NCPanelViewContextDidChangeNotification
                                                      object:m_TestContext];
}

- (nc::panel::PanelViewLayoutsStorage &)layoutStorage
{
    return *m_TestLayouts;
}

- (nc::panel::PaneLifecycleSubscription)subscribeToPaneLifecycle:
    (nc::panel::PaneLifecycleObserver)_observer
{
    auto deliveries = m_TestLifecycleDeliveries;
    return m_TestLifecycle->Subscribe(
        [observer = std::move(_observer), deliveries](const nc::core::PaneLifecycleEvent &_event) {
            ++*deliveries;
            observer(_event);
        });
}

- (nc::panel::data::Model &)mutableTestData
{
    return m_TestData;
}

- (void)setTestGeneration:(const unsigned long)_generation
{
    m_TestGeneration = _generation;
}

- (void)setTestFocusedItem:(VFSListingItem)_item
{
    [m_TestContext setTestItem:std::move(_item)];
}

- (void)publishTestFallbackAtDisabledLayoutIndex
{
    m_TestLayoutIndex = 3;
    [m_TestContext setTestPresentationLayout:*nc::panel::PanelViewLayoutsStorage::LastResortLayout()];
    [NSNotificationCenter.defaultCenter postNotificationName:NCPanelViewContextDidChangeNotification
                                                      object:m_TestContext];
}

- (void)clearTestContext
{
    m_TestContext = nil;
}

- (void)postContextChange
{
    [NSNotificationCenter.defaultCenter postNotificationName:NCPanelViewContextDidChangeNotification object:m_TestContext];
}

- (nc::core::PaneLifecycleProducer &)testLifecycle
{
    return *m_TestLifecycle;
}

- (std::shared_ptr<size_t>)testLifecycleDeliveries
{
    return m_TestLifecycleDeliveries;
}

@end

#define PREFIX "PanelControllerPaneStoreAdapter "

TEST_CASE(PREFIX "rejects missing production sources before the initial read")
{
    REQUIRE(nc::dispatch_is_main_queue());
    CHECK_THROWS_AS(PanelControllerPaneStoreAdapter(nil), std::invalid_argument);

    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:1 paneId:PaneId{1}];
    [controller clearTestContext];
    CHECK_THROWS_AS(PanelControllerPaneStoreAdapter(controller), std::invalid_argument);
}

TEST_CASE(PREFIX "maps every legacy sort mode into semantic pane state")
{
    using SortMode = nc::panel::data::SortMode;
    struct Mapping {
        SortMode::Mode legacy;
        PaneSortKey key;
        PaneSortDirection direction;
        PaneGroupingKey grouping_key;
    };
    constexpr std::array mappings{
        Mapping{SortMode::SortNoSort, PaneSortKey::Unsorted, PaneSortDirection::None, PaneGroupingKey::Name},
        Mapping{SortMode::SortByRawCName, PaneSortKey::RawName, PaneSortDirection::Ascending, PaneGroupingKey::Name},
        Mapping{SortMode::SortByName, PaneSortKey::Name, PaneSortDirection::Ascending, PaneGroupingKey::Name},
        Mapping{SortMode::SortByNameRev, PaneSortKey::Name, PaneSortDirection::Descending, PaneGroupingKey::Name},
        Mapping{SortMode::SortByExt, PaneSortKey::Extension, PaneSortDirection::Ascending, PaneGroupingKey::Extension},
        Mapping{SortMode::SortByExtRev, PaneSortKey::Extension, PaneSortDirection::Descending, PaneGroupingKey::Extension},
        Mapping{SortMode::SortBySize, PaneSortKey::Size, PaneSortDirection::Descending, PaneGroupingKey::Size},
        Mapping{SortMode::SortBySizeRev, PaneSortKey::Size, PaneSortDirection::Ascending, PaneGroupingKey::Size},
        Mapping{SortMode::SortByModTime,
                PaneSortKey::ModifiedTime,
                PaneSortDirection::Descending,
                PaneGroupingKey::ModifiedTime},
        Mapping{SortMode::SortByModTimeRev,
                PaneSortKey::ModifiedTime,
                PaneSortDirection::Ascending,
                PaneGroupingKey::ModifiedTime},
        Mapping{SortMode::SortByBirthTime,
                PaneSortKey::CreatedTime,
                PaneSortDirection::Descending,
                PaneGroupingKey::CreatedTime},
        Mapping{SortMode::SortByBirthTimeRev,
                PaneSortKey::CreatedTime,
                PaneSortDirection::Ascending,
                PaneGroupingKey::CreatedTime},
        Mapping{SortMode::SortByAddTime,
                PaneSortKey::AddedTime,
                PaneSortDirection::Descending,
                PaneGroupingKey::AddedTime},
        Mapping{SortMode::SortByAddTimeRev,
                PaneSortKey::AddedTime,
                PaneSortDirection::Ascending,
                PaneGroupingKey::AddedTime},
        Mapping{SortMode::SortByAccessTime,
                PaneSortKey::AccessedTime,
                PaneSortDirection::Descending,
                PaneGroupingKey::AccessedTime},
        Mapping{SortMode::SortByAccessTimeRev,
                PaneSortKey::AccessedTime,
                PaneSortDirection::Ascending,
                PaneGroupingKey::AccessedTime},
    };

    for( const Mapping &mapping : mappings ) {
        SortMode mode;
        mode.sort = mapping.legacy;
        mode.collation = SortMode::Collation::Natural;
        mode.sep_dirs = true;
        mode.extensionless_dirs = true;
        const auto state = nc::panel::ProjectPaneSortState(mode);
        CAPTURE(static_cast<int>(mapping.legacy));
        CHECK(state.key == mapping.key);
        CHECK(state.direction == mapping.direction);
        CHECK(state.collation == PaneTextCollation::Natural);
        CHECK(state.separates_directories);
        CHECK(state.extensionless_directories);
        CHECK(nc::panel::ProjectPaneGroupingState(mode, false) == PaneGroupingState{});
        const PaneGroupingState grouping = nc::panel::ProjectPaneGroupingState(mode, true);
        CHECK(grouping.enabled);
        CHECK(grouping.key == mapping.grouping_key);
    }

    SortMode mode;
    mode.sort = SortMode::SortByName;
    mode.collation = SortMode::Collation::CaseInsensitive;
    CHECK(nc::panel::ProjectPaneSortState(mode).collation == PaneTextCollation::CaseInsensitive);
    mode.collation = SortMode::Collation::CaseSensitive;
    CHECK(nc::panel::ProjectPaneSortState(mode).collation == PaneTextCollation::CaseSensitive);
    mode.sort = static_cast<SortMode::Mode>(126);
    mode.collation = static_cast<SortMode::Collation>(3);
    const auto invalid = nc::panel::ProjectPaneSortState(mode);
    CHECK(invalid.key == PaneSortKey::Unknown);
    CHECK(invalid.direction == PaneSortDirection::None);
    CHECK(invalid.collation == PaneTextCollation::Unknown);
    const PaneGroupingState invalid_grouping = nc::panel::ProjectPaneGroupingState(mode, true);
    CHECK(invalid_grouping.enabled);
    CHECK(invalid_grouping.key == PaneGroupingKey::Unknown);
}

TEST_CASE(PREFIX "strictly restores every projected legacy sort mode")
{
    using SortMode = nc::panel::data::SortMode;
    constexpr std::array modes{
        SortMode::SortNoSort,
        SortMode::SortByRawCName,
        SortMode::SortByName,
        SortMode::SortByNameRev,
        SortMode::SortByExt,
        SortMode::SortByExtRev,
        SortMode::SortBySize,
        SortMode::SortBySizeRev,
        SortMode::SortByModTime,
        SortMode::SortByModTimeRev,
        SortMode::SortByBirthTime,
        SortMode::SortByBirthTimeRev,
        SortMode::SortByAddTime,
        SortMode::SortByAddTimeRev,
        SortMode::SortByAccessTime,
        SortMode::SortByAccessTimeRev,
    };
    constexpr std::array collations{
        SortMode::Collation::Natural,
        SortMode::Collation::CaseInsensitive,
        SortMode::Collation::CaseSensitive,
    };

    for( size_t mode_index = 0; mode_index < modes.size(); ++mode_index ) {
        for( const SortMode::Collation collation : collations ) {
            SortMode original;
            original.sort = modes[mode_index];
            original.collation = collation;
            original.sep_dirs = (mode_index & 1U) != 0;
            original.extensionless_dirs = (mode_index & 2U) != 0;
            CAPTURE(static_cast<int>(original.sort), static_cast<int>(original.collation));

            const auto restored = nc::panel::RestorePanelSortMode(nc::panel::ProjectPaneSortState(original));
            REQUIRE(restored);
            CHECK(*restored == original);
        }
    }
}

TEST_CASE(PREFIX "rejects incomplete contradictory and unknown semantic sort states")
{
    const auto rejects = [](const nc::core::PaneSortState &_state) {
        CHECK_FALSE(nc::panel::RestorePanelSortMode(_state));
    };

    nc::core::PaneSortState state{
        .key = PaneSortKey::Name,
        .direction = PaneSortDirection::Ascending,
        .collation = PaneTextCollation::Natural,
        .separates_directories = true,
        .extensionless_directories = true,
    };

    state.key = PaneSortKey::Unknown;
    rejects(state);
    state.key = static_cast<PaneSortKey>(255);
    rejects(state);

    state.key = PaneSortKey::Unsorted;
    state.direction = PaneSortDirection::Ascending;
    rejects(state);

    state.key = PaneSortKey::RawName;
    state.direction = PaneSortDirection::Descending;
    rejects(state);

    state.key = PaneSortKey::Name;
    state.direction = PaneSortDirection::None;
    rejects(state);

    state.key = PaneSortKey::Extension;
    rejects(state);
    state.key = PaneSortKey::Size;
    rejects(state);
    state.key = PaneSortKey::ModifiedTime;
    rejects(state);
    state.key = PaneSortKey::CreatedTime;
    rejects(state);
    state.key = PaneSortKey::AddedTime;
    rejects(state);
    state.key = PaneSortKey::AccessedTime;
    rejects(state);

    state.key = PaneSortKey::Name;
    state.direction = static_cast<PaneSortDirection>(255);
    rejects(state);

    state.direction = PaneSortDirection::Ascending;
    state.collation = PaneTextCollation::Unknown;
    rejects(state);
    state.collation = static_cast<PaneTextCollation>(255);
    rejects(state);
}

TEST_CASE(PREFIX "maps actual presentation layouts and only validated layout indexes")
{
    nc::panel::PanelViewLayout icons;
    icons.layout = nc::panel::PanelBriefViewColumnsLayout{};
    CHECK(nc::panel::ProjectPaneViewState(icons, 0) ==
          PaneViewState{.mode = PaneViewMode::Icons, .layout_index = 0});

    nc::panel::PanelViewLayout details;
    details.layout = nc::panel::PanelListViewColumnsLayout{};
    CHECK(nc::panel::ProjectPaneViewState(details, 2) ==
          PaneViewState{.mode = PaneViewMode::Details, .layout_index = 2});

    nc::panel::PanelViewLayout gallery;
    gallery.layout = nc::panel::PanelGalleryViewLayout{};
    CHECK(nc::panel::ProjectPaneViewState(gallery, std::nullopt) ==
          PaneViewState{.mode = PaneViewMode::Gallery});
    CHECK(nc::panel::ProjectPaneViewState(gallery, -1) ==
          PaneViewState{.mode = PaneViewMode::Gallery});

    nc::panel::PanelViewLayout disabled;
    CHECK(nc::panel::ProjectPaneViewState(disabled, 3) == PaneViewState{});
}

TEST_CASE(PREFIX "projects unloaded and uniform committed models")
{
    REQUIRE(nc::dispatch_is_main_queue());
    Model model;
    const PaneGroupingState grouping = nc::panel::ProjectPaneGroupingState(model.SortMode(), true);
    nc::panel::PanelViewLayout layout;
    layout.layout = nc::panel::PanelListViewColumnsLayout{};
    const PaneViewState view_state = nc::panel::ProjectPaneViewState(layout, 1);
    const PaneHistoryAvailability history_availability{
        .can_go_back = true,
        .can_go_forward = true,
    };
    constexpr uint64_t history_entry_id = 77;

    const auto empty =
        ProjectPaneState(model, 3, {}, grouping, view_state, history_availability, history_entry_id);
    CHECK(empty.location_generation == 3);
    CHECK(empty.load_phase == PaneLoadPhase::Empty);
    CHECK(empty.listing == VFSListing::EmptyListing());
    CHECK_FALSE(empty.is_uniform);
    CHECK(empty.host == nullptr);
    CHECK(empty.path.empty());
    CHECK(empty.display_title.empty());
    CHECK(empty.item_count == 0);
    CHECK(empty.selected_count == 0);
    CHECK(empty.selected_items.empty());
    CHECK_FALSE(empty.focused_item);
    CHECK(empty.sort_state.key == PaneSortKey::Name);
    CHECK(empty.sort_state.direction == PaneSortDirection::Ascending);
    CHECK(empty.sort_state.collation == PaneTextCollation::CaseInsensitive);
    CHECK(empty.sort_state.separates_directories);
    CHECK(empty.grouping_state == grouping);
    CHECK(empty.view_state == view_state);
    CHECK(empty.history_availability == history_availability);
    CHECK(empty.current_history_entry_id == history_entry_id);
    CHECK(empty.shows_hidden_files);

    auto filtering = model.HardFiltering();
    filtering.show_hidden = false;
    model.SetHardFiltering(filtering);
    model.Load(UniformListing(), Model::PanelType::Directory);
    model.CustomFlagsSelectSorted(model.SortedIndexForName("beta"), true);
    const auto loaded =
        ProjectPaneState(model, 4, {}, grouping, view_state, history_availability, history_entry_id);
    CHECK(loaded.location_generation == 4);
    CHECK(loaded.load_phase == PaneLoadPhase::Loaded);
    CHECK(loaded.is_uniform);
    CHECK(loaded.host == VFSHost::DummyHost());
    CHECK(loaded.listing == model.ListingPtr());
    CHECK(loaded.path == "/fixture/");
    CHECK(loaded.display_title == model.VerboseDirectoryFullPath());
    CHECK(loaded.item_count == 2);
    CHECK(loaded.selected_count == 1);
    REQUIRE(loaded.selected_items.size() == 1);
    CHECK(loaded.selected_items[0].Filename() == "beta");
    CHECK(loaded.selected_items[0].Listing() == loaded.listing);
    CHECK(loaded.selected_bytes == 31);
    CHECK_FALSE(loaded.focused_item);
    CHECK(loaded.sort_state == empty.sort_state);
    CHECK(loaded.grouping_state == empty.grouping_state);
    CHECK(loaded.view_state == empty.view_state);
    CHECK(loaded.history_availability == empty.history_availability);
    CHECK(loaded.current_history_entry_id == empty.current_history_entry_id);
    CHECK_FALSE(loaded.shows_hidden_files);
}

TEST_CASE(PREFIX "projects exact selected identities in deterministic display order")
{
    REQUIRE(nc::dispatch_is_main_queue());
    Model model;
    nc::panel::data::SortMode sorting;
    sorting.sort = nc::panel::data::SortMode::SortByNameRev;
    model.SetSortMode(sorting);
    model.Load(UniformListing(), Model::PanelType::Directory);
    model.CustomFlagsSelectSorted(model.SortedIndexForName("alpha"), true);
    model.CustomFlagsSelectSorted(model.SortedIndexForName("beta"), true);

    const auto state = ProjectPaneState(model, 5);
    CHECK(state.selected_count == 2);
    REQUIRE(state.selected_items.size() == 2);
    CHECK(state.selected_items[0].Filename() == "beta");
    CHECK(state.selected_items[1].Filename() == "alpha");
    CHECK(state.selected_items[0].Listing() == state.listing);
    CHECK(state.selected_items[1].Listing() == state.listing);
    CHECK(state.selected_items[0] == model.EntryAtSortPosition(model.SortedIndexForName("beta")));
    CHECK(state.selected_items[1] == model.EntryAtSortPosition(model.SortedIndexForName("alpha")));
}

TEST_CASE(PREFIX "projects hidden-file visibility before and after loading")
{
    REQUIRE(nc::dispatch_is_main_queue());
    Model model;

    CHECK(ProjectPaneState(model, 1).shows_hidden_files);

    auto filtering = model.HardFiltering();
    filtering.show_hidden = false;
    model.SetHardFiltering(filtering);
    CHECK_FALSE(ProjectPaneState(model, 1).shows_hidden_files);

    model.Load(UniformListing(), Model::PanelType::Directory);
    CHECK_FALSE(ProjectPaneState(model, 2).shows_hidden_files);

    filtering.show_hidden = true;
    model.SetHardFiltering(filtering);
    CHECK(ProjectPaneState(model, 2).shows_hidden_files);
}

TEST_CASE(PREFIX "projects exact focused items from uniform and non-uniform models")
{
    REQUIRE(nc::dispatch_is_main_queue());
    Model model;
    model.Load(UniformListing(), Model::PanelType::Directory);
    const VFSListingItem uniform_focus = model.EntryAtSortPosition(model.SortedIndexForName("alpha"));
    REQUIRE(uniform_focus);

    const auto uniform = ProjectPaneState(model, 5, uniform_focus);
    REQUIRE(uniform.focused_item);
    CHECK(uniform.focused_item == uniform_focus);
    CHECK(uniform.focused_item.Filename() == "alpha");

    model.Load(NonUniformListing(), Model::PanelType::Temporary);
    const VFSListingItem non_uniform_focus = model.EntryAtSortPosition(model.SortedIndexForName("beta"));
    REQUIRE(non_uniform_focus);

    const auto non_uniform = ProjectPaneState(model, 6, non_uniform_focus);
    REQUIRE(non_uniform.focused_item);
    CHECK(non_uniform.focused_item == non_uniform_focus);
    CHECK(non_uniform.focused_item.Filename() == "beta");
}

TEST_CASE(PREFIX "suppresses stale and foreign focused items")
{
    REQUIRE(nc::dispatch_is_main_queue());
    Model model;
    model.Load(UniformListing(), Model::PanelType::Directory);
    const VFSListingItem stale_focus = model.EntryAtSortPosition(model.SortedIndexForName("alpha"));
    REQUIRE(stale_focus);

    model.Load(UniformListing(), Model::PanelType::Directory);
    CHECK_FALSE(ProjectPaneState(model, 7, stale_focus).focused_item);

    const auto foreign_listing = NonUniformListing();
    const VFSListingItem foreign_focus = foreign_listing->Item(0);
    REQUIRE(foreign_focus);
    CHECK_FALSE(ProjectPaneState(model, 7, foreign_focus).focused_item);
}

TEST_CASE(PREFIX "projects non-uniform listings without requesting a common host")
{
    REQUIRE(nc::dispatch_is_main_queue());
    Model model;
    model.Load(NonUniformListing(), Model::PanelType::Temporary);

    const auto state = ProjectPaneState(model, 8);
    CHECK(state.load_phase == PaneLoadPhase::Loaded);
    CHECK_FALSE(state.is_uniform);
    CHECK(state.host == nullptr);
    CHECK(state.path.empty());
    CHECK(state.display_title == "Search results");
    CHECK(state.item_count == 2);
}

TEST_CASE(PREFIX "reads controller state after scoped context notifications")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:10 paneId:PaneId{41}];
    PaneStoreTestPanelController *const other_controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:20 paneId:PaneId{42}];
    PanelControllerPaneStoreAdapter bridge(controller);

    const auto initial = bridge.Store().Snapshot();
    CHECK(initial.pane_id == PaneId{41});
    CHECK(initial.state.location_generation == 10);
    CHECK(initial.state.load_phase == PaneLoadPhase::Empty);
    CHECK(initial.state.grouping_state == PaneGroupingState{});
    CHECK(initial.state.view_state ==
          PaneViewState{.mode = PaneViewMode::Icons, .layout_index = 0});

    [controller mutableTestData].Load(UniformListing(), Model::PanelType::Directory);
    [controller setTestGeneration:11];
    [other_controller postContextChange];
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
    CHECK(bridge.Store().Snapshot().revision == 0);

    [controller postContextChange];
    REQUIRE(RunMainLoopUntil([&] { return bridge.Store().Snapshot().revision == 1; }));
    const auto updated = bridge.Store().Snapshot();
    CHECK(updated.state.location_generation == 11);
    CHECK(updated.state.load_phase == PaneLoadPhase::Loaded);
    CHECK(updated.state.listing == [controller mutableTestData].ListingPtr());
}

TEST_CASE(PREFIX "rebuilds grouping and layout preferences without replacing listing identity")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:10 paneId:PaneId{411}];
    PanelControllerPaneStoreAdapter bridge(controller);

    const auto initial = bridge.Store().Snapshot();
    CHECK(initial.state.load_phase == PaneLoadPhase::Empty);
    CHECK(initial.state.grouping_state == PaneGroupingState{});
    CHECK(initial.state.view_state ==
          PaneViewState{.mode = PaneViewMode::Icons, .layout_index = 0});

    controller.view.explorerDetailsGroupingEnabled = false;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
    CHECK(bridge.Store().Snapshot().revision == initial.revision);

    controller.view.explorerDetailsGroupingEnabled = true;
    REQUIRE(RunMainLoopUntil([&] {
        return bridge.Store().Snapshot().revision == initial.revision + 1;
    }));
    const auto grouped = bridge.Store().Snapshot();
    CHECK(grouped.state.grouping_state ==
          PaneGroupingState{.enabled = true, .key = PaneGroupingKey::Name});
    CHECK(grouped.listing_generation == initial.listing_generation);

    auto sorting = [controller mutableTestData].SortMode();
    sorting.sort = nc::panel::data::SortMode::SortBySize;
    [controller mutableTestData].SetSortMode(sorting);
    [controller postContextChange];
    REQUIRE(RunMainLoopUntil([&] {
        return bridge.Store().Snapshot().revision == grouped.revision + 1;
    }));
    const auto regrouped = bridge.Store().Snapshot();
    CHECK(regrouped.state.grouping_state ==
          PaneGroupingState{.enabled = true, .key = PaneGroupingKey::Size});
    CHECK(regrouped.listing_generation == grouped.listing_generation);

    controller.layoutIndex = 1;
    REQUIRE(RunMainLoopUntil([&] {
        return bridge.Store().Snapshot().revision == regrouped.revision + 1;
    }));
    const auto details = bridge.Store().Snapshot();
    CHECK(details.state.view_state ==
          PaneViewState{.mode = PaneViewMode::Details, .layout_index = 1});
    CHECK(details.state.grouping_state == regrouped.state.grouping_state);
    CHECK(details.listing_generation == regrouped.listing_generation);

    controller.layoutIndex = 1;
    controller.layoutIndex = 3;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
    CHECK(bridge.Store().Snapshot().revision == details.revision);

    [controller publishTestFallbackAtDisabledLayoutIndex];
    REQUIRE(RunMainLoopUntil([&] {
        return bridge.Store().Snapshot().revision == details.revision + 1;
    }));
    const auto fallback = bridge.Store().Snapshot();
    CHECK(fallback.state.view_state == PaneViewState{.mode = PaneViewMode::Icons});
    CHECK(fallback.listing_generation == details.listing_generation);
}

TEST_CASE(PREFIX "rebuilds history state and preserves navigation event sequencing")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:10 paneId:PaneId{412}];
    PanelControllerPaneStoreAdapter bridge(controller);
    nc::panel::History &history = controller.history;

    const auto initial = bridge.Store().Snapshot();
    CHECK(initial.state.load_phase == PaneLoadPhase::Empty);
    CHECK(initial.state.history_availability == PaneHistoryAvailability{});
    CHECK_FALSE(initial.state.current_history_entry_id);

    const VFSListingPtr first = UniformListing("/history-first/");
    history.Put(*first);
    const auto first_id = history.GetNavigationState().current_entry_id;
    REQUIRE(first_id);
    REQUIRE(RunMainLoopUntil([&] {
        return bridge.Store().Snapshot().revision == initial.revision + 1;
    }));
    const auto first_recorded = bridge.Store().Snapshot();
    CHECK(first_recorded.state.history_availability == PaneHistoryAvailability{});
    CHECK(first_recorded.state.current_history_entry_id == first_id);
    CHECK(first_recorded.listing_generation == initial.listing_generation);

    history.Put(*first);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
    CHECK(bridge.Store().Snapshot().revision == first_recorded.revision);
    CHECK(history.GetNavigationState().current_entry_id == first_id);

    const VFSListingPtr second = UniformListing("/history-second/");
    history.Put(*second);
    const auto second_id = history.GetNavigationState().current_entry_id;
    REQUIRE(second_id);
    CHECK(*second_id > *first_id);
    REQUIRE(RunMainLoopUntil([&] {
        return bridge.Store().Snapshot().revision == first_recorded.revision + 1;
    }));
    const auto recorded = bridge.Store().Snapshot();
    CHECK(recorded.state.history_availability ==
          PaneHistoryAvailability{.can_go_back = true, .can_go_forward = false});
    CHECK(recorded.state.current_history_entry_id == second_id);
    CHECK(recorded.listing_generation == initial.listing_generation);

    history.MoveBack();
    const auto request = [controller testLifecycle].Start(NavigationDescriptor("/history-first/"));
    const auto started = bridge.Store().Snapshot();
    CHECK(started.revision == recorded.revision + 1);
    CHECK(started.state.load_phase == PaneLoadPhase::Loading);
    CHECK(started.state.history_availability == recorded.state.history_availability);
    CHECK(started.state.current_history_entry_id == second_id);
    CHECK(started.listing_generation == recorded.listing_generation);

    REQUIRE(RunMainLoopUntil([&] {
        return bridge.Store().Snapshot().revision == recorded.revision + 2;
    }));
    const auto moved_back = bridge.Store().Snapshot();
    CHECK(moved_back.state.load_phase == PaneLoadPhase::Loading);
    CHECK(moved_back.state.history_availability ==
          PaneHistoryAvailability{.can_go_back = false, .can_go_forward = true});
    CHECK(moved_back.state.current_history_entry_id == first_id);
    CHECK(moved_back.listing_generation == recorded.listing_generation);

    CHECK([controller testLifecycle].Finish(
              request,
              PaneLifecycleCancelled{nc::core::PaneCancellationReason::User}) ==
          PaneLifecycleProducer::FinishResult::Published);
    const auto cancelled = bridge.Store().Snapshot();
    CHECK(cancelled.state.load_phase == PaneLoadPhase::Empty);
    CHECK(cancelled.state.history_availability == moved_back.state.history_availability);
    CHECK(cancelled.state.current_history_entry_id == first_id);

    history.MoveForth();
    REQUIRE(RunMainLoopUntil([&] {
        return bridge.Store().Snapshot().revision == cancelled.revision + 1;
    }));
    const auto moved_forward = bridge.Store().Snapshot();
    CHECK(moved_forward.state.history_availability == recorded.state.history_availability);
    CHECK(moved_forward.state.current_history_entry_id == second_id);
    CHECK(moved_forward.listing_generation == recorded.listing_generation);
}

TEST_CASE(PREFIX "publishes identity-only history movement with a stable listing generation")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:10 paneId:PaneId{413}];
    PanelControllerPaneStoreAdapter bridge(controller);
    nc::panel::History &history = controller.history;
    const auto initial = bridge.Store().Snapshot();

    history.Put(*UniformListing("/history-one/"));
    history.Put(*UniformListing("/history-two/"));
    history.Put(*UniformListing("/history-three/"));
    history.Put(*UniformListing("/history-four/"));
    const auto fourth_id = history.GetNavigationState().current_entry_id;
    REQUIRE(fourth_id);
    REQUIRE(RunMainLoopUntil([&] {
        return bridge.Store().Snapshot().revision == initial.revision + 1;
    }));
    const auto recorded = bridge.Store().Snapshot();
    CHECK(recorded.state.history_availability ==
          PaneHistoryAvailability{.can_go_back = true, .can_go_forward = false});
    CHECK(recorded.state.current_history_entry_id == fourth_id);

    history.MoveBack();
    const auto third_id = history.GetNavigationState().current_entry_id;
    REQUIRE(third_id);
    REQUIRE(RunMainLoopUntil([&] {
        return bridge.Store().Snapshot().revision == recorded.revision + 1;
    }));
    const auto first_middle = bridge.Store().Snapshot();
    CHECK(first_middle.state.history_availability ==
          PaneHistoryAvailability{.can_go_back = true, .can_go_forward = true});
    CHECK(first_middle.state.current_history_entry_id == third_id);
    CHECK(first_middle.listing_generation == recorded.listing_generation);

    history.MoveBack();
    const auto second_id = history.GetNavigationState().current_entry_id;
    REQUIRE(second_id);
    CHECK(second_id != third_id);
    REQUIRE(RunMainLoopUntil([&] {
        return bridge.Store().Snapshot().revision == first_middle.revision + 1;
    }));
    const auto second_middle = bridge.Store().Snapshot();
    CHECK(second_middle.state.history_availability == first_middle.state.history_availability);
    CHECK(second_middle.state.current_history_entry_id == second_id);
    CHECK(second_middle.listing_generation == recorded.listing_generation);
}

TEST_CASE(PREFIX "rebuilds hidden-file visibility after a scoped context notification")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:11 paneId:PaneId{45}];
    [controller mutableTestData].Load(UniformListing(), Model::PanelType::Directory);
    PanelControllerPaneStoreAdapter bridge(controller);

    const auto initial = bridge.Store().Snapshot();
    CHECK(initial.state.shows_hidden_files);

    auto filtering = [controller mutableTestData].HardFiltering();
    filtering.show_hidden = false;
    [controller mutableTestData].SetHardFiltering(filtering);
    [controller postContextChange];

    REQUIRE(RunMainLoopUntil([&] { return bridge.Store().Snapshot().revision == initial.revision + 1; }));
    const auto updated = bridge.Store().Snapshot();
    CHECK_FALSE(updated.state.shows_hidden_files);
    CHECK(updated.state.listing == initial.state.listing);
    CHECK(updated.listing_generation == initial.listing_generation);
}

TEST_CASE(PREFIX "rebuilds semantic sort state and rematerializes selected display order")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:11 paneId:PaneId{451}];
    Model &model = [controller mutableTestData];
    model.Load(UniformListing(), Model::PanelType::Directory);
    model.CustomFlagsSelectSorted(model.SortedIndexForName("alpha"), true);
    model.CustomFlagsSelectSorted(model.SortedIndexForName("beta"), true);
    PanelControllerPaneStoreAdapter bridge(controller);

    const auto initial = bridge.Store().Snapshot();
    CHECK(initial.state.sort_state.key == PaneSortKey::Name);
    CHECK(initial.state.sort_state.direction == PaneSortDirection::Ascending);
    REQUIRE(initial.state.selected_items.size() == 2);
    CHECK(initial.state.selected_items[0].Filename() == "alpha");
    CHECK(initial.state.selected_items[1].Filename() == "beta");
    const auto *const initial_selection = initial.state.selected_items.StorageIdentity();
    REQUIRE(initial_selection != nullptr);

    auto sorting = model.SortMode();
    sorting.sort = nc::panel::data::SortMode::SortByNameRev;
    model.SetSortMode(sorting);
    [controller postContextChange];

    REQUIRE(RunMainLoopUntil([&] { return bridge.Store().Snapshot().revision == initial.revision + 1; }));
    const auto reversed = bridge.Store().Snapshot();
    CHECK(reversed.listing_generation == initial.listing_generation);
    CHECK(reversed.state.listing == initial.state.listing);
    CHECK(reversed.state.sort_state.key == PaneSortKey::Name);
    CHECK(reversed.state.sort_state.direction == PaneSortDirection::Descending);
    REQUIRE(reversed.state.selected_items.size() == 2);
    CHECK(reversed.state.selected_items[0].Filename() == "beta");
    CHECK(reversed.state.selected_items[1].Filename() == "alpha");
    CHECK(reversed.state.selected_items.StorageIdentity() != initial_selection);

    [controller postContextChange];
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
    CHECK(bridge.Store().Snapshot().revision == reversed.revision);
    CHECK(bridge.Store().Snapshot().listing_generation == reversed.listing_generation);
}

TEST_CASE(PREFIX "rebuilds exact selection without advancing listing generation")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:11 paneId:PaneId{46}];
    [controller mutableTestData].Load(UniformListing(), Model::PanelType::Directory);
    PanelControllerPaneStoreAdapter bridge(controller);

    const auto initial = bridge.Store().Snapshot();
    CHECK(initial.state.selected_items.empty());
    CHECK(initial.state.selected_count == 0);

    const int beta_position = [controller mutableTestData].SortedIndexForName("beta");
    REQUIRE(beta_position >= 0);
    [controller mutableTestData].CustomFlagsSelectSorted(beta_position, true);
    [controller postContextChange];

    REQUIRE(RunMainLoopUntil([&] { return bridge.Store().Snapshot().revision == initial.revision + 1; }));
    const auto selected = bridge.Store().Snapshot();
    CHECK(selected.listing_generation == initial.listing_generation);
    CHECK(selected.state.selected_count == 1);
    REQUIRE(selected.state.selected_items.size() == 1);
    CHECK(selected.state.selected_items[0].Filename() == "beta");
    CHECK(selected.state.selected_items[0].Listing() == selected.state.listing);
    const auto *const selected_payload = selected.state.selected_items.StorageIdentity();
    REQUIRE(selected_payload != nullptr);

    [controller postContextChange];
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
    CHECK(bridge.Store().Snapshot().revision == selected.revision);
    CHECK(bridge.Store().Snapshot().state.selected_items.StorageIdentity() == selected_payload);

    nc::panel::data::SortMode sorting;
    sorting.sort = nc::panel::data::SortMode::SortByNameRev;
    [controller mutableTestData].SetSortMode(sorting);
    [controller postContextChange];
    REQUIRE(RunMainLoopUntil([&] { return bridge.Store().Snapshot().revision == selected.revision + 1; }));
    const auto sorted = bridge.Store().Snapshot();
    CHECK(sorted.listing_generation == selected.listing_generation);
    CHECK(sorted.state.sort_state.key == PaneSortKey::Name);
    CHECK(sorted.state.sort_state.direction == PaneSortDirection::Descending);
    CHECK(sorted.state.selected_items.StorageIdentity() == selected_payload);

    const int sorted_beta_position = [controller mutableTestData].SortedIndexForName("beta");
    REQUIRE(sorted_beta_position >= 0);
    [controller mutableTestData].CustomFlagsSelectSorted(sorted_beta_position, false);
    [controller postContextChange];
    REQUIRE(RunMainLoopUntil([&] { return bridge.Store().Snapshot().revision == sorted.revision + 1; }));
    const auto cleared = bridge.Store().Snapshot();
    CHECK(cleared.listing_generation == sorted.listing_generation);
    CHECK(cleared.state.selected_count == 0);
    CHECK(cleared.state.selected_items.empty());
}

TEST_CASE(PREFIX "rebuilds focused item after a cursor-only context notification")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:12 paneId:PaneId{43}];
    [controller mutableTestData].Load(UniformListing(), Model::PanelType::Directory);
    const VFSListingItem alpha =
        [controller mutableTestData].EntryAtSortPosition([controller mutableTestData].SortedIndexForName("alpha"));
    const VFSListingItem beta =
        [controller mutableTestData].EntryAtSortPosition([controller mutableTestData].SortedIndexForName("beta"));
    REQUIRE(alpha);
    REQUIRE(beta);
    [controller mutableTestData].CustomFlagsSelectSorted(
        [controller mutableTestData].SortedIndexForName("beta"), true);
    [controller setTestFocusedItem:alpha];
    PanelControllerPaneStoreAdapter bridge(controller);

    const auto initial = bridge.Store().Snapshot();
    REQUIRE(initial.state.focused_item);
    CHECK(initial.state.focused_item == alpha);
    CHECK(initial.state.location_generation == 12);
    REQUIRE(initial.state.selected_items.size() == 1);
    const auto *const selected_payload = initial.state.selected_items.StorageIdentity();
    REQUIRE(selected_payload != nullptr);

    [controller setTestFocusedItem:beta];
    [controller postContextChange];
    REQUIRE(RunMainLoopUntil([&] { return bridge.Store().Snapshot().revision == initial.revision + 1; }));
    const auto updated = bridge.Store().Snapshot();
    REQUIRE(updated.state.focused_item);
    CHECK(updated.state.focused_item == beta);
    CHECK(updated.state.location_generation == initial.state.location_generation);
    CHECK(updated.state.listing == initial.state.listing);
    CHECK(updated.listing_generation == initial.listing_generation);
    CHECK(updated.state.selected_items.StorageIdentity() == selected_payload);
}

TEST_CASE(PREFIX "does not reinterpret the old cursor against a newly committed listing")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:12 paneId:PaneId{44}];
    [controller mutableTestData].Load(UniformListing(), Model::PanelType::Directory);
    const VFSListingItem old_focus =
        [controller mutableTestData].EntryAtSortPosition([controller mutableTestData].SortedIndexForName("alpha"));
    REQUIRE(old_focus);
    [controller setTestFocusedItem:old_focus];
    PanelControllerPaneStoreAdapter bridge(controller);

    const auto request = [controller testLifecycle].Start(NavigationDescriptor("/search/"));
    const auto new_listing = NonUniformListing();
    [controller mutableTestData].Load(new_listing, Model::PanelType::Temporary);
    [controller setTestGeneration:13];
    const VFSListingItem premature_focus =
        [controller mutableTestData].EntryAtSortPosition([controller mutableTestData].SortedIndexForName("alpha"));
    const VFSListingItem restored_focus =
        [controller mutableTestData].EntryAtSortPosition([controller mutableTestData].SortedIndexForName("beta"));
    REQUIRE(premature_focus);
    REQUIRE(restored_focus);
    // The view can already resolve the old numeric cursor through the new model before it emits
    // the context notification that establishes the semantically restored cursor.
    [controller setTestFocusedItem:premature_focus];

    CHECK([controller testLifecycle].Finish(
              request,
              PaneLifecycleCommitted{.controller_generation = 13, .listing = new_listing}) ==
          PaneLifecycleProducer::FinishResult::Published);
    const auto committed = bridge.Store().Snapshot();
    CHECK(committed.state.listing == new_listing);
    CHECK_FALSE(committed.state.focused_item);

    [controller postContextChange];
    // Cursor restoration remains synchronous but need not emit a second notification when its
    // numeric position is unchanged. The queued rebuild must sample this final live item.
    [controller setTestFocusedItem:restored_focus];
    REQUIRE(RunMainLoopUntil([&] { return bridge.Store().Snapshot().revision == committed.revision + 1; }));
    const auto restored = bridge.Store().Snapshot();
    REQUIRE(restored.state.focused_item);
    CHECK(restored.state.focused_item == restored_focus);
}

TEST_CASE(PREFIX "owns its controller and snapshots retain engine references after teardown")
{
    REQUIRE(nc::dispatch_is_main_queue());
    __weak PaneStoreTestPanelController *weak_controller = nil;
    NSObject *context = nil;
    std::unique_ptr<PanelControllerPaneStoreAdapter> bridge;
    nc::core::PaneSnapshot retained_snapshot;

    @autoreleasepool {
        PaneStoreTestPanelController *controller =
            [[PaneStoreTestPanelController alloc] initWithGeneration:7 paneId:PaneId{5}];
        [controller mutableTestData].Load(UniformListing(), Model::PanelType::Directory);
        [controller mutableTestData].CustomFlagsSelectSorted(
            [controller mutableTestData].SortedIndexForName("beta"), true);
        const VFSListingItem focused_item =
            [controller mutableTestData].EntryAtSortPosition([controller mutableTestData].SortedIndexForName("beta"));
        REQUIRE(focused_item);
        [controller setTestFocusedItem:focused_item];
        weak_controller = controller;
        context = controller.view;
        bridge = std::make_unique<PanelControllerPaneStoreAdapter>(controller);
        retained_snapshot = bridge->Store().Snapshot();
        controller = nil;

        CHECK(weak_controller != nil);
        CHECK(retained_snapshot.state.listing != nullptr);
        CHECK(retained_snapshot.state.host != nullptr);
        CHECK(retained_snapshot.state.focused_item == focused_item);
        REQUIRE(retained_snapshot.state.selected_items.size() == 1);
        CHECK(retained_snapshot.state.selected_items[0] == focused_item);
        bridge.reset();
    }

    CHECK(weak_controller == nil);
    CHECK(retained_snapshot.state.listing != nullptr);
    CHECK(retained_snapshot.state.host != nullptr);
    REQUIRE(retained_snapshot.state.focused_item);
    REQUIRE(retained_snapshot.state.selected_items.size() == 1);
    CHECK(retained_snapshot.state.focused_item.Filename() == "beta");
    CHECK(retained_snapshot.state.focused_item.Listing() == retained_snapshot.state.listing);
    CHECK(retained_snapshot.state.selected_items[0].Filename() == "beta");
    CHECK(retained_snapshot.state.selected_items[0].Listing() == retained_snapshot.state.listing);
    [NSNotificationCenter.defaultCenter postNotificationName:NCPanelViewContextDidChangeNotification object:context];
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
}

TEST_CASE(PREFIX "seeds an active navigation and commits the exact post-model projection once")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:7 paneId:PaneId{61}];
    const auto old_listing = UniformListing();
    [controller mutableTestData].Load(old_listing, Model::PanelType::Directory);
    const auto request = [controller testLifecycle].Start(NavigationDescriptor("/next/"));

    PanelControllerPaneStoreAdapter bridge(controller);
    const auto seeded = bridge.Store().Snapshot();
    CHECK(seeded.state.load_phase == PaneLoadPhase::Loading);
    CHECK(seeded.state.location_generation == 7);
    CHECK(seeded.state.listing == old_listing);

    std::vector<nc::core::PaneSnapshot> observed;
    const auto ticket = bridge.Store().Observe(
        [&](const nc::core::PaneSnapshot &_snapshot) { observed.emplace_back(_snapshot); });
    const auto new_listing = UniformListing();
    [controller mutableTestData].Load(new_listing, Model::PanelType::Directory);
    [controller setTestGeneration:8];
    CHECK([controller testLifecycle].Finish(
              request,
              PaneLifecycleCommitted{.controller_generation = 8, .listing = new_listing}) ==
          PaneLifecycleProducer::FinishResult::Published);
    [controller postContextChange];
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);

    REQUIRE(observed.size() == 1);
    CHECK(observed[0].state.load_phase == PaneLoadPhase::Loaded);
    CHECK(observed[0].state.location_generation == 8);
    CHECK(observed[0].state.listing == new_listing);
    CHECK(observed[0].listing_generation == 1);
    CHECK(bridge.Store().Snapshot() == observed[0]);
}

TEST_CASE(PREFIX "commits an exact same-generation refresh projection with new listing content")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:7 paneId:PaneId{611}];
    const auto old_listing = UniformListing();
    [controller mutableTestData].Load(old_listing, Model::PanelType::Directory);
    PanelControllerPaneStoreAdapter bridge(controller);
    const auto initial = bridge.Store().Snapshot();

    const auto request = [controller testLifecycle].Start(RefreshDescriptor());
    const auto refreshing = bridge.Store().Snapshot();
    auto expected_refreshing = initial.state;
    expected_refreshing.load_phase = PaneLoadPhase::Refreshing;
    CHECK(refreshing.state == expected_refreshing);
    CHECK(refreshing.revision == initial.revision + 1);
    CHECK(refreshing.listing_generation == initial.listing_generation);

    const auto new_listing = UniformListing();
    REQUIRE(new_listing != old_listing);
    [controller mutableTestData].Load(new_listing, Model::PanelType::Directory);
    const auto exact_post_model_projection = ProjectPaneState(
        [controller data],
        7,
        {},
        initial.state.grouping_state,
        initial.state.view_state);
    REQUIRE(exact_post_model_projection.listing == new_listing);
    CHECK([controller testLifecycle].Finish(
              request,
              PaneLifecycleCommitted{.controller_generation = 7, .listing = new_listing}) ==
          PaneLifecycleProducer::FinishResult::Published);

    const auto committed = bridge.Store().Snapshot();
    CHECK(committed.state == exact_post_model_projection);
    CHECK(committed.state.load_phase == PaneLoadPhase::Loaded);
    CHECK(committed.state.location_generation == initial.state.location_generation);
    CHECK(committed.state.listing == new_listing);
    CHECK_FALSE(committed.state.visible_error);
    CHECK(committed.revision == initial.revision + 2);
    CHECK(committed.listing_generation == initial.listing_generation + 1);
}

TEST_CASE(PREFIX "reduces navigation failure and cancellation without replacing committed content")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:4 paneId:PaneId{62}];
    const auto listing = UniformListing();
    [controller mutableTestData].Load(listing, Model::PanelType::Directory);
    PanelControllerPaneStoreAdapter bridge(controller);

    const auto failed_request = [controller testLifecycle].Start(NavigationDescriptor("/missing/"));
    const auto error = NavigationFailure();
    CHECK([controller testLifecycle].Finish(failed_request, PaneLifecycleFailed{error}) ==
          PaneLifecycleProducer::FinishResult::Published);
    const auto failed = bridge.Store().Snapshot();
    CHECK(failed.state.load_phase == PaneLoadPhase::Failed);
    CHECK(failed.state.listing == listing);
    REQUIRE(failed.state.visible_error);
    CHECK(*failed.state.visible_error == error);
    CHECK(failed.listing_generation == 0);

    const auto cancelled_request = [controller testLifecycle].Start(NavigationDescriptor("/cancelled/"));
    CHECK([controller testLifecycle].Finish(
              cancelled_request,
              PaneLifecycleCancelled{nc::core::PaneCancellationReason::User}) ==
          PaneLifecycleProducer::FinishResult::Published);
    const auto cancelled = bridge.Store().Snapshot();
    CHECK(cancelled.state.load_phase == PaneLoadPhase::Loaded);
    CHECK(cancelled.state.listing == listing);
    CHECK_FALSE(cancelled.state.visible_error);
    CHECK(cancelled.listing_generation == 0);
}

TEST_CASE(PREFIX "replays a retained navigation failure at construction and resumes contiguously")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:4 paneId:PaneId{621}];
    const auto listing = UniformListing();
    [controller mutableTestData].Load(listing, Model::PanelType::Directory);

    const auto failed_request = [controller testLifecycle].Start(NavigationDescriptor("/missing/"));
    const auto error = NavigationFailure();
    CHECK([controller testLifecycle].Finish(failed_request, PaneLifecycleFailed{error}) ==
          PaneLifecycleProducer::FinishResult::Published);
    [[maybe_unused]] const auto rejected = [controller testLifecycle].Reject(
        NavigationDescriptor("/invalid/"),
        PaneLifecycleRejected{.reason = PaneRejectionReason::InvalidRequest});

    PanelControllerPaneStoreAdapter bridge(controller);
    const auto failed = bridge.Store().Snapshot();
    CHECK(failed.state.load_phase == PaneLoadPhase::Failed);
    CHECK(failed.state.listing == listing);
    REQUIRE(failed.state.visible_error);
    CHECK(*failed.state.visible_error == error);

    const auto next = [controller testLifecycle].Start(NavigationDescriptor("/next/"));
    const auto loading = bridge.Store().Snapshot();
    CHECK(loading.state.load_phase == PaneLoadPhase::Loading);
    CHECK_FALSE(loading.state.visible_error);
    CHECK([controller testLifecycle].Finish(
              next,
              PaneLifecycleCancelled{nc::core::PaneCancellationReason::User}) ==
          PaneLifecycleProducer::FinishResult::Published);
    CHECK(bridge.Store().Snapshot().state.load_phase == PaneLoadPhase::Loaded);
}

TEST_CASE(PREFIX "constructs reentrantly during Failed from one seed and replay boundary")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:5 paneId:PaneId{622}];
    const auto listing = UniformListing();
    [controller mutableTestData].Load(listing, Model::PanelType::Directory);
    std::unique_ptr<PanelControllerPaneStoreAdapter> bridge;
    const auto construction_hook = [controller testLifecycle].Observe(
        [&](const PaneLifecycleEvent &_event) {
            if( std::holds_alternative<PaneLifecycleFailed>(_event.payload) )
                bridge = std::make_unique<PanelControllerPaneStoreAdapter>(controller);
        });

    const auto failed_request = [controller testLifecycle].Start(NavigationDescriptor("/failed/"));
    const auto error = NavigationFailure();
    CHECK([controller testLifecycle].Finish(failed_request, PaneLifecycleFailed{error}) ==
          PaneLifecycleProducer::FinishResult::Published);

    REQUIRE(bridge);
    const auto snapshot = bridge->Store().Snapshot();
    CHECK(snapshot.state.load_phase == PaneLoadPhase::Failed);
    CHECK(snapshot.state.listing == listing);
    REQUIRE(snapshot.state.visible_error);
    CHECK(*snapshot.state.visible_error == error);
}

TEST_CASE(PREFIX "same-kind supersession has no intermediate committed publication")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:1 paneId:PaneId{63}];
    [controller mutableTestData].Load(UniformListing(), Model::PanelType::Directory);
    PanelControllerPaneStoreAdapter bridge(controller);
    std::vector<PaneLoadPhase> phases;
    const auto ticket = bridge.Store().Observe(
        [&](const nc::core::PaneSnapshot &_snapshot) { phases.emplace_back(_snapshot.state.load_phase); });

    const auto first = [controller testLifecycle].Start(NavigationDescriptor("/first/"));
    const auto second = [controller testLifecycle].SupersedeAndStart(NavigationDescriptor("/second/"));
    CHECK([controller testLifecycle].Finish(
              first,
              PaneLifecycleCancelled{nc::core::PaneCancellationReason::InternalAbort}) ==
          PaneLifecycleProducer::FinishResult::StaleRequest);
    const auto final_listing = UniformListing();
    [controller mutableTestData].Load(final_listing, Model::PanelType::Directory);
    [controller setTestGeneration:2];
    CHECK([controller testLifecycle].Finish(
              second,
              PaneLifecycleCommitted{.controller_generation = 2, .listing = final_listing}) ==
          PaneLifecycleProducer::FinishResult::Published);

    CHECK(phases == std::vector<PaneLoadPhase>{PaneLoadPhase::Loading, PaneLoadPhase::Loaded});
}

TEST_CASE(PREFIX "teardown removes lifecycle and context observations")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:1 paneId:PaneId{64}];
    const auto deliveries = [controller testLifecycleDeliveries];
    auto bridge = std::make_unique<PanelControllerPaneStoreAdapter>(controller);
    const auto request = [controller testLifecycle].Start(NavigationDescriptor());
    CHECK(*deliveries == 1);
    bridge.reset();

    CHECK([controller testLifecycle].Finish(
              request,
              PaneLifecycleCancelled{nc::core::PaneCancellationReason::User}) ==
          PaneLifecycleProducer::FinishResult::Published);
    [controller postContextChange];
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
    CHECK(*deliveries == 1);
}

TEST_CASE(PREFIX "teardown from a pane snapshot callback completes the lifecycle delivery safely")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:1 paneId:PaneId{65}];
    [controller mutableTestData].Load(UniformListing(), Model::PanelType::Directory);
    const auto deliveries = [controller testLifecycleDeliveries];
    auto bridge = std::make_unique<PanelControllerPaneStoreAdapter>(controller);
    nc::core::PaneStoreAdapter::ObservationTicket snapshot_observation;
    snapshot_observation = bridge->Store().Observe([&](const nc::core::PaneSnapshot &_snapshot) {
        if( _snapshot.state.load_phase == PaneLoadPhase::Loading )
            bridge.reset();
    });

    const auto request = [controller testLifecycle].Start(NavigationDescriptor("/teardown/"));
    CHECK_FALSE(bridge);
    CHECK(*deliveries == 1);

    CHECK([controller testLifecycle].Finish(
              request,
              PaneLifecycleCancelled{nc::core::PaneCancellationReason::User}) ==
          PaneLifecycleProducer::FinishResult::Published);
    CHECK(*deliveries == 1);
    snapshot_observation = {};
}

TEST_CASE(PREFIX "teardown from an earlier lifecycle observer invalidates the captured bridge callback")
{
    REQUIRE(nc::dispatch_is_main_queue());
    PaneStoreTestPanelController *const controller =
        [[PaneStoreTestPanelController alloc] initWithGeneration:1 paneId:PaneId{66}];
    [controller mutableTestData].Load(UniformListing(), Model::PanelType::Directory);
    std::unique_ptr<PanelControllerPaneStoreAdapter> bridge;
    const auto destruction_hook = [controller testLifecycle].Observe(
        [&](const PaneLifecycleEvent &_event) {
            if( std::holds_alternative<nc::core::PaneLifecycleStarted>(_event.payload) )
                bridge.reset();
        });
    const auto deliveries = [controller testLifecycleDeliveries];
    bridge = std::make_unique<PanelControllerPaneStoreAdapter>(controller);

    const auto request = [controller testLifecycle].Start(NavigationDescriptor("/destroy-bridge/"));
    CHECK_FALSE(bridge);
    // FireObservers captured the bridge callback before the earlier hook removed its ticket. The
    // wrapper is invoked once, while its weak bridge lock prevents reduction after destruction.
    CHECK(*deliveries == 1);

    CHECK([controller testLifecycle].Finish(
              request,
              PaneLifecycleCancelled{nc::core::PaneCancellationReason::User}) ==
          PaneLifecycleProducer::FinishResult::Published);
    CHECK(*deliveries == 1);
}
