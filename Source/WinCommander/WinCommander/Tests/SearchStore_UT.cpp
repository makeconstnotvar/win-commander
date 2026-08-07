// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Search/SearchPlanning.h>
#include <WinCommander/Core/Search/SearchStore.h>

#include <cmath>
#include <utility>

namespace {

using namespace nc::core;

SearchRequest Request(const SearchScope _scope = SearchScope::CurrentFolder)
{
    return {.query = "report", .scope = _scope};
}

SearchPlanningFacts DirectFacts()
{
    return {
        .current_folder = "/work",
        .current_disk_root = "/",
        .provider_available = true,
        .provider_is_native = true,
        .provider_supports_recursive = true,
        .provider_supports_current_disk = true,
        .provider_supports_metadata = true,
        .provider_supports_content = true,
        .provider_supports_hidden_items = true,
        .provider_reports_determinate_progress = true,
    };
}

SearchPlanningFacts SpotlightFacts()
{
    auto facts = DirectFacts();
    facts.spotlight_available = true;
    facts.spotlight_index_available = true;
    facts.spotlight_supports_content = true;
    facts.full_disk_access = true;
    return facts;
}

SearchPlan Plan(const SearchScope _scope = SearchScope::CurrentFolder)
{
    auto plan = SearchPlanning::Plan(Request(_scope),
                                     _scope == SearchScope::SpotlightWholeMac ? SpotlightFacts() : DirectFacts());
    REQUIRE(plan);
    return std::move(*plan);
}

SearchStore Store(const PaneId _pane = PaneId{41})
{
    auto store = SearchStore::Create(_pane);
    REQUIRE(store);
    return std::move(*store);
}

SearchRunId StartRunning(SearchStore &_store, const SearchPlan &_plan)
{
    const auto run = _store.Start(_plan);
    REQUIRE(run);
    REQUIRE(_store.MarkRunning(*run));
    return *run;
}

void CheckFailure(const SearchStoreMutationResult &_result, const SearchStoreFailure _failure)
{
    REQUIRE_FALSE(_result);
    CHECK(_result.error() == _failure);
}

} // namespace

#define PREFIX "nc::core::SearchStore "

TEST_CASE(PREFIX "binds an idle store to one nonzero pane")
{
    const auto invalid = SearchStore::Create(PaneId{});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error() == SearchStoreFailure::ZeroPaneId);

    const auto snapshot = Store(PaneId{77}).Snapshot();
    CHECK(snapshot.pane_id == PaneId{77});
    CHECK(snapshot.revision == 0);
    CHECK(snapshot.phase == SearchPhase::Idle);
    CHECK_FALSE(snapshot.run_id);
}

TEST_CASE(PREFIX "normalizes owned textual criteria and canonical extension")
{
    auto request = Request();
    request.query = "  annual report  ";
    request.filters.extension = " .pdf ";
    request.filters.content = "  revenue  ";

    const auto normalized = SearchPlanning::Normalize(std::move(request));
    REQUIRE(normalized);
    CHECK(normalized->query == "annual report");
    CHECK(normalized->filters.extension == "pdf");
    CHECK(normalized->filters.content == "revenue");
}

TEST_CASE(PREFIX "rejects empty criteria and inverted ranges")
{
    SearchRequest empty;
    auto invalid = SearchPlanning::Normalize(empty);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error() == SearchPlanningFailure::EmptyCriteria);

    auto size = Request();
    size.filters.size = {.minimum_bytes = 2, .maximum_bytes = 1};
    invalid = SearchPlanning::Normalize(size);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error() == SearchPlanningFailure::InvalidSizeRange);

    auto modified = Request();
    modified.filters.modified = {.earliest_seconds = 20, .latest_seconds = 10};
    invalid = SearchPlanning::Normalize(modified);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error() == SearchPlanningFailure::InvalidModifiedRange);
}

TEST_CASE(PREFIX "plans direct folder recursive and disk roots from explicit capabilities")
{
    const auto folder = SearchPlanning::Plan(Request(), DirectFacts());
    REQUIRE(folder);
    CHECK(folder->backend.kind == SearchBackendKind::DirectScan);
    CHECK(folder->backend.support == SearchBackendSupport::Supported);
    CHECK(folder->execution_root == "/work");
    CHECK(folder->backend.capabilities.determinate_progress);

    const auto recursive = SearchPlanning::Plan(Request(SearchScope::Recursive), DirectFacts());
    REQUIRE(recursive);
    CHECK(recursive->backend.support == SearchBackendSupport::Supported);
    CHECK(recursive->backend.capabilities.recursive_scope);

    const auto disk = SearchPlanning::Plan(Request(SearchScope::CurrentDisk), DirectFacts());
    REQUIRE(disk);
    CHECK(disk->execution_root == "/");
    CHECK(disk->backend.capabilities.current_disk_scope);
}

