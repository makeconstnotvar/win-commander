// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/States/Explorer/ExplorerSearchController.h>
#include <WinCommander/States/FilePanels/PanelController.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

using namespace nc::core;
using namespace nc::explorer;

class SearchControllerTestHost final : public nc::vfs::Host
{
public:
    SearchControllerTestHost() : Host("/", nullptr, "search-controller-tests") {}
};

VFSListingPtr SingleListing(const VFSHostPtr &_host, std::string _directory, std::string _filename)
{
    nc::vfs::ListingInput input;
    input.hosts[0] = _host;
    input.directories[0] = std::move(_directory);
    input.filenames.emplace_back(std::move(_filename));
    input.unix_modes.emplace_back(S_IFREG | 0644);
    input.unix_types.emplace_back(DT_REG);
    return VFSListing::Build(std::move(input));
}

VFSListingPtr ResultListing(const VFSHostPtr &_host)
{
    std::vector<VFSListingPtr> listings{
        SingleListing(_host, "/first/", "alpha.txt"),
        SingleListing(_host, "/second/", "beta.txt"),
    };
    nc::vfs::ListingInput input = VFSListing::Compose(listings);
    input.title = "Search Results";
    return VFSListing::Build(std::move(input));
}

VFSListingPtr EmptyResultListing()
{
    nc::vfs::ListingInput input = VFSListing::Compose({});
    input.title = "Search Results";
    return VFSListing::Build(std::move(input));
}

class FakePanelAccess final : public ExplorerSearchPanelAccess
{
public:
    explicit FakePanelAccess(const VFSHostPtr &_host)
    {
        content = ExplorerSearchPanelContent{
            .pane_id = PaneId{91},
            .listing = SingleListing(_host, "/origin/", "seed.txt"),
            .data_generation = 7,
            .uniform_host = _host,
            .uniform_directory = "/origin/",
        };
    }

    std::optional<ExplorerSearchPanelContent> Capture() const override
    {
        return available ? std::optional<ExplorerSearchPanelContent>{content} : std::nullopt;
    }

    void CommitListing(const VFSListingPtr &_listing) override
    {
        ++commit_count;
        content.listing = _listing;
        ++content.data_generation;
        content.uniform_host.reset();
        content.uniform_directory.clear();
        content.focused_item = _listing && !_listing->Empty() ? _listing->Item(0) : VFSListingItem{};
    }

    bool SubmitReveal(std::shared_ptr<nc::panel::DirectoryChangeRequest> _request) override
    {
        reveal = std::move(_request);
        return reveal_result;
    }

    void ReplaceExternally(const VFSHostPtr &_host)
    {
        content.listing = SingleListing(_host, "/external/", "outside.txt");
        ++content.data_generation;
        content.uniform_host = _host;
        content.uniform_directory = "/external/";
        content.focused_item = {};
    }

    bool available = true;
    bool reveal_result = true;
    int commit_count = 0;
    ExplorerSearchPanelContent content;
    std::shared_ptr<nc::panel::DirectoryChangeRequest> reveal;
};

class FakeBackendRun final : public ExplorerSearchBackendRun
{
public:
    void Stop() noexcept override { stopped.store(true); }
    void Wait() noexcept override { waited.store(true); }

    std::atomic_bool stopped = false;
    std::atomic_bool waited = false;
};

class ControlledBackend final : public ExplorerSearchBackend
{
public:
    struct Invocation {
        ExplorerSearchBackendInput input;
        ProgressCallback progress;
        CompletionCallback completion;
        std::shared_ptr<FakeBackendRun> run;
    };

    std::shared_ptr<ExplorerSearchBackendRun>
    Start(ExplorerSearchBackendInput _input, ProgressCallback _progress, CompletionCallback _completion) override
    {
        if( throw_on_start )
            throw std::runtime_error{"scripted backend start failure"};
        auto run = std::make_shared<FakeBackendRun>();
        invocations.emplace_back(Invocation{
            .input = std::move(_input),
            .progress = std::move(_progress),
            .completion = std::move(_completion),
            .run = run,
        });
        return run;
    }

    std::vector<Invocation> invocations;
    bool throw_on_start = false;
};

