// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <Utility/TemporaryFileStorage.h>
#include <Utility/UTI.h>
#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/States/Explorer/NCExplorerInspectorView.h>
#include <WinCommander/States/Explorer/NCExplorerPaneStateView.h>
#include <WinCommander/States/Explorer/NCExplorerState.h>
#include <WinCommander/States/FilePanels/PanelController.h>
#include <WinCommander/States/FilePanels/PanelView.h>
#include <WinCommander/States/FilePanels/Gallery/PanelGalleryCentralView.h>
#include <WinCommander/States/FilePanels/Views/QuickLookPanel.h>
#include <WinCommander/States/FilePanels/Views/QuickLookVFSBridge.h>
#include <Panel/PanelData.h>
#include <cerrno>
#include <sys/dirent.h>
#include <sys/stat.h>

@interface NCExplorerInspectorView (ExplorerInspectorViewTesting)
@property(nonatomic, readonly) nc::explorer::InspectorState renderedStateForTesting;
@property(nonatomic, readonly) BOOL previewVisibleForTesting;
@property(nonatomic, readonly) BOOL refreshingVisibleForTesting;
@property(nonatomic, readonly) BOOL errorVisibleForTesting;
@property(nonatomic, readonly) NSString *statusTextForTesting;
@property(nonatomic, readonly) NSString *errorTextForTesting;
@property(nonatomic, readonly) NSString *multipleSummaryForTesting;
@property(nonatomic, readonly) NSTextField *filenameFieldForTesting;
@property(nonatomic, readonly) NSTextField *pathFieldForTesting;
@property(nonatomic, readonly) NSTextField *sizeFieldForTesting;
@property(nonatomic, readonly) NSTextField *createdFieldForTesting;
@property(nonatomic, readonly) NSTextField *modifiedFieldForTesting;
@property(nonatomic, readonly) NSTextField *accessedFieldForTesting;
@property(nonatomic, readonly) NSTextField *tagsFieldForTesting;
@property(nonatomic, readonly) NSTextField *permissionsFieldForTesting;
@property(nonatomic, readonly) NSTextField *ownerGroupFieldForTesting;
@property(nonatomic, readonly) NCPanelGalleryCentralView *previewViewForTesting;
@end

@interface NCExplorerState (ExplorerInspectorViewTesting)
- (instancetype)initForTestingWithFrame:(NSRect)_frame
                        panelController:(PanelController *)_panel
                              panelView:(NSView *)_panel_view
                          inspectorView:(NCExplorerInspectorView *)_inspector
                         QLPanelAdaptor:(NCPanelQLPanelAdaptor *)_ql_panel_adaptor;
- (void)applyPaneSnapshotForTesting:(const nc::core::PaneSnapshot &)_snapshot;
@property(nonatomic, readonly) NSSplitView *contentSplitViewForTesting;
@property(nonatomic, readonly) NSView *panelContainerForTesting;
@property(nonatomic, readonly) NCExplorerInspectorView *inspectorViewForTesting;
@property(nonatomic, readonly) NCExplorerPaneStateView *paneStateViewForTesting;
@end

@interface NCExplorerPaneStateView (ExplorerInspectorStateTesting)
@property(nonatomic, readonly) nc::core::PaneVisualKind renderedKindForTesting;
@property(nonatomic, readonly) BOOL skeletonVisibleForTesting;
@property(nonatomic, readonly) NSString *messageTextForTesting;
@end

@interface ExplorerInspectorStatePanelView : NSView
- (instancetype)initWithModel:(nc::panel::data::Model &)_model;
@property(nonatomic, weak) id<NCPanelViewKeystrokeSink> keystrokeSink;
@property(nonatomic) BOOL headerBarVisible;
@end

@implementation ExplorerInspectorStatePanelView {
    nc::panel::data::Model *m_TestModel;
    int m_TestCursor;
    __weak id<NCPanelViewKeystrokeSink> _keystrokeSink;
    BOOL _headerBarVisible;
}

@synthesize keystrokeSink = _keystrokeSink;
@synthesize headerBarVisible = _headerBarVisible;