TEST_CASE(PREFIX "exposes direct backend limitations instead of silently dropping filters")
{
    auto request = Request(SearchScope::Recursive);
    request.filters.content = "needle";
    request.filters.file_type = SearchFileType::RegularFile;
    request.filters.include_hidden = true;
    auto facts = DirectFacts();
    facts.provider_supports_recursive = false;
    facts.provider_supports_content = false;
    facts.provider_supports_metadata = false;
    facts.provider_supports_hidden_items = false;

    const auto plan = SearchPlanning::Plan(request, facts);
    REQUIRE(plan);
    CHECK(plan->backend.support == SearchBackendSupport::Unsupported);
    CHECK(plan->backend.limitations.size() == 4);
    CHECK(plan->backend.limitations[0] == SearchBackendLimitation::RecursiveScopeUnavailable);
    CHECK(plan->backend.limitations[1] == SearchBackendLimitation::ContentSearchUnavailable);
    CHECK(plan->backend.limitations[2] == SearchBackendLimitation::MetadataSearchUnavailable);
    CHECK(plan->backend.limitations[3] == SearchBackendLimitation::HiddenItemsUnavailable);
}

TEST_CASE(PREFIX "distinguishes Spotlight service index and permission limitations")
{
    auto facts = SpotlightFacts();
    facts.spotlight_available = false;
    auto plan = SearchPlanning::Plan(Request(SearchScope::SpotlightWholeMac), facts);
    REQUIRE(plan);
    CHECK(plan->backend.support == SearchBackendSupport::Unavailable);
    CHECK(plan->backend.limitations[0] == SearchBackendLimitation::SpotlightUnavailable);

    facts.spotlight_available = true;
    facts.spotlight_index_available = false;
    plan = SearchPlanning::Plan(Request(SearchScope::SpotlightWholeMac), facts);
    REQUIRE(plan);
    CHECK(plan->backend.support == SearchBackendSupport::IndexUnavailable);
    CHECK(plan->backend.limitations[0] == SearchBackendLimitation::SpotlightIndexUnavailable);

    facts.spotlight_index_available = true;
    facts.full_disk_access = false;
    plan = SearchPlanning::Plan(Request(SearchScope::SpotlightWholeMac), facts);
    REQUIRE(plan);
    CHECK(plan->backend.support == SearchBackendSupport::Supported);
    CHECK(plan->backend.limitations[0] == SearchBackendLimitation::FullDiskAccessMissing);
}

TEST_CASE(PREFIX "rejects inconsistent support and limitation descriptors")
{
    auto plan = Plan();
    CHECK(SearchPlanning::IsValid(plan));

    plan.backend.limitations = {SearchBackendLimitation::ProviderUnavailable};
    CHECK_FALSE(SearchPlanning::IsValid(plan));

    plan = Plan();
    plan.backend.support = SearchBackendSupport::Unavailable;
    CHECK_FALSE(SearchPlanning::IsValid(plan));
    plan.backend.limitations = {SearchBackendLimitation::ProviderUnavailable};
    CHECK(SearchPlanning::IsValid(plan));

    plan.backend.limitations.emplace_back(SearchBackendLimitation::ProviderUnavailable);
    CHECK_FALSE(SearchPlanning::IsValid(plan));

    plan = Plan();
    plan.backend.support = SearchBackendSupport::Unsupported;
    plan.backend.limitations = {SearchBackendLimitation::FullDiskAccessMissing};
    CHECK_FALSE(SearchPlanning::IsValid(plan));
    plan.request.filters.content = "needle";
    plan.backend.capabilities.content = false;
    plan.backend.limitations = {SearchBackendLimitation::ContentSearchUnavailable};
    CHECK(SearchPlanning::IsValid(plan));
}