SearchPlanningFacts Facts()
{
    return {
        .current_folder = "/wrong/",
        .current_disk_root = "/",
        .provider_available = true,
        .provider_is_native = true,
        .provider_supports_recursive = true,
        .provider_supports_current_disk = true,
        .provider_supports_metadata = true,
        .provider_supports_content = true,
        .provider_supports_hidden_items = true,
        .provider_reports_determinate_progress = false,
        .spotlight_available = true,
        .spotlight_index_available = true,
        .spotlight_supports_content = true,
        .full_disk_access = true,
    };
}

SearchRequest Request(std::string _query = "report")
{
    return {.query = std::move(_query), .scope = SearchScope::Recursive};
}

bool RunMainLoopUntil(const std::function<bool()> &_predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while( std::chrono::steady_clock::now() < deadline ) {
        if( _predicate() )
            return true;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }
    return _predicate();
}

struct Fixture {
    Fixture()
    {
        panel = std::make_shared<FakePanelAccess>(host);
        backend = std::make_shared<ControlledBackend>();
        controller = [[ExplorerSearchController alloc]
            initWithPanelAccess:panel
                         paneId:PaneId{91}
                backendProvider:[backend = backend](SearchBackendKind) { return backend; }
                 snapshotHandler:[this](std::optional<SearchSnapshot> _snapshot) {
                   snapshots.emplace_back(std::move(_snapshot));
                 }];
    }

    SearchSnapshot Snapshot() const
    {
        REQUIRE(!snapshots.empty());
        REQUIRE(snapshots.back());
        return *snapshots.back();
    }

    std::shared_ptr<SearchControllerTestHost> host = std::make_shared<SearchControllerTestHost>();
    std::shared_ptr<FakePanelAccess> panel;
    std::shared_ptr<ControlledBackend> backend;
    ExplorerSearchController *controller;
    std::vector<std::optional<SearchSnapshot>> snapshots;
};

ExplorerSearchBackendCompletion Results(const VFSListingPtr &_listing,
                                        const ExplorerSearchBackendCompletionKind _kind =
                                            ExplorerSearchBackendCompletionKind::Completed)
{
    return {.kind = _kind, .listing = _listing, .accepted_count = _listing ? _listing->Count() : 0};
}

} // namespace

#define PREFIX "ExplorerSearchController "

TEST_CASE(PREFIX "publishes an owned Idle draft then commits exactly one final listing")
{
    REQUIRE([NSThread isMainThread]);
    Fixture fixture;
    SearchRequest draft;
    draft.scope = SearchScope::SpotlightWholeMac;
    REQUIRE([fixture.controller presentWithPlanningFacts:Facts() initialRequest:draft]);
    CHECK(fixture.Snapshot().phase == SearchPhase::Idle);
    REQUIRE(fixture.Snapshot().request);
    CHECK(fixture.Snapshot().request->scope == SearchScope::SpotlightWholeMac);

    REQUIRE([fixture.controller startSearch:Request()]);
    REQUIRE(fixture.backend->invocations.size() == 1);
    CHECK(fixture.backend->invocations[0].input.plan.execution_root == "/origin/");
    fixture.backend->invocations[0].progress(
        {.current_location = "/origin/subdir/", .scanned_count = 4, .found_count = 1});
    REQUIRE(RunMainLoopUntil([&] { return fixture.Snapshot().found_count == 1; }));

    const VFSListingPtr results = ResultListing(fixture.host);
    fixture.backend->invocations[0].completion(Results(results));
    REQUIRE(RunMainLoopUntil([&] { return fixture.Snapshot().phase == SearchPhase::Completed; }));
    CHECK(fixture.panel->commit_count == 1);
    CHECK(fixture.panel->content.listing == results);
    REQUIRE(fixture.Snapshot().results);
    CHECK(fixture.Snapshot().results->count == 2);
    CHECK([fixture.controller canRevealFocusedResult]);
}

TEST_CASE(PREFIX "starts the exact owned draft and contains backend construction exceptions")
{
    REQUIRE([NSThread isMainThread]);
    Fixture fixture;
    const SearchRequest draft = Request("owned draft");
    REQUIRE([fixture.controller presentWithPlanningFacts:Facts() initialRequest:draft]);
    REQUIRE([fixture.controller startPresentedDraft]);
    REQUIRE(fixture.backend->invocations.size() == 1);
    CHECK(fixture.backend->invocations[0].input.plan.request == draft);

    [fixture.controller cancel];
    fixture.backend->throw_on_start = true;
    CHECK_FALSE([fixture.controller startSearch:Request("throws")]);
    CHECK(fixture.Snapshot().phase == SearchPhase::Failed);
    REQUIRE(fixture.Snapshot().failure);
    CHECK(fixture.Snapshot().failure->code == SearchFailureCode::ExecutionFailed);
}

