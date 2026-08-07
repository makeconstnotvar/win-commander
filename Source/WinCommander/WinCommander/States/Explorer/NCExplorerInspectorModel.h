// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <WinCommander/Core/Metadata/FileMetadataSnapshot.h>
#include <WinCommander/Core/Pane/PaneSnapshot.h>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nc::explorer {

/** The mutually exclusive, toolkit-independent states rendered by the Explorer inspector. */
enum class InspectorState : uint8_t {
    Hidden,
    Empty,
    PaneLoading,
    Single,
    Multiple,
    PaneError
};

/**
 * Exact-pane read model for the Explorer Details / Preview inspector.
 *
 * Apply accepts only the pane identity supplied at construction. Foreign and stale snapshots are
 * rejected without replacing current presentation. Selected items take precedence over focus, and
 * the synthetic parent entry is never projected into inspector metadata. Refresh is an orthogonal
 * non-blocking flag over the last committed body; a non-failed visible error is retained as a banner.
 */
class InspectorModel
{
public:
    explicit constexpr InspectorModel(const core::PaneId _pane_id) noexcept : m_PaneId(_pane_id) {}

    [[nodiscard]] bool Apply(const core::PaneSnapshot &_snapshot);

    [[nodiscard]] constexpr InspectorState State() const noexcept { return m_State; }
    [[nodiscard]] constexpr bool IsRefreshing() const noexcept { return m_IsRefreshing; }
    [[nodiscard]] std::span<const core::FileMetadataSnapshot> Items() const noexcept { return m_Items; }
    /**
     * Exact presentation handle for embedded preview; populated only while State() is Single.
     * Items() remains authority-free. This handle must not enter Registry payloads or mutations.
     */
    [[nodiscard]] const VFSListingItem &PreviewItem() const noexcept { return m_PreviewItem; }
    [[nodiscard]] const std::optional<core::FileManagerError> &Error() const noexcept { return m_Error; }
    [[nodiscard]] constexpr std::optional<uint64_t> LastAcceptedRevision() const noexcept
    {
        return m_LastAcceptedRevision;
    }

private:
    core::PaneId m_PaneId;
    std::optional<uint64_t> m_LastAcceptedRevision;
    InspectorState m_State = InspectorState::Hidden;
    bool m_IsRefreshing = false;
    std::vector<core::FileMetadataSnapshot> m_Items;
    VFSListingItem m_PreviewItem;
    std::optional<core::FileManagerError> m_Error;
};

} // namespace nc::explorer
