// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PaneLifecycleReducer.h"

#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace nc::core {

namespace {

bool IsValidSortState(const PaneSortState &_sort_state, const PaneLoadPhase _load_phase) noexcept
{
    switch( _sort_state.collation ) {
        case PaneTextCollation::Unknown:
            if( _load_phase == PaneLoadPhase::Loaded )
                return false;
            break;
        case PaneTextCollation::Natural:
        case PaneTextCollation::CaseInsensitive:
        case PaneTextCollation::CaseSensitive:
            break;
        default:
            return false;
    }

    switch( _sort_state.key ) {
        case PaneSortKey::Unknown:
            return _load_phase != PaneLoadPhase::Loaded &&
                   _sort_state.direction == PaneSortDirection::None;
        case PaneSortKey::Unsorted:
            return _sort_state.direction == PaneSortDirection::None;
        case PaneSortKey::RawName:
            return _sort_state.direction == PaneSortDirection::Ascending;
        case PaneSortKey::Name:
        case PaneSortKey::Extension:
        case PaneSortKey::Size:
        case PaneSortKey::ModifiedTime:
        case PaneSortKey::CreatedTime:
        case PaneSortKey::AddedTime:
        case PaneSortKey::AccessedTime:
            return _sort_state.direction == PaneSortDirection::Ascending ||
                   _sort_state.direction == PaneSortDirection::Descending;
        default:
            return false;
    }
}

PaneGroupingKey GroupingKeyForSort(const PaneSortKey _sort_key) noexcept
{
    switch( _sort_key ) {
        case PaneSortKey::Unsorted:
        case PaneSortKey::RawName:
        case PaneSortKey::Name:
            return PaneGroupingKey::Name;
        case PaneSortKey::Extension:
            return PaneGroupingKey::Extension;
        case PaneSortKey::Size:
            return PaneGroupingKey::Size;
        case PaneSortKey::ModifiedTime:
            return PaneGroupingKey::ModifiedTime;
        case PaneSortKey::CreatedTime:
            return PaneGroupingKey::CreatedTime;
        case PaneSortKey::AddedTime:
            return PaneGroupingKey::AddedTime;
        case PaneSortKey::AccessedTime:
            return PaneGroupingKey::AccessedTime;
        case PaneSortKey::Unknown:
        default:
            return PaneGroupingKey::Unknown;
    }
}

bool IsValidGroupingState(const PaneSortState &_sort_state,
                          const PaneGroupingState &_grouping_state) noexcept
{
    if( !_grouping_state.enabled )
        return _grouping_state.key == PaneGroupingKey::Unknown;
    const PaneGroupingKey expected_key = GroupingKeyForSort(_sort_state.key);
    return expected_key != PaneGroupingKey::Unknown && _grouping_state.key == expected_key;
}

bool IsValidViewState(const PaneViewState &_view_state, const PaneLoadPhase _load_phase) noexcept
{
    if( _view_state.layout_index && *_view_state.layout_index < 0 )
        return false;

    switch( _view_state.mode ) {
        case PaneViewMode::Unknown:
            return _load_phase != PaneLoadPhase::Loaded && !_view_state.layout_index;
        case PaneViewMode::Icons:
        case PaneViewMode::Details:
        case PaneViewMode::Gallery:
            return true;
        default:
            return false;
    }
}

bool IsValidHistoryState(const PaneState &_state) noexcept
{
    if( _state.current_history_entry_id && *_state.current_history_entry_id == 0 )
        return false;
    if( !_state.current_history_entry_id &&
        (_state.history_availability.can_go_back || _state.history_availability.can_go_forward) )
        return false;
    return true;
}

} // namespace

PaneLifecycleReducer::PaneLifecycleReducer(const PaneId _pane_id, PaneState _initial_committed_state)
    : m_PaneId(_pane_id)
{
    if( !IsCommittedProjection(_initial_committed_state) )
        throw std::invalid_argument("PaneLifecycleReducer requires an Empty or Loaded committed projection");
    m_Owned.committed_state = std::move(_initial_committed_state);
    m_Owned.state = m_Owned.committed_state;
}

PaneId PaneLifecycleReducer::Pane() const noexcept
{
    return m_PaneId;
}

const PaneState &PaneLifecycleReducer::State() const noexcept
{
    return m_Owned.state;
}

