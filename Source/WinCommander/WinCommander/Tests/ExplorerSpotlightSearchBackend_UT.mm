// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/Core/Search/SearchPlanning.h>
#include <WinCommander/States/Explorer/ExplorerSpotlightSearchBackend.h>

#include <atomic>
#include <mutex>
#include <set>
#include <sys/dirent.h>
#include <sys/stat.h>

namespace {

using namespace nc::core;
using namespace nc::explorer;

struct FakeMetadataQueryState final {
    ExplorerSpotlightQueryAvailability start_status = ExplorerSpotlightQueryAvailability::Started;
    std::optional<ExplorerSpotlightQueryPlan> plan;
    ExplorerSpotlightMetadataQueryCallbacks callbacks;
    std::atomic_size_t stop_count = 0;
    std::atomic_size_t destruction_count = 0;
    std::mutex mutex;

    void EmitStarted()
    {
        const auto callback = CopyCallbacks().started;
        if( callback )
            callback();
    }

    void EmitUpdated(std::vector<std::string> _paths)
    {
        const auto callback = CopyCallbacks().updated;
        if( callback )
            callback(std::move(_paths));
    }

    void EmitFinished(std::vector<std::string> _paths)
    {
        const auto callback = CopyCallbacks().finished;
        if( callback )
            callback(std::move(_paths));
    }

    void EmitUnavailable(const ExplorerSpotlightQueryAvailability _availability)
    {
        const auto callback = CopyCallbacks().unavailable;
        if( callback )
            callback(_availability);
    }

private:
    ExplorerSpotlightMetadataQueryCallbacks CopyCallbacks()
    {
        const std::lock_guard lock{mutex};
        return callbacks;
    }
};

class FakeMetadataQuery final : public ExplorerSpotlightMetadataQuery
{
public:
    explicit FakeMetadataQuery(std::shared_ptr<FakeMetadataQueryState> _state) : m_State(std::move(_state)) {}
    ~FakeMetadataQuery() override { ++m_State->destruction_count; }

    ExplorerSpotlightQueryAvailability
    Start(const ExplorerSpotlightQueryPlan &_plan, ExplorerSpotlightMetadataQueryCallbacks _callbacks) override
    {
        const std::lock_guard lock{m_State->mutex};
        m_State->plan = _plan;
        m_State->callbacks = std::move(_callbacks);
        return m_State->start_status;
    }

    void Stop() noexcept override { ++m_State->stop_count; }

private:
    std::shared_ptr<FakeMetadataQueryState> m_State;
};

class SpotlightResultTestHost final : public nc::vfs::Host
{
public:
    SpotlightResultTestHost() : Host("/", nullptr, "spotlight-result-tests") {}