- (instancetype)initWithModel:(nc::panel::data::Model &)_model
{
    self = [super initWithFrame:NSMakeRect(0, 0, 600, 500)];
    if( self ) {
        m_TestModel = &_model;
        m_TestCursor = -1;
    }
    return self;
}

- (void)setCurpos:(int)_position
{
    m_TestCursor = m_TestModel->IsValidSortPosition(_position) ? _position : -1;
}

- (int)curpos
{
    return m_TestCursor;
}

- (VFSListingItem)item
{
    return m_TestModel->EntryAtSortPosition(m_TestCursor);
}

- (void)addKeystrokeSink:(id<NCPanelViewKeystrokeSink>)_sink
{
    self.keystrokeSink = _sink;
}

- (void)removeKeystrokeSink:(id<NCPanelViewKeystrokeSink>)_sink
{
    if( self.keystrokeSink == _sink )
        self.keystrokeSink = nil;
}

@end

@interface ExplorerInspectorStatePanelController : PanelController
- (instancetype)initWithModel:(nc::panel::data::Model &)_model
                         view:(ExplorerInspectorStatePanelView *)_view
                       paneID:(nc::core::PaneId)_pane_id;
@end

@implementation ExplorerInspectorStatePanelController {
    nc::panel::data::Model *m_TestModel;
    ExplorerInspectorStatePanelView *m_TestView;
    nc::core::PaneId m_TestPaneID;
}

- (instancetype)initWithModel:(nc::panel::data::Model &)_model
                         view:(ExplorerInspectorStatePanelView *)_view
                       paneID:(nc::core::PaneId)_pane_id
{
    self = [super init];
    if( self ) {
        m_TestModel = &_model;
        m_TestView = _view;
        m_TestPaneID = _pane_id;
    }
    return self;
}

- (const nc::panel::data::Model &)data
{
    return *m_TestModel;
}

- (PanelView *)view
{
    return reinterpret_cast<PanelView *>(m_TestView);
}

- (nc::core::PaneId)paneId
{
    return m_TestPaneID;
}

@end

@interface NCPanelGalleryCentralView (ExplorerInspectorViewTesting)
@property(nonatomic, readonly) uint64_t previewTicketForTesting;
@property(nonatomic, readonly) BOOL hasPublishedPreviewForTesting;
@property(nonatomic, readonly) NSString *currentPreviewPathForTesting;
- (void)commitPreviewedIcon:(NSImage *)_image ticket:(uint64_t)_ticket;
- (void)commitVFSQL:(const VFSListingItem &)_item native:(NSURL *)_url ticket:(uint64_t)_ticket;
@end