PaneLifecycleReducerResult PaneLifecycleReducer::SeedActive(PaneActiveRequest _active_request)
{
    if( !_active_request.request_id || m_Owned.last_event_sequence || m_Owned.active_request ||
        m_Owned.expected_replacement )
        return {.status = PaneLifecycleReducerStatus::InvalidTransition};

    // Copying current public/owned values and recomposing may allocate. Both finish before the
    // single nothrow move that publishes the candidate, preserving the reducer on exceptions.
    OwnedState next = m_Owned;
    next.active_request = std::move(_active_request);
    ClearTerminalOverlay(next);
    Recompose(next);
    return Commit(std::move(next), PaneLifecycleReducerStatus::Applied);
}

PaneLifecycleReducerResult PaneLifecycleReducer::SeedRetainedFailure(
    PaneActiveRequest _failed_request,
    const PaneLifecycleEvent &_failure,
    const uint64_t _checkpoint_sequence)
{
    const auto *failed = std::get_if<PaneLifecycleFailed>(&_failure.payload);
    if( !_failed_request.request_id || m_Owned.last_event_sequence || m_Owned.active_request ||
        m_Owned.expected_replacement || _failure.pane_id != m_PaneId ||
        _failure.request_id != _failed_request.request_id ||
        _failure.descriptor != _failed_request.descriptor || failed == nullptr ||
        _failure.event_sequence == 0 || _checkpoint_sequence < _failure.event_sequence )
        return {.status = PaneLifecycleReducerStatus::InvalidTransition};

    OwnedState next = m_Owned;
    next.last_event_sequence = _checkpoint_sequence;
    next.visible_error = failed->error;
    next.navigation_failed = _failure.descriptor.kind == PaneRequestKind::Navigation;
    next.commit_projection_failed = false;
    Recompose(next);
    return Commit(std::move(next), PaneLifecycleReducerStatus::Applied);
}

PaneLifecycleReducerResult PaneLifecycleReducer::UpdateCommittedProjection(PaneState _committed_state)
{
    if( !IsCommittedProjection(_committed_state, &m_Owned.committed_state) )
        return {.status = PaneLifecycleReducerStatus::InvalidCommittedProjection};
    if( _committed_state.location_generation < m_Owned.committed_state.location_generation )
        return {.status = PaneLifecycleReducerStatus::StaleCommittedProjection};

    const bool is_new_location =
        _committed_state.location_generation > m_Owned.committed_state.location_generation;
    OwnedState next = m_Owned;
    next.committed_state = std::move(_committed_state);
    if( is_new_location && !next.active_request && !next.expected_replacement )
        ClearTerminalOverlay(next);
    Recompose(next);
    return Commit(std::move(next), PaneLifecycleReducerStatus::Applied);
}