TEST_CASE(PREFIX "supersedes stale runs and fences an externally replaced listing")
{
    REQUIRE([NSThread isMainThread]);
    Fixture fixture;
    REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
    REQUIRE([fixture.controller startSearch:Request("first")]);
    REQUIRE([fixture.controller startSearch:Request("second")]);
    REQUIRE(fixture.backend->invocations.size() == 2);
    CHECK(fixture.backend->invocations[0].run->stopped.load());

    fixture.backend->invocations[0].completion(Results(ResultListing(fixture.host)));
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.03, false);
    CHECK(fixture.panel->commit_count == 0);

    fixture.panel->ReplaceExternally(fixture.host);
    fixture.backend->invocations[1].completion(Results(ResultListing(fixture.host)));
    REQUIRE(RunMainLoopUntil([&] { return !fixture.controller.isPresented; }));
    CHECK(fixture.panel->commit_count == 0);
    CHECK_FALSE([fixture.controller snapshot]);
}

TEST_CASE(PREFIX "cancellation is non-blocking and rejects a late backend completion")
{
    REQUIRE([NSThread isMainThread]);
    Fixture fixture;
    REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
    REQUIRE([fixture.controller startSearch:Request()]);
    auto run = fixture.backend->invocations[0].run;
    [fixture.controller cancel];
    CHECK(run->stopped.load());
    CHECK(fixture.Snapshot().phase == SearchPhase::Cancelled);
    REQUIRE(RunMainLoopUntil([&] { return run->waited.load(); }));

    fixture.backend->invocations[0].completion(Results(ResultListing(fixture.host)));
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.03, false);
    CHECK(fixture.panel->commit_count == 0);
    CHECK(fixture.Snapshot().phase == SearchPhase::Cancelled);
}

TEST_CASE(PREFIX "maps root failure permission-limited no-results and too-many terminal outcomes")
{
    REQUIRE([NSThread isMainThread]);

    SECTION("root failure")
    {
        Fixture fixture;
        REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
        REQUIRE([fixture.controller startSearch:Request()]);
        fixture.backend->invocations[0].completion({
            .kind = ExplorerSearchBackendCompletionKind::Failed,
            .failure = SearchFailure{.code = SearchFailureCode::PermissionDenied, .detail = "root denied"},
        });
        REQUIRE(RunMainLoopUntil([&] { return fixture.Snapshot().phase == SearchPhase::Failed; }));
        CHECK(fixture.panel->commit_count == 0);
        REQUIRE(fixture.Snapshot().failure);
        CHECK(fixture.Snapshot().failure->code == SearchFailureCode::PermissionDenied);
    }

    SECTION("malformed failure")
    {
        Fixture fixture;
        REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
        REQUIRE([fixture.controller startSearch:Request()]);
        fixture.backend->invocations[0].completion({
            .kind = ExplorerSearchBackendCompletionKind::Failed,
            .failure = SearchFailure{.code = SearchFailureCode::ExecutionFailed, .detail = ""},
        });
        REQUIRE(RunMainLoopUntil([&] { return fixture.Snapshot().phase == SearchPhase::Failed; }));
        REQUIRE(fixture.Snapshot().failure);
        CHECK(fixture.Snapshot().failure->code == SearchFailureCode::InvalidBackendReply);
        CHECK_FALSE(fixture.Snapshot().failure->detail.empty());
    }

    SECTION("inconsistent result count")
    {
        Fixture fixture;
        REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
        REQUIRE([fixture.controller startSearch:Request()]);
        auto malformed = Results(ResultListing(fixture.host));
        malformed.accepted_count = 0;
        fixture.backend->invocations[0].completion(std::move(malformed));
        REQUIRE(RunMainLoopUntil([&] { return fixture.Snapshot().phase == SearchPhase::Failed; }));
        CHECK(fixture.panel->commit_count == 0);
        REQUIRE(fixture.Snapshot().failure);
        CHECK(fixture.Snapshot().failure->code == SearchFailureCode::InvalidBackendReply);
    }

    SECTION("permission partial")
    {
        Fixture fixture;
        REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
        REQUIRE([fixture.controller startSearch:Request()]);
        auto completion = Results(ResultListing(fixture.host), ExplorerSearchBackendCompletionKind::PermissionLimited);
        completion.limitations = {SearchBackendLimitation::PermissionDeniedLocations};
        fixture.backend->invocations[0].completion(std::move(completion));
        REQUIRE(RunMainLoopUntil(
            [&] { return fixture.Snapshot().phase == SearchPhase::PermissionLimitedResults; }));
        CHECK(fixture.panel->commit_count == 1);
        CHECK([fixture.controller canRevealFocusedResult]);
    }

    SECTION("no results")
    {
        Fixture fixture;
        REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
        REQUIRE([fixture.controller startSearch:Request()]);
        fixture.backend->invocations[0].completion(Results(EmptyResultListing()));
        REQUIRE(RunMainLoopUntil([&] { return fixture.Snapshot().phase == SearchPhase::NoResults; }));
        CHECK(fixture.panel->commit_count == 1);
    }

    SECTION("too many")
    {
        Fixture fixture;
        REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
        REQUIRE([fixture.controller startSearch:Request()]);
        fixture.backend->invocations[0].completion(
            Results(ResultListing(fixture.host), ExplorerSearchBackendCompletionKind::TooManyResults));
        REQUIRE(RunMainLoopUntil([&] { return fixture.Snapshot().phase == SearchPhase::TooManyResults; }));
        CHECK(fixture.panel->commit_count == 1);
        CHECK([fixture.controller canRevealFocusedResult]);
    }
}

