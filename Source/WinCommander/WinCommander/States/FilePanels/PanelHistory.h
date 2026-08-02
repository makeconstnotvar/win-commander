// Copyright (C) 2013-2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/VFS_fwd.h>
#include "ListingPromise.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>

namespace nc::core {
class VFSInstanceManager;
}

namespace nc::panel {

/**
 * This class is not thread-safe.
 */
class History
{
public:
    using Path = ListingPromise;
    using EntryId = uint64_t;

    struct NavigationAvailability {
        bool can_go_back = false;
        bool can_go_forward = false;

        constexpr bool operator==(const NavigationAvailability &) const noexcept = default;
    };

    struct NavigationState {
        NavigationAvailability availability;
        std::optional<EntryId> current_entry_id;

        constexpr bool operator==(const NavigationState &) const noexcept = default;
    };

    [[nodiscard]] bool IsRecording() const noexcept;
    [[nodiscard]] unsigned Length() const noexcept;
    [[nodiscard]] bool Empty() const noexcept;

    [[nodiscard]] bool CanMoveForth() const noexcept;

    /**
     * Will throw if CanMoveForth() == false.
     */
    void MoveForth();

    [[nodiscard]] bool CanMoveBack() const noexcept;

    [[nodiscard]] NavigationAvailability GetNavigationAvailability() const noexcept;
    [[nodiscard]] NavigationState GetNavigationState() const noexcept;

    /** Replaces the synchronous advisory callback without emitting an initial notification. */
    void SetNavigationStateChangeCallback(std::function<void()> _callback);

    /**
     * Will throw if CanMoveBack() == false.
     */
    void MoveBack();

    /**
     * Will turn History into "recording" state.
     * If history was in playing state - will discard anything in front of current position.
     */
    void Put(const VFSListing &_listing);

    /**
     * Will return nullptr if history is in "recording" state.
     */
    [[nodiscard]] const Path *CurrentPlaying() const;

    /**
     * Will put History in "playing" state and adjust playing position accordingly,
     * and return current history element
     */
    const Path *RewindAt(size_t _indx);

    /**
     * Returns the one most recently visited, either in a recording state or in a playing state.
     */
    [[nodiscard]] const Path *MostRecent() const;

    [[nodiscard]] std::vector<std::reference_wrapper<const Path>> All() const;

    [[nodiscard]] const std::string &LastNativeDirectoryVisited() const noexcept;

    void SetVFSInstanceManager(core::VFSInstanceManager &_mgr);

private:
    struct Entry {
        EntryId id;
        Path path;
    };

    [[nodiscard]] EntryId MintEntryId();
    void NotifyNavigationStateChanged(const NavigationState &_before) noexcept;

    std::deque<Entry> m_History;
    // lesser the index - farther the history entry
    // most recent entry is at .size()-1
    unsigned m_PlayingPosition = 0; // have meaningful value only when m_IsRecording==false
    bool m_IsRecording = true;
    std::string m_LastNativeDirectory;
    core::VFSInstanceManager *m_VFSMgr = nullptr;
    std::function<void()> m_NavigationStateChangeCallback;
    EntryId m_NextEntryId = 1;
    static constexpr size_t m_HistoryLength = 128;
};

} // namespace nc::panel
