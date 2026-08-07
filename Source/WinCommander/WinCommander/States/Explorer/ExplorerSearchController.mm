// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ExplorerSearchController.h"

#include <WinCommander/Core/Search/SearchStore.h>
#include <WinCommander/States/FilePanels/PanelController.h>
#include <WinCommander/States/FilePanels/PanelView.h>

#include <Panel/PanelData.h>

#include <algorithm>
#include <dispatch/dispatch.h>
#include <ranges>
#include <string>
#include <utility>

namespace nc::explorer {

namespace {

class PanelControllerSearchAccess final : public ExplorerSearchPanelAccess
{
public:
    explicit PanelControllerSearchAccess(PanelController *_panel) : m_Panel(_panel) {}

    [[nodiscard]] std::optional<ExplorerSearchPanelContent> Capture() const override
    {
        PanelController *const panel = m_Panel;
        if( !panel )
            return std::nullopt;
        try {
            const panel::data::Model &data = panel.data;
            if( !data.IsLoaded() || !data.ListingPtr() )
                return std::nullopt;
            const VFSListingPtr listing = data.ListingPtr();
            ExplorerSearchPanelContent content{
                .pane_id = panel.paneId,
                .listing = listing,
                .data_generation = panel.dataGeneration,
                .focused_item = panel.view.item,
            };
            if( listing->IsUniform() ) {
                content.uniform_host = listing->Host();
                content.uniform_directory = listing->Directory();
            }
            return content;
        } catch( ... ) {
            return std::nullopt;
        }
    }

    void CommitListing(const VFSListingPtr &_listing) override
    {
        if( PanelController *const panel = m_Panel; panel && _listing )
            [panel loadListing:_listing];
    }

