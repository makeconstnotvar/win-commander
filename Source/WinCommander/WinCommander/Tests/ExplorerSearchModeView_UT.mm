// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/States/Explorer/NCExplorerSearchModeView.h>

#include <limits>

using namespace nc::core;
using Catch::Approx;

@interface NCExplorerSearchModeView (ExplorerSearchModeViewTesting)
- (CGFloat)visibleHeightForTesting;
- (NSSearchField *)queryForTesting;
- (NSPopUpButton *)scopeForTesting;
- (NSSegmentedControl *)nameMatchForTesting;
- (NSTextField *)extensionForTesting;
- (NSPopUpButton *)fileTypeForTesting;
- (NSTextField *)minimumSizeForTesting;
- (NSTextField *)maximumSizeForTesting;
- (NSTextField *)modifiedAfterForTesting;
- (NSTextField *)modifiedBeforeForTesting;
- (NSTextField *)contentQueryForTesting;
- (NSButton *)includeHiddenForTesting;
- (NSButton *)startForTesting;
- (NSButton *)cancelForTesting;
- (NSButton *)revealForTesting;
- (NSButton *)closeForTesting;
- (NSString *)statusForTesting;
- (NSString *)backendForTesting;
- (NSString *)locationForTesting;
- (NSString *)countsForTesting;
- (NSString *)limitationsForTesting;
- (NSProgressIndicator *)progressForTesting;
@end

namespace {

SearchSnapshot Snapshot(const SearchPhase _phase)
{
    return {
        .pane_id = PaneId{37},
        .revision = 1,
        .phase = _phase,
    };
}

SearchRequest Request(const SearchScope _scope = SearchScope::CurrentFolder,
                      const SearchNameMatch _match = SearchNameMatch::Contains)
{
    SearchRequest request;
    request.query = "report";
    request.scope = _scope;
    request.filters.name_match = _match;
    return request;
}

SearchBackendDescriptor DirectBackend()
{
    return {
        .kind = SearchBackendKind::DirectScan,
        .support = SearchBackendSupport::Supported,
        .capabilities = {.name = true, .recursive_scope = true, .determinate_progress = true},
    };
}

NCExplorerSearchModeView *MakeView()
{
    return [[NCExplorerSearchModeView alloc] initWithFrame:NSMakeRect(0, 0, 1000, 156)];
}

} // namespace

#define PREFIX "NCExplorerSearchModeView "

TEST_CASE(PREFIX "collapses completely without an active pane search surface")
{
    REQUIRE([NSThread isMainThread]);
    NCExplorerSearchModeView *const view = MakeView();
    [view applySnapshot:std::nullopt resultSelectionEligible:false];

    CHECK(view.hidden);
    CHECK(view.visibleHeightForTesting == 0.0);
    CHECK([view.accessibilityIdentifier isEqualToString:@"wincommander.explorer.searchMode"]);
    CHECK([view.accessibilityRole isEqualToString:NSAccessibilityGroupRole]);
    CHECK(view.accessibilityLabel.length > 0);
}

TEST_CASE(PREFIX "shows an idle query surface with explicit scope and match controls")
{
    REQUIRE([NSThread isMainThread]);
    NCExplorerSearchModeView *const view = MakeView();
    auto snapshot = Snapshot(SearchPhase::Idle);
    snapshot.request = Request(SearchScope::Recursive, SearchNameMatch::Exact);
    snapshot.request->filters.extension = "pdf";
    snapshot.request->filters.file_type = SearchFileType::RegularFile;
    snapshot.request->filters.size = {.minimum_bytes = 1024, .maximum_bytes = 8192};
    snapshot.request->filters.modified = {.earliest_seconds = 100, .latest_seconds = 200};
    snapshot.request->filters.content = "quarterly revenue";
    snapshot.request->filters.include_hidden = true;
    [view applySnapshot:snapshot resultSelectionEligible:false];

    CHECK_FALSE(view.hidden);
    CHECK(view.visibleHeightForTesting == 156.0);
    CHECK([view.queryForTesting.stringValue isEqualToString:@"report"]);
    CHECK(view.scopeForTesting.selectedItem.tag == static_cast<NSInteger>(SearchScope::Recursive));
    CHECK(view.nameMatchForTesting.selectedSegment == 1);
    CHECK([view.extensionForTesting.stringValue isEqualToString:@"pdf"]);
    CHECK(view.fileTypeForTesting.selectedItem.tag == static_cast<NSInteger>(SearchFileType::RegularFile));
    CHECK([view.minimumSizeForTesting.stringValue isEqualToString:@"1024"]);
    CHECK([view.maximumSizeForTesting.stringValue isEqualToString:@"8192"]);
    CHECK([view.modifiedAfterForTesting.stringValue isEqualToString:@"100"]);
    CHECK([view.modifiedBeforeForTesting.stringValue isEqualToString:@"200"]);
    CHECK([view.contentQueryForTesting.stringValue isEqualToString:@"quarterly revenue"]);
    CHECK(view.includeHiddenForTesting.state == NSControlStateValueOn);
    CHECK(view.startForTesting.enabled);
    CHECK_FALSE(view.cancelForTesting.enabled);
    CHECK_FALSE(view.revealForTesting.enabled);
    CHECK(view.statusForTesting.length > 0);
}

