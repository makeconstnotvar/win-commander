// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ExplorerSpotlightSearchBackend.h"

#include "SearchResultListingBuilder.h"

#include <Cocoa/Cocoa.h>
#include <WinCommander/Core/Search/SearchPlanning.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace nc::explorer {

namespace {

using Availability = ExplorerSpotlightQueryAvailability;
using Completion = ExplorerSearchBackendCompletion;
using CompletionKind = ExplorerSearchBackendCompletionKind;
using PredicateArgument = ExplorerSpotlightPredicateArgument;
using QueryCallbacks = ExplorerSpotlightMetadataQueryCallbacks;
using QueryPlan = ExplorerSpotlightQueryPlan;

constexpr std::string_view g_FSName = "kMDItemFSName";
constexpr std::string_view g_Path = "kMDItemPath";
constexpr std::string_view g_ContentTypeTree = "kMDItemContentTypeTree";
constexpr std::string_view g_Size = "kMDItemFSSize";
constexpr std::string_view g_Modified = "kMDItemFSContentChangeDate";
constexpr std::string_view g_TextContent = "kMDItemTextContent";
constexpr std::string_view g_Invisible = "kMDItemFSInvisible";

struct PredicateBuilder final {
    void Add(std::string _format, std::vector<PredicateArgument> _arguments)
    {
        formats.emplace_back(std::move(_format));
        arguments.insert(arguments.end(),
                         std::make_move_iterator(_arguments.begin()),
                         std::make_move_iterator(_arguments.end()));
    }

    [[nodiscard]] QueryPlan Build()
    {
        std::string format;
        for( size_t index = 0; index != formats.size(); ++index ) {
            if( index != 0 )
                format += " AND ";
            format += '(';
            format += formats[index];
            format += ')';
        }
        return {
            .predicate_format = std::move(format),
            .predicate_arguments = std::move(arguments),
            .scope = ExplorerSpotlightQueryScope::LocalComputer,
        };
    }

    std::vector<std::string> formats;
    std::vector<PredicateArgument> arguments;
};

std::string String(const std::string_view _value)
{
    return std::string{_value};
}

void AddFileTypePredicate(PredicateBuilder &_builder, const core::SearchFileType _type)
{
    using enum core::SearchFileType;
    switch( _type ) {
        case Any:
            return;
        case RegularFile:
            _builder.Add("NOT (ANY %K == %@) AND NOT (ANY %K == %@)",
                         {String(g_ContentTypeTree), std::string{"public.folder"},
                          String(g_ContentTypeTree), std::string{"public.symlink"}});
            return;
        case Directory:
            _builder.Add("ANY %K == %@ AND NOT (ANY %K == %@)",
                         {String(g_ContentTypeTree), std::string{"public.folder"},
                          String(g_ContentTypeTree), std::string{"com.apple.package"}});
            return;
        case SymbolicLink:
            _builder.Add("ANY %K == %@", {String(g_ContentTypeTree), std::string{"public.symlink"}});
            return;
        case Package:
            _builder.Add("ANY %K == %@", {String(g_ContentTypeTree), std::string{"com.apple.package"}});
            return;
        case Other:
            _builder.Add("NOT (ANY %K == %@) AND NOT (ANY %K == %@) AND NOT (ANY %K == %@)",
                         {String(g_ContentTypeTree), std::string{"public.folder"},
                          String(g_ContentTypeTree), std::string{"public.symlink"},
                          String(g_ContentTypeTree), std::string{"com.apple.package"}});
            return;
    }
}

NSArray *PredicateArguments(const std::vector<PredicateArgument> &_arguments)
{
    NSMutableArray *const values = [NSMutableArray arrayWithCapacity:_arguments.size()];
    for( const PredicateArgument &argument : _arguments ) {
        id value = std::visit(
            [](const auto &_value) -> id {
                using T = std::decay_t<decltype(_value)>;
                if constexpr( std::is_same_v<T, std::string> )
                    return [NSString stringWithUTF8String:_value.c_str()];
                else if constexpr( std::is_same_v<T, uint64_t> )
                    return [NSNumber numberWithUnsignedLongLong:_value];
                else if constexpr( std::is_same_v<T, bool> )
                    return [NSNumber numberWithBool:_value];
                else
                    return [NSDate dateWithTimeIntervalSince1970:static_cast<NSTimeInterval>(_value.seconds)];
            },
            argument);
        if( value == nil )
            return nil;
        [values addObject:value];
    }
    return values;
}

struct CallbackBox final {
    std::mutex mutex;
    bool active = true;
    QueryCallbacks callbacks;
};

template <class Function>
void WithCallbacks(const std::weak_ptr<CallbackBox> &_weak_box, Function _function)
{
    const std::shared_ptr<CallbackBox> box = _weak_box.lock();
    if( !box )
        return;
    QueryCallbacks callbacks;
    {
        const std::lock_guard lock{box->mutex};
        if( !box->active )
            return;
        callbacks = box->callbacks;
    }
    _function(callbacks);
}

std::optional<std::vector<std::string>> ExactPaths(NSMetadataQuery *_query)
{
    std::vector<std::string> paths;
    bool updates_disabled = false;
    @try {
        [_query disableUpdates];
        updates_disabled = true;
        NSArray *const results = _query.results;
        paths.reserve(results.count);
        for( id result in results ) {
            if( ![result isKindOfClass:NSMetadataItem.class] )
                continue;
            NSString *const path = [static_cast<NSMetadataItem *>(result) valueForAttribute:@"kMDItemPath"];
            if( ![path isKindOfClass:NSString.class] )
                continue;
            const char *const utf8 = path.UTF8String;
            if( utf8 != nullptr )
                paths.emplace_back(utf8);
        }
        [_query enableUpdates];
        return paths;
    }
    @catch( NSException * ) {
        if( updates_disabled ) {
            @try {
                [_query enableUpdates];
            }
            @catch( NSException * ) {
            }
        }
        return std::nullopt;
    }
}

class NativeMetadataQuery final : public ExplorerSpotlightMetadataQuery
{
public:
    ~NativeMetadataQuery() override { Stop(); }