    [[nodiscard]] bool SubmitReveal(std::shared_ptr<panel::DirectoryChangeRequest> _request) override
    {
        PanelController *const panel = m_Panel;
        return panel && _request && static_cast<bool>([panel GoToDirWithContext:std::move(_request)]);
    }

private:
    __weak PanelController *m_Panel;
};

[[nodiscard]] bool SameContent(const ExplorerSearchPanelContent &_lhs,
                               const ExplorerSearchPanelContent &_rhs) noexcept
{
    return _lhs.pane_id == _rhs.pane_id && _lhs.data_generation == _rhs.data_generation &&
           _lhs.listing == _rhs.listing;
}

[[nodiscard]] bool IsActive(const core::SearchPhase _phase) noexcept
{
    return _phase == core::SearchPhase::Preparing || _phase == core::SearchPhase::Running;
}

[[nodiscard]] bool HasLimitation(const core::SearchBackendDescriptor &_backend,
                                 const core::SearchBackendLimitation _limitation) noexcept
{
    return std::ranges::find(_backend.limitations, _limitation) != _backend.limitations.end();
}

[[nodiscard]] bool HasLimitation(const ExplorerSearchBackendCompletion &_completion,
                                 const core::SearchBackendLimitation _limitation) noexcept
{
    return std::ranges::find(_completion.limitations, _limitation) != _completion.limitations.end();
}

[[nodiscard]] bool HasDuplicateLimitations(const ExplorerSearchBackendCompletion &_completion) noexcept
{
    for( size_t index = 0; index != _completion.limitations.size(); ++index ) {
        if( std::ranges::find(_completion.limitations.begin() + static_cast<std::ptrdiff_t>(index + 1),
                             _completion.limitations.end(),
                             _completion.limitations[index]) != _completion.limitations.end() )
            return true;
    }
    return false;
}

[[nodiscard]] bool HasOnlyRuntimeResultLimitations(const ExplorerSearchBackendCompletion &_completion) noexcept
{
    return std::ranges::all_of(_completion.limitations, [](const core::SearchBackendLimitation _limitation) {
        return _limitation == core::SearchBackendLimitation::PermissionDeniedLocations ||
               _limitation == core::SearchBackendLimitation::ResultPathsUnavailable;
    });
}

[[nodiscard]] bool IsValidResultTerminal(const ExplorerSearchBackendCompletion &_completion,
                                         const core::SearchPlan &_plan) noexcept
{
    if( _completion.failure || !_completion.listing || HasDuplicateLimitations(_completion) ||
        !HasOnlyRuntimeResultLimitations(_completion) )
        return false;

    const bool permission = HasLimitation(_completion, core::SearchBackendLimitation::PermissionDeniedLocations);
    const bool missing = HasLimitation(_completion, core::SearchBackendLimitation::ResultPathsUnavailable);
    const bool planned_permission = HasLimitation(_plan.backend, core::SearchBackendLimitation::FullDiskAccessMissing);
    switch( _completion.kind ) {
        case ExplorerSearchBackendCompletionKind::Completed:
            return !permission && !missing && !planned_permission;
        case ExplorerSearchBackendCompletionKind::Partial:
            return !permission && missing && !planned_permission;
        case ExplorerSearchBackendCompletionKind::PermissionLimited:
            return permission || planned_permission;
        case ExplorerSearchBackendCompletionKind::TooManyResults:
            return true;
        case ExplorerSearchBackendCompletionKind::IndexUnavailable:
        case ExplorerSearchBackendCompletionKind::BackendUnavailable:
        case ExplorerSearchBackendCompletionKind::Cancelled:
        case ExplorerSearchBackendCompletionKind::Failed:
            return false;
    }
    return false;
}

[[nodiscard]] bool IsValidAvailabilityTerminal(const ExplorerSearchBackendCompletion &_completion,
                                               const core::SearchPlan &_plan) noexcept
{
    if( _completion.listing || _completion.accepted_count != 0 || HasDuplicateLimitations(_completion) ||
        (_completion.failure && _completion.failure->detail.empty()) )
        return false;

    core::SearchBackendLimitation required;
    switch( _completion.kind ) {
        case ExplorerSearchBackendCompletionKind::IndexUnavailable:
            if( _plan.backend.kind != core::SearchBackendKind::Spotlight )
                return false;
            required = core::SearchBackendLimitation::SpotlightIndexUnavailable;
            break;
        case ExplorerSearchBackendCompletionKind::BackendUnavailable:
            required = _plan.backend.kind == core::SearchBackendKind::Spotlight
                           ? core::SearchBackendLimitation::SpotlightUnavailable
                           : core::SearchBackendLimitation::ProviderUnavailable;
            break;
        case ExplorerSearchBackendCompletionKind::Completed:
        case ExplorerSearchBackendCompletionKind::Partial:
        case ExplorerSearchBackendCompletionKind::PermissionLimited:
        case ExplorerSearchBackendCompletionKind::TooManyResults:
        case ExplorerSearchBackendCompletionKind::Cancelled:
        case ExplorerSearchBackendCompletionKind::Failed:
            return false;
    }
    return _completion.limitations.size() == 1 && _completion.limitations.front() == required;
}

void Reap(std::shared_ptr<ExplorerSearchBackendRun> _run)
{
    if( !_run )
        return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      _run->Wait();
    });
}

struct WeakSearchControllerRef final {
    __weak ExplorerSearchController *controller;
};

} // namespace

} // namespace nc::explorer

using nc::core::SearchBackendKind;
using nc::core::SearchBackendLimitation;
using nc::core::SearchBackendSupport;
using nc::core::SearchCompletionKind;
using nc::core::SearchFailure;
using nc::core::SearchFailureCode;
using nc::core::SearchPhase;
using nc::core::SearchPlan;
using nc::core::SearchPlanning;
using nc::core::SearchPlanningFacts;
using nc::core::SearchRequest;
using nc::core::SearchResultReference;
using nc::core::SearchRunId;
using nc::core::SearchSnapshot;
using nc::core::SearchStore;
using nc::explorer::ExplorerSearchBackend;
using nc::explorer::ExplorerSearchBackendCompletion;
using nc::explorer::ExplorerSearchBackendCompletionKind;
using nc::explorer::ExplorerSearchBackendInput;
using nc::explorer::ExplorerSearchBackendProgress;
using nc::explorer::ExplorerSearchBackendProvider;
using nc::explorer::ExplorerSearchBackendRun;
using nc::explorer::ExplorerSearchPanelAccess;
using nc::explorer::ExplorerSearchPanelContent;
using nc::explorer::ExplorerSearchSnapshotHandler;
using nc::explorer::HasLimitation;
using nc::explorer::IsActive;
using nc::explorer::IsValidAvailabilityTerminal;
using nc::explorer::IsValidResultTerminal;
using nc::explorer::PanelControllerSearchAccess;
using nc::explorer::Reap;
using nc::explorer::SameContent;
using nc::explorer::WeakSearchControllerRef;

