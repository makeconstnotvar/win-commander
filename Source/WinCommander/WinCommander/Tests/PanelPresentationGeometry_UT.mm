// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <Cocoa/Cocoa.h>
#include <WinCommander/States/FilePanels/Gallery/Layout.h>
#include <WinCommander/States/FilePanels/Helpers/Pasteboard.h>
#include <WinCommander/States/FilePanels/List/PanelListViewGeometry.h>
#include <WinCommander/States/FilePanels/List/PanelListViewProjection.h>

using nc::panel::PanelListViewGeometry;
using nc::panel::PanelListViewGroupKey;
using nc::panel::PanelListViewGroupKind;
using nc::panel::PanelListViewProjection;
using nc::panel::PanelListViewProjectionItem;
using nc::panel::PanelListViewProjectionRow;
using nc::panel::PasteboardFileOperation;
using nc::panel::PasteboardSupport;
using nc::panel::gallery::BuildItemLayout;

#define PREFIX "Explorer presentation geometry "

TEST_CASE(PREFIX "Details uses a readable 28 point row")
{
    const PanelListViewGeometry geometry([NSFont systemFontOfSize:13.0], 1, 9);

    CHECK(geometry.LineHeight() == 28);
    CHECK(geometry.IconSize() == 16);
    CHECK(geometry.TextBaseLine() == 8);
    CHECK(geometry.FilenameOffsetInColumn() == 30);
}

TEST_CASE(PREFIX "Gallery item grows with icon scale and text lines")
{
    const auto compact = BuildItemLayout(32, 16, 4, 1);
    const auto large = BuildItemLayout(64, 16, 4, 2);

    CHECK(compact.icon_size == 32);
    CHECK(compact.text_lines == 1);
    CHECK(large.icon_size == 64);
    CHECK(large.text_lines == 2);
    CHECK(large.width > compact.width);
    CHECK(large.height > compact.height);
    CHECK(large.icon_left_margin + large.icon_size + large.icon_right_margin == large.width);
}

TEST_CASE(PREFIX "Details identity projection keeps model indices unchanged")
{
    PanelListViewProjection projection;
    projection.RebuildIdentity(3);

    CHECK(projection.IsIdentity());
    CHECK(projection.RowsCount() == 3);
    for( int index = 0; index < 3; ++index ) {
        CHECK(projection.SortedIndexForRow(index) == index);
        CHECK(projection.RowForSortedIndex(index) == index);
    }
    CHECK(projection.SortedIndexForRow(-1) == -1);
    CHECK(projection.SortedIndexForRow(3) == -1);
    CHECK(projection.RowForSortedIndex(-1) == -1);
    CHECK(projection.RowForSortedIndex(3) == -1);
}

