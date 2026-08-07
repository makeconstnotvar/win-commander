// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "SearchStore.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace nc::core {

namespace {

[[nodiscard]] bool IsTerminal(const SearchPhase _phase) noexcept
{
    switch( _phase ) {
        case SearchPhase::Idle:
        case SearchPhase::Preparing:
        case SearchPhase::Running:
            return false;
        case SearchPhase::PartiallyCompleted:
        case SearchPhase::Completed:
        case SearchPhase::Cancelled:
        case SearchPhase::Failed:
        case SearchPhase::NoResults:
        case SearchPhase::TooManyResults:
        case SearchPhase::IndexUnavailable:
        case SearchPhase::BackendUnavailable:
        case SearchPhase::PermissionLimitedResults:
            return true;
    }
    return true;
}

[[nodiscard]] bool HasLimitation(const SearchBackendDescriptor &_backend,
                                 const SearchBackendLimitation _limitation) noexcept
{
    return std::ranges::find(_backend.limitations, _limitation) != _backend.limitations.end();
}

} // namespace

SearchStore::SearchStore(const PaneId _pane_id) noexcept
{
    m_State.pane_id = _pane_id;
}

SearchStore::CreateResult SearchStore::Create(const PaneId _pane_id) noexcept
{
    if( _pane_id.value == 0 )
        return std::unexpected(SearchStoreFailure::ZeroPaneId);
    return SearchStore{_pane_id};
}

SearchStore::StartResult SearchStore::Start(const SearchPlan &_plan)
{
    if( !SearchPlanning::IsValid(_plan) )
        return std::unexpected(SearchStoreFailure::InvalidPlan);
    if( m_LastRunGeneration == std::numeric_limits<uint64_t>::max() )
        return std::unexpected(SearchStoreFailure::RunIdExhausted);

    const SearchRunId run_id{.pane_id = m_State.pane_id, .generation = ++m_LastRunGeneration};
    const uint64_t next_revision = m_State.revision + 1;
    m_State = {
        .pane_id = m_State.pane_id,
        .revision = next_revision,
        .run_id = run_id,
        .phase = SearchPhase::Preparing,
        .request = _plan.request,
        .backend = _plan.backend,
    };

    switch( _plan.backend.support ) {
        case SearchBackendSupport::Supported:
            break;
        case SearchBackendSupport::IndexUnavailable:
            m_State.phase = SearchPhase::IndexUnavailable;
            break;
        case SearchBackendSupport::Unsupported:
        case SearchBackendSupport::Unavailable:
            m_State.phase = SearchPhase::BackendUnavailable;
            break;
    }

    return run_id;
}

SearchStoreMutationResult SearchStore::ValidateActive(const SearchRunId _run_id) const noexcept
{
    if( !m_State.run_id )
        return std::unexpected(SearchStoreFailure::NoActiveRun);
    if( _run_id != *m_State.run_id )
        return std::unexpected(SearchStoreFailure::StaleRun);
    if( IsTerminal(m_State.phase) )
        return std::unexpected(SearchStoreFailure::TerminalRun);
    return {};
}

bool SearchStore::IsActivePhase() const noexcept
{
    return m_State.phase == SearchPhase::Preparing || m_State.phase == SearchPhase::Running;
}

SearchStoreMutationResult SearchStore::MarkRunning(const SearchRunId _run_id)
{
    if( auto valid = ValidateActive(_run_id); !valid )
        return valid;
    if( m_State.phase != SearchPhase::Preparing )
        return std::unexpected(SearchStoreFailure::InvalidTransition);

    m_State.phase = SearchPhase::Running;
    ++m_State.revision;
    return {};
}

SearchStoreMutationResult SearchStore::UpdateProgress(const SearchRunId _run_id, SearchProgressUpdate _update)
{
    if( auto valid = ValidateActive(_run_id); !valid )
        return valid;
    if( m_State.phase != SearchPhase::Running )
        return std::unexpected(SearchStoreFailure::InvalidTransition);

    const bool empty =
        !_update.determinate_progress && !_update.current_location && !_update.scanned_count && !_update.found_count;
    if( empty )
        return std::unexpected(SearchStoreFailure::InvalidProgress);
    if( _update.determinate_progress &&
        (!std::isfinite(*_update.determinate_progress) || *_update.determinate_progress < 0.0 ||
         *_update.determinate_progress > 1.0 ||
         (m_State.determinate_progress && *_update.determinate_progress < *m_State.determinate_progress)) )
        return std::unexpected(SearchStoreFailure::InvalidProgress);
    if( _update.current_location && _update.current_location->empty() )
        return std::unexpected(SearchStoreFailure::InvalidProgress);
    if( _update.scanned_count && m_State.scanned_count && *_update.scanned_count < *m_State.scanned_count )
        return std::unexpected(SearchStoreFailure::InvalidProgress);
    if( _update.found_count && m_State.found_count && *_update.found_count < *m_State.found_count )
        return std::unexpected(SearchStoreFailure::InvalidProgress);
    if( _update.determinate_progress )
        m_State.determinate_progress = _update.determinate_progress;
    if( _update.current_location )
        m_State.current_location = std::move(_update.current_location);
    if( _update.scanned_count )
        m_State.scanned_count = _update.scanned_count;
    if( _update.found_count )
        m_State.found_count = _update.found_count;
    ++m_State.revision;
    return {};
}