    Availability Start(const QueryPlan &_plan, QueryCallbacks _callbacks) override
    {
        if( _plan.scope != ExplorerSpotlightQueryScope::LocalComputer || _plan.predicate_format.empty() )
            return Availability::SpotlightUnavailable;

        @autoreleasepool {
            @try {
                NSString *const format = [NSString stringWithUTF8String:_plan.predicate_format.c_str()];
                NSArray *const arguments = PredicateArguments(_plan.predicate_arguments);
                if( format == nil || arguments == nil )
                    return Availability::SpotlightUnavailable;
                NSPredicate *const predicate = [NSPredicate predicateWithFormat:format argumentArray:arguments];
                if( predicate == nil )
                    return Availability::SpotlightUnavailable;

                const std::lock_guard lock{m_Mutex};
                if( m_Stopped )
                    return Availability::SpotlightUnavailable;
                m_Query = [[NSMetadataQuery alloc] init];
                m_Query.predicate = predicate;
                m_Query.searchScopes = @[NSMetadataQueryLocalComputerScope];
                m_Query.notificationBatchingInterval = 0.1;
                m_Box = std::make_shared<CallbackBox>();
                m_Box->callbacks = std::move(_callbacks);

                const std::weak_ptr<CallbackBox> weak_box = m_Box;
                NSNotificationCenter *const center = NSNotificationCenter.defaultCenter;
                id observer = [center addObserverForName:NSMetadataQueryDidStartGatheringNotification
                                                  object:m_Query
                                                   queue:nil
                                              usingBlock:^(__unused NSNotification *notification) {
                                                WithCallbacks(weak_box, [](const QueryCallbacks &_value) {
                                                  if( _value.started )
                                                      _value.started();
                                                });
                                              }];
                [m_Observers addObject:observer];

                NSMetadataQuery *const query = m_Query;
                observer = [center addObserverForName:NSMetadataQueryDidUpdateNotification
                                               object:query
                                                queue:nil
                                           usingBlock:^(__unused NSNotification *notification) {
                                             const auto paths = ExactPaths(query);
                                             WithCallbacks(weak_box, [&](const QueryCallbacks &_value) {
                                               if( paths && _value.updated )
                                                   _value.updated(*paths);
                                               else if( !paths && _value.unavailable )
                                                   _value.unavailable(Availability::SpotlightUnavailable);
                                             });
                                           }];
                [m_Observers addObject:observer];

                observer = [center addObserverForName:NSMetadataQueryDidFinishGatheringNotification
                                               object:query
                                                queue:nil
                                           usingBlock:^(__unused NSNotification *notification) {
                                             const auto paths = ExactPaths(query);
                                             WithCallbacks(weak_box, [&](const QueryCallbacks &_value) {
                                               if( paths && _value.finished )
                                                   _value.finished(*paths);
                                               else if( !paths && _value.unavailable )
                                                   _value.unavailable(Availability::SpotlightUnavailable);
                                             });
                                           }];
                [m_Observers addObject:observer];

                return [m_Query startQuery] ? Availability::Started : Availability::IndexUnavailable;
            }
            @catch( NSException * ) {
                return Availability::SpotlightUnavailable;
            }
        }
    }