TEST_CASE(PREFIX "starts pane-bound monotonically increasing runs and owns the plan")
{
    auto store = Store(PaneId{9});
    auto plan = Plan();
    const auto first = store.Start(plan);
    REQUIRE(first);
    CHECK(first->pane_id == PaneId{9});
    CHECK(first->generation == 1);

    plan.request.query = "mutated";
    plan.backend.limitations.emplace_back(SearchBackendLimitation::ProviderUnavailable);
    const auto observed = store.Snapshot();
    REQUIRE(observed.request);
    REQUIRE(observed.backend);
    CHECK(observed.request->query == "report");
    CHECK(observed.backend->limitations.empty());

    const auto second = store.Start(Plan());
    REQUIRE(second);
    CHECK(second->generation == 2);
    CHECK(store.Snapshot().revision == 2);
}

TEST_CASE(PREFIX "new run supersedes delayed events from the previous run")
{
    auto store = Store();
    const auto first = store.Start(Plan());
    REQUIRE(first);
    const auto second = store.Start(Plan(SearchScope::Recursive));
    REQUIRE(second);
    const auto baseline = store.Snapshot();

    CheckFailure(store.MarkRunning(*first), SearchStoreFailure::StaleRun);
    CHECK(store.Snapshot() == baseline);
    REQUIRE(store.MarkRunning(*second));
}

TEST_CASE(PREFIX "maps unavailable backend descriptors to explicit terminal phases")
{
    auto store = Store();
    auto facts = SpotlightFacts();
    facts.spotlight_index_available = false;
    auto index_plan = SearchPlanning::Plan(Request(SearchScope::SpotlightWholeMac), facts);
    REQUIRE(index_plan);
    const auto index_run = store.Start(*index_plan);
    REQUIRE(index_run);
    CHECK(store.Snapshot().phase == SearchPhase::IndexUnavailable);
    CheckFailure(store.MarkRunning(*index_run), SearchStoreFailure::TerminalRun);

    facts.spotlight_available = false;
    facts.spotlight_index_available = true;
    auto unavailable_plan = SearchPlanning::Plan(Request(SearchScope::SpotlightWholeMac), facts);
    REQUIRE(unavailable_plan);
    REQUIRE(store.Start(*unavailable_plan));
    CHECK(store.Snapshot().phase == SearchPhase::BackendUnavailable);
}

TEST_CASE(PREFIX "publishes monotonic determinate progress counts and owned location")
{
    auto store = Store();
    const auto run = StartRunning(store, Plan(SearchScope::Recursive));
    SearchProgressUpdate update{
        .determinate_progress = 0.25,
        .current_location = "/work/a",
        .scanned_count = 100,
        .found_count = 4,
    };
    REQUIRE(store.UpdateProgress(run, update));
    update.current_location = "/mutated";

    const auto snapshot = store.Snapshot();
    CHECK(snapshot.determinate_progress == 0.25);
    CHECK(snapshot.current_location == "/work/a");
    CHECK(snapshot.scanned_count == 100);
    CHECK(snapshot.found_count == 4);
}

TEST_CASE(PREFIX "atomically rejects invalid or regressing progress")
{
    auto store = Store();
    const auto run = StartRunning(store, Plan());
    REQUIRE(store.UpdateProgress(run, {.determinate_progress = 0.5, .scanned_count = 10, .found_count = 2}));
    const auto baseline = store.Snapshot();

    CheckFailure(store.UpdateProgress(run, {.determinate_progress = 0.4}), SearchStoreFailure::InvalidProgress);
    CheckFailure(store.UpdateProgress(run, {.determinate_progress = std::nan("")}),
                 SearchStoreFailure::InvalidProgress);
    CheckFailure(store.UpdateProgress(run, {.scanned_count = 1}), SearchStoreFailure::InvalidProgress);
    CHECK(store.Snapshot() == baseline);
}

TEST_CASE(PREFIX "accepts found-only progress when a backend has no scanned candidate counter")
{
    auto store = Store();
    const auto run = StartRunning(store, Plan());
    REQUIRE(store.UpdateProgress(run, {.current_location = "/work/a", .found_count = 1}));
    REQUIRE(store.UpdateProgress(run, {.found_count = 3}));

    const auto snapshot = store.Snapshot();
    CHECK_FALSE(snapshot.scanned_count);
    CHECK(snapshot.found_count == 3);
}