TEST_CASE(PREFIX "renders running determinate progress location counts backend and cancel authority")
{
    REQUIRE([NSThread isMainThread]);
    NCExplorerSearchModeView *const view = MakeView();
    auto snapshot = Snapshot(SearchPhase::Running);
    snapshot.request = Request(SearchScope::Recursive);
    snapshot.backend = DirectBackend();
    snapshot.determinate_progress = 0.375;
    snapshot.current_location = "/Users/test/Documents/Archive";
    snapshot.scanned_count = 120;
    snapshot.found_count = 7;

    int cancelled = 0;
    [view setCancelHandler:[&] { ++cancelled; }];
    [view applySnapshot:snapshot resultSelectionEligible:true];

    CHECK_FALSE(view.progressForTesting.indeterminate);
    CHECK(view.progressForTesting.doubleValue == Approx(0.375));
    CHECK(view.cancelForTesting.enabled);
    CHECK_FALSE(view.startForTesting.enabled);
    CHECK_FALSE(view.revealForTesting.enabled);
    CHECK(view.backendForTesting.length > 0);
    CHECK([view.locationForTesting containsString:@"/Users/test/Documents/Archive"]);
    CHECK(view.countsForTesting.length > 0);
    [view.cancelForTesting performClick:nil];
    CHECK(cancelled == 1);

    NSString *const accessibility_value = static_cast<NSString *>(view.accessibilityValue);
    CHECK([accessibility_value containsString:view.statusForTesting]);
    CHECK(view.progressForTesting.accessibilityLabel.length > 0);
}

TEST_CASE(PREFIX "uses indeterminate progress and names every active backend limitation")
{
    REQUIRE([NSThread isMainThread]);
    NCExplorerSearchModeView *const view = MakeView();
    auto snapshot = Snapshot(SearchPhase::Preparing);
    snapshot.request = Request(SearchScope::SpotlightWholeMac);
    snapshot.backend = SearchBackendDescriptor{
        .kind = SearchBackendKind::Spotlight,
        .support = SearchBackendSupport::Supported,
        .limitations = {
            SearchBackendLimitation::FullDiskAccessMissing,
            SearchBackendLimitation::ContentSearchUnavailable,
            SearchBackendLimitation::SpotlightIndexUnavailable,
        },
    };
    snapshot.determinate_progress = std::numeric_limits<double>::quiet_NaN();
    [view applySnapshot:snapshot resultSelectionEligible:false];

    CHECK(view.progressForTesting.indeterminate);
    CHECK(view.limitationsForTesting.length > 0);
    CHECK([view.limitationsForTesting containsString:@";"]);
    CHECK([static_cast<NSString *>(view.accessibilityValue) containsString:view.limitationsForTesting]);
    CHECK([static_cast<NSString *>(view.accessibilityHelp) isEqualToString:view.limitationsForTesting]);
}

TEST_CASE(PREFIX "reveals only an eligible selected result from completed or partial results")
{
    REQUIRE([NSThread isMainThread]);
    NCExplorerSearchModeView *const view = MakeView();
    int reveals = 0;
    [view setRevealOriginalHandler:[&] { ++reveals; }];

    auto completed = Snapshot(SearchPhase::Completed);
    completed.results = SearchResultReference{.count = 3, .generation = 4, .token = "result-token"};
    [view applySnapshot:completed resultSelectionEligible:true];
    CHECK(view.revealForTesting.enabled);
    [view.revealForTesting performClick:nil];
    CHECK(reveals == 1);

    [view applySnapshot:completed resultSelectionEligible:false];
    CHECK_FALSE(view.revealForTesting.enabled);

    auto partial = Snapshot(SearchPhase::PartiallyCompleted);
    [view applySnapshot:partial resultSelectionEligible:true];
    CHECK(view.revealForTesting.enabled);

    auto limited = Snapshot(SearchPhase::PermissionLimitedResults);
    [view applySnapshot:limited resultSelectionEligible:true];
    CHECK(view.revealForTesting.enabled);

    auto too_many = Snapshot(SearchPhase::TooManyResults);
    [view applySnapshot:too_many resultSelectionEligible:true];
    CHECK(view.revealForTesting.enabled);
}