TEST_CASE(PREFIX "rejects terminal payload mismatches before committing a listing")
{
    REQUIRE([NSThread isMainThread]);

    const auto check_rejected = [](ExplorerSearchBackendCompletion _completion) {
        Fixture fixture;
        REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
        REQUIRE([fixture.controller startSearch:Request()]);
        fixture.backend->invocations[0].completion(std::move(_completion));
        REQUIRE(RunMainLoopUntil([&] { return fixture.Snapshot().phase == SearchPhase::Failed; }));
        CHECK(fixture.panel->commit_count == 0);
        REQUIRE(fixture.Snapshot().failure);
        CHECK(fixture.Snapshot().failure->code == SearchFailureCode::InvalidBackendReply);
    };

    SECTION("complete with missing result paths")
    {
        auto completion = Results(ResultListing(std::make_shared<SearchControllerTestHost>()));
        completion.limitations = {SearchBackendLimitation::ResultPathsUnavailable};
        check_rejected(std::move(completion));
    }
    SECTION("partial without a missing-path limitation")
    {
        check_rejected(Results(ResultListing(std::make_shared<SearchControllerTestHost>()),
                               ExplorerSearchBackendCompletionKind::Partial));
    }
    SECTION("permission-limited with only a missing-path limitation")
    {
        auto completion = Results(ResultListing(std::make_shared<SearchControllerTestHost>()),
                                  ExplorerSearchBackendCompletionKind::PermissionLimited);
        completion.limitations = {SearchBackendLimitation::ResultPathsUnavailable};
        check_rejected(std::move(completion));
    }
}

TEST_CASE(PREFIX "fails closed on mismatched runtime availability replies")
{
    REQUIRE([NSThread isMainThread]);

    SECTION("direct backend cannot report an index outage")
    {
        Fixture fixture;
        REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
        REQUIRE([fixture.controller startSearch:Request()]);
        fixture.backend->invocations[0].completion({
            .kind = ExplorerSearchBackendCompletionKind::IndexUnavailable,
            .limitations = {SearchBackendLimitation::SpotlightIndexUnavailable},
        });
        REQUIRE(RunMainLoopUntil([&] { return fixture.Snapshot().phase == SearchPhase::Failed; }));
        CHECK(fixture.panel->commit_count == 0);
        REQUIRE(fixture.Snapshot().failure);
        CHECK(fixture.Snapshot().failure->code == SearchFailureCode::InvalidBackendReply);
    }

    SECTION("direct backend cannot report a Spotlight service outage")
    {
        Fixture fixture;
        REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
        REQUIRE([fixture.controller startSearch:Request()]);
        fixture.backend->invocations[0].completion({
            .kind = ExplorerSearchBackendCompletionKind::BackendUnavailable,
            .limitations = {SearchBackendLimitation::SpotlightUnavailable},
        });
        REQUIRE(RunMainLoopUntil([&] { return fixture.Snapshot().phase == SearchPhase::Failed; }));
        CHECK(fixture.panel->commit_count == 0);
        REQUIRE(fixture.Snapshot().failure);
        CHECK(fixture.Snapshot().failure->code == SearchFailureCode::InvalidBackendReply);
    }
}

