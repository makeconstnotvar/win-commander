// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ExplorerViewSettingsBinding.h"

#include <algorithm>

namespace nc::explorer {

ExplorerViewSettingsBindingPolicy::ExplorerViewSettingsBindingPolicy(const core::PaneId _pane_id) noexcept :
    m_PaneId(_pane_id)
{
}

ExplorerViewSettingsBindingAction
ExplorerViewSettingsBindingPolicy::Observe(const ExplorerViewSettingsObservation &_observation)
{
    using Action = ExplorerViewSettingsBindingAction;
    if( m_PaneId.value == 0 || _observation.pane_id != m_PaneId || !_observation.settings ||
        (_observation.load_phase != core::PaneLoadPhase::Loaded &&
         _observation.load_phase != core::PaneLoadPhase::Refreshing) ||
        !_observation.is_uniform || _observation.host_identity == nullptr || _observation.path.empty() ||
        _observation.path.back() != '/' ) {
        return Action::Rejected;
    }
    if( m_LastRevision && _observation.revision < *m_LastRevision )
        return Action::Rejected;
    if( m_LastObservationSequence && _observation.observation_sequence < *m_LastObservationSequence )
        return Action::Rejected;

    LocationKey location{.generation = _observation.location_generation,
                         .host_identity = _observation.host_identity,
                         .path = _observation.path};
    if( !m_Location || *m_Location != location ) {
        m_Location = std::move(location);
        m_LastRevision = _observation.revision;
        m_LastObservationSequence = _observation.observation_sequence;
        m_Current.reset();
        m_RestoreTarget.reset();
        m_RestoreFenceSequence = 0;
        return Action::LoadLocation;
    }

    m_LastRevision = std::max(*m_LastRevision, _observation.revision);
    m_LastObservationSequence = std::max(*m_LastObservationSequence, _observation.observation_sequence);
    if( m_RestoreTarget ) {
        if( _observation.observation_sequence <= m_RestoreFenceSequence )
            return Action::None;
        if( *_observation.settings == *m_RestoreTarget ) {
            m_Current = *_observation.settings;
            m_RestoreTarget.reset();
            return Action::RestoreSettled;
        }
        m_RestoreTarget.reset();
        return Action::RestoreDiverged;
    }

    if( !m_Current || *m_Current != *_observation.settings )
        return Action::StoreCurrent;
    return Action::None;
}

bool ExplorerViewSettingsBindingPolicy::AcceptCurrent(
    const ExplorerViewSettingsObservation &_observation) noexcept
{
    if( !IsCurrent(_observation) || !_observation.settings )
        return false;
    m_Current = *_observation.settings;
    m_RestoreTarget.reset();
    return true;
}

bool ExplorerViewSettingsBindingPolicy::BeginRestore(const ExplorerViewSettings &_target) noexcept
{
    if( !m_Location || !m_LastRevision || !m_LastObservationSequence )
        return false;
    m_RestoreTarget = _target;
    m_RestoreFenceSequence = *m_LastObservationSequence;
    return true;
}

bool ExplorerViewSettingsBindingPolicy::IsCurrent(
    const ExplorerViewSettingsObservation &_observation) const noexcept
{
    if( !m_Location || !m_LastRevision || !m_LastObservationSequence || _observation.pane_id != m_PaneId ||
        _observation.revision != *m_LastRevision ||
        _observation.observation_sequence != *m_LastObservationSequence ) {
        return false;
    }
    return m_Location->generation == _observation.location_generation &&
           m_Location->host_identity == _observation.host_identity && m_Location->path == _observation.path;
}

} // namespace nc::explorer