PaneLifecycleReducerResult PaneLifecycleReducer::Apply(const PaneLifecycleEvent &_event,
                                                       const PaneState *_post_commit_projection)
{
    if( _event.pane_id != m_PaneId )
        return {.status = PaneLifecycleReducerStatus::WrongPane};
    if( !_event.request_id )
        return {.status = PaneLifecycleReducerStatus::InvalidTransition};

    switch( ValidateSequence(_event.event_sequence) ) {
        case SequenceValidation::Stale:
            return {.status = PaneLifecycleReducerStatus::StaleSequence};
        case SequenceValidation::Gap:
            return {.status = PaneLifecycleReducerStatus::SequenceGap};
        case SequenceValidation::Current:
            break;
    }

    if( std::holds_alternative<PaneLifecycleStarted>(_event.payload) ) {
        if( m_Owned.active_request )
            return {.status = PaneLifecycleReducerStatus::InvalidTransition};
        if( m_Owned.expected_replacement && *m_Owned.expected_replacement != _event.request_id )
            return {.status = PaneLifecycleReducerStatus::InvalidTransition};

        OwnedState next = m_Owned;
        next.last_event_sequence = _event.event_sequence;
        next.active_request = PaneActiveRequest{_event.request_id, _event.descriptor};
        next.expected_replacement.reset();
        ClearTerminalOverlay(next);
        Recompose(next);
        return Commit(std::move(next), PaneLifecycleReducerStatus::Applied);
    }

    if( const auto *committed = std::get_if<PaneLifecycleCommitted>(&_event.payload) ) {
        if( !MatchesActive(_event) || m_Owned.expected_replacement )
            return {.status = PaneLifecycleReducerStatus::InvalidTransition};

        const bool generation_fits =
            committed->controller_generation <= std::numeric_limits<unsigned long>::max();
        const bool projection_matches =
            _post_commit_projection != nullptr &&
            IsCommittedProjection(*_post_commit_projection, &m_Owned.committed_state) &&
            generation_fits &&
            _post_commit_projection->location_generation == committed->controller_generation &&
            _post_commit_projection->listing == committed->listing &&
            _post_commit_projection->location_generation >= m_Owned.committed_state.location_generation;
        if( !projection_matches ) {
            OwnedState next = m_Owned;
            next.last_event_sequence = _event.event_sequence;
            next.active_request.reset();
            next.expected_replacement.reset();
            ClearTerminalOverlay(next);
            next.commit_projection_failed = true;
            Recompose(next);
            return Commit(std::move(next), PaneLifecycleReducerStatus::CommitProjectionMismatch);
        }

        OwnedState next = m_Owned;
        next.committed_state = *_post_commit_projection;
        next.last_event_sequence = _event.event_sequence;
        next.active_request.reset();
        next.expected_replacement.reset();
        ClearTerminalOverlay(next);
        Recompose(next);
        return Commit(std::move(next), PaneLifecycleReducerStatus::Applied);
    }

    if( const auto *failed = std::get_if<PaneLifecycleFailed>(&_event.payload) ) {
        if( !MatchesActive(_event) || m_Owned.expected_replacement )
            return {.status = PaneLifecycleReducerStatus::InvalidTransition};

        OwnedState next = m_Owned;
        next.last_event_sequence = _event.event_sequence;
        next.active_request.reset();
        next.expected_replacement.reset();
        next.visible_error = failed->error;
        next.navigation_failed = _event.descriptor.kind == PaneRequestKind::Navigation;
        next.commit_projection_failed = false;
        Recompose(next);
        return Commit(std::move(next), PaneLifecycleReducerStatus::Applied);
    }

    if( std::holds_alternative<PaneLifecycleCancelled>(_event.payload) ) {
        if( !MatchesActive(_event) || m_Owned.expected_replacement )
            return {.status = PaneLifecycleReducerStatus::InvalidTransition};

        OwnedState next = m_Owned;
        next.last_event_sequence = _event.event_sequence;
        next.active_request.reset();
        next.expected_replacement.reset();
        ClearTerminalOverlay(next);
        Recompose(next);
        return Commit(std::move(next), PaneLifecycleReducerStatus::Applied);
    }

    if( const auto *superseded = std::get_if<PaneLifecycleSuperseded>(&_event.payload) ) {
        if( !MatchesActive(_event) || m_Owned.expected_replacement || !superseded->replacement ||
            superseded->replacement == _event.request_id )
            return {.status = PaneLifecycleReducerStatus::InvalidTransition};

        OwnedState next = m_Owned;
        next.last_event_sequence = _event.event_sequence;
        next.active_request.reset();
        next.expected_replacement = superseded->replacement;
        // Keep the public state untouched. The immediately following replacement Started changes the
        // public phase directly, without an intermediate committed-state publication.
        return Commit(std::move(next), PaneLifecycleReducerStatus::NoVisibleChange);
    }

    if( std::holds_alternative<PaneLifecycleRejected>(_event.payload) ) {
        if( m_Owned.expected_replacement )
            return {.status = PaneLifecycleReducerStatus::InvalidTransition};
        OwnedState next = m_Owned;
        next.last_event_sequence = _event.event_sequence;
        // Rejected attempts never became authoritative pane work. Their optional admission error is
        // request-result/presenter input, not the pane's accepted lifecycle failure.
        return Commit(std::move(next), PaneLifecycleReducerStatus::NoVisibleChange);
    }

    return {.status = PaneLifecycleReducerStatus::InvalidTransition};
}

