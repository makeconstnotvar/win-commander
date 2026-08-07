// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "../source/Copying/FileAlreadyExistDialog.h"
#include "../source/ModalDialogResponses.h"

@interface NCOpsFileAlreadyExistDialog (Q18TestingActions)
- (IBAction)OnOverwrite:(id)_sender;
- (IBAction)OnOverwriteOlder:(id)_sender;
- (IBAction)OnSkip:(id)_sender;
- (IBAction)OnAppend:(id)_sender;
- (IBAction)OnCancel:(id)_sender;
- (IBAction)OnKeepBoth:(id)_sender;
- (void)endDialogWithReturnCode:(NSInteger)_returnCode;
@end

static NSInteger g_Q18LastReturnCode;

@interface Q18ConflictActionProbe : NCOpsFileAlreadyExistDialog
@end

@implementation Q18ConflictActionProbe
- (void)endDialogWithReturnCode:(NSInteger)_returnCode
{
    g_Q18LastReturnCode = _returnCode;
}
@end

namespace FileAlreadyExistDialogTests {

using namespace nc::ops;

static NSView *FindView(NSView *_root, NSString *_identifier)
{
    if( [_root.accessibilityIdentifier isEqualToString:_identifier] )
        return _root;
    for( NSView *child in _root.subviews )
        if( NSView *match = FindView(child, _identifier) )
            return match;
    return nil;
}

static NCOpsFileAlreadyExistDialog *MakeDialog(std::shared_ptr<AsyncDialogResponse> _context)
{
    struct stat source = {};
    source.st_size = 123;
    source.st_mtime = 1'700'000'000;
    struct stat destination = {};
    destination.st_size = 456;
    destination.st_mtime = 1'710'000'000;
    return [[NCOpsFileAlreadyExistDialog alloc] initWithDestPath:"/destination/report.txt"
                                                     withSourceStat:source
                                                withDestinationStat:destination
                                                         andContext:std::move(_context)];
}

static Q18ConflictActionProbe *MakeActionProbe()
{
    struct stat source = {};
    struct stat destination = {};
    return [[Q18ConflictActionProbe alloc] initWithDestPath:"/destination/report.txt"
                                            withSourceStat:source
                                       withDestinationStat:destination
                                                andContext:std::make_shared<AsyncDialogResponse>()];
}

#define PREFIX "FileAlreadyExistDialog: "

TEST_CASE(PREFIX "exposes explicit accessible conflict actions with a destructive Replace")
{
    @autoreleasepool {
        const auto dialog = MakeDialog(std::make_shared<AsyncDialogResponse>());
        dialog.allowAppending = true;
        dialog.allowKeepingBoth = true;
        dialog.singleItem = false;

        NSWindow *const window = dialog.window;
        REQUIRE(window != nil);
        REQUIRE([window.accessibilityIdentifier isEqualToString:@"wincommander.operation.conflict.window"]);
        CHECK(window.accessibilityLabel.length > 0);

        auto button = [&](NSString *identifier) -> NSButton * {
            NSView *const view = FindView(window.contentView, identifier);
            return [view isKindOfClass:NSButton.class] ? static_cast<NSButton *>(view) : nil;
        };

        NSButton *const replace = button(@"wincommander.operation.conflict.replace");
        NSButton *const skip = button(@"wincommander.operation.conflict.skip");
        NSButton *const keep_both = button(@"wincommander.operation.conflict.keepBoth");
        NSButton *const append = button(@"wincommander.operation.conflict.append");
        NSButton *const cancel = button(@"wincommander.operation.conflict.cancel");
        NSButton *const apply_to_all = button(@"wincommander.operation.conflict.applyToAll");
        REQUIRE(replace != nil);
        REQUIRE(skip != nil);
        REQUIRE(keep_both != nil);
        REQUIRE(append != nil);
        REQUIRE(cancel != nil);
        REQUIRE(apply_to_all != nil);

        CHECK(([replace.title isEqualToString:@"Replace"] || [replace.title isEqualToString:@"Заменить"]));
        CHECK([replace.accessibilityLabel isEqualToString:replace.title]);
        CHECK(replace.action == @selector(OnOverwrite:));
        CHECK(skip.action == @selector(OnSkip:));
        CHECK(keep_both.action == @selector(OnKeepBoth:));
        CHECK(append.action == @selector(OnAppend:));
        CHECK(cancel.action == @selector(OnCancel:));
        if( @available(macOS 11.0, *) )
            CHECK(replace.hasDestructiveAction);

        auto options = static_cast<NSPopUpButton *>(
            FindView(window.contentView, @"wincommander.operation.conflict.replaceOptions"));
        REQUIRE([options isKindOfClass:NSPopUpButton.class]);
        REQUIRE(options.lastItem != nil);
        CHECK(([options.lastItem.title isEqualToString:@"Replace Older"] ||
               [options.lastItem.title isEqualToString:@"Заменить старый"]));
        CHECK(options.lastItem.action == @selector(OnOverwriteOlder:));

        CHECK_FALSE(apply_to_all.hidden);
        CHECK(apply_to_all.enabled);
        CHECK(apply_to_all.state == NSControlStateValueOff);
    }
}

TEST_CASE(PREFIX "single item removes every Apply to all authority")
{
    @autoreleasepool {
        const auto context = std::make_shared<AsyncDialogResponse>();
        const auto dialog = MakeDialog(context);
        dialog.singleItem = true;

        NSWindow *const window = dialog.window;
        REQUIRE(window != nil);
        auto apply_to_all = static_cast<NSButton *>(
            FindView(window.contentView, @"wincommander.operation.conflict.applyToAll"));
        REQUIRE([apply_to_all isKindOfClass:NSButton.class]);
        CHECK(apply_to_all.hidden);
        CHECK_FALSE(apply_to_all.enabled);
        CHECK(apply_to_all.state == NSControlStateValueOff);

        apply_to_all.state = NSControlStateValueOn;
        [dialog endDialogWithReturnCode:nc::ops::NSModalResponseSkip];
        CHECK_FALSE(context->IsApplyToAllSet());

        apply_to_all.state = NSControlStateValueOn;
        dialog.singleItem = false;
        CHECK_FALSE(apply_to_all.hidden);
        CHECK(apply_to_all.enabled);
        dialog.singleItem = true;
        CHECK(apply_to_all.hidden);
        CHECK_FALSE(apply_to_all.enabled);
        CHECK(apply_to_all.state == NSControlStateValueOff);
    }
}

TEST_CASE(PREFIX "retains exact legacy decision response mappings")
{
    @autoreleasepool {
        const auto dialog = MakeActionProbe();

        [dialog OnOverwrite:nil];
        CHECK(g_Q18LastReturnCode == nc::ops::NSModalResponseOverwrite);
        [dialog OnOverwriteOlder:nil];
        CHECK(g_Q18LastReturnCode == nc::ops::NSModalResponseOverwriteOld);
        [dialog OnSkip:nil];
        CHECK(g_Q18LastReturnCode == nc::ops::NSModalResponseSkip);
        [dialog OnKeepBoth:nil];
        CHECK(g_Q18LastReturnCode == nc::ops::NSModalResponseKeepBoth);
        [dialog OnAppend:nil];
        CHECK(g_Q18LastReturnCode == nc::ops::NSModalResponseAppend);
        [dialog OnCancel:nil];
        CHECK(g_Q18LastReturnCode == nc::ops::NSModalResponseStop);
    }
}

#undef PREFIX

} // namespace FileAlreadyExistDialogTests