namespace {

class TestUTIDB final : public nc::utility::UTIDB
{
public:
    std::string UTIForExtension(std::string_view) const override { return "dyn.test"; }
    bool IsDeclaredUTI(std::string_view) const override { return false; }
    bool IsDynamicUTI(std::string_view) const override { return true; }
    bool ConformsTo(std::string_view, std::string_view) const override { return false; }
};

class NullTemporaryStorage final : public nc::utility::TemporaryFileStorage
{
public:
    std::optional<std::string> MakeDirectory(std::string_view) override { return std::nullopt; }
    std::optional<OpenedFile> OpenFile(std::string_view) override { return std::nullopt; }
};

VFSListingPtr Listing()
{
    nc::vfs::ListingInput input;
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = VFSHost::DummyHost();
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/fixture/";
    input.filenames = {"..", "Alpha", "Beta"};
    input.unix_modes = {S_IFDIR | 0755, S_IFDIR | 0640, S_IFDIR | 0750};
    input.unix_types = {DT_DIR, DT_DIR, DT_DIR};
    input.sizes.reset(nc::base::variable_container<>::type::sparse);
    input.sizes.insert(1, 1234);
    input.sizes.insert(2, 66);
    input.atimes.reset(nc::base::variable_container<>::type::sparse);
    input.atimes.insert(1, 1'700'000'001);
    input.mtimes.reset(nc::base::variable_container<>::type::sparse);
    input.mtimes.insert(1, 1'700'000'002);
    input.btimes.reset(nc::base::variable_container<>::type::sparse);
    input.btimes.insert(1, 1'700'000'003);
    input.uids.reset(nc::base::variable_container<>::type::sparse);
    input.uids.insert(1, 501);
    input.gids.reset(nc::base::variable_container<>::type::sparse);
    input.gids.insert(1, 20);
    input.tags.emplace(1,
                       std::vector<nc::utility::Tags::Tag>{
                           {nc::utility::Tags::Tag::Internalize("Important"), nc::utility::Tags::Color::Red}});
    return VFSListing::Build(std::move(input));
}

nc::core::PaneSnapshot Snapshot(const nc::core::PaneId _pane_id,
                                const uint64_t _revision,
                                const nc::core::PaneLoadPhase _phase,
                                const VFSListingPtr &_listing = {})
{
    nc::core::PaneSnapshot snapshot;
    snapshot.pane_id = _pane_id;
    snapshot.revision = _revision;
    snapshot.state.load_phase = _phase;
    snapshot.state.listing = _listing;
    return snapshot;
}

nc::core::FileManagerError PermissionFailure()
{
    return nc::core::FileManagerError{
        .code = {.domain = "ExplorerInspectorViewTest", .value = EACCES},
        .category = nc::core::FileManagerErrorCategory::PermissionError,
        .severity = nc::core::FileManagerErrorSeverity::BlockingError,
        .user_message_key = "errors.permission",
        .user_message = "Access denied.",
        .technical_message = "Test permission failure.",
        .original_error = nc::Error{nc::Error::POSIX, EACCES},
    };
}

NCExplorerInspectorView *
View(const nc::core::PaneId _pane_id, const TestUTIDB &_uti_db, nc::panel::QuickLookVFSBridge &_bridge)
{
    return [[NCExplorerInspectorView alloc] initWithFrame:NSMakeRect(0, 0, 320, 640)
                                                   paneID:_pane_id
                                                    UTIDB:_uti_db
                                QLHazardousExtensionsList:"*"
                                              QLVFSBridge:_bridge];
}

class ExplorerStateFixture
{
public:
    explicit ExplorerStateFixture(const nc::core::PaneId _pane_id = nc::core::PaneId{201})
        : m_Bridge(m_Storage), m_PaneID(_pane_id)
    {
        m_Listing = Listing();
        m_Model.Load(m_Listing, nc::panel::data::Model::PanelType::Directory);
        m_PanelView = [[ExplorerInspectorStatePanelView alloc] initWithModel:m_Model];
        m_Panel = [[ExplorerInspectorStatePanelController alloc] initWithModel:m_Model
                                                                          view:m_PanelView
                                                                        paneID:m_PaneID];
        m_Inspector = View(m_PaneID, m_UTIDB, m_Bridge);
        m_State = [[NCExplorerState alloc] initForTestingWithFrame:NSMakeRect(0, 0, 1000, 640)
                                                   panelController:m_Panel
                                                         panelView:m_PanelView
                                                     inspectorView:m_Inspector
                                                    QLPanelAdaptor:nil];
    }

    void Apply(const nc::core::PaneSnapshot &_snapshot)
    {
        if( _snapshot.state.focused_item )
            m_PanelView.curpos = m_Model.SortPositionOfEntry(_snapshot.state.focused_item);
        [m_State applyPaneSnapshotForTesting:_snapshot];
    }

    [[nodiscard]] NCExplorerState *State() const noexcept { return m_State; }
    [[nodiscard]] NCExplorerInspectorView *Inspector() const noexcept { return m_Inspector; }
    [[nodiscard]] NCExplorerPaneStateView *PaneStateView() const noexcept
    {
        return m_State.paneStateViewForTesting;
    }
    [[nodiscard]] ExplorerInspectorStatePanelController *Panel() const noexcept { return m_Panel; }
    [[nodiscard]] ExplorerInspectorStatePanelView *PanelView() const noexcept { return m_PanelView; }
    [[nodiscard]] const VFSListingPtr &ListingValue() const noexcept { return m_Listing; }
    [[nodiscard]] nc::panel::data::Model &Model() noexcept { return m_Model; }
    [[nodiscard]] nc::core::PaneId PaneID() const noexcept { return m_PaneID; }

private:
    TestUTIDB m_UTIDB;
    NullTemporaryStorage m_Storage;
    nc::panel::QuickLookVFSBridge m_Bridge;
    nc::panel::data::Model m_Model;
    nc::core::PaneId m_PaneID;
    VFSListingPtr m_Listing;
    __strong ExplorerInspectorStatePanelView *m_PanelView;
    __strong ExplorerInspectorStatePanelController *m_Panel;
    __strong NCExplorerInspectorView *m_Inspector;
    __strong NCExplorerState *m_State;
};

} // namespace

#define PREFIX "NCExplorerInspectorView "

TEST_CASE(PREFIX "renders exact single-item preview metadata and accessibility")
{
    const nc::core::PaneId pane{101};
    TestUTIDB uti_db;
    NullTemporaryStorage storage;
    nc::panel::QuickLookVFSBridge bridge{storage};
    NCExplorerInspectorView *const view = View(pane, uti_db, bridge);
    REQUIRE(view != nil);
    CHECK(view.renderedStateForTesting == nc::explorer::InspectorState::Hidden);
    CHECK(view.statusTextForTesting.length > 0);
    CHECK_FALSE(view.previewVisibleForTesting);

    const auto listing = Listing();
    auto snapshot = Snapshot(pane, 1, nc::core::PaneLoadPhase::Loaded, listing);
    snapshot.state.focused_item = listing->Item(1);
    REQUIRE([view applyPaneSnapshot:snapshot]);

    CHECK(view.renderedStateForTesting == nc::explorer::InspectorState::Single);
    CHECK(view.previewVisibleForTesting);
    CHECK(view.previewViewForTesting.hasPublishedPreviewForTesting);
    CHECK([view.filenameFieldForTesting.stringValue isEqualToString:@"Alpha"]);
    CHECK([view.pathFieldForTesting.stringValue isEqualToString:@"/fixture/Alpha"]);
    CHECK([view.sizeFieldForTesting.stringValue containsString:@"1234"]);
    CHECK_FALSE([view.createdFieldForTesting.stringValue isEqualToString:@"—"]);
    CHECK_FALSE([view.modifiedFieldForTesting.stringValue isEqualToString:@"—"]);
    CHECK_FALSE([view.accessedFieldForTesting.stringValue isEqualToString:@"—"]);
    CHECK([view.tagsFieldForTesting.stringValue isEqualToString:@"Important"]);
    CHECK([view.permissionsFieldForTesting.stringValue isEqualToString:@"rw-r----- (0640)"]);
    CHECK([view.ownerGroupFieldForTesting.stringValue isEqualToString:@"501 / 20"]);

    CHECK([view.accessibilityIdentifier isEqualToString:@"wincommander.explorer.inspector"]);
    CHECK(view.accessibilityLabel.length > 0);
    CHECK([view.previewViewForTesting.accessibilityIdentifier
        isEqualToString:@"wincommander.explorer.inspector.preview"]);
    CHECK(view.previewViewForTesting.accessibilityLabel.length > 0);
    CHECK([view.filenameFieldForTesting.accessibilityIdentifier
        isEqualToString:@"wincommander.explorer.inspector.filename"]);
    CHECK(view.filenameFieldForTesting.accessibilityLabel.length > 0);
    CHECK([view.pathFieldForTesting.accessibilityValue isEqualToString:@"/fixture/Alpha"]);
}

TEST_CASE(PREFIX "preserves the rendered presentation for foreign and stale snapshots")
{
    const nc::core::PaneId pane{102};
    TestUTIDB uti_db;
    NullTemporaryStorage storage;
    nc::panel::QuickLookVFSBridge bridge{storage};
    NCExplorerInspectorView *const view = View(pane, uti_db, bridge);
    const auto listing = Listing();

    auto current = Snapshot(pane, 4, nc::core::PaneLoadPhase::Loaded, listing);
    current.state.focused_item = listing->Item(1);
    REQUIRE([view applyPaneSnapshot:current]);

    auto foreign = Snapshot(nc::core::PaneId{103}, 100, nc::core::PaneLoadPhase::Loaded, listing);
    foreign.state.focused_item = listing->Item(2);
    CHECK_FALSE([view applyPaneSnapshot:foreign]);
    CHECK([view.filenameFieldForTesting.stringValue isEqualToString:@"Alpha"]);
    CHECK(view.previewVisibleForTesting);

    auto stale = Snapshot(pane, 3, nc::core::PaneLoadPhase::Failed, listing);
    stale.state.visible_error = PermissionFailure();
    CHECK_FALSE([view applyPaneSnapshot:stale]);
    CHECK(view.renderedStateForTesting == nc::explorer::InspectorState::Single);
    CHECK([view.filenameFieldForTesting.stringValue isEqualToString:@"Alpha"]);
    CHECK_FALSE(view.errorVisibleForTesting);
}

TEST_CASE(PREFIX "rebind clears the old presentation and restarts revision admission for the new pane")
{
    const nc::core::PaneId first_pane{112};
    const nc::core::PaneId second_pane{113};
    TestUTIDB uti_db;
    NullTemporaryStorage storage;
    nc::panel::QuickLookVFSBridge bridge{storage};
    NCExplorerInspectorView *const view = View(first_pane, uti_db, bridge);
    const auto listing = Listing();

    auto first = Snapshot(first_pane, 9, nc::core::PaneLoadPhase::Loaded, listing);
    first.state.focused_item = listing->Item(1);
    REQUIRE([view applyPaneSnapshot:first]);
    REQUIRE(view.renderedStateForTesting == nc::explorer::InspectorState::Single);

    REQUIRE([view rebindToPaneID:second_pane]);
    CHECK(view.renderedStateForTesting == nc::explorer::InspectorState::Hidden);
    CHECK_FALSE(view.previewVisibleForTesting);
    CHECK_FALSE([view applyPaneSnapshot:first]);

    auto second = Snapshot(second_pane, 1, nc::core::PaneLoadPhase::Loaded, listing);
    second.state.focused_item = listing->Item(2);
    REQUIRE([view applyPaneSnapshot:second]);
    CHECK([view.filenameFieldForTesting.stringValue isEqualToString:@"Beta"]);
}

TEST_CASE(PREFIX "renders multiple loading and empty states and clears preview")
{
    const nc::core::PaneId pane{104};
    TestUTIDB uti_db;
    NullTemporaryStorage storage;
    nc::panel::QuickLookVFSBridge bridge{storage};
    NCExplorerInspectorView *const view = View(pane, uti_db, bridge);
    const auto listing = Listing();

    auto single = Snapshot(pane, 1, nc::core::PaneLoadPhase::Loaded, listing);
    single.state.focused_item = listing->Item(1);
    REQUIRE([view applyPaneSnapshot:single]);
    REQUIRE(view.previewViewForTesting.hasPublishedPreviewForTesting);

    auto multiple = Snapshot(pane, 2, nc::core::PaneLoadPhase::Loaded, listing);
    multiple.state.selected_items = nc::core::PaneSelectedItems{listing->Item(1), listing->Item(2)};
    REQUIRE([view applyPaneSnapshot:multiple]);
    CHECK(view.renderedStateForTesting == nc::explorer::InspectorState::Multiple);
    CHECK_FALSE(view.previewVisibleForTesting);
    CHECK_FALSE(view.previewViewForTesting.hasPublishedPreviewForTesting);
    CHECK([view.multipleSummaryForTesting containsString:@"2 items selected"]);

    auto loading = Snapshot(pane, 3, nc::core::PaneLoadPhase::Loading, listing);
    loading.state.focused_item = listing->Item(1);
    REQUIRE([view applyPaneSnapshot:loading]);
    CHECK(view.renderedStateForTesting == nc::explorer::InspectorState::PaneLoading);
    CHECK(view.statusTextForTesting.length > 0);
    CHECK_FALSE(view.previewVisibleForTesting);
    CHECK_FALSE(view.refreshingVisibleForTesting);

    REQUIRE([view applyPaneSnapshot:Snapshot(pane, 4, nc::core::PaneLoadPhase::Empty)]);
    CHECK(view.renderedStateForTesting == nc::explorer::InspectorState::Empty);
    CHECK(view.statusTextForTesting.length > 0);
    CHECK_FALSE(view.previewVisibleForTesting);
}

TEST_CASE(PREFIX "renders refresh and visible-error banner without replacing the body")
{
    const nc::core::PaneId pane{105};
    TestUTIDB uti_db;
    NullTemporaryStorage storage;
    nc::panel::QuickLookVFSBridge bridge{storage};
    NCExplorerInspectorView *const view = View(pane, uti_db, bridge);
    const auto listing = Listing();

    auto refreshing = Snapshot(pane, 1, nc::core::PaneLoadPhase::Refreshing, listing);
    refreshing.state.focused_item = listing->Item(1);
    refreshing.state.visible_error = PermissionFailure();
    REQUIRE([view applyPaneSnapshot:refreshing]);
    CHECK(view.renderedStateForTesting == nc::explorer::InspectorState::Single);
    CHECK(view.previewVisibleForTesting);
    CHECK(view.refreshingVisibleForTesting);
    CHECK(view.errorVisibleForTesting);
    CHECK([view.errorTextForTesting isEqualToString:@"Access denied."]);

    auto failed = Snapshot(pane, 2, nc::core::PaneLoadPhase::Failed, listing);
    failed.state.visible_error = PermissionFailure();
    REQUIRE([view applyPaneSnapshot:failed]);
    CHECK(view.renderedStateForTesting == nc::explorer::InspectorState::PaneError);
    CHECK_FALSE(view.previewVisibleForTesting);
    CHECK_FALSE(view.previewViewForTesting.hasPublishedPreviewForTesting);
    CHECK_FALSE(view.refreshingVisibleForTesting);
    CHECK(view.errorVisibleForTesting);
    CHECK(view.statusTextForTesting.length > 0);
}

TEST_CASE(PREFIX "Explorer state projects admitted folder states over the mounted panel")
{
    ExplorerStateFixture fixture;
    NCExplorerPaneStateView *const pane_state = fixture.PaneStateView();
    REQUIRE(pane_state != nil);
    CHECK(pane_state.superview == fixture.State().panelContainerForTesting);
    CHECK(pane_state.hidden);

    auto loading = Snapshot(fixture.PaneID(), 1, nc::core::PaneLoadPhase::Loading, nullptr);
    fixture.Apply(loading);
    CHECK_FALSE(pane_state.hidden);
    CHECK(pane_state.renderedKindForTesting == nc::core::PaneVisualKind::Loading);
    CHECK(pane_state.skeletonVisibleForTesting);

    auto empty = Snapshot(fixture.PaneID(), 2, nc::core::PaneLoadPhase::Loaded, VFSListing::EmptyListing());
    empty.state.item_count = 0;
    fixture.Apply(empty);
    CHECK_FALSE(pane_state.hidden);
    CHECK(pane_state.renderedKindForTesting == nc::core::PaneVisualKind::EmptyFolder);

    auto denied = Snapshot(fixture.PaneID(), 3, nc::core::PaneLoadPhase::Failed, nullptr);
    denied.state.visible_error = PermissionFailure();
    fixture.Apply(denied);
    CHECK_FALSE(pane_state.hidden);
    CHECK(pane_state.renderedKindForTesting == nc::core::PaneVisualKind::PermissionBlocked);
    CHECK(pane_state.messageTextForTesting.length > 0);

    auto ready = Snapshot(fixture.PaneID(), 4, nc::core::PaneLoadPhase::Loaded, fixture.ListingValue());
    ready.state.item_count = static_cast<int32_t>(fixture.ListingValue()->Count());
    fixture.Apply(ready);
    CHECK(pane_state.hidden);
    CHECK(pane_state.renderedKindForTesting == nc::core::PaneVisualKind::Ready);
}

TEST_CASE(PREFIX "Explorer state split keeps inspector bounds and visibility compare-and-set")
{
    ExplorerStateFixture fixture;
    auto snapshot = Snapshot(fixture.PaneID(), 1, nc::core::PaneLoadPhase::Loaded, fixture.ListingValue());
    snapshot.state.focused_item = fixture.ListingValue()->Item(1);
    fixture.Apply(snapshot);

    NCExplorerState *const state = fixture.State();
    NSSplitView *const split = state.contentSplitViewForTesting;
    REQUIRE(split != nil);
    REQUIRE(split.subviews.count == 2);
    CHECK(split.subviews[0] == state.panelContainerForTesting);
    CHECK(split.subviews[1] == state.inspectorViewForTesting);
    CHECK([state previewPaneVisibleForPanel:fixture.Panel()]);
    CHECK(state.inspectorViewForTesting.frame.size.width >= 280.0);
    CHECK(state.inspectorViewForTesting.frame.size.width <= 520.0);

    [split setPosition:split.bounds.size.width ofDividerAtIndex:0];
    CHECK(state.inspectorViewForTesting.frame.size.width >= 280.0);
    CHECK(state.panelContainerForTesting.frame.size.width >= 360.0);
    [split setPosition:0.0 ofDividerAtIndex:0];
    CHECK(state.inspectorViewForTesting.frame.size.width <= 520.0);
    CHECK(state.panelContainerForTesting.frame.size.width >= 360.0);

    PanelController *const foreign = reinterpret_cast<PanelController *>([NSObject new]);
    CHECK_FALSE([state previewPaneVisibleForPanel:foreign]);
    CHECK_FALSE([state setPreviewPaneVisible:false expected:true forPanel:foreign]);
    REQUIRE([state setPreviewPaneVisible:false expected:true forPanel:fixture.Panel()]);
    CHECK_FALSE([state previewPaneVisibleForPanel:fixture.Panel()]);
    CHECK(state.inspectorViewForTesting.hidden);
    CHECK_FALSE([state setPreviewPaneVisible:true expected:true forPanel:fixture.Panel()]);
    REQUIRE([state setPreviewPaneVisible:true expected:false forPanel:fixture.Panel()]);
    CHECK([state previewPaneVisibleForPanel:fixture.Panel()]);
    CHECK_FALSE(state.inspectorViewForTesting.hidden);
    CHECK(state.inspectorViewForTesting.frame.size.width >= 280.0);
    CHECK(state.inspectorViewForTesting.frame.size.width <= 520.0);
}

TEST_CASE(PREFIX "Explorer state presents an exact right-click item that differs from focus")
{
    ExplorerStateFixture fixture;
    auto snapshot = Snapshot(fixture.PaneID(), 7, nc::core::PaneLoadPhase::Loaded, fixture.ListingValue());
    snapshot.listing_generation = 3;
    snapshot.state.focused_item = fixture.ListingValue()->Item(1);
    fixture.Apply(snapshot);
    REQUIRE([fixture.State() setPreviewPaneVisible:false expected:true forPanel:fixture.Panel()]);

    nc::core::FileGetInfoPresentation presentation;
    presentation.source = nc::core::CommandInvocationSource::ContextMenu;
    presentation.items.emplace_back(nc::core::CopyFileMetadataSnapshot(fixture.ListingValue()->Item(2)));
    REQUIRE([fixture.State() presentFileGetInfo:presentation forPanel:fixture.Panel()]);

    CHECK(fixture.PanelView().item == fixture.ListingValue()->Item(2));
    CHECK([fixture.Inspector().filenameFieldForTesting.stringValue isEqualToString:@"Beta"]);
    CHECK([fixture.State() previewPaneVisibleForPanel:fixture.Panel()]);

    nc::core::FileGetInfoPresentation stale = presentation;
    stale.items.front().size = 999;
    CHECK_FALSE([fixture.State() presentFileGetInfo:stale forPanel:fixture.Panel()]);
    PanelController *const foreign = reinterpret_cast<PanelController *>([NSObject new]);
    CHECK_FALSE([fixture.State() presentFileGetInfo:presentation forPanel:foreign]);
}

TEST_CASE(PREFIX "Explorer state requires exact selected metadata order for batch Get Info")
{
    ExplorerStateFixture fixture;
    auto snapshot = Snapshot(fixture.PaneID(), 9, nc::core::PaneLoadPhase::Loaded, fixture.ListingValue());
    snapshot.state.focused_item = fixture.ListingValue()->Item(1);
    snapshot.state.selected_items =
        nc::core::PaneSelectedItems{fixture.ListingValue()->Item(1), fixture.ListingValue()->Item(2)};
    fixture.Apply(snapshot);

    nc::core::FileGetInfoPresentation exact;
    exact.items.emplace_back(nc::core::CopyFileMetadataSnapshot(fixture.ListingValue()->Item(1)));
    exact.items.emplace_back(nc::core::CopyFileMetadataSnapshot(fixture.ListingValue()->Item(2)));
    REQUIRE([fixture.State() presentFileGetInfo:exact forPanel:fixture.Panel()]);
    CHECK(fixture.Inspector().renderedStateForTesting == nc::explorer::InspectorState::Multiple);

    std::swap(exact.items[0], exact.items[1]);
    CHECK_FALSE([fixture.State() presentFileGetInfo:exact forPanel:fixture.Panel()]);
}

TEST_CASE(PREFIX "gallery clear invalidates stale publication and permits the same exact item again")
{
    TestUTIDB uti_db;
    NullTemporaryStorage storage;
    nc::panel::QuickLookVFSBridge bridge{storage};
    NCPanelGalleryCentralView *const gallery =
        [[NCPanelGalleryCentralView alloc] initWithFrame:NSMakeRect(0, 0, 240, 180)
                                                   UTIDB:uti_db
                               QLHazardousExtensionsList:"*"
                                             QLVFSBridge:bridge];
    REQUIRE(gallery != nil);

    const uint64_t old_ticket = gallery.previewTicketForTesting;
    NSImage *const synthetic = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
    [gallery commitPreviewedIcon:synthetic ticket:old_ticket];
    REQUIRE(gallery.hasPublishedPreviewForTesting);

    NSView *const container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 240, 180)];
    [container addSubview:gallery];
    CHECK(gallery.hasPublishedPreviewForTesting);
    CHECK(gallery.previewTicketForTesting == old_ticket + 1);

    [gallery clearPreview];
    CHECK(gallery.previewTicketForTesting == old_ticket + 2);
    CHECK_FALSE(gallery.hasPublishedPreviewForTesting);
    CHECK(gallery.currentPreviewPathForTesting.length == 0);
    [gallery commitPreviewedIcon:synthetic ticket:old_ticket];
    CHECK_FALSE(gallery.hasPublishedPreviewForTesting);

    const auto listing = Listing();
    [gallery commitVFSQL:listing->Item(1) native:[NSURL fileURLWithPath:@"/fixture/stale"] ticket:old_ticket];
    CHECK_FALSE(gallery.hasPublishedPreviewForTesting);
    NSView *const ql_view = [gallery valueForKey:@"m_QLView"];
    NSImageView *const fallback_view = [gallery valueForKey:@"m_FallbackImageView"];
    CHECK_FALSE(ql_view.hidden);
    CHECK(fallback_view.hidden);

    [gallery showVFSItem:listing->Item(1)];
    REQUIRE(gallery.hasPublishedPreviewForTesting);
    CHECK([gallery.currentPreviewPathForTesting isEqualToString:@"/fixture/Alpha"]);
    [gallery clearPreview];
    CHECK_FALSE(gallery.hasPublishedPreviewForTesting);
    [gallery showVFSItem:listing->Item(1)];
    CHECK(gallery.hasPublishedPreviewForTesting);
    CHECK([gallery.currentPreviewPathForTesting isEqualToString:@"/fixture/Alpha"]);
    [gallery removeFromSuperview];
    CHECK_FALSE(gallery.hasPublishedPreviewForTesting);
    CHECK(gallery.currentPreviewPathForTesting.length == 0);
}

#undef PREFIX
