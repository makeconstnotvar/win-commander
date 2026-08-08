// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/States/Explorer/NCExplorerCommandPaletteView.h>

#include <string>
#include <vector>

@interface ExplorerPaletteTestDelegate : NSObject <NCExplorerCommandPaletteDelegate>
@property(nonatomic) NSUInteger dismissCount;
@property(nonatomic) NSUInteger chooseCount;
- (const std::string &)lastChosenId;
@end

@implementation ExplorerPaletteTestDelegate {
    NSUInteger _dismissCount;
    NSUInteger _chooseCount;
    std::string m_LastChosenId;
}
@synthesize dismissCount = _dismissCount;
@synthesize chooseCount = _chooseCount;
- (const std::string &)lastChosenId
{
    return m_LastChosenId;
}
- (void)commandPalette:(NCExplorerCommandPaletteView *) [[maybe_unused]] _palette
    didChooseCommandId:(const std::string &)_command_id
{
    ++self.chooseCount;
    m_LastChosenId = _command_id;
}
- (void)commandPaletteDidDismiss:(NCExplorerCommandPaletteView *) [[maybe_unused]] _palette
{
    ++self.dismissCount;
}
@end

namespace {

using nc::core::CommandPaletteEntry;

struct Fixture {
    Fixture()
    {
        palette = [[NCExplorerCommandPaletteView alloc] initWithFrame:NSMakeRect(0, 0, 560, 400)];
        delegate = [ExplorerPaletteTestDelegate new];
        palette.paletteDelegate = delegate;
        [palette setRoster:{
                               {.id = "file.copy", .title = "Copy", .subtitle = "File", .enabled = true},
                               {.id = "file.copyPath", .title = "Copy Item Path", .subtitle = "File", .enabled = true},
                               {.id = "file.trash", .title = "Move to Trash", .subtitle = "File", .enabled = false},
                               {.id = "view.hidden", .title = "Show Hidden Files", .subtitle = "View", .enabled = true},
                           }];
    }

    __strong NCExplorerCommandPaletteView *palette;
    __strong ExplorerPaletteTestDelegate *delegate;
};

} // namespace

#define PREFIX "NCExplorerCommandPaletteView "

TEST_CASE(PREFIX "lists the whole roster and preselects the first row")
{
    Fixture fixture;
    CHECK([fixture.palette visibleCommandIdsForTesting].size() == 4);
    CHECK([fixture.palette selectedCommandIdForTesting] == "file.copy");
}

TEST_CASE(PREFIX "narrows to the query and moves the selection to the new best match")
{
    Fixture fixture;
    [fixture.palette setQueryForTesting:@"hidden"];

    const std::vector<std::string> visible = [fixture.palette visibleCommandIdsForTesting];
    REQUIRE(visible.size() == 1);
    CHECK(visible[0] == "view.hidden");
    // The selection follows the ranking rather than staying on a row the query no longer describes.
    CHECK([fixture.palette selectedCommandIdForTesting] == "view.hidden");
}

TEST_CASE(PREFIX "moves the selection with the arrow keys and clamps at both ends")
{
    Fixture fixture;
    CHECK([fixture.palette selectedCommandIdForTesting] == "file.copy");

    CHECK([fixture.palette moveSelectionByForTesting:1]);
    CHECK([fixture.palette selectedCommandIdForTesting] == "file.copyPath");

    // Clamping, not wrapping: a held arrow key must settle at an end rather than cycle past it.
    CHECK([fixture.palette moveSelectionByForTesting:-5]);
    CHECK([fixture.palette selectedCommandIdForTesting] == "file.copy");
    CHECK([fixture.palette moveSelectionByForTesting:99]);
    CHECK([fixture.palette selectedCommandIdForTesting] == "view.hidden");
}

TEST_CASE(PREFIX "commits the selected row exactly once, dismissing before it reports the choice")
{
    Fixture fixture;
    [fixture.palette setQueryForTesting:@"copy item"];
    REQUIRE([fixture.palette selectedCommandIdForTesting] == "file.copyPath");

    CHECK([fixture.palette commitSelectionForTesting]);
    CHECK(fixture.delegate.chooseCount == 1);
    CHECK(fixture.delegate.lastChosenId == "file.copyPath");
    // Dismissal precedes the choice so the command never runs underneath a still-open palette.
    CHECK(fixture.delegate.dismissCount == 1);
}

TEST_CASE(PREFIX "refuses to commit a disabled row or an empty result set")
{
    Fixture fixture;

    SECTION("a disabled row is listed but not runnable")
    {
        [fixture.palette setQueryForTesting:@"trash"];
        REQUIRE([fixture.palette selectedCommandIdForTesting] == "file.trash");
        // Committing must do nothing at all - silently closing would look like the command ran.
        CHECK_FALSE([fixture.palette commitSelectionForTesting]);
        CHECK(fixture.delegate.chooseCount == 0);
        CHECK(fixture.delegate.dismissCount == 0);
    }
    SECTION("a query that matches nothing has nothing to commit")
    {
        [fixture.palette setQueryForTesting:@"zzzzz"];
        CHECK([fixture.palette visibleCommandIdsForTesting].empty());
        CHECK([fixture.palette selectedCommandIdForTesting].empty());
        CHECK_FALSE([fixture.palette commitSelectionForTesting]);
        CHECK_FALSE([fixture.palette moveSelectionByForTesting:1]);
        CHECK(fixture.delegate.chooseCount == 0);
    }
}

TEST_CASE(PREFIX "resets the query when a new roster is installed")
{
    Fixture fixture;
    [fixture.palette setQueryForTesting:@"hidden"];
    REQUIRE([fixture.palette visibleCommandIdsForTesting].size() == 1);

    [fixture.palette setRoster:{{.id = "a.one", .title = "Alpha", .enabled = true},
                                {.id = "b.two", .title = "Beta", .enabled = true}}];

    // A stale query against a fresh roster would show a filtered view of commands the user never
    // typed against.
    CHECK([fixture.palette visibleCommandIdsForTesting].size() == 2);
    CHECK([fixture.palette selectedCommandIdForTesting] == "a.one");
}

TEST_CASE(PREFIX "tolerates an empty roster without offering anything")
{
    Fixture fixture;
    [fixture.palette setRoster:{}];
    CHECK([fixture.palette visibleCommandIdsForTesting].empty());
    CHECK([fixture.palette selectedCommandIdForTesting].empty());
    CHECK_FALSE([fixture.palette commitSelectionForTesting]);
    CHECK(fixture.delegate.chooseCount == 0);
}
