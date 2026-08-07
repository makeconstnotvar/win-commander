// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NCExplorerInspectorModel.h"

#include <VFS/VFS.h>

namespace nc::explorer {

bool InspectorModel::Apply(const core::PaneSnapshot &_snapshot)
{
    if( _snapshot.pane_id != m_PaneId )
        return false;
    if( m_LastAcceptedRevision && _snapshot.revision < *m_LastAcceptedRevision )
        return false;

    m_LastAcceptedRevision = _snapshot.revision;
    m_Items.clear();
    m_PreviewItem = {};
    m_Error = _snapshot.state.visible_error;
    m_IsRefreshing = false;

    if( _snapshot.state.load_phase == core::PaneLoadPhase::Failed ) {
        m_State = InspectorState::PaneError;
        return true;
    }

    switch( _snapshot.state.load_phase ) {
        case core::PaneLoadPhase::Loading:
            m_State = InspectorState::PaneLoading;
            return true;
        case core::PaneLoadPhase::Refreshing:
            m_IsRefreshing = true;
            break;
        case core::PaneLoadPhase::Empty:
            m_State = InspectorState::Empty;
            return true;
        case core::PaneLoadPhase::Failed:
            // Handled above, including the valid no-detail failure projection.
            m_State = InspectorState::PaneError;
            return true;
        case core::PaneLoadPhase::Loaded:
            break;
    }

    const auto append = [this](const vfs::ListingItem &_item) {
        if( _item && !_item.IsDotDot() ) {
            if( m_Items.empty() )
                m_PreviewItem = _item;
            else
                m_PreviewItem = {};
            m_Items.emplace_back(core::CopyFileMetadataSnapshot(_item));
        }
    };
    if( !_snapshot.state.selected_items.empty() ) {
        m_Items.reserve(_snapshot.state.selected_items.size());
        for( const vfs::ListingItem &item : _snapshot.state.selected_items )
            append(item);
    }
    else {
        append(_snapshot.state.focused_item);
    }

    if( m_Items.empty() )
        m_State = InspectorState::Empty;
    else if( m_Items.size() == 1 )
        m_State = InspectorState::Single;
    else
        m_State = InspectorState::Multiple;
    return true;
}

} // namespace nc::explorer
