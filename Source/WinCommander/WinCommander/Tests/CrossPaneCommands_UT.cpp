// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Commands/CrossPaneCommands.h>

namespace {

using nc::core::CrossPaneAvailability;
using nc::core::CrossPaneCommand;
using nc::core::CrossPaneRefusal;
using nc::core::CrossPaneSide;
using nc::core::EvaluateCrossPaneCommand;

CrossPaneSide Side(const std::string_view _path,
                   const bool _writable = true,
                   const size_t _items = 1,
                   const std::string_view _provider = "native")
{
    return CrossPaneSide{.is_uniform = true,
                         .is_writable = _writable,
                         .actionable_items = _items,
                         .path = _path,
                         .provider_id = _provider};
}

} // namespace

#define PREFIX "nc::core::EvaluateCrossPaneCommand "

TEST_CASE(PREFIX "offers every transfer when both sides can play their part")
{
    const auto left = Side("/left");
    const auto right = Side("/right");
    for( const auto command : {CrossPaneCommand::CopyToOpposite,
                               CrossPaneCommand::CopyFromOpposite,
                               CrossPaneCommand::MoveToOpposite,
                               CrossPaneCommand::MoveFromOpposite,
                               CrossPaneCommand::SwapPanes} ) {
        CHECK(EvaluateCrossPaneCommand(command, left, &right).Enabled());
    }
}

TEST_CASE(PREFIX "asks writability of whichever side actually receives")
{
    const auto writable = Side("/left");
    const auto read_only = Side("/right", false);

    // A copy needs the receiving side writable, and does not care about the giving one.
    CHECK_FALSE(EvaluateCrossPaneCommand(CrossPaneCommand::CopyToOpposite, writable, &read_only).Enabled());
    CHECK(EvaluateCrossPaneCommand(CrossPaneCommand::CopyToOpposite, read_only, &writable).Enabled());
    CHECK(EvaluateCrossPaneCommand(CrossPaneCommand::CopyFromOpposite, writable, &read_only).Enabled());
    CHECK(EvaluateCrossPaneCommand(CrossPaneCommand::CopyToOpposite, writable, &read_only).refusal ==
          CrossPaneRefusal::DestinationReadOnly);
}

TEST_CASE(PREFIX "will not offer a move it could only half-perform")
{
    // A move has to remove the originals. Offering one that can copy but not delete would leave the
    // user with two copies and a failure, which is worse than not offering it at all.
    const auto writable = Side("/left");
    const auto read_only = Side("/right", false);

    const auto from_read_only = EvaluateCrossPaneCommand(CrossPaneCommand::MoveFromOpposite, writable, &read_only);
    CHECK_FALSE(from_read_only.Enabled());
    CHECK(from_read_only.refusal == CrossPaneRefusal::SourceReadOnly);

    // The same pair is fine for a copy in that direction.
    CHECK(EvaluateCrossPaneCommand(CrossPaneCommand::CopyFromOpposite, writable, &read_only).Enabled());
}

TEST_CASE(PREFIX "needs something to act on, on the side the items come from")
{
    const auto empty = Side("/left", true, 0);
    const auto full = Side("/right");

    CHECK(EvaluateCrossPaneCommand(CrossPaneCommand::CopyToOpposite, empty, &full).refusal ==
          CrossPaneRefusal::NothingToAct);
    // Coming the other way, the selection that matters is the opposite pane's.
    CHECK(EvaluateCrossPaneCommand(CrossPaneCommand::CopyFromOpposite, empty, &full).Enabled());
    CHECK(EvaluateCrossPaneCommand(CrossPaneCommand::CopyFromOpposite, full, &empty).refusal ==
          CrossPaneRefusal::NothingToAct);
}

TEST_CASE(PREFIX "refuses a transfer between two views of one directory, and only a transfer")
{
    const auto here = Side("/same");
    const auto also_here = Side("/same");

    for( const auto command : {CrossPaneCommand::CopyToOpposite,
                               CrossPaneCommand::CopyFromOpposite,
                               CrossPaneCommand::MoveToOpposite,
                               CrossPaneCommand::MoveFromOpposite} ) {
        CHECK(EvaluateCrossPaneCommand(command, here, &also_here).refusal == CrossPaneRefusal::SameDirectory);
    }
    // Swapping two panes that happen to show one directory is harmless and still does what it says.
    CHECK(EvaluateCrossPaneCommand(CrossPaneCommand::SwapPanes, here, &also_here).Enabled());
}

TEST_CASE(PREFIX "treats one path in two providers as two directories")
{
    // The most ordinary thing a two-pane file manager does: copy between a local folder and its
    // counterpart on a remote host. Comparing paths alone would refuse it.
    const auto local = Side("/work", true, 1, "native");
    const auto remote = Side("/work", true, 1, "sftp");
    CHECK(EvaluateCrossPaneCommand(CrossPaneCommand::CopyToOpposite, local, &remote).Enabled());
    CHECK(EvaluateCrossPaneCommand(CrossPaneCommand::MoveToOpposite, local, &remote).Enabled());
}

TEST_CASE(PREFIX "refuses a listing that is not a single directory")
{
    // Search results have no "here" to copy from or into.
    CrossPaneSide non_uniform = Side("/left");
    non_uniform.is_uniform = false;
    const auto other = Side("/right");

    CHECK(EvaluateCrossPaneCommand(CrossPaneCommand::CopyToOpposite, non_uniform, &other).refusal ==
          CrossPaneRefusal::NotUniform);
    CHECK(EvaluateCrossPaneCommand(CrossPaneCommand::CopyFromOpposite, other, &non_uniform).refusal ==
          CrossPaneRefusal::NotUniform);
    // Swap does not move data, so it does not care.
    CHECK(EvaluateCrossPaneCommand(CrossPaneCommand::SwapPanes, non_uniform, &other).Enabled());
}

TEST_CASE(PREFIX "offers nothing across panes when there is no second pane")
{
    const auto only = Side("/left");
    for( const auto command : {CrossPaneCommand::CopyToOpposite,
                               CrossPaneCommand::CopyFromOpposite,
                               CrossPaneCommand::MoveToOpposite,
                               CrossPaneCommand::MoveFromOpposite,
                               CrossPaneCommand::SwapPanes} ) {
        const auto availability = EvaluateCrossPaneCommand(command, only, nullptr);
        CHECK_FALSE(availability.Enabled());
        CHECK(availability.refusal == CrossPaneRefusal::NoOppositePane);
    }
}

#undef PREFIX
