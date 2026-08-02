// Copyright (C) 2013-2021 Michael Kazakov. Subject to GNU General Public License version 3.
#include "PanelHistory.h"
#include "../../Core/VFSInstanceManager.h"
#include <iterator>
#include <stdexcept>

namespace nc::panel {

bool History::IsRecording() const noexcept
{
    return m_IsRecording;
}

bool History::CanMoveForth() const noexcept
{
    return GetNavigationAvailability().can_go_forward;
}

bool History::CanMoveBack() const noexcept
{
    return GetNavigationAvailability().can_go_back;
}

History::NavigationAvailability History::GetNavigationAvailability() const noexcept
{
    return GetNavigationState().availability;
}

History::NavigationState History::GetNavigationState() const noexcept
{
    NavigationState state;
    if( m_History.empty() )
        return state;

    if( m_IsRecording ) {
        state.availability.can_go_back = m_History.size() >= 2;
        state.current_entry_id = m_History.back().id;
        return state;
    }

    assert(m_PlayingPosition < m_History.size());
    state.availability = {
        .can_go_back = m_PlayingPosition > 0,
        .can_go_forward = m_PlayingPosition < m_History.size() - 1,
    };
    state.current_entry_id = m_History[m_PlayingPosition].id;
    return state;
}

void History::SetNavigationStateChangeCallback(std::function<void()> _callback)
{
    m_NavigationStateChangeCallback = std::move(_callback);
}

void History::NotifyNavigationStateChanged(const NavigationState &_before) noexcept
{
    if( _before == GetNavigationState() || !m_NavigationStateChangeCallback )
        return;
    try {
        // Copy before invocation so reentrant callback replacement cannot invalidate the target
        // currently executing. This advisory boundary must not roll back a committed mutation.
        const std::function<void()> callback = m_NavigationStateChangeCallback;
        callback();
    } catch( ... ) {
    }
}

History::EntryId History::MintEntryId()
{
    if( m_NextEntryId == 0 )
        throw std::overflow_error("PanelHistory entry identity space is exhausted");
    return m_NextEntryId++;
}

void History::MoveForth()
{
    const NavigationState before = GetNavigationState();
    if( !CanMoveForth() )
        throw std::logic_error("PanelHistory::MoveForth called when CanMoveForth()==false");

    if( m_IsRecording )
        return;
    if( m_History.size() < 2 )
        return;
    if( m_PlayingPosition < m_History.size() - 1 )
        m_PlayingPosition++;
    NotifyNavigationStateChanged(before);
}

void History::MoveBack()
{
    const NavigationState before = GetNavigationState();
    if( !CanMoveBack() )
        throw std::logic_error("PanelHistory::MoveBack called when CanMoveBack()==false");

    if( m_IsRecording ) {
        m_IsRecording = false;
        m_PlayingPosition = static_cast<unsigned>(m_History.size()) - 2;
    }
    else {
        m_PlayingPosition--;
    }
    NotifyNavigationStateChanged(before);
}

const History::Path *History::CurrentPlaying() const
{
    if( m_IsRecording )
        return nullptr;
    return &m_History[m_PlayingPosition].path;
}

const History::Path *History::MostRecent() const
{
    if( m_IsRecording ) {
        if( !m_History.empty() )
            return &m_History.back().path;
        return nullptr;
    }
    else {
        assert(m_PlayingPosition < m_History.size());
        return &m_History[m_PlayingPosition].path;
    }
}

void History::Put(const VFSListing &_listing)
{
    const NavigationState before = GetNavigationState();
    if( _listing.IsUniform() && _listing.Host()->IsNativeFS() )
        m_LastNativeDirectory = _listing.Directory();

    const auto adapter = [this](const std::shared_ptr<VFSHost> &_host) -> core::VFSInstancePromise {
        if( !m_VFSMgr )
            return {};
        return m_VFSMgr->TameVFS(_host);
    };
    ListingPromise promise{_listing, adapter};

    if( m_IsRecording ) {
        if( !m_History.empty() && m_History.back().path == promise )
            return;
        m_History.emplace_back(Entry{.id = MintEntryId(), .path = std::move(promise)});
        if( m_History.size() > m_HistoryLength )
            m_History.pop_front();
    }
    else {
        assert(m_PlayingPosition < m_History.size());
        auto i = begin(m_History);
        advance(i, m_PlayingPosition);
        if( i->path != promise ) {
            Entry branch{.id = MintEntryId(), .path = std::move(promise)};
            m_History.emplace_back(std::move(branch));

            auto first_forward = m_History.begin();
            advance(first_forward, m_PlayingPosition + 1);
            const auto appended_branch = std::prev(m_History.end());
            m_History.erase(first_forward, appended_branch);

            m_IsRecording = true;
            if( m_History.size() > m_HistoryLength )
                m_History.pop_front();
        }
    }
    NotifyNavigationStateChanged(before);
}

unsigned History::Length() const noexcept
{
    return static_cast<unsigned>(m_History.size());
}

bool History::Empty() const noexcept
{
    return m_History.empty();
}

std::vector<std::reference_wrapper<const History::Path>> History::All() const
{
    std::vector<std::reference_wrapper<const Path>> res;
    res.reserve(m_History.size());
    for( auto &i : m_History )
        res.emplace_back(std::cref(i.path));
    return res;
}

const History::Path *History::RewindAt(size_t _indx)
{
    if( _indx >= m_History.size() )
        return nullptr;

    const NavigationState before = GetNavigationState();
    m_IsRecording = false;
    m_PlayingPosition = static_cast<unsigned>(_indx);

    const Path *const current = CurrentPlaying();
    NotifyNavigationStateChanged(before);
    return current;
}

const std::string &History::LastNativeDirectoryVisited() const noexcept
{
    return m_LastNativeDirectory;
}

void History::SetVFSInstanceManager(core::VFSInstanceManager &_mgr)
{
    m_VFSMgr = &_mgr;
}

} // namespace nc::panel