SearchStoreMutationResult SearchStore::ReportLimitation(const SearchRunId _run_id,
                                                        const SearchBackendLimitation _limitation)
{
    if( auto valid = ValidateActive(_run_id); !valid )
        return valid;
    if( !IsActivePhase() )
        return std::unexpected(SearchStoreFailure::InvalidTransition);
    if( (_limitation != SearchBackendLimitation::PermissionDeniedLocations &&
         _limitation != SearchBackendLimitation::ResultPathsUnavailable) ||
        HasLimitation(*m_State.backend, _limitation) )
        return std::unexpected(SearchStoreFailure::InvalidLimitation);

    m_State.backend->limitations.emplace_back(_limitation);
    ++m_State.revision;
    return {};
}

SearchStoreMutationResult SearchStore::PublishResults(const SearchRunId _run_id, SearchResultReference _results)
{
    if( auto valid = ValidateActive(_run_id); !valid )
        return valid;
    if( m_State.phase != SearchPhase::Running )
        return std::unexpected(SearchStoreFailure::InvalidTransition);
    if( _results.generation == 0 || _results.token.empty() ||
        (m_State.results && _results.generation <= m_State.results->generation) )
        return std::unexpected(SearchStoreFailure::InvalidResults);

    m_State.results = std::move(_results);
    ++m_State.revision;
    return {};
}

void SearchStore::CommitTerminal(const SearchPhase _phase) noexcept
{
    m_State.phase = _phase;
    m_State.current_location.reset();
    ++m_State.revision;
}

SearchStoreMutationResult SearchStore::Complete(const SearchRunId _run_id, const SearchCompletionKind _kind)
{
    if( auto valid = ValidateActive(_run_id); !valid )
        return valid;
    if( !IsActivePhase() )
        return std::unexpected(SearchStoreFailure::InvalidTransition);

    const uint64_t count = m_State.results ? m_State.results->count : 0;
    switch( _kind ) {
        case SearchCompletionKind::Complete:
            CommitTerminal(count == 0 ? SearchPhase::NoResults : SearchPhase::Completed);
            return {};
        case SearchCompletionKind::Partial:
            if( !m_State.results || m_State.backend->limitations.empty() )
                return std::unexpected(SearchStoreFailure::MissingResults);
            CommitTerminal(SearchPhase::PartiallyCompleted);
            return {};
        case SearchCompletionKind::TooManyResults:
            if( !m_State.results )
                return std::unexpected(SearchStoreFailure::MissingResults);
            CommitTerminal(SearchPhase::TooManyResults);
            return {};
        case SearchCompletionKind::PermissionLimited:
            if( !HasLimitation(*m_State.backend, SearchBackendLimitation::FullDiskAccessMissing) &&
                !HasLimitation(*m_State.backend, SearchBackendLimitation::PermissionDeniedLocations) )
                return std::unexpected(SearchStoreFailure::LimitationMismatch);
            CommitTerminal(SearchPhase::PermissionLimitedResults);
            return {};
    }
    return std::unexpected(SearchStoreFailure::InvalidTransition);
}

SearchStoreMutationResult SearchStore::Cancel(const SearchRunId _run_id)
{
    if( auto valid = ValidateActive(_run_id); !valid )
        return valid;
    CommitTerminal(SearchPhase::Cancelled);
    return {};
}

SearchStoreMutationResult SearchStore::Fail(const SearchRunId _run_id, SearchFailure _failure)
{
    if( auto valid = ValidateActive(_run_id); !valid )
        return valid;
    if( _failure.detail.empty() )
        return std::unexpected(SearchStoreFailure::InvalidTransition);

    m_State.failure = std::move(_failure);
    CommitTerminal(SearchPhase::Failed);
    return {};
}

void SearchStore::Reset() noexcept
{
    const uint64_t next_revision = m_State.revision + 1;
    m_State = {.pane_id = m_State.pane_id, .revision = next_revision};
}

} // namespace nc::core
