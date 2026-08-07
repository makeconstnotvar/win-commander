// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <Operations/Job.h>
#include <Operations/Pool.h>
#include <WinCommander/States/Explorer/NCExplorerOperationProgressView.h>
#include <WinCommander/States/Explorer/ExplorerOperationProgressController.h>
#include <atomic>
#include <thread>

using namespace nc::core;
using namespace std::chrono_literals;
using Catch::Approx;

@interface NCExplorerOperationProgressView (ExplorerOperationProgressViewTesting)
- (CGFloat)visibleHeightForTesting;
- (NSString *)titleForTesting;
- (NSString *)stateForTesting;
- (NSString *)currentItemForTesting;
- (NSString *)progressForTesting;
- (NSString *)speedForTesting;
- (NSString *)etaForTesting;
- (NSString *)additionalForTesting;
- (NSProgressIndicator *)indicatorForTesting;
@end

@interface ExplorerOperationProgressController (ExplorerOperationProgressControllerTesting)
- (void)refreshNowForTesting;
@end

namespace {

struct ProgressTestJob final : nc::ops::Job {
    void SetCurrentItem(std::string _path) { PublishCurrentItemPath(std::move(_path)); }
    void Perform() override
    {
        while( !done && !IsStopped() )
            std::this_thread::yield();
        if( !IsStopped() )
            SetCompleted();
    }
    std::atomic_bool done{false};
};

struct ProgressTestOperation final : nc::ops::Operation {
    explicit ProgressTestOperation(std::string _title)
    {
        SetTitle(std::move(_title));
        job.Statistics().SetPreferredSource(nc::ops::Statistics::SourceType::Bytes);
        job.Statistics().CommitEstimated(nc::ops::Statistics::SourceType::Bytes, 4096);
        job.Statistics().CommitProcessed(nc::ops::Statistics::SourceType::Bytes, 1024);
        job.SetCurrentItem("/source/current.bin");
    }
    ~ProgressTestOperation() override { Wait(); }
    nc::ops::Job *GetJob() noexcept override { return &job; }
    ProgressTestJob job;
};

} // namespace

#define PREFIX "NCExplorerOperationProgressView "

TEST_CASE(PREFIX "is absent from layout when the exact window Pool has no operations")
{
    REQUIRE([NSThread isMainThread]);
    NCExplorerOperationProgressView *const view =
        [[NCExplorerOperationProgressView alloc] initWithFrame:NSMakeRect(0, 0, 640, 44)];
    [view applySnapshot:std::nullopt];

    CHECK(view.hidden);
    CHECK(view.visibleHeightForTesting == 0.0);
    CHECK([view.accessibilityIdentifier isEqualToString:@"wincommander.explorer.operationProgress"]);
    CHECK([view.accessibilityRole isEqualToString:NSAccessibilityGroupRole]);
}

TEST_CASE(PREFIX "presents determinate byte progress speed ETA state and operation count")
{
    REQUIRE([NSThread isMainThread]);
    NCExplorerOperationProgressView *const view =
        [[NCExplorerOperationProgressView alloc] initWithFrame:NSMakeRect(0, 0, 640, 44)];
    const ExplorerOperationProgressSnapshot snapshot{
        .title = "Copying Documents",
        .current_item_path = "/Users/test/Documents/report.pdf",
        .lifecycle = ExplorerOperationLifecycle::Running,
        .unit = ExplorerOperationProgressUnit::Bytes,
        .processed = 1'048'576,
        .total = 4'194'304,
        .fraction = 0.25,
        .speed_per_second = 524'288.0,
        .eta = 6s,
        .indeterminate = false,
        .active_count = 3,
        .additional_count = 2,
    };
    [view applySnapshot:snapshot];

    CHECK_FALSE(view.hidden);
    CHECK(view.visibleHeightForTesting == 44.0);
    CHECK([view.titleForTesting isEqualToString:@"Copying Documents"]);
    CHECK(view.stateForTesting.length > 0);
    CHECK([view.currentItemForTesting isEqualToString:@"report.pdf"]);
    CHECK(view.progressForTesting.length > 0);
    CHECK(view.speedForTesting.length > 0);
    CHECK(view.etaForTesting.length > 0);
    CHECK(view.additionalForTesting.length > 0);
    CHECK_FALSE(view.indicatorForTesting.indeterminate);
    CHECK(view.indicatorForTesting.doubleValue == Approx(0.25));
    NSString *const accessibility_value = static_cast<NSString *>(view.accessibilityValue);
    CHECK(accessibility_value.length > 0);
}

TEST_CASE(PREFIX "presents waiting and unknown item totals without color-only semantics")
{
    REQUIRE([NSThread isMainThread]);
    NCExplorerOperationProgressView *const view =
        [[NCExplorerOperationProgressView alloc] initWithFrame:NSMakeRect(0, 0, 640, 44)];
    const ExplorerOperationProgressSnapshot snapshot{
        .title = "Moving Items",
        .lifecycle = ExplorerOperationLifecycle::WaitingForUser,
        .unit = ExplorerOperationProgressUnit::Items,
        .processed = 2,
        .total = 0,
        .fraction = 0.0,
        .speed_per_second = std::nullopt,
        .eta = std::nullopt,
        .indeterminate = true,
        .active_count = 1,
        .additional_count = 0,
    };
    [view applySnapshot:snapshot];

    CHECK(view.stateForTesting.length > 0);
    CHECK(view.progressForTesting.length > 0);
    CHECK(view.indicatorForTesting.indeterminate);
    CHECK(view.speedForTesting.length == 0);
    CHECK(view.etaForTesting.length == 0);
    NSString *const accessibility_value = static_cast<NSString *>(view.accessibilityValue);
    CHECK([accessibility_value containsString:view.stateForTesting]);
}

TEST_CASE(PREFIX "controller remains bound to the exact per-window Pool and removes terminal progress")
{
    REQUIRE([NSThread isMainThread]);
    const auto first_pool = nc::ops::Pool::Make();
    const auto other_pool = nc::ops::Pool::Make();
    auto first_operation = std::make_shared<ProgressTestOperation>("First window copy");
    auto other_operation = std::make_shared<ProgressTestOperation>("Other window copy");
    NCExplorerOperationProgressView *const view =
        [[NCExplorerOperationProgressView alloc] initWithFrame:NSMakeRect(0, 0, 640, 44)];
    ExplorerOperationProgressController *const controller =
        [[ExplorerOperationProgressController alloc] initWithPool:*first_pool view:view];

    first_pool->Enqueue(first_operation);
    other_pool->Enqueue(other_operation);
    [controller refreshNowForTesting];
    CHECK([view.titleForTesting isEqualToString:@"First window copy"]);
    CHECK([view.currentItemForTesting isEqualToString:@"current.bin"]);
    CHECK_FALSE([view.titleForTesting isEqualToString:@"Other window copy"]);
    CHECK_FALSE(view.hidden);

    first_operation->job.done = true;
    other_operation->job.done = true;
    REQUIRE(first_operation->Wait(1s));
    REQUIRE(other_operation->Wait(1s));
    for( int attempt = 0; attempt != 1000 && (!first_pool->Empty() || !other_pool->Empty()); ++attempt )
        std::this_thread::sleep_for(1ms);
    REQUIRE(first_pool->Empty());
    REQUIRE(other_pool->Empty());
    [controller refreshNowForTesting];
    CHECK(view.hidden);
    CHECK(view.visibleHeightForTesting == 0.0);
}

#undef PREFIX