    void Stop() noexcept override
    {
        @autoreleasepool {
            std::shared_ptr<CallbackBox> box;
            NSMetadataQuery *query = nil;
            NSArray *observers = nil;
            {
                const std::lock_guard lock{m_Mutex};
                if( m_Stopped )
                    return;
                m_Stopped = true;
                box = std::move(m_Box);
                query = m_Query;
                observers = [m_Observers copy];
                [m_Observers removeAllObjects];
            }
            if( box ) {
                const std::lock_guard lock{box->mutex};
                box->active = false;
                box->callbacks = {};
            }
            @try {
                for( id observer in observers )
                    [NSNotificationCenter.defaultCenter removeObserver:observer];
                [query stopQuery];
            }
            @catch( NSException * ) {
            }
        }
    }

private:
    std::mutex m_Mutex;
    bool m_Stopped = false;
    __strong NSMetadataQuery *m_Query = nil;
    __strong NSMutableArray *m_Observers = [NSMutableArray array];
    std::shared_ptr<CallbackBox> m_Box;
};

bool HasLimitation(const core::SearchPlan &_plan, const core::SearchBackendLimitation _limitation)
{
    return std::ranges::find(_plan.backend.limitations, _limitation) != _plan.backend.limitations.end();
}

bool HasHiddenPathComponent(const std::string_view _path) noexcept
{
    size_t start = 0;
    while( start < _path.size() ) {
        while( start < _path.size() && _path[start] == '/' )
            ++start;
        const size_t end = _path.find('/', start);
        const std::string_view component = _path.substr(start, end == std::string_view::npos ? end : end - start);
        if( component.size() > 1 && component.front() == '.' && component != ".." )
            return true;
        if( end == std::string_view::npos )
            break;
        start = end + 1;
    }
    return false;
}

class SpotlightRun final : public ExplorerSearchBackendRun, public std::enable_shared_from_this<SpotlightRun>
{
public:
    SpotlightRun(ExplorerSearchBackendInput _input,
                 ExplorerSearchBackend::ProgressCallback _progress,
                 ExplorerSearchBackend::CompletionCallback _completion,
                 std::unique_ptr<ExplorerSpotlightMetadataQuery> _query)
        : m_Input(std::move(_input)), m_Progress(std::move(_progress)), m_Completion(std::move(_completion)),
          m_Query(std::move(_query))
    {
    }

    ~SpotlightRun() override
    {
        Stop();
        Wait();
    }

    void Begin(const QueryPlan &_plan)
    {
        if( _plan.predicate_format.empty() ) {
            OnUnavailable(Availability::SpotlightUnavailable);
            return;
        }
        const std::weak_ptr<SpotlightRun> weak_self = shared_from_this();
        QueryCallbacks callbacks{
            .started = [weak_self] {
                if( const auto self = weak_self.lock() )
                    self->OnStarted();
            },
            .updated = [weak_self](std::vector<std::string> _paths) {
                if( const auto self = weak_self.lock() )
                    self->OnPaths(std::move(_paths), false);
            },
            .finished = [weak_self](std::vector<std::string> _paths) {
                if( const auto self = weak_self.lock() )
                    self->OnPaths(std::move(_paths), true);
            },
            .unavailable = [weak_self](const Availability _availability) {
                if( const auto self = weak_self.lock() )
                    self->OnUnavailable(_availability);
            },
        };

        Availability availability = Availability::SpotlightUnavailable;
        try {
            availability = m_Query ? m_Query->Start(_plan, std::move(callbacks))
                                   : Availability::SpotlightUnavailable;
        } catch( ... ) {
            availability = Availability::SpotlightUnavailable;
        }
        if( availability != Availability::Started )
            OnUnavailable(availability);
    }