TEST_CASE(PREFIX "accepts only increasing nonempty result commit references")
{
    auto store = Store();
    const auto run = StartRunning(store, Plan());
    REQUIRE(store.PublishResults(run, {.count = 2, .generation = 7, .token = "listing-7"}));
    const auto baseline = store.Snapshot();

    CheckFailure(store.PublishResults(run, {.count = 3, .generation = 7, .token = "listing-repeat"}),
                 SearchStoreFailure::InvalidResults);
    CheckFailure(store.PublishResults(run, {.count = 3, .generation = 8, .token = {}}),
                 SearchStoreFailure::InvalidResults);
    CHECK(store.Snapshot() == baseline);

    REQUIRE(store.PublishResults(run, {.count = 3, .generation = 8, .token = "listing-8"}));
    CHECK(store.Snapshot().results->count == 3);
}

TEST_CASE(PREFIX "derives completed and no-results phases from committed count")
{
    auto empty_store = Store();
    const auto empty_run = StartRunning(empty_store, Plan());
    REQUIRE(empty_store.Complete(empty_run, SearchCompletionKind::Complete));
    CHECK(empty_store.Snapshot().phase == SearchPhase::NoResults);

    auto result_store = Store();
    const auto result_run = StartRunning(result_store, Plan());
    REQUIRE(result_store.PublishResults(result_run, {.count = 5, .generation = 1, .token = "results"}));
    REQUIRE(result_store.Complete(result_run, SearchCompletionKind::Complete));
    CHECK(result_store.Snapshot().phase == SearchPhase::Completed);
    CHECK(result_store.Snapshot().results->count == 5);
}

TEST_CASE(PREFIX "keeps bounded results for too-many and limitation-backed partial completion")
{
    auto too_many_store = Store();
    const auto too_many_run = StartRunning(too_many_store, Plan());
    REQUIRE(too_many_store.PublishResults(too_many_run, {.count = 1000, .generation = 1, .token = "cap"}));
    REQUIRE(too_many_store.Complete(too_many_run, SearchCompletionKind::TooManyResults));
    CHECK(too_many_store.Snapshot().phase == SearchPhase::TooManyResults);

    auto partial_store = Store();
    auto facts = SpotlightFacts();
    facts.full_disk_access = false;
    auto partial_plan = SearchPlanning::Plan(Request(SearchScope::SpotlightWholeMac), facts);
    REQUIRE(partial_plan);
    const auto partial_run = StartRunning(partial_store, *partial_plan);
    REQUIRE(partial_store.PublishResults(partial_run, {.count = 4, .generation = 1, .token = "partial"}));
    REQUIRE(partial_store.Complete(partial_run, SearchCompletionKind::Partial));
    CHECK(partial_store.Snapshot().phase == SearchPhase::PartiallyCompleted);
}

TEST_CASE(PREFIX "requires the matching permission limitation for permission-limited completion")
{
    auto store = Store();
    const auto ordinary_run = StartRunning(store, Plan());
    CheckFailure(store.Complete(ordinary_run, SearchCompletionKind::PermissionLimited),
                 SearchStoreFailure::LimitationMismatch);
    CHECK(store.Snapshot().phase == SearchPhase::Running);

    auto facts = SpotlightFacts();
    facts.full_disk_access = false;
    const auto limited_plan = SearchPlanning::Plan(Request(SearchScope::SpotlightWholeMac), facts);
    REQUIRE(limited_plan);
    const auto limited_run = store.Start(*limited_plan);
    REQUIRE(limited_run);
    REQUIRE(store.Complete(*limited_run, SearchCompletionKind::PermissionLimited));
    CHECK(store.Snapshot().phase == SearchPhase::PermissionLimitedResults);
}

TEST_CASE(PREFIX "fences runtime skipped-location limitations to the active run")
{
    auto store = Store();
    const auto old_run = StartRunning(store, Plan(SearchScope::Recursive));
    const auto run = StartRunning(store, Plan(SearchScope::Recursive));
    const auto baseline = store.Snapshot();

    CheckFailure(store.ReportLimitation(old_run, SearchBackendLimitation::PermissionDeniedLocations),
                 SearchStoreFailure::StaleRun);
    CheckFailure(store.ReportLimitation(run, SearchBackendLimitation::ContentSearchUnavailable),
                 SearchStoreFailure::InvalidLimitation);
    CHECK(store.Snapshot() == baseline);

    REQUIRE(store.ReportLimitation(run, SearchBackendLimitation::PermissionDeniedLocations));
    CheckFailure(store.ReportLimitation(run, SearchBackendLimitation::PermissionDeniedLocations),
                 SearchStoreFailure::InvalidLimitation);
    REQUIRE(store.Complete(run, SearchCompletionKind::PermissionLimited));
    CHECK(store.Snapshot().phase == SearchPhase::PermissionLimitedResults);

    const auto partial_run = StartRunning(store, Plan(SearchScope::Recursive));
    REQUIRE(store.ReportLimitation(partial_run, SearchBackendLimitation::ResultPathsUnavailable));
    REQUIRE(store.PublishResults(partial_run, {.count = 1, .generation = 1, .token = "partial"}));
    REQUIRE(store.Complete(partial_run, SearchCompletionKind::Partial));
    CHECK(store.Snapshot().phase == SearchPhase::PartiallyCompleted);
}