@implementation ExplorerSearchController {
    nc::core::PaneId m_PaneId;
    std::shared_ptr<ExplorerSearchPanelAccess> m_PanelAccess;
    ExplorerSearchBackendProvider m_BackendProvider;
    ExplorerSearchSnapshotHandler m_SnapshotHandler;
    std::optional<SearchStore> m_Store;
    bool m_Presented;
    bool m_CommittingListing;
    std::optional<SearchPlanningFacts> m_PlanningFacts;
    std::optional<SearchRequest> m_Draft;
    std::optional<ExplorerSearchPanelContent> m_Origin;
    std::optional<ExplorerSearchPanelContent> m_ExpectedVisibleContent;
    std::optional<ExplorerSearchPanelContent> m_ActiveContent;
    std::optional<SearchPlan> m_ActivePlan;
    std::optional<SearchRunId> m_ActiveRunId;
    std::shared_ptr<ExplorerSearchBackendRun> m_BackendRun;
    VFSListingPtr m_LastResultListing;
    uint64_t m_ResultGeneration;
}

- (instancetype)initWithPanel:(PanelController *)_panel
                       paneId:(nc::core::PaneId)_pane_id
              backendProvider:(ExplorerSearchBackendProvider)_backend_provider
               snapshotHandler:(ExplorerSearchSnapshotHandler)_snapshot_handler
{
    if( !_panel )
        return nil;
    return [self initWithPanelAccess:std::make_shared<PanelControllerSearchAccess>(_panel)
                              paneId:_pane_id
                     backendProvider:std::move(_backend_provider)
                      snapshotHandler:std::move(_snapshot_handler)];
}

- (instancetype)initWithPanelAccess:(std::shared_ptr<ExplorerSearchPanelAccess>)_panel_access
                             paneId:(nc::core::PaneId)_pane_id
                    backendProvider:(ExplorerSearchBackendProvider)_backend_provider
                     snapshotHandler:(ExplorerSearchSnapshotHandler)_snapshot_handler
{
    self = [super init];
    if( !self )
        return nil;
    auto store = SearchStore::Create(_pane_id);
    if( !_panel_access || !_backend_provider || !_snapshot_handler || !store )
        return nil;
    m_PaneId = _pane_id;
    m_PanelAccess = std::move(_panel_access);
    m_BackendProvider = std::move(_backend_provider);
    m_SnapshotHandler = std::move(_snapshot_handler);
    m_Store.emplace(std::move(*store));
    return self;
}

- (BOOL)presentWithPlanningFacts:(SearchPlanningFacts)_facts
{
    SearchRequest request;
    request.scope = nc::core::SearchScope::CurrentFolder;
    return [self presentWithPlanningFacts:std::move(_facts) initialRequest:std::move(request)];
}

- (BOOL)presentWithPlanningFacts:(SearchPlanningFacts)_facts initialRequest:(SearchRequest)_initial_request
{
    dispatch_assert_queue(dispatch_get_main_queue());
    const std::optional<ExplorerSearchPanelContent> content = m_PanelAccess->Capture();
    if( !content || content->pane_id != m_PaneId || !content->listing || !content->uniform_host ||
        content->uniform_directory.empty() )
        return NO;

    [self stopActiveRunPublishingCancellation:false];
    m_Store->Reset();
    _facts.current_folder = content->uniform_directory;
    m_PlanningFacts = std::move(_facts);
    m_Draft = std::move(_initial_request);
    m_Origin = content;
    m_ExpectedVisibleContent = content;
    m_ActiveContent.reset();
    m_ActivePlan.reset();
    m_ActiveRunId.reset();
    m_LastResultListing.reset();
    m_Presented = true;
    [self publishSnapshot];
    return YES;
}

