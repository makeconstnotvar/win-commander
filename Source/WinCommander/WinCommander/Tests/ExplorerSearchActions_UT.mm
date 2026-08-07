// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/States/Explorer/NCExplorerSearchPresenting.h>
#include <WinCommander/States/FilePanels/Actions/FindFiles.h>
#include <WinCommander/States/FilePanels/Actions/SpotlightSearch.h>
#include <WinCommander/States/FilePanels/PanelController.h>
#include <WinCommander/States/FilePanels/PanelView.h>
#include <sys/dirent.h>
#include <sys/stat.h>

namespace {

class ExplorerSearchActionTestHost final : public nc::vfs::Host
{
public:
    ExplorerSearchActionTestHost() : Host("/", nullptr, "explorer-search-action-tests") {}
};

VFSListingItem SearchActionItem()
{
    nc::vfs::ListingInput input;
    input.hosts[0] = std::make_shared<ExplorerSearchActionTestHost>();
    input.directories[0] = "/fixture/";
    input.filenames = {"item.txt"};
    input.unix_modes = {S_IFREG | 0644};
    input.unix_types = {DT_REG};
    return VFSListing::Build(std::move(input))->Item(0);
}

} // namespace

@interface ExplorerSearchActionPresenter : NSObject <NCExplorerSearchPresenting>
@property(nonatomic) BOOL canPresent;
@property(nonatomic) BOOL presentationResult;
@property(nonatomic, readonly) NSInteger presentationCount;
@property(nonatomic, readonly, copy, nullable) NSString *lastQuery;
@property(nonatomic, readonly) NCExplorerSearchPreferredScope lastScope;
@property(nonatomic, readonly, weak, nullable) PanelController *lastPanel;
@end

@implementation ExplorerSearchActionPresenter {
    BOOL m_CanPresent;
    BOOL m_PresentationResult;
    NSInteger m_PresentationCount;
    NSString *m_LastQuery;
    NCExplorerSearchPreferredScope m_LastScope;
    __weak PanelController *m_LastPanel;
}

@synthesize canPresent = m_CanPresent;
@synthesize presentationResult = m_PresentationResult;

- (NSInteger)presentationCount
{
    return m_PresentationCount;
}

- (NSString *)lastQuery
{
    return m_LastQuery;
}

- (NCExplorerSearchPreferredScope)lastScope
{
    return m_LastScope;
}

- (PanelController *)lastPanel
{
    return m_LastPanel;
}

- (BOOL)canPresentSearchForPanel:(PanelController *)_panel
{
    return _panel != nil && m_CanPresent;
}

- (BOOL)presentSearchForPanel:(PanelController *)_panel
                initialQuery:(NSString *)_query
              preferredScope:(NCExplorerSearchPreferredScope)_scope
{
    ++m_PresentationCount;
    m_LastPanel = _panel;
    m_LastQuery = [_query copy];
    m_LastScope = _scope;
    return m_PresentationResult;
}

@end

@interface ExplorerSearchActionPanelView : NSObject
- (instancetype)initWithItem:(VFSListingItem)_item;
- (VFSListingItem)item;
@end

@implementation ExplorerSearchActionPanelView {
    VFSListingItem m_Item;
}

- (instancetype)initWithItem:(VFSListingItem)_item
{
    self = [super init];
    if( self )
        m_Item = std::move(_item);
    return self;
}

- (VFSListingItem)item
{
    return m_Item;
}

@end

@interface ExplorerSearchActionPanelController : PanelController
- (instancetype)initWithState:(nullable id)_state uniform:(bool)_uniform item:(VFSListingItem)_item;
@end

@implementation ExplorerSearchActionPanelController {
    id m_TestState;
    bool m_TestUniform;
    ExplorerSearchActionPanelView *m_TestView;
}

- (instancetype)initWithState:(id)_state uniform:(bool)_uniform item:(VFSListingItem)_item
{
    self = [super init];
    if( self ) {
        m_TestState = _state;
        m_TestUniform = _uniform;
        m_TestView = [[ExplorerSearchActionPanelView alloc] initWithItem:std::move(_item)];
    }
    return self;
}

- (id<NCPanelControllerHostingState>)state
{
    return static_cast<id<NCPanelControllerHostingState>>(m_TestState);
}

- (bool)isUniform
{
    return m_TestUniform;
}

- (PanelView *)view
{
    return static_cast<PanelView *>(m_TestView);
}

@end

namespace {

nc::panel::actions::FindFiles SearchFindFilesAction()
{
    return nc::panel::actions::FindFiles{[](NSRect) -> NCViewerView * { return nil; },
                                         []() -> NCViewerViewController * { return nil; }};
}

} // namespace

