// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nc::core {

/** The cross-pane commands the dual-pane command bar offers (spec §28.2). */
enum class CrossPaneCommand : uint8_t {
    CopyToOpposite,
    CopyFromOpposite,
    MoveToOpposite,
    MoveFromOpposite,
    SwapPanes
};

/** What one pane contributes to the decision. */
struct CrossPaneSide {
    /** A single directory. Search results and other non-uniform listings have no "here". */
    bool is_uniform = false;
    bool is_writable = false;
    /** Selected entries, or the focused one when nothing is selected. Excludes "..". */
    size_t actionable_items = 0;
    /** Absolute directory path as the provider reports it. */
    std::string_view path;
    /** Provider identity, so two panes on one path in different providers stay distinct. */
    std::string_view provider_id;

    friend bool operator==(const CrossPaneSide &, const CrossPaneSide &) = default;
};

/** Why a cross-pane command is not on offer. */
enum class CrossPaneRefusal : uint8_t {
    /** It is. */
    None,
    /** There is no second pane to move anything to. */
    NoOppositePane,
    /** One of the two listings is not a single directory. */
    NotUniform,
    /** Nothing is selected or focused on the side the items would come from. */
    NothingToAct,
    /** The receiving side cannot be written to. */
    DestinationReadOnly,
    /** A move needs to remove the originals, and that side will not allow it. */
    SourceReadOnly,
    /** Both panes are showing the same directory, so the command would mean nothing. */
    SameDirectory
};

struct CrossPaneAvailability {
    CrossPaneRefusal refusal = CrossPaneRefusal::None;

    [[nodiscard]] bool Enabled() const noexcept { return refusal == CrossPaneRefusal::None; }

    friend bool operator==(const CrossPaneAvailability &, const CrossPaneAvailability &) = default;
};

/**
 * Whether a cross-pane command may be offered, and why not when it may not.
 *
 * The command bar and the function keys ask the same question, so they answer it the same way -
 * a button that is enabled while its key does nothing, or the reverse, is the kind of disagreement
 * a user reads as the application being broken.
 *
 * The decisions worth naming:
 *
 * - **Direction decides which side must be writable.** A copy needs the *receiving* side writable;
 *   a move needs that and the *giving* side too, because it has to remove the originals. Offering a
 *   move that can copy but not delete would leave the user with two copies and a failure.
 * - **Same-directory is refused for the transfers, and only for them.** Copying a folder onto
 *   itself is not a thing to offer; swapping two panes that show the same directory is harmless and
 *   still does what it says.
 * - **"Same" is by provider *and* path.** The same path in two different providers - a local folder
 *   and its counterpart on a remote host - is not the same directory, and refusing that transfer
 *   would block the most ordinary thing a two-pane file manager does.
 * - **Swap asks almost nothing.** It moves no data, so it needs neither writability nor a
 *   selection; only a second pane to swap with.
 */
[[nodiscard]] CrossPaneAvailability
EvaluateCrossPaneCommand(CrossPaneCommand _command, const CrossPaneSide &_active, const CrossPaneSide *_opposite);

} // namespace nc::core