- (BOOL)startSearch:(SearchRequest)_request
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( !m_Presented || !m_Origin || !m_PlanningFacts || !m_ExpectedVisibleContent )
        return NO;
    const std::optional<ExplorerSearchPanelContent> current = m_PanelAccess->Capture();
    if( !current || current->pane_id != m_PaneId || !SameContent(*current, *m_ExpectedVisibleContent) ) {
        [self invalidateForExternalContentChange];
        return NO;
    }

    SearchPlanning::Result planned = SearchPlanning::Plan(std::move(_request), *m_PlanningFacts);
    if( !planned )
        return NO;
    SearchPlan plan = std::move(*planned);
    m_Draft = plan.request;

    [self stopActiveRunPublishingCancellation:false];
    std::shared_ptr<ExplorerSearchBackend> backend;
    if( plan.backend.support == SearchBackendSupport::Supported ) {
        try {
            backend = m_BackendProvider(plan.backend.kind);
        } catch( ... ) {
            backend.reset();
        }
    }
    if( plan.backend.support == SearchBackendSupport::Supported && !backend ) {
        plan.backend.support = SearchBackendSupport::Unavailable;
        const SearchBackendLimitation limitation = plan.backend.kind == SearchBackendKind::Spotlight
                                                         ? SearchBackendLimitation::SpotlightUnavailable
                                                         : SearchBackendLimitation::ProviderUnavailable;
        if( !HasLimitation(plan.backend, limitation) )
            plan.backend.limitations.emplace_back(limitation);
    }

    const SearchStore::StartResult started = m_Store->Start(plan);
    if( !started )
        return NO;
    m_ActivePlan = plan;
    m_ActiveRunId = *started;
    m_ActiveContent = current;
    [self publishSnapshot];

    if( plan.backend.support != SearchBackendSupport::Supported ) {
        m_ActiveRunId.reset();
        m_ActiveContent.reset();
        return YES;
    }
    if( !m_Store->MarkRunning(*started) )
        return NO;

    const auto weak_controller =
        std::make_shared<WeakSearchControllerRef>(WeakSearchControllerRef{.controller = self});
    const SearchRunId run_id = *started;
    ExplorerSearchBackendInput input{
        .plan = plan,
        .origin_host = m_Origin->uniform_host,
    };
    try {
        m_BackendRun = backend->Start(
            std::move(input),
            [weak_controller, run_id](ExplorerSearchBackendProgress _progress) {
                dispatch_async(dispatch_get_main_queue(), ^{
                  ExplorerSearchController *const controller = weak_controller->controller;
                  [controller acceptProgress:std::move(_progress) forRun:run_id];
                });
            },
            [weak_controller, run_id](ExplorerSearchBackendCompletion _completion) {
                dispatch_async(dispatch_get_main_queue(), ^{
                  ExplorerSearchController *const controller = weak_controller->controller;
                  [controller acceptCompletion:std::move(_completion) forRun:run_id];
                });
            });
    } catch( ... ) {
        m_BackendRun.reset();
    }
    if( !m_BackendRun ) {
        static_cast<void>(m_Store->Fail(*started,
                                        SearchFailure{.code = SearchFailureCode::ExecutionFailed,
                                                      .detail = "Search backend refused execution"}));
        m_ActiveRunId.reset();
        m_ActiveContent.reset();
        [self publishSnapshot];
        return NO;
    }
    [self publishSnapshot];
    return YES;
}

- (BOOL)startPresentedDraft
{
    dispatch_assert_queue(dispatch_get_main_queue());
    return m_Draft ? [self startSearch:*m_Draft] : NO;
}

- (void)acceptProgress:(ExplorerSearchBackendProgress)_progress forRun:(SearchRunId)_run_id
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( ![self validateCallbackForRun:_run_id] )
        return;
    nc::core::SearchProgressUpdate update{
        .determinate_progress = std::move(_progress.determinate_progress),
        .current_location = std::move(_progress.current_location),
        .scanned_count = std::move(_progress.scanned_count),
        .found_count = std::move(_progress.found_count),
    };
    if( m_Store->UpdateProgress(_run_id, std::move(update)) )
        [self publishSnapshot];
}