#define PREFIX "nc::panel::actions Explorer Search "

TEST_CASE(PREFIX "Find Files is available through an Explorer presenter for a non-uniform empty panel")
{
    ExplorerSearchActionPresenter *const presenter = [ExplorerSearchActionPresenter new];
    presenter.canPresent = YES;
    presenter.presentationResult = YES;
    ExplorerSearchActionPanelController *const panel =
        [[ExplorerSearchActionPanelController alloc] initWithState:presenter uniform:false item:{}];
    const auto action = SearchFindFilesAction();

    CHECK(action.Predicate(panel));
}

TEST_CASE(PREFIX "Find Files forwards an exact text-field query to current-folder Search Mode")
{
    ExplorerSearchActionPresenter *const presenter = [ExplorerSearchActionPresenter new];
    presenter.canPresent = YES;
    presenter.presentationResult = YES;
    ExplorerSearchActionPanelController *const panel =
        [[ExplorerSearchActionPanelController alloc] initWithState:presenter uniform:false item:{}];
    NSTextField *const breadcrumb_field = [[NSTextField alloc] initWithFrame:NSZeroRect];
    breadcrumb_field.stringValue = @"exact breadcrumb query";
    const auto action = SearchFindFilesAction();

    action.Perform(panel, breadcrumb_field);

    CHECK(presenter.presentationCount == 1);
    CHECK(presenter.lastPanel == panel);
    CHECK([presenter.lastQuery isEqualToString:@"exact breadcrumb query"]);
    CHECK(presenter.lastScope == NCExplorerSearchPreferredScopeCurrentFolder);
}

TEST_CASE(PREFIX "Spotlight selects the whole-Mac preferred Search Mode scope")
{
    ExplorerSearchActionPresenter *const presenter = [ExplorerSearchActionPresenter new];
    presenter.canPresent = YES;
    presenter.presentationResult = YES;
    ExplorerSearchActionPanelController *const panel =
        [[ExplorerSearchActionPanelController alloc] initWithState:presenter uniform:false item:{}];
    const nc::panel::actions::SpotlightSearch action;

    action.Perform(panel, nil);

    CHECK(presenter.presentationCount == 1);
    CHECK(presenter.lastPanel == panel);
    CHECK(presenter.lastQuery == nil);
    CHECK(presenter.lastScope == NCExplorerSearchPreferredScopeSpotlightWholeMac);
}

TEST_CASE(PREFIX "Explorer presenter refusal fails closed")
{
    SECTION("admission refusal")
    {
        ExplorerSearchActionPresenter *const presenter = [ExplorerSearchActionPresenter new];
        presenter.canPresent = NO;
        ExplorerSearchActionPanelController *const panel =
            [[ExplorerSearchActionPanelController alloc] initWithState:presenter uniform:true item:{}];
        const auto find_files = SearchFindFilesAction();
        const nc::panel::actions::SpotlightSearch spotlight;

        CHECK_FALSE(find_files.Predicate(panel));
        find_files.Perform(panel, nil);
        spotlight.Perform(panel, nil);
        CHECK(presenter.presentationCount == 0);
    }

    SECTION("presentation refusal")
    {
        ExplorerSearchActionPresenter *const presenter = [ExplorerSearchActionPresenter new];
        presenter.canPresent = YES;
        presenter.presentationResult = NO;
        ExplorerSearchActionPanelController *const panel =
            [[ExplorerSearchActionPanelController alloc] initWithState:presenter uniform:false item:{}];
        const auto find_files = SearchFindFilesAction();

        find_files.Perform(panel, nil);
        CHECK(presenter.presentationCount == 1);
        CHECK(presenter.lastScope == NCExplorerSearchPreferredScopeCurrentFolder);
    }
}

TEST_CASE(PREFIX "Commander panels preserve the legacy Find Files predicate")
{
    const auto action = SearchFindFilesAction();

    ExplorerSearchActionPanelController *const uniform =
        [[ExplorerSearchActionPanelController alloc] initWithState:nil uniform:true item:{}];
    ExplorerSearchActionPanelController *const focused =
        [[ExplorerSearchActionPanelController alloc] initWithState:nil uniform:false item:SearchActionItem()];
    ExplorerSearchActionPanelController *const unavailable =
        [[ExplorerSearchActionPanelController alloc] initWithState:nil uniform:false item:{}];

    CHECK(action.Predicate(uniform));
    CHECK(action.Predicate(focused));
    CHECK_FALSE(action.Predicate(unavailable));
}

#undef PREFIX