TEST_CASE(PREFIX "Details grouped projection maps headers around model items")
{
    const PanelListViewGroupKey group_a{PanelListViewGroupKind::NameInitial, "A"};
    const PanelListViewGroupKey group_b{PanelListViewGroupKind::NameInitial, "B"};
    const std::vector<PanelListViewProjectionItem> items = {
        {.sorted_index = 0, .group = {}, .is_dotdot = true},
        {.sorted_index = 1, .group = group_a},
        {.sorted_index = 2, .group = group_a},
        {.sorted_index = 3, .group = group_b},
    };

    PanelListViewProjection projection;
    projection.RebuildGrouped(items);

    CHECK_FALSE(projection.IsIdentity());
    CHECK(projection.RowsCount() == 6);
    CHECK(projection.SortedIndexForRow(0) == 0);
    CHECK(projection.SortedIndexForRow(1) == -1);
    CHECK(projection.SortedIndexForRow(2) == 1);
    CHECK(projection.SortedIndexForRow(3) == 2);
    CHECK(projection.SortedIndexForRow(4) == -1);
    CHECK(projection.SortedIndexForRow(5) == 3);
    CHECK(projection.RowForSortedIndex(0) == 0);
    CHECK(projection.RowForSortedIndex(1) == 2);
    CHECK(projection.RowForSortedIndex(2) == 3);
    CHECK(projection.RowForSortedIndex(3) == 5);

    REQUIRE(projection.RowAt(1));
    CHECK(projection.RowAt(1)->kind == PanelListViewProjectionRow::Kind::GroupHeader);
    REQUIRE(projection.GroupAt(projection.RowAt(1)->group_index));
    CHECK(projection.GroupAt(projection.RowAt(1)->group_index)->header_row == 1);
    CHECK(projection.GroupAt(projection.RowAt(1)->group_index)->item_count == 2);

    REQUIRE(projection.RowAt(4));
    REQUIRE(projection.GroupAt(projection.RowAt(4)->group_index));
    CHECK(projection.GroupAt(projection.RowAt(4)->group_index)->header_row == 4);
    CHECK(projection.GroupAt(projection.RowAt(4)->group_index)->item_count == 1);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
TEST_CASE(PREFIX "Cut pasteboard marker selects move semantics")
{
    NSPasteboard *const pasteboard = [NSPasteboard pasteboardWithUniqueName];
    REQUIRE(pasteboard);

    [pasteboard clearContents];
    CHECK_FALSE(PasteboardSupport::CanReadFileList(pasteboard));
    CHECK(PasteboardSupport::FileOperation(pasteboard) == PasteboardFileOperation::Copy);

    // A marker without this process's nonce/changeCount/path snapshot must never turn Paste into Move.
    [pasteboard declareTypes:@[@"com.wincommander.file-list.move"] owner:nil];
    REQUIRE([pasteboard setData:NSData.data forType:@"com.wincommander.file-list.move"]);
    CHECK(PasteboardSupport::FileOperation(pasteboard) == PasteboardFileOperation::Copy);

    [pasteboard clearContents];
    [pasteboard declareTypes:@[NSFilenamesPboardType] owner:nil];
    REQUIRE([pasteboard setPropertyList:@[@"/tmp/a", @"/tmp/b"] forType:NSFilenamesPboardType]);
    CHECK(PasteboardSupport::CanReadFileList(pasteboard));
    REQUIRE(PasteboardSupport::MarkCurrentFileListForMove(pasteboard));
    CHECK(PasteboardSupport::FileOperation(pasteboard) == PasteboardFileOperation::Move);
    const auto cut_token = PasteboardSupport::CurrentCutToken(pasteboard);
    REQUIRE(cut_token);
    CHECK(PasteboardSupport::IsCutItem(pasteboard, "/tmp/a"));
    CHECK_FALSE(PasteboardSupport::IsCutItem(pasteboard, "/tmp/other"));

    // A Cut can be owned by only one move operation at a time.
    CHECK(PasteboardSupport::TryClaimCut(pasteboard, *cut_token));
    CHECK(PasteboardSupport::IsCutInFlight(pasteboard));
    CHECK_FALSE(PasteboardSupport::TryClaimCut(pasteboard, *cut_token));
    CHECK_FALSE(PasteboardSupport::CancelCut(pasteboard));
    CHECK(PasteboardSupport::ReleaseCut(pasteboard, *cut_token));
    CHECK_FALSE(PasteboardSupport::IsCutInFlight(pasteboard));

    // Cancelling Cut keeps the standard file list, so a later Paste remains a safe Copy.
    CHECK(PasteboardSupport::CancelCut(pasteboard));
    CHECK(PasteboardSupport::CanReadFileList(pasteboard));
    CHECK(PasteboardSupport::FileOperation(pasteboard) == PasteboardFileOperation::Copy);

    REQUIRE(PasteboardSupport::MarkCurrentFileListForMove(pasteboard));

    // Any clipboard replacement invalidates the process-local move intent.
    [pasteboard clearContents];
    [pasteboard declareTypes:@[NSFilenamesPboardType] owner:nil];
    REQUIRE([pasteboard setPropertyList:@[@"/tmp/c"] forType:NSFilenamesPboardType]);
    CHECK(PasteboardSupport::FileOperation(pasteboard) == PasteboardFileOperation::Copy);

    REQUIRE(PasteboardSupport::MarkCurrentFileListForMove(pasteboard));
    const auto replacement_token = PasteboardSupport::CurrentCutToken(pasteboard);
    REQUIRE(replacement_token);
    CHECK_FALSE(PasteboardSupport::ConsumeCut(pasteboard, *cut_token));
    CHECK(PasteboardSupport::TryClaimCut(pasteboard, *replacement_token));
    CHECK(PasteboardSupport::ConsumeCut(pasteboard, *replacement_token));
    CHECK_FALSE(PasteboardSupport::CanReadFileList(pasteboard));
    CHECK(PasteboardSupport::FileOperation(pasteboard) == PasteboardFileOperation::Copy);
    CHECK_FALSE(PasteboardSupport::ConsumeCut(pasteboard, *replacement_token));

    // Explicit Move reserves a normal file-list generation and consumes it exactly once.
    [pasteboard clearContents];
    [pasteboard declareTypes:@[NSFilenamesPboardType] owner:nil];
    REQUIRE([pasteboard setPropertyList:@[@"/tmp/d"] forType:NSFilenamesPboardType]);
    CHECK(PasteboardSupport::CanReadFileList(pasteboard));
    const auto move_token = PasteboardSupport::TryClaimCurrentFileListForMove(pasteboard);
    REQUIRE(move_token);
    CHECK(PasteboardSupport::IsFileListMoveInFlight(pasteboard));
    CHECK(PasteboardSupport::IsFileListMoveClaimCurrent(pasteboard, *move_token));
    CHECK_FALSE(PasteboardSupport::TryClaimCurrentFileListForMove(pasteboard));

    [pasteboard clearContents];
    [pasteboard declareTypes:@[NSFilenamesPboardType] owner:nil];
    REQUIRE([pasteboard setPropertyList:@[@"/tmp/e"] forType:NSFilenamesPboardType]);
    CHECK_FALSE(PasteboardSupport::IsFileListMoveClaimCurrent(pasteboard, *move_token));
    CHECK(PasteboardSupport::ReleaseFileListMove(pasteboard, *move_token));
    CHECK_FALSE(PasteboardSupport::IsFileListMoveInFlight(pasteboard));
    CHECK(PasteboardSupport::CanReadFileList(pasteboard));

    const auto replacement_move_token = PasteboardSupport::TryClaimCurrentFileListForMove(pasteboard);
    REQUIRE(replacement_move_token);
    CHECK(PasteboardSupport::ConsumeFileListMove(pasteboard, *replacement_move_token));
    CHECK_FALSE(PasteboardSupport::CanReadFileList(pasteboard));
    CHECK_FALSE(PasteboardSupport::TryClaimCurrentFileListForMove(pasteboard));
}
#pragma clang diagnostic pop
