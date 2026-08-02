// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "PaneLifecycleProducer.h"

#include <type_traits>

namespace nc::core {

enum class PaneLifecycleReducerStatus : uint8_t {
    Applied,
    NoVisibleChange,
    WrongPane,
    StaleSequence,
    SequenceGap,
    InvalidTransition,
    InvalidCommittedProjection,
    StaleCommittedProjection,
    CommitProjectionMismatch
};

struct PaneLifecycleReducerResult {
    PaneLifecycleReducerStatus status = PaneLifecycleReducerStatus::InvalidTransition;
    bool state_changed = false;

    bool operator==(const PaneLifecycleReducerResult &) const noexcept = default;
};

/**
 * Pure reducer that composes committed Panel model projections with one pane's ordered lifecycle.
 *
 * Committed projections contain only Empty/Loaded engine state. Lifecycle overlays Loading,
 * Refreshing, navigation failure and typed visible error without mutating the retained committed
 * location/listing. The first observed event may start at any positive sequence; subsequent events
 * must be strictly contiguous. Mutating methods publish a fully prepared candidate atomically; an
 * exception while copying owned values leaves the reducer unchanged.
 */
class PaneLifecycleReducer final
{
public:
    PaneLifecycleReducer(PaneId _pane_id, PaneState _initial_committed_state);

    [[nodiscard]] PaneId Pane() const noexcept;
    [[nodiscard]] const PaneState &State() const noexcept;

    /**
     * Seeds an accepted request returned by PaneLifecycleProducer::Active() before observation
     * starts. No synthetic event sequence is invented, so its terminal may be the first observed
     * event at any positive sequence.
     */
    [[nodiscard]] PaneLifecycleReducerResult SeedActive(PaneActiveRequest _active_request);

    /**
     * Initializes a joining reducer from an accepted historical failure. The event keeps its
     * original identity; checkpoint_sequence advances the private cursor across events that
     * occurred before live observation began, including non-authoritative rejections.
     */
    [[nodiscard]] PaneLifecycleReducerResult SeedRetainedFailure(
        PaneActiveRequest _failed_request,
        const PaneLifecycleEvent &_failure,
        uint64_t _checkpoint_sequence);

    /** Merges a context/model projection without inferring lifecycle from that projection. */
    [[nodiscard]] PaneLifecycleReducerResult UpdateCommittedProjection(PaneState _committed_state);

    /**
     * Applies one producer event. Committed requires the exact post-model-commit projection; other
     * events ignore the optional projection.
     */
    [[nodiscard]] PaneLifecycleReducerResult Apply(
        const PaneLifecycleEvent &_event,
        const PaneState *_post_commit_projection = nullptr);

private:
    enum class SequenceValidation : uint8_t {
        Current,
        Stale,
        Gap
    };

    struct OwnedState {
        PaneState committed_state;
        PaneState state;
        std::optional<uint64_t> last_event_sequence;
        std::optional<PaneActiveRequest> active_request;
        std::optional<PaneRequestId> expected_replacement;
        std::optional<FileManagerError> visible_error;
        bool navigation_failed = false;
        bool commit_projection_failed = false;
    };

    static_assert(std::is_nothrow_move_constructible_v<OwnedState>);
    static_assert(std::is_nothrow_move_assignable_v<OwnedState>);

    [[nodiscard]] SequenceValidation ValidateSequence(uint64_t _sequence) const noexcept;
    [[nodiscard]] bool MatchesActive(const PaneLifecycleEvent &_event) const noexcept;
    [[nodiscard]] static bool IsCommittedProjection(
        const PaneState &_state,
        const PaneState *_trusted_selection_projection = nullptr);
    static void Recompose(OwnedState &_state);
    static void ClearTerminalOverlay(OwnedState &_state) noexcept;
    [[nodiscard]] PaneLifecycleReducerResult Commit(OwnedState _next,
                                                    PaneLifecycleReducerStatus _status) noexcept;

    PaneId m_PaneId;
    OwnedState m_Owned;
};

} // namespace nc::core