TEST_CASE(PREFIX "publishes exact runtime availability transitions without committing results")
{
    REQUIRE([NSThread isMainThread]);

    SECTION("direct provider unavailable")
    {
        Fixture fixture;
        REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
        REQUIRE([fixture.controller startSearch:Request()]);
        fixture.backend->invocations[0].completion({
            .kind = ExplorerSearchBackendCompletionKind::BackendUnavailable,
            .limitations = {SearchBackendLimitation::ProviderUnavailable},
        });
        REQUIRE(RunMainLoopUntil([&] { return fixture.Snapshot().phase == SearchPhase::BackendUnavailable; }));
        CHECK(fixture.panel->commit_count == 0);
        REQUIRE(fixture.Snapshot().backend);
        CHECK(fixture.Snapshot().backend->kind == SearchBackendKind::DirectScan);
    }

    SECTION("Spotlight index unavailable")
    {
        Fixture fixture;
        REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
        auto request = Request();
        request.scope = SearchScope::SpotlightWholeMac;
        REQUIRE([fixture.controller startSearch:std::move(request)]);
        fixture.backend->invocations[0].completion({
            .kind = ExplorerSearchBackendCompletionKind::IndexUnavailable,
            .limitations = {SearchBackendLimitation::SpotlightIndexUnavailable},
        });
        REQUIRE(RunMainLoopUntil([&] { return fixture.Snapshot().phase == SearchPhase::IndexUnavailable; }));
        CHECK(fixture.panel->commit_count == 0);
        REQUIRE(fixture.Snapshot().backend);
        CHECK(fixture.Snapshot().backend->kind == SearchBackendKind::Spotlight);
    }
}

TEST_CASE(PREFIX "preserves its committed result identity and reveals the exact focused original")
{
    REQUIRE([NSThread isMainThread]);
    Fixture fixture;
    REQUIRE([fixture.controller presentWithPlanningFacts:Facts()]);
    REQUIRE([fixture.controller startSearch:Request()]);
    fixture.backend->invocations[0].completion(Results(ResultListing(fixture.host)));
    REQUIRE(RunMainLoopUntil([&] { return fixture.Snapshot().phase == SearchPhase::Completed; }));

    [fixture.controller synchronizeExternalContentChange];
    CHECK(fixture.controller.isPresented);
    REQUIRE([fixture.controller revealFocusedResult]);
    REQUIRE(fixture.panel->reveal);
    CHECK(fixture.panel->reveal->VFS == fixture.host);
    CHECK(fixture.panel->reveal->RequestedDirectory == "/first/");
    CHECK(fixture.panel->reveal->RequestFocusedEntry == "alpha.txt");
    CHECK(fixture.panel->reveal->InitiatedByUser);

    fixture.panel->ReplaceExternally(fixture.host);
    [fixture.controller synchronizeExternalContentChange];
    CHECK_FALSE(fixture.controller.isPresented);
}

TEST_CASE(PREFIX "teardown weakly fences callbacks and reaps the backend off main")
{
    REQUIRE([NSThread isMainThread]);
    auto host = std::make_shared<SearchControllerTestHost>();
    auto panel = std::make_shared<FakePanelAccess>(host);
    auto backend = std::make_shared<ControlledBackend>();
    __weak ExplorerSearchController *weak_controller;
    @autoreleasepool {
        ExplorerSearchController *controller = [[ExplorerSearchController alloc]
            initWithPanelAccess:panel
                         paneId:PaneId{91}
                backendProvider:[backend](SearchBackendKind) { return backend; }
                 snapshotHandler:[](std::optional<SearchSnapshot>) {}];
        weak_controller = controller;
        REQUIRE([controller presentWithPlanningFacts:Facts()]);
        REQUIRE([controller startSearch:Request()]);
        controller = nil;
    }
    REQUIRE(RunMainLoopUntil([&] { return weak_controller == nil; }));
    REQUIRE(backend->invocations.size() == 1);
    CHECK(backend->invocations[0].run->stopped.load());
    REQUIRE(RunMainLoopUntil([&] { return backend->invocations[0].run->waited.load(); }));
    backend->invocations[0].completion(Results(ResultListing(host)));
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.03, false);
    CHECK(panel->commit_count == 0);
}

#undef PREFIX