- (void)acceptCompletion:(ExplorerSearchBackendCompletion)_completion forRun:(SearchRunId)_run_id
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( ![self validateCallbackForRun:_run_id] )
        return;

    Reap(std::move(m_BackendRun));
    m_ActiveRunId.reset();
    m_ActiveContent.reset();

    const auto fail_invalid_reply = [self, _run_id](std::string _detail) {
        static_cast<void>(m_Store->Fail(
            _run_id,
            SearchFailure{.code = SearchFailureCode::InvalidBackendReply, .detail = std::move(_detail)}));
        [self publishSnapshot];
    };

    switch( _completion.kind ) {
        case ExplorerSearchBackendCompletionKind::Cancelled:
            if( _completion.listing || _completion.accepted_count != 0 || !_completion.limitations.empty() ||
                _completion.failure ) {
                fail_invalid_reply("Search backend returned an invalid cancellation");
                return;
            }
            static_cast<void>(m_Store->Cancel(_run_id));
            [self publishSnapshot];
            return;
        case ExplorerSearchBackendCompletionKind::Failed:
            if( _completion.listing || _completion.accepted_count != 0 || !_completion.limitations.empty() ||
                !_completion.failure || _completion.failure->detail.empty() ) {
                fail_invalid_reply("Search backend returned an invalid failure");
                return;
            }
            static_cast<void>(m_Store->Fail(_run_id, std::move(*_completion.failure)));
            [self publishSnapshot];
            return;
        case ExplorerSearchBackendCompletionKind::IndexUnavailable:
            if( !m_ActivePlan || !IsValidAvailabilityTerminal(_completion, *m_ActivePlan) ) {
                fail_invalid_reply("Search backend returned invalid index availability");
                return;
            }
            [self publishRuntimeUnavailableForPlan:_completion
                                              kind:SearchBackendSupport::IndexUnavailable
                                            oldRun:_run_id];
            return;
        case ExplorerSearchBackendCompletionKind::BackendUnavailable:
            if( !m_ActivePlan || !IsValidAvailabilityTerminal(_completion, *m_ActivePlan) ) {
                fail_invalid_reply("Search backend returned invalid service availability");
                return;
            }
            [self publishRuntimeUnavailableForPlan:_completion
                                              kind:SearchBackendSupport::Unavailable
                                            oldRun:_run_id];
            return;
        case ExplorerSearchBackendCompletionKind::Completed:
        case ExplorerSearchBackendCompletionKind::Partial:
        case ExplorerSearchBackendCompletionKind::PermissionLimited:
        case ExplorerSearchBackendCompletionKind::TooManyResults:
            break;
    }

    if( !m_ActivePlan || !IsValidResultTerminal(_completion, *m_ActivePlan) ) {
        fail_invalid_reply("Search backend returned an inconsistent terminal result");
        return;
    }
    if( _completion.listing->IsUniform() || _completion.listing->Title().empty() ||
        _completion.accepted_count != _completion.listing->Count() ) {
        static_cast<void>(m_Store->Fail(
            _run_id,
            SearchFailure{.code = SearchFailureCode::InvalidBackendReply,
                          .detail = "Search backend returned an inconsistent result listing"}));
        [self publishSnapshot];
        return;
    }
    for( const SearchBackendLimitation limitation : _completion.limitations ) {
        if( (limitation != SearchBackendLimitation::PermissionDeniedLocations &&
             limitation != SearchBackendLimitation::ResultPathsUnavailable) ||
            (!m_Store->ReportLimitation(_run_id, limitation) &&
             !HasLimitation(*m_Store->Snapshot().backend, limitation)) ) {
            static_cast<void>(
                m_Store->Fail(_run_id,
                              SearchFailure{.code = SearchFailureCode::InvalidBackendReply,
                                            .detail = "Search backend returned an invalid runtime limitation"}));
            [self publishSnapshot];
            return;
        }
    }

    bool commit_threw = false;
    m_CommittingListing = true;
    try {
        m_PanelAccess->CommitListing(_completion.listing);
    } catch( ... ) {
        commit_threw = true;
    }
    m_CommittingListing = false;
    if( commit_threw ) {
        static_cast<void>(m_Store->Fail(_run_id,
                                        SearchFailure{.code = SearchFailureCode::InvalidBackendReply,
                                                      .detail = "Search result listing commit failed"}));
        [self publishSnapshot];
        return;
    }
    const std::optional<ExplorerSearchPanelContent> committed = m_PanelAccess->Capture();
    if( !committed || committed->pane_id != m_PaneId || committed->listing != _completion.listing ) {
        static_cast<void>(m_Store->Fail(_run_id,
                                        SearchFailure{.code = SearchFailureCode::InvalidBackendReply,
                                                      .detail = "Search result listing was not committed"}));
        [self publishSnapshot];
        return;
    }
    m_LastResultListing = _completion.listing;
    m_ExpectedVisibleContent = committed;

    const uint64_t generation = ++m_ResultGeneration;
    const SearchResultReference reference{
        .count = _completion.accepted_count,
        .generation = generation,
        .token = "search:" + std::to_string(m_PaneId.value) + ":" + std::to_string(_run_id.generation) + ":" +
                 std::to_string(generation),
    };
    if( !m_Store->PublishResults(_run_id, reference) ) {
        static_cast<void>(m_Store->Fail(_run_id,
                                        SearchFailure{.code = SearchFailureCode::InvalidBackendReply,
                                                      .detail = "Search result reference was rejected"}));
        [self publishSnapshot];
        return;
    }

    const SearchCompletionKind completion_kind = [&] {
        switch( _completion.kind ) {
            case ExplorerSearchBackendCompletionKind::Completed:
                return SearchCompletionKind::Complete;
            case ExplorerSearchBackendCompletionKind::Partial:
                return SearchCompletionKind::Partial;
            case ExplorerSearchBackendCompletionKind::PermissionLimited:
                return SearchCompletionKind::PermissionLimited;
            case ExplorerSearchBackendCompletionKind::TooManyResults:
                return SearchCompletionKind::TooManyResults;
            case ExplorerSearchBackendCompletionKind::IndexUnavailable:
            case ExplorerSearchBackendCompletionKind::BackendUnavailable:
            case ExplorerSearchBackendCompletionKind::Cancelled:
            case ExplorerSearchBackendCompletionKind::Failed:
                return SearchCompletionKind::Complete;
        }
        return SearchCompletionKind::Complete;
    }();
    if( !m_Store->Complete(_run_id, completion_kind) )
        static_cast<void>(m_Store->Fail(_run_id,
                                        SearchFailure{.code = SearchFailureCode::InvalidBackendReply,
                                                      .detail = "Search completion was rejected"}));
    [self publishSnapshot];
}

