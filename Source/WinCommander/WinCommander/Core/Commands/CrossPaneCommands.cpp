// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CrossPaneCommands.h"

namespace nc::core {

namespace {

CrossPaneAvailability Refuse(const CrossPaneRefusal _refusal) noexcept
{
    return CrossPaneAvailability{.refusal = _refusal};
}

/** Two panes showing the same directory - by provider as well as path. */
bool SamePlace(const CrossPaneSide &_a, const CrossPaneSide &_b) noexcept
{
    // The same path in two different providers is not the same directory. Treating it as one would
    // refuse the most ordinary thing a two-pane file manager does: copy between a local folder and
    // its counterpart on a remote host.
    return _a.provider_id == _b.provider_id && _a.path == _b.path && !_a.path.empty();
}

/** Judges a transfer once its giving and receiving sides are known. */
CrossPaneAvailability EvaluateTransfer(const CrossPaneSide &_from,
                                       const CrossPaneSide &_to,
                                       const bool _removes_originals) noexcept
{
    if( !_from.is_uniform || !_to.is_uniform )
        return Refuse(CrossPaneRefusal::NotUniform);
    if( SamePlace(_from, _to) )
        return Refuse(CrossPaneRefusal::SameDirectory);
    if( _from.actionable_items == 0 )
        return Refuse(CrossPaneRefusal::NothingToAct);
    if( !_to.is_writable )
        return Refuse(CrossPaneRefusal::DestinationReadOnly);
    // A move has to remove the originals. Offering one that can copy but not delete would leave the
    // user with two copies and a failure, which is worse than not offering it.
    if( _removes_originals && !_from.is_writable )
        return Refuse(CrossPaneRefusal::SourceReadOnly);
    return CrossPaneAvailability{};
}

} // namespace

CrossPaneAvailability EvaluateCrossPaneCommand(const CrossPaneCommand _command,
                                               const CrossPaneSide &_active,
                                               const CrossPaneSide *_opposite)
{
    if( _opposite == nullptr )
        return Refuse(CrossPaneRefusal::NoOppositePane);

    switch( _command ) {
        case CrossPaneCommand::CopyToOpposite:
            return EvaluateTransfer(_active, *_opposite, false);
        case CrossPaneCommand::CopyFromOpposite:
            return EvaluateTransfer(*_opposite, _active, false);
        case CrossPaneCommand::MoveToOpposite:
            return EvaluateTransfer(_active, *_opposite, true);
        case CrossPaneCommand::MoveFromOpposite:
            return EvaluateTransfer(*_opposite, _active, true);
        case CrossPaneCommand::SwapPanes:
            // Moves no data, so it needs neither writability nor a selection - and swapping two
            // panes that happen to show one directory is harmless and still does what it says.
            return CrossPaneAvailability{};
    }
    return Refuse(CrossPaneRefusal::NoOppositePane);
}

} // namespace nc::core