    void Stop() noexcept override
    {
        m_StopRequested.store(true, std::memory_order_relaxed);
        bool complete_now = false;
        {
            const std::lock_guard lock{m_Mutex};
            if( !m_Done && !m_Finishing ) {
                m_Finishing = true;
                complete_now = true;
            }
        }
        if( m_Query )
            m_Query->Stop();
        if( complete_now )
            Deliver({.kind = CompletionKind::Cancelled});
    }

    void Wait() noexcept override
    {
        {
            std::unique_lock lock{m_Mutex};
            m_Condition.wait(lock, [this] { return m_Done; });
        }
        if( m_Worker.joinable() ) {
            if( m_Worker.get_id() == std::this_thread::get_id() )
                m_Worker.detach();
            else
                m_Worker.join();
        }
    }

private:
    void OnStarted()
    {
        ExplorerSearchBackend::ProgressCallback progress;
        {
            const std::lock_guard lock{m_Mutex};
            if( m_Done || m_Finishing )
                return;
            progress = m_Progress;
        }
        if( progress )
            progress({.found_count = 0});
    }

    void OnPaths(std::vector<std::string> _paths, const bool _finished)
    {
        bool limit_reached = false;
        uint64_t found_count = 0;
        ExplorerSearchBackend::ProgressCallback progress;
        {
            const std::lock_guard lock{m_Mutex};
            if( m_Done || m_Finishing )
                return;
            std::unordered_set<std::string> seen;
            std::vector<std::string> current_paths;
            size_t current_path_bytes = 0;
            for( std::string &path : _paths ) {
                if( path.empty() || path.front() != '/' || path.find('\0') != std::string::npos )
                    continue;
                if( !m_Input.plan.request.filters.include_hidden && HasHiddenPathComponent(path) )
                    continue;
                if( !seen.emplace(path).second )
                    continue;
                if( current_paths.size() >= m_Input.limits.maximum_results ||
                    path.size() > m_Input.limits.maximum_path_bytes - current_path_bytes ) {
                    limit_reached = true;
                    break;
                }
                current_path_bytes += path.size();
                current_paths.emplace_back(std::move(path));
            }
            m_PathBytes = current_path_bytes;
            m_Paths = std::move(current_paths);
            m_MaxObservedFoundCount = std::max<uint64_t>(m_MaxObservedFoundCount, m_Paths.size());
            found_count = m_MaxObservedFoundCount;
            progress = m_Progress;
        }

        if( progress )
            progress({.found_count = found_count});
        if( limit_reached )
            Finish(CompletionKind::TooManyResults);
        else if( _finished )
            Finish(HasLimitation(m_Input.plan, core::SearchBackendLimitation::FullDiskAccessMissing)
                       ? CompletionKind::PermissionLimited
                       : CompletionKind::Completed);
    }

    void OnUnavailable(const Availability _availability)
    {
        if( _availability == Availability::Started )
            return;
        const CompletionKind kind = _availability == Availability::IndexUnavailable
                                      ? CompletionKind::IndexUnavailable
                                      : CompletionKind::BackendUnavailable;
        const core::SearchBackendLimitation limitation = _availability == Availability::IndexUnavailable
                                                           ? core::SearchBackendLimitation::SpotlightIndexUnavailable
                                                           : core::SearchBackendLimitation::SpotlightUnavailable;
        bool accepted = false;
        {
            const std::lock_guard lock{m_Mutex};
            if( !m_Done && !m_Finishing ) {
                m_Finishing = true;
                accepted = true;
            }
        }
        if( !accepted )
            return;
        if( m_Query )
            m_Query->Stop();
        Deliver({
            .kind = kind,
            .limitations = {limitation},
            .failure = core::SearchFailure{
                .code = core::SearchFailureCode::ExecutionFailed,
                .detail = _availability == Availability::IndexUnavailable ? "Spotlight index is unavailable"
                                                                           : "Spotlight service is unavailable",
            },
        });
    }