- (void)publishRuntimeUnavailableForPlan:(const ExplorerSearchBackendCompletion &)_completion
                                    kind:(SearchBackendSupport)_support
                                  oldRun:(SearchRunId)_old_run
{
    if( !m_ActivePlan ) {
        static_cast<void>(m_Store->Fail(
            _old_run,
            SearchFailure{.code = SearchFailureCode::InvalidBackendReply,
                          .detail = "Search backend lost its active availability plan"}));
        [self publishSnapshot];
        return;
    }
    SearchPlan unavailable = *m_ActivePlan;
    unavailable.backend.support = _support;
    for( const SearchBackendLimitation limitation : _completion.limitations ) {
        if( !HasLimitation(unavailable.backend, limitation) )
            unavailable.backend.limitations.emplace_back(limitation);
    }
    if( !m_Store->Start(unavailable) )
        static_cast<void>(m_Store->Fail(
            _old_run,
            SearchFailure{.code = SearchFailureCode::InvalidBackendReply,
                          .detail = "Search backend returned an invalid availability plan"}));
    [self publishSnapshot];
}

- (BOOL)validateCallbackForRun:(SearchRunId)_run_id
{
    if( !m_Presented || !m_ActiveRunId || *m_ActiveRunId != _run_id || !m_ActiveContent )
        return NO;
    const SearchSnapshot snapshot = m_Store->Snapshot();
    if( !snapshot.run_id || *snapshot.run_id != _run_id || !IsActive(snapshot.phase) )
        return NO;
    const std::optional<ExplorerSearchPanelContent> current = m_PanelAccess->Capture();
    if( !current || current->pane_id != m_PaneId || !SameContent(*current, *m_ActiveContent) ) {
        [self invalidateForExternalContentChange];
        return NO;
    }
    return YES;
}