PaneLifecycleReducer::SequenceValidation PaneLifecycleReducer::ValidateSequence(const uint64_t _sequence) const noexcept
{
    if( _sequence == 0 )
        return SequenceValidation::Gap;
    if( !m_Owned.last_event_sequence )
        return SequenceValidation::Current;
    if( _sequence <= *m_Owned.last_event_sequence )
        return SequenceValidation::Stale;
    if( *m_Owned.last_event_sequence == std::numeric_limits<uint64_t>::max() ||
        _sequence != *m_Owned.last_event_sequence + 1 )
        return SequenceValidation::Gap;
    return SequenceValidation::Current;
}

bool PaneLifecycleReducer::MatchesActive(const PaneLifecycleEvent &_event) const noexcept
{
    return m_Owned.active_request && m_Owned.active_request->request_id == _event.request_id &&
           m_Owned.active_request->descriptor == _event.descriptor;
}

bool PaneLifecycleReducer::IsCommittedProjection(
    const PaneState &_state,
    const PaneState *_trusted_selection_projection)
{
    if( _state.visible_error )
        return false;
    if( !IsValidSortState(_state.sort_state, _state.load_phase) )
        return false;
    if( !IsValidGroupingState(_state.sort_state, _state.grouping_state) )
        return false;
    if( !IsValidViewState(_state.view_state, _state.load_phase) )
        return false;
    if( !IsValidHistoryState(_state) )
        return false;

    if( _state.load_phase == PaneLoadPhase::Empty ) {
        return !_state.focused_item && _state.selected_items.empty() && _state.item_count == 0 &&
               _state.selected_count == 0 && _state.selected_bytes == 0;
    }
    if( _state.load_phase != PaneLoadPhase::Loaded || !_state.listing )
        return false;

    if( _state.focused_item &&
        (_state.focused_item.Listing() != _state.listing ||
         _state.focused_item.Index() >= _state.listing->Count()) )
        return false;

    const size_t selected_size = _state.selected_items.size();
    if( selected_size > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
        _state.item_count < 0 ||
        _state.selected_count < 0 ||
        _state.selected_count > _state.item_count ||
        static_cast<size_t>(_state.selected_count) != selected_size ||
        _state.selected_bytes < 0 ||
        (_state.selected_count == 0 && _state.selected_bytes != 0) )
        return false;

    const bool selection_was_validated =
        selected_size != 0 && _trusted_selection_projection != nullptr &&
        _state.listing == _trusted_selection_projection->listing &&
        _state.selected_items.StorageIdentity() ==
            _trusted_selection_projection->selected_items.StorageIdentity();
    if( selection_was_validated )
        return true;

    std::unordered_set<unsigned long> selected_indexes;
    selected_indexes.reserve(selected_size);
    for( size_t index = 0; index < selected_size; ++index ) {
        const VFSListingItem &item = _state.selected_items[index];
        if( !item || item.Listing() != _state.listing || item.Index() >= _state.listing->Count() )
            return false;
        if( !selected_indexes.emplace(item.Index()).second )
            return false;
    }
    return true;
}

void PaneLifecycleReducer::Recompose(OwnedState &_state)
{
    PaneState next = _state.committed_state;
    next.visible_error = _state.visible_error;
    if( _state.active_request ) {
        next.load_phase = _state.active_request->descriptor.kind == PaneRequestKind::Navigation
                            ? PaneLoadPhase::Loading
                            : PaneLoadPhase::Refreshing;
    }
    else if( _state.expected_replacement ) {
        // SupersedeAndStart publishes a synchronous pair. Preserve the accepted work phase even if
        // a caller merges a model projection while reducing that pair.
        next.load_phase = _state.state.load_phase;
    }
    else if( _state.navigation_failed || _state.commit_projection_failed ) {
        next.load_phase = PaneLoadPhase::Failed;
    }
    _state.state = std::move(next);
}

void PaneLifecycleReducer::ClearTerminalOverlay(OwnedState &_state) noexcept
{
    _state.visible_error.reset();
    _state.navigation_failed = false;
    _state.commit_projection_failed = false;
}

PaneLifecycleReducerResult PaneLifecycleReducer::Commit(OwnedState _next,
                                                        const PaneLifecycleReducerStatus _status) noexcept
{
    const bool state_changed = _next.state != m_Owned.state;
    const auto status = !state_changed && _status == PaneLifecycleReducerStatus::Applied
                          ? PaneLifecycleReducerStatus::NoVisibleChange
                          : _status;
    m_Owned = std::move(_next);
    return {.status = status, .state_changed = state_changed};
}

} // namespace nc::core