    std::expected<VFSListingPtr, nc::Error>
    FetchSingleItemListing(std::string_view _path,
                           unsigned long,
                           [[maybe_unused]] const VFSCancelChecker &_cancel_checker) override
    {
        {
            const std::lock_guard lock{m_Mutex};
            m_Fetched.emplace_back(_path);
        }
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

    std::vector<std::string> Fetched() const
    {
        const std::lock_guard lock{m_Mutex};
        return m_Fetched;
    }

private:
    mutable std::mutex m_Mutex;
    std::vector<std::string> m_Fetched;
};

SearchPlan SpotlightPlan(SearchRequest _request)
{
    SearchPlanningFacts facts{
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
        .spotlight_available = true,
        .spotlight_index_available = true,
        .spotlight_supports_content = true,
        .full_disk_access = true,
    };
    const auto plan = SearchPlanning::Plan(std::move(_request), std::move(facts));
    REQUIRE(plan);
    return *plan;
}

SearchPlan SpotlightPlan()
{
    SearchRequest request;
    request.query = "report";
    request.scope = SearchScope::SpotlightWholeMac;
    return SpotlightPlan(std::move(request));
}

ExplorerSearchBackendInput Input(std::shared_ptr<SpotlightResultTestHost> _host)
{
    return {
        .plan = SpotlightPlan(),
        .origin_host = std::move(_host),
        .fetch_flags = 0,
    };
}

ExplorerSpotlightSearchBackend Backend(const std::shared_ptr<FakeMetadataQueryState> &_state)
{
    return ExplorerSpotlightSearchBackend{[_state] { return std::make_unique<FakeMetadataQuery>(_state); }};
}

std::vector<std::string> StringArguments(const ExplorerSpotlightQueryPlan &_plan)
{
    std::vector<std::string> strings;
    for( const ExplorerSpotlightPredicateArgument &argument : _plan.predicate_arguments ) {
        if( const auto value = std::get_if<std::string>(&argument) )
            strings.emplace_back(*value);
    }
    return strings;
}

} // namespace

#define PREFIX "nc::explorer::ExplorerSpotlightSearchBackend "

TEST_CASE(PREFIX "builds one parameterized local-computer predicate for every supported filter")
{
    SearchRequest request;
    request.query = "annual \"report\"";
    request.scope = SearchScope::SpotlightWholeMac;
    request.filters.name_match = SearchNameMatch::Exact;
    request.filters.extension = "pdf";
    request.filters.file_type = SearchFileType::RegularFile;
    request.filters.size = {.minimum_bytes = 20, .maximum_bytes = 500};
    request.filters.modified = {.earliest_seconds = 100, .latest_seconds = 200};
    request.filters.content = "gross margin";

    const auto query = ExplorerSpotlightSearchBackend::BuildQueryPlan(SpotlightPlan(request));
    REQUIRE(query);
    CHECK(query->scope == ExplorerSpotlightQueryScope::LocalComputer);
    CHECK(query->predicate_format.find("==[cd] %@") != std::string::npos);
    CHECK(query->predicate_format.find("ENDSWITH[cd]") != std::string::npos);
    CHECK(query->predicate_format.find("kMDItem") == std::string::npos);
    CHECK(query->predicate_format.find(request.query) == std::string::npos);
    CHECK(query->predicate_format.find(*request.filters.content) == std::string::npos);

    const std::vector<std::string> strings = StringArguments(*query);
    CHECK(std::ranges::find(strings, request.query) != strings.end());
    CHECK(std::ranges::find(strings, ".pdf") != strings.end());
    CHECK(std::ranges::find(strings, *request.filters.content) != strings.end());
    CHECK(std::ranges::find(strings, "kMDItemFSInvisible") != strings.end());

    request.filters.name_match = SearchNameMatch::Contains;
    request.filters.include_hidden = true;
    const auto contains = ExplorerSpotlightSearchBackend::BuildQueryPlan(SpotlightPlan(request));
    REQUIRE(contains);
    CHECK(contains->predicate_format.find("CONTAINS[cd]") != std::string::npos);
    const std::vector<std::string> contains_strings = StringArguments(*contains);
    CHECK(std::ranges::find(contains_strings, "kMDItemFSInvisible") == contains_strings.end());
}

TEST_CASE(PREFIX "publishes incremental counts then materializes only the final exact path snapshot")
{
    auto state = std::make_shared<FakeMetadataQueryState>();
    auto host = std::make_shared<SpotlightResultTestHost>();
    auto backend = Backend(state);
    std::vector<uint64_t> found;
    std::optional<ExplorerSearchBackendCompletion> completion;
    const auto run = backend.Start(
        Input(host),
        [&](const ExplorerSearchBackendProgress _progress) {
            if( _progress.found_count )
                found.emplace_back(*_progress.found_count);
        },
        [&](ExplorerSearchBackendCompletion _completion) { completion = std::move(_completion); });

    state->EmitStarted();
    state->EmitUpdated({"/zeta.txt", "/alpha.txt", "/removed.txt", "/alpha.txt", "relative.txt"});
    state->EmitFinished({"/alpha.txt", "/bravo.txt", "/zeta.txt", "/.hidden/private.txt"});
    run->Wait();

    REQUIRE(completion);
    CHECK(completion->kind == ExplorerSearchBackendCompletionKind::Completed);
    CHECK(completion->accepted_count == 3);
    REQUIRE(completion->listing);
    CHECK(completion->listing->Count() == 3);
    CHECK(found == std::vector<uint64_t>{0, 3, 3});
    CHECK(host->Fetched() == std::vector<std::string>{"/alpha.txt", "/bravo.txt", "/zeta.txt"});
    REQUIRE(state->plan);
    CHECK(state->plan->scope == ExplorerSpotlightQueryScope::LocalComputer);
}

TEST_CASE(PREFIX "stops gathering at the exact count or path-byte cap and returns bounded results")
{
    SECTION("result count")
    {
        auto state = std::make_shared<FakeMetadataQueryState>();
        auto host = std::make_shared<SpotlightResultTestHost>();
        auto input = Input(host);
        input.limits.maximum_results = 2;
        auto backend = Backend(state);
        std::optional<ExplorerSearchBackendCompletion> completion;
        const auto run = backend.Start(std::move(input), {}, [&](auto _value) { completion = std::move(_value); });

        state->EmitUpdated({"/a", "/b", "/c"});
        run->Wait();

        REQUIRE(completion);
        CHECK(completion->kind == ExplorerSearchBackendCompletionKind::TooManyResults);
        CHECK(completion->accepted_count == 2);
        CHECK(state->stop_count > 0);
    }

    SECTION("path bytes")
    {
        auto state = std::make_shared<FakeMetadataQueryState>();
        auto host = std::make_shared<SpotlightResultTestHost>();
        auto input = Input(host);
        input.limits.maximum_path_bytes = 2;
        auto backend = Backend(state);
        std::optional<ExplorerSearchBackendCompletion> completion;
        const auto run = backend.Start(std::move(input), {}, [&](auto _value) { completion = std::move(_value); });

        state->EmitUpdated({"/a", "/long"});
        run->Wait();

        REQUIRE(completion);
        CHECK(completion->kind == ExplorerSearchBackendCompletionKind::TooManyResults);
        CHECK(completion->accepted_count == 1);
        REQUIRE(completion->listing);
        CHECK(completion->listing->Path(0) == "/a");
    }
}

TEST_CASE(PREFIX "maps metadata query start and runtime failures to typed availability")
{
    SECTION("index start failure")
    {
        auto state = std::make_shared<FakeMetadataQueryState>();
        state->start_status = ExplorerSpotlightQueryAvailability::IndexUnavailable;
        auto backend = Backend(state);
        std::optional<ExplorerSearchBackendCompletion> completion;
        const auto run = backend.Start(Input(std::make_shared<SpotlightResultTestHost>()),
                                       {},
                                       [&](auto _value) { completion = std::move(_value); });
        run->Wait();

        REQUIRE(completion);
        CHECK(completion->kind == ExplorerSearchBackendCompletionKind::IndexUnavailable);
        CHECK(completion->limitations ==
              std::vector<SearchBackendLimitation>{SearchBackendLimitation::SpotlightIndexUnavailable});
    }

    SECTION("runtime service failure")
    {
        auto state = std::make_shared<FakeMetadataQueryState>();
        auto backend = Backend(state);
        std::optional<ExplorerSearchBackendCompletion> completion;
        const auto run = backend.Start(Input(std::make_shared<SpotlightResultTestHost>()),
                                       {},
                                       [&](auto _value) { completion = std::move(_value); });
        state->EmitUnavailable(ExplorerSpotlightQueryAvailability::SpotlightUnavailable);
        run->Wait();

        REQUIRE(completion);
        CHECK(completion->kind == ExplorerSearchBackendCompletionKind::BackendUnavailable);
        CHECK(completion->limitations ==
              std::vector<SearchBackendLimitation>{SearchBackendLimitation::SpotlightUnavailable});
    }
}

TEST_CASE(PREFIX "cancellation fences late notifications and releases callback ownership")
{
    auto state = std::make_shared<FakeMetadataQueryState>();
    auto backend = Backend(state);
    std::atomic_size_t completion_count = 0;
    std::optional<ExplorerSearchBackendCompletionKind> kind;
    auto owner = std::make_shared<int>(42);
    const std::weak_ptr<int> weak_owner = owner;
    auto run = backend.Start(
        Input(std::make_shared<SpotlightResultTestHost>()),
        [owner](ExplorerSearchBackendProgress) {},
        [owner, &completion_count, &kind](ExplorerSearchBackendCompletion _completion) {
            ++completion_count;
            kind = _completion.kind;
        });
    owner.reset();

    run->Stop();
    run->Wait();
    CHECK(kind == ExplorerSearchBackendCompletionKind::Cancelled);
    CHECK(completion_count == 1);
    CHECK(weak_owner.expired());
    CHECK(state->stop_count > 0);

    state->EmitUpdated({"/late.txt"});
    state->EmitFinished({"/late.txt"});
    state->EmitUnavailable(ExplorerSpotlightQueryAvailability::IndexUnavailable);
    CHECK(completion_count == 1);

    const std::weak_ptr<ExplorerSearchBackendRun> weak_run = run;
    run.reset();
    CHECK(weak_run.expired());
    CHECK(state->destruction_count == 1);
}

#undef PREFIX