TEST_CASE(PREFIX "renders no-results too-many unavailable permission-limited and failure states as text")
{
    REQUIRE([NSThread isMainThread]);
    NCExplorerSearchModeView *const view = MakeView();

    for( const SearchPhase phase : {SearchPhase::NoResults,
                                    SearchPhase::TooManyResults,
                                    SearchPhase::IndexUnavailable,
                                    SearchPhase::BackendUnavailable,
                                    SearchPhase::PermissionLimitedResults} ) {
        auto snapshot = Snapshot(phase);
        [view applySnapshot:snapshot resultSelectionEligible:false];
        CAPTURE(static_cast<int>(phase));
        CHECK(view.statusForTesting.length > 0);
        CHECK([static_cast<NSString *>(view.accessibilityValue) containsString:view.statusForTesting]);
        CHECK(view.progressForTesting.indeterminate);
    }

    auto failed = Snapshot(SearchPhase::Failed);
    failed.failure = SearchFailure{.code = SearchFailureCode::PermissionDenied, .detail = "Archive access denied"};
    [view applySnapshot:failed resultSelectionEligible:false];
    CHECK([view.statusForTesting containsString:@"Archive access denied"]);
}

TEST_CASE(PREFIX "emits an owned request and narrow reveal cancel and close callbacks")
{
    REQUIRE([NSThread isMainThread]);
    NCExplorerSearchModeView *const view = MakeView();
    auto idle = Snapshot(SearchPhase::Idle);
    [view applySnapshot:idle resultSelectionEligible:false];

    std::optional<SearchRequest> received;
    int closed = 0;
    [view setStartHandler:[&](SearchRequest _request) { received = std::move(_request); }];
    [view setCloseHandler:[&] { ++closed; }];
    view.queryForTesting.stringValue = @"annual report";
    [view.scopeForTesting selectItemWithTag:static_cast<NSInteger>(SearchScope::CurrentDisk)];
    view.nameMatchForTesting.selectedSegment = 1;
    view.extensionForTesting.stringValue = @".pdf";
    [view.fileTypeForTesting selectItemWithTag:static_cast<NSInteger>(SearchFileType::Directory)];
    view.minimumSizeForTesting.stringValue = @"4096";
    view.maximumSizeForTesting.stringValue = @"1048576";
    view.modifiedAfterForTesting.stringValue = @"1700000000";
    view.modifiedBeforeForTesting.stringValue = @"1800000000";
    view.contentQueryForTesting.stringValue = @"signed agreement";
    view.includeHiddenForTesting.state = NSControlStateValueOn;
    [view controlTextDidChange:[NSNotification notificationWithName:NSControlTextDidChangeNotification
                                                             object:view.queryForTesting]];
    REQUIRE(view.startForTesting.enabled);
    [view.startForTesting performClick:nil];

    REQUIRE(received);
    CHECK(received->query == "annual report");
    CHECK(received->scope == SearchScope::CurrentDisk);
    CHECK(received->filters.name_match == SearchNameMatch::Exact);
    CHECK(received->filters.file_type == SearchFileType::Directory);
    CHECK(received->filters.extension == ".pdf");
    CHECK(received->filters.size.minimum_bytes == 4096);
    CHECK(received->filters.size.maximum_bytes == 1048576);
    CHECK(received->filters.modified.earliest_seconds == 1700000000);
    CHECK(received->filters.modified.latest_seconds == 1800000000);
    CHECK(received->filters.content == "signed agreement");
    CHECK(received->filters.include_hidden);

    [view.closeForTesting performClick:nil];
    CHECK(closed == 1);
    CHECK([view.queryForTesting.accessibilityIdentifier isEqualToString:@"wincommander.explorer.searchMode.query"]);
    CHECK(view.queryForTesting.accessibilityLabel.length > 0);
    CHECK(view.queryForTesting.accessibilityHelp.length > 0);
    CHECK([view.scopeForTesting.accessibilityIdentifier isEqualToString:@"wincommander.explorer.searchMode.scope"]);
    CHECK(view.scopeForTesting.accessibilityLabel.length > 0);
    for( NSView *const control : @[
             view.extensionForTesting,
             view.fileTypeForTesting,
             view.minimumSizeForTesting,
             view.maximumSizeForTesting,
             view.modifiedAfterForTesting,
             view.modifiedBeforeForTesting,
             view.contentQueryForTesting,
             view.includeHiddenForTesting,
         ] ) {
        CHECK(control.accessibilityIdentifier.length > 0);
        CHECK(control.accessibilityLabel.length > 0);
    }
}

TEST_CASE(PREFIX "consumes Return when synchronous Start changes the snapshot")
{
    REQUIRE([NSThread isMainThread]);
    NCExplorerSearchModeView *const view = MakeView();
    auto idle = Snapshot(SearchPhase::Idle);
    idle.request = Request();
    [view applySnapshot:idle resultSelectionEligible:false];

    int starts = 0;
    [view setStartHandler:[&](SearchRequest) {
      ++starts;
      auto running = Snapshot(SearchPhase::Running);
      running.request = Request();
      running.backend = DirectBackend();
      [view applySnapshot:running resultSelectionEligible:false];
    }];
    NSEvent *const enter = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                          characters:@"\r"
                         charactersIgnoringModifiers:@"\r"
                                           isARepeat:NO
                                             keyCode:36];
    REQUIRE(enter);
    CHECK([view performKeyEquivalent:enter]);
    CHECK(starts == 1);
    CHECK_FALSE(view.startForTesting.enabled);
}

#undef PREFIX