- (void)cancel
{
    dispatch_assert_queue(dispatch_get_main_queue());
    [self stopActiveRunPublishingCancellation:true];
}

- (void)stopActiveRunPublishingCancellation:(bool)_publish
{
    if( m_BackendRun ) {
        m_BackendRun->Stop();
        Reap(std::move(m_BackendRun));
    }
    if( m_ActiveRunId ) {
        const SearchRunId run_id = *m_ActiveRunId;
        const SearchSnapshot snapshot = m_Store->Snapshot();
        if( snapshot.run_id && *snapshot.run_id == run_id && IsActive(snapshot.phase) )
            static_cast<void>(m_Store->Cancel(run_id));
    }
    m_ActiveRunId.reset();
    m_ActiveContent.reset();
    if( _publish && m_Presented )
        [self publishSnapshot];
}

- (void)invalidateForExternalContentChange
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( m_CommittingListing )
        return;
    [self close];
}

- (void)synchronizeExternalContentChange
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( m_CommittingListing || !m_Presented || !m_ExpectedVisibleContent )
        return;
    const std::optional<ExplorerSearchPanelContent> current = m_PanelAccess->Capture();
    if( current && current->pane_id == m_PaneId && SameContent(*current, *m_ExpectedVisibleContent) )
        return;
    [self close];
}

- (void)close
{
    dispatch_assert_queue(dispatch_get_main_queue());
    [self stopActiveRunPublishingCancellation:false];
    m_Store->Reset();
    m_Presented = false;
    m_PlanningFacts.reset();
    m_Draft.reset();
    m_Origin.reset();
    m_ExpectedVisibleContent.reset();
    m_ActivePlan.reset();
    m_LastResultListing.reset();
    if( m_SnapshotHandler )
        m_SnapshotHandler(std::nullopt);
}

- (BOOL)canRevealFocusedResult
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( !m_Presented || !m_LastResultListing || !m_ExpectedVisibleContent )
        return NO;
    const SearchPhase phase = m_Store->Snapshot().phase;
    if( phase != SearchPhase::Completed && phase != SearchPhase::PartiallyCompleted &&
        phase != SearchPhase::PermissionLimitedResults && phase != SearchPhase::TooManyResults )
        return NO;
    const std::optional<ExplorerSearchPanelContent> content = m_PanelAccess->Capture();
    if( !content || content->pane_id != m_PaneId || !SameContent(*content, *m_ExpectedVisibleContent) ||
        content->listing != m_LastResultListing || !content->focused_item ||
        content->focused_item.Listing() != m_LastResultListing ||
        content->focused_item.Index() >= m_LastResultListing->Count() )
        return NO;

    const VFSListingItem item = content->focused_item;
    return item.Host() && !item.Directory().empty() && !item.Filename().empty() && !item.IsDotDot();
}

- (BOOL)revealFocusedResult
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( !self.canRevealFocusedResult )
        return NO;
    const std::optional<ExplorerSearchPanelContent> content = m_PanelAccess->Capture();
    if( !content )
        return NO;
    const VFSListingItem item = content->focused_item;
    auto request = std::make_shared<nc::panel::DirectoryChangeRequest>();
    request->RequestedDirectory = item.Directory();
    request->VFS = item.Host();
    request->RequestFocusedEntry = item.Filename();
    request->PerformAsynchronous = true;
    request->LoadPreviousViewState = true;
    request->InitiatedByUser = true;
    return m_PanelAccess->SubmitReveal(std::move(request));
}

- (std::optional<SearchSnapshot>)snapshot
{
    dispatch_assert_queue(dispatch_get_main_queue());
    if( !m_Presented )
        return std::nullopt;
    SearchSnapshot snapshot = m_Store->Snapshot();
    if( snapshot.phase == SearchPhase::Idle && m_Draft )
        snapshot.request = m_Draft;
    return snapshot;
}

- (BOOL)isPresented
{
    return m_Presented;
}

- (void)publishSnapshot
{
    if( m_SnapshotHandler )
        m_SnapshotHandler(self.snapshot);
}

- (void)dealloc
{
    if( m_BackendRun ) {
        m_BackendRun->Stop();
        Reap(std::move(m_BackendRun));
    }
}

@end