    void Finish(const CompletionKind _kind)
    {
        std::vector<std::string> paths;
        {
            const std::lock_guard lock{m_Mutex};
            if( m_Done || m_Finishing )
                return;
            m_Finishing = true;
            paths = m_Paths;
        }
        if( m_Query )
            m_Query->Stop();

        const std::shared_ptr<SpotlightRun> self = shared_from_this();
        m_Worker = std::thread([self, paths = std::move(paths), _kind] {
            self->BuildAndDeliver(std::move(paths), _kind);
        });
    }

    void BuildAndDeliver(std::vector<std::string> _paths, CompletionKind _kind)
    {
        if( m_StopRequested.load(std::memory_order_relaxed) ) {
            Deliver({.kind = CompletionKind::Cancelled});
            return;
        }
        if( !m_Input.origin_host ) {
            Deliver({
                .kind = CompletionKind::BackendUnavailable,
                .limitations = {core::SearchBackendLimitation::ProviderUnavailable},
                .failure = core::SearchFailure{
                    .code = core::SearchFailureCode::InvalidBackendReply,
                    .detail = "Spotlight result host is unavailable",
                },
            });
            return;
        }

        std::ranges::sort(_paths);
        std::vector<nc::vfs::VFSPath> vfs_paths;
        vfs_paths.reserve(_paths.size());
        for( std::string &path : _paths )
            vfs_paths.emplace_back(m_Input.origin_host, std::move(path));

        const SearchResultListingBuildResult result = BuildSearchResultListing(
            std::move(vfs_paths),
            {
                .fetch_flags = m_Input.fetch_flags,
                .maximum_results = m_Input.limits.maximum_results,
                .maximum_path_bytes = m_Input.limits.maximum_path_bytes,
                .title = "Spotlight Search Results",
            },
            [this] { return m_StopRequested.load(std::memory_order_relaxed); });

        if( result.status == SearchResultListingBuildStatus::Cancelled ) {
            Deliver({.kind = CompletionKind::Cancelled});
            return;
        }
        if( result.status == SearchResultListingBuildStatus::Failed ) {
            Deliver({
                .kind = CompletionKind::Failed,
                .failure = core::SearchFailure{
                    .code = core::SearchFailureCode::InvalidBackendReply,
                    .detail = "Spotlight results could not be materialized",
                },
            });
            return;
        }

        if( result.failed_count != 0 ) {
            std::string detail = result.first_failure ? result.first_failure->LocalizedFailureReason() : std::string{};
            if( detail.empty() && result.first_failure )
                detail = result.first_failure->Description();
            if( detail.empty() )
                detail = "Spotlight result materialization failed";
            Deliver({
                .kind = CompletionKind::Failed,
                .failure = core::SearchFailure{
                    .code = core::SearchFailureCode::ExecutionFailed,
                    .detail = std::move(detail),
                },
            });
            return;
        }
        std::vector<core::SearchBackendLimitation> limitations;
        if( result.permission_denied_count != 0 ) {
            limitations.emplace_back(core::SearchBackendLimitation::PermissionDeniedLocations);
            if( _kind == CompletionKind::Completed || _kind == CompletionKind::Partial )
                _kind = CompletionKind::PermissionLimited;
        }
        if( result.missing_count != 0 ) {
            limitations.emplace_back(core::SearchBackendLimitation::ResultPathsUnavailable);
            if( _kind == CompletionKind::Completed )
                _kind = CompletionKind::Partial;
        }
        Deliver({
            .kind = _kind,
            .listing = result.listing,
            .accepted_count = result.accepted_count,
            .limitations = std::move(limitations),
        });
    }

    void Deliver(Completion _completion)
    {
        ExplorerSearchBackend::CompletionCallback callback;
        {
            const std::lock_guard lock{m_Mutex};
            if( m_Done || m_Delivering )
                return;
            m_Delivering = true;
            callback = std::move(m_Completion);
            m_Progress = {};
        }
        if( callback )
            callback(std::move(_completion));
        {
            const std::lock_guard lock{m_Mutex};
            m_Delivering = false;
            m_Done = true;
        }
        m_Condition.notify_all();
    }