TEST_CASE(PREFIX "keeps root permission failure separate from partial descendant results")
{
    auto store = Store();
    const auto run = StartRunning(store, Plan(SearchScope::Recursive));
    REQUIRE(store.Fail(run, {.code = SearchFailureCode::PermissionDenied, .detail = "root is unreadable"}));

    const auto snapshot = store.Snapshot();
    CHECK(snapshot.phase == SearchPhase::Failed);
    CHECK(snapshot.failure->code == SearchFailureCode::PermissionDenied);
    CHECK(snapshot.backend->limitations.empty());
}

TEST_CASE(PREFIX "makes cancellation terminal and rejects later completion")
{
    auto store = Store();
    const auto run = StartRunning(store, Plan());
    REQUIRE(store.Cancel(run));
    CHECK(store.Snapshot().phase == SearchPhase::Cancelled);
    CheckFailure(store.Complete(run, SearchCompletionKind::Complete), SearchStoreFailure::TerminalRun);
    CheckFailure(store.UpdateProgress(run, {.scanned_count = 1}), SearchStoreFailure::TerminalRun);
}

TEST_CASE(PREFIX "retains an owned failure and reset retires the active run")
{
    auto store = Store();
    const auto run = StartRunning(store, Plan());
    SearchFailure failure{.code = SearchFailureCode::ProviderDisconnected, .detail = "connection lost"};
    REQUIRE(store.Fail(run, failure));
    failure.detail = "mutated";
    CHECK(store.Snapshot().phase == SearchPhase::Failed);
    CHECK(store.Snapshot().failure->detail == "connection lost");

    store.Reset();
    CHECK(store.Snapshot().phase == SearchPhase::Idle);
    CHECK_FALSE(store.Snapshot().run_id);
    CheckFailure(store.MarkRunning(run), SearchStoreFailure::NoActiveRun);
}

TEST_CASE(PREFIX "rejects a forged unnormalized or capability-inconsistent plan")
{
    auto store = Store();
    auto unnormalized = Plan();
    unnormalized.request.query = " report ";
    const auto baseline = store.Snapshot();
    auto result = store.Start(unnormalized);
    REQUIRE_FALSE(result);
    CHECK(result.error() == SearchStoreFailure::InvalidPlan);
    CHECK(store.Snapshot() == baseline);

    auto inconsistent = Plan();
    inconsistent.request.filters.content = "needle";
    inconsistent.backend.capabilities.content = false;
    result = store.Start(inconsistent);
    REQUIRE_FALSE(result);
    CHECK(result.error() == SearchStoreFailure::InvalidPlan);
    CHECK(store.Snapshot() == baseline);

    inconsistent = Plan();
    inconsistent.backend.capabilities.name = false;
    result = store.Start(inconsistent);
    REQUIRE_FALSE(result);
    CHECK(result.error() == SearchStoreFailure::InvalidPlan);
    CHECK(store.Snapshot() == baseline);

    inconsistent = Plan();
    inconsistent.request.filters.extension = "pdf";
    inconsistent.backend.capabilities.extension = false;
    result = store.Start(inconsistent);
    REQUIRE_FALSE(result);
    CHECK(result.error() == SearchStoreFailure::InvalidPlan);
    CHECK(store.Snapshot() == baseline);

    inconsistent = Plan();
    inconsistent.backend.capabilities.whole_mac_scope = true;
    result = store.Start(inconsistent);
    REQUIRE_FALSE(result);
    CHECK(result.error() == SearchStoreFailure::InvalidPlan);
    CHECK(store.Snapshot() == baseline);
}