    ExplorerSearchBackendInput m_Input;
    ExplorerSearchBackend::ProgressCallback m_Progress;
    ExplorerSearchBackend::CompletionCallback m_Completion;
    std::unique_ptr<ExplorerSpotlightMetadataQuery> m_Query;
    std::mutex m_Mutex;
    std::condition_variable m_Condition;
    std::atomic_bool m_StopRequested = false;
    bool m_Finishing = false;
    bool m_Delivering = false;
    bool m_Done = false;
    size_t m_PathBytes = 0;
    uint64_t m_MaxObservedFoundCount = 0;
    std::vector<std::string> m_Paths;
    std::thread m_Worker;
};

} // namespace

std::expected<QueryPlan, ExplorerSpotlightQueryPlanFailure>
ExplorerSpotlightSearchBackend::BuildQueryPlan(const core::SearchPlan &_plan)
{
    if( !core::SearchPlanning::IsValid(_plan) )
        return std::unexpected(ExplorerSpotlightQueryPlanFailure::InvalidPlan);
    if( _plan.backend.kind != core::SearchBackendKind::Spotlight ||
        _plan.request.scope != core::SearchScope::SpotlightWholeMac )
        return std::unexpected(ExplorerSpotlightQueryPlanFailure::WrongBackend);
    if( _plan.backend.support != core::SearchBackendSupport::Supported )
        return std::unexpected(ExplorerSpotlightQueryPlanFailure::InvalidPlan);

    PredicateBuilder builder;
    builder.Add("%K BEGINSWITH %@", {String(g_Path), std::string{"/"}});
    if( !_plan.request.query.empty() ) {
        builder.Add(_plan.request.filters.name_match == core::SearchNameMatch::Exact ? "%K ==[cd] %@"
                                                                                    : "%K CONTAINS[cd] %@",
                    {String(g_FSName), _plan.request.query});
    }
    if( _plan.request.filters.extension ) {
        builder.Add("%K ENDSWITH[cd] %@",
                    {String(g_FSName), std::string{"."} + *_plan.request.filters.extension});
    }
    AddFileTypePredicate(builder, _plan.request.filters.file_type);
    if( _plan.request.filters.size.minimum_bytes )
        builder.Add("%K >= %@", {String(g_Size), *_plan.request.filters.size.minimum_bytes});
    if( _plan.request.filters.size.maximum_bytes )
        builder.Add("%K <= %@", {String(g_Size), *_plan.request.filters.size.maximum_bytes});
    if( _plan.request.filters.modified.earliest_seconds )
        builder.Add("%K >= %@",
                    {String(g_Modified), ExplorerSpotlightUnixTime{*_plan.request.filters.modified.earliest_seconds}});
    if( _plan.request.filters.modified.latest_seconds )
        builder.Add("%K <= %@",
                    {String(g_Modified), ExplorerSpotlightUnixTime{*_plan.request.filters.modified.latest_seconds}});
    if( _plan.request.filters.content )
        builder.Add("%K CONTAINS[cd] %@", {String(g_TextContent), *_plan.request.filters.content});
    if( !_plan.request.filters.include_hidden ) {
        builder.Add("(%K == NIL OR %K == %@) AND NOT (%K BEGINSWITH %@)",
                    {String(g_Invisible), String(g_Invisible), false, String(g_FSName), std::string{"."}});
    }
    return builder.Build();
}

ExplorerSpotlightSearchBackend::ExplorerSpotlightSearchBackend()
    : ExplorerSpotlightSearchBackend([] { return std::make_unique<NativeMetadataQuery>(); })
{
}

ExplorerSpotlightSearchBackend::ExplorerSpotlightSearchBackend(MetadataQueryFactory _query_factory)
    : m_QueryFactory(std::move(_query_factory))
{
}

std::shared_ptr<ExplorerSearchBackendRun>
ExplorerSpotlightSearchBackend::Start(ExplorerSearchBackendInput _input,
                                      ProgressCallback _progress,
                                      CompletionCallback _completion)
{
    const auto query_plan = BuildQueryPlan(_input.plan);
    std::unique_ptr<ExplorerSpotlightMetadataQuery> query;
    try {
        if( m_QueryFactory )
            query = m_QueryFactory();
    } catch( ... ) {
    }
    const auto run = std::make_shared<SpotlightRun>(
        std::move(_input), std::move(_progress), std::move(_completion), std::move(query));
    if( !query_plan ) {
        run->Begin({});
        return run;
    }
    run->Begin(*query_plan);
    return run;
}

} // namespace nc::explorer
