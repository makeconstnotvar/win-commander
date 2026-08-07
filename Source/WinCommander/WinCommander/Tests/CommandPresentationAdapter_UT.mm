// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Bootstrap/NCEditMenuPresentationDelegate.h>
#include <WinCommander/States/CommandPresentationAdapter.h>
#include <WinCommander/States/FilePanels/Actions/CopyToPasteboard.h>
#include <Base/dispatch_cpp.h>

@interface CommandPresentationTestBundle : NSBundle
- (instancetype)initWithStrings:(NSDictionary<NSString *, NSString *> *)_strings;
@end

@implementation CommandPresentationTestBundle {
    NSDictionary<NSString *, NSString *> *m_Strings;
}

- (instancetype)initWithStrings:(NSDictionary<NSString *, NSString *> *)_strings
{
    self = [super init];
    if( self )
        m_Strings = [_strings copy];
    return self;
}

- (NSString *)localizedStringForKey:(NSString *)_key value:(NSString *)_value table:(NSString *) [[maybe_unused]] _table
{
    if( NSString *const localized = m_Strings[_key] )
        return localized;
    return _value != nil ? _value : _key;
}

@end

namespace {

using nc::core::CommandState;
using nc::core::CommandCheckState;
using nc::core::DisabledReason;
using nc::presentation::CommandPresentationAdapter;

CommandState DisabledState(std::string _key = "commands.file.copy.disabled.selectionEmpty")
{
    CommandState state;
    state.enabled = false;
    state.disabled_reason = DisabledReason{
        .code = "fixture.disabled",
        .user_message_key = std::move(_key),
        .technical_message = "INTERNAL TECHNICAL DETAIL",
    };
    return state;
}

CommandPresentationTestBundle *TestBundle()
{
    return [[CommandPresentationTestBundle alloc]
        initWithStrings:@{
            @"commands.disabled.generic" : @"Unavailable",
            @"commands.file.copy.title" : @"Localized Copy",
            @"commands.file.copy.disabled.selectionEmpty" : @"Select an item",
            @"commands.file.cut.title" : @"Localized Cut",
            @"commands.file.cut.disabled.selectionEmpty" : @"Select a local item",
        }];
}

NSEvent *KeyDown(NSString *_characters, NSEventModifierFlags _flags)
{
    return [NSEvent keyEventWithType:NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:_flags
                           timestamp:0
                        windowNumber:0
                             context:nil
                          characters:_characters
         charactersIgnoringModifiers:_characters
                           isARepeat:false
                             keyCode:0];
}

} // namespace

#define PREFIX "CommandPresentationAdapter "

TEST_CASE(PREFIX "applies one localized disabled reason to a button")
{
    REQUIRE(nc::dispatch_is_main_queue());
    NSButton *const button = [NSButton buttonWithTitle:@"Copy" target:nil action:nil];

    CommandPresentationAdapter::Apply(DisabledState(), button, TestBundle());

    CHECK_FALSE(button.enabled);
    CHECK_FALSE(button.hidden);
    CHECK([button.toolTip isEqualToString:@"Select an item"]);
    CHECK([button.accessibilityHelp isEqualToString:@"Select an item"]);
    CHECK_FALSE([button.toolTip containsString:@"INTERNAL TECHNICAL DETAIL"]);
}

TEST_CASE(PREFIX "uses the generic localized reason for empty invalid and missing keys")
{
    REQUIRE(nc::dispatch_is_main_queue());
    NSButton *const button = [NSButton buttonWithTitle:@"Copy" target:nil action:nil];
    CommandPresentationTestBundle *const bundle = TestBundle();

    for( std::string key : {std::string{}, std::string{"\xFF"}, std::string{"commands.missing.reason"}} ) {
        CommandPresentationAdapter::Apply(DisabledState(std::move(key)), button, bundle);
        CHECK([button.toolTip isEqualToString:@"Unavailable"]);
        CHECK([button.accessibilityHelp isEqualToString:@"Unavailable"]);
        CHECK_FALSE([button.toolTip containsString:@"commands."]);
        CHECK_FALSE([button.toolTip containsString:@"INTERNAL"]);
    }
}

TEST_CASE(PREFIX "clears stale button presentation for enabled and hidden states")
{
    REQUIRE(nc::dispatch_is_main_queue());
    NSButton *const button = [NSButton buttonWithTitle:@"Copy" target:nil action:nil];
    button.toolTip = @"stale";
    button.accessibilityHelp = @"stale";

    CommandPresentationAdapter::Apply(CommandState{}, button, TestBundle());
    CHECK(button.enabled);
    CHECK_FALSE(button.hidden);
    CHECK(button.toolTip == nil);
    CHECK(button.accessibilityHelp == nil);

    CommandState hidden = DisabledState();
    hidden.visible = false;
    CommandPresentationAdapter::Apply(hidden, button, TestBundle());
    CHECK_FALSE(button.enabled);
    CHECK(button.hidden);
    CHECK(button.toolTip == nil);
    CHECK(button.accessibilityHelp == nil);
}

TEST_CASE(PREFIX "maps semantic check state to button state")
{
    REQUIRE(nc::dispatch_is_main_queue());
    NSButton *const button = [NSButton checkboxWithTitle:@"Show Hidden Files" target:nil action:nil];
    CommandState state;

    state.check_state = CommandCheckState::On;
    CommandPresentationAdapter::Apply(state, button, TestBundle());
    CHECK(button.state == NSControlStateValueOn);
    CHECK_FALSE(button.allowsMixedState);

    state.check_state = CommandCheckState::Mixed;
    CommandPresentationAdapter::Apply(state, button, TestBundle());
    CHECK(button.state == NSControlStateValueMixed);
    CHECK(button.allowsMixedState);

    state.check_state = CommandCheckState::Off;
    CommandPresentationAdapter::Apply(state, button, TestBundle());
    CHECK(button.state == NSControlStateValueOff);
    CHECK_FALSE(button.allowsMixedState);
}

TEST_CASE(PREFIX "applies menu validation state without taking over AppKit enablement")
{
    REQUIRE(nc::dispatch_is_main_queue());
    NSMenuItem *const item = [[NSMenuItem alloc] initWithTitle:@"Copy" action:nil keyEquivalent:@""];
    item.enabled = true;

    CHECK_FALSE(CommandPresentationAdapter::Apply(DisabledState(), item, TestBundle()));
    CHECK(item.enabled);
    CHECK_FALSE(item.hidden);
    CHECK([item.toolTip isEqualToString:@"Select an item"]);
    CHECK([item.accessibilityHelp isEqualToString:@"Select an item"]);

    CHECK(CommandPresentationAdapter::Apply(CommandState{}, item, TestBundle()));
    CHECK(item.enabled);
    CHECK_FALSE(item.hidden);
    CHECK(item.toolTip == nil);
    CHECK(item.accessibilityHelp == nil);
}

TEST_CASE(PREFIX "clears hidden menu reasons and returns disabled validation")
{
    REQUIRE(nc::dispatch_is_main_queue());
    NSMenuItem *const item = [[NSMenuItem alloc] initWithTitle:@"Copy" action:nil keyEquivalent:@""];
    CommandState hidden = DisabledState();
    hidden.visible = false;

    CHECK_FALSE(CommandPresentationAdapter::Apply(hidden, item, TestBundle()));
    CHECK(item.hidden);
    CHECK(item.toolTip == nil);
    CHECK(item.accessibilityHelp == nil);
}

TEST_CASE(PREFIX "maps semantic check state to menu item state")
{
    REQUIRE(nc::dispatch_is_main_queue());
    NSMenuItem *const item =
        [[NSMenuItem alloc] initWithTitle:@"Show Hidden Files" action:nil keyEquivalent:@""];
    CommandState state;

    state.check_state = CommandCheckState::On;
    CHECK(CommandPresentationAdapter::Apply(state, item, TestBundle()));
    CHECK(item.state == NSControlStateValueOn);

    state.check_state = CommandCheckState::Mixed;
    CHECK(CommandPresentationAdapter::Apply(state, item, TestBundle()));
    CHECK(item.state == NSControlStateValueMixed);

    state.enabled = false;
    state.check_state = CommandCheckState::Off;
    CHECK_FALSE(CommandPresentationAdapter::Apply(state, item, TestBundle()));
    CHECK(item.state == NSControlStateValueOff);
}

TEST_CASE(PREFIX "restores the shared Edit Cut, Copy and Paste items when their menu closes")
{
    REQUIRE(nc::dispatch_is_main_queue());
    NSMenu *const menu = [[NSMenu alloc] initWithTitle:@"Edit"];
    NSMenuItem *const cut_item =
        [[NSMenuItem alloc] initWithTitle:@"Вырезать" action:@selector(cut:) keyEquivalent:@"x"];
    NSMenuItem *const copy_item =
        [[NSMenuItem alloc] initWithTitle:@"Копировать" action:@selector(copy:) keyEquivalent:@"c"];
    NSMenuItem *const paste_item =
        [[NSMenuItem alloc] initWithTitle:@"Вставить" action:@selector(paste:) keyEquivalent:@"v"];
    NSMenuItem *const select_all_item =
        [[NSMenuItem alloc] initWithTitle:@"Выбрать все" action:@selector(selectAll:) keyEquivalent:@"a"];
    [menu addItem:cut_item];
    [menu addItem:copy_item];
    [menu addItem:paste_item];
    [menu addItem:select_all_item];
    NCEditMenuPresentationDelegate *const delegate =
        [[NCEditMenuPresentationDelegate alloc] initWithCutMenuItem:cut_item
                                                      copyMenuItem:copy_item
                                                     pasteMenuItem:paste_item
                                                 selectAllMenuItem:select_all_item];
    REQUIRE(delegate != nil);

    cut_item.title = @"Cut 2 Items";
    cut_item.hidden = true;
    cut_item.toolTip = @"Select a local item";
    cut_item.accessibilityHelp = @"Select a local item";
    copy_item.title = @"Copy 3 Items";
    copy_item.hidden = true;
    copy_item.toolTip = @"Select an item";
    copy_item.accessibilityHelp = @"Select an item";
    paste_item.title = @"Paste Files";
    paste_item.hidden = true;
    paste_item.toolTip = @"Choose a writable folder";
    paste_item.accessibilityHelp = @"Choose a writable folder";
    select_all_item.title = @"Select 7 Items";
    select_all_item.hidden = true;
    select_all_item.toolTip = @"No visible items";
    select_all_item.accessibilityHelp = @"No visible items";
    [delegate menuDidClose:menu];

    CHECK([cut_item.title isEqualToString:@"Вырезать"]);
    CHECK_FALSE(cut_item.hidden);
    CHECK(cut_item.toolTip == nil);
    CHECK(cut_item.accessibilityHelp == nil);
    CHECK([copy_item.title isEqualToString:@"Копировать"]);
    CHECK_FALSE(copy_item.hidden);
    CHECK(copy_item.toolTip == nil);
    CHECK(copy_item.accessibilityHelp == nil);
    CHECK([paste_item.title isEqualToString:@"Вставить"]);
    CHECK_FALSE(paste_item.hidden);
    CHECK(paste_item.toolTip == nil);
    CHECK(paste_item.accessibilityHelp == nil);
    CHECK([select_all_item.title isEqualToString:@"Выбрать все"]);
    CHECK_FALSE(select_all_item.hidden);
    CHECK(select_all_item.toolTip == nil);
    CHECK(select_all_item.accessibilityHelp == nil);
}

TEST_CASE(PREFIX "rejects a missing shared menu item")
{
    REQUIRE(nc::dispatch_is_main_queue());
    NSMenuItem *const cut_item =
        [[NSMenuItem alloc] initWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    NSMenuItem *const copy_item =
        [[NSMenuItem alloc] initWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    NSMenuItem *const paste_item =
        [[NSMenuItem alloc] initWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    NSMenuItem *const select_all_item =
        [[NSMenuItem alloc] initWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    CHECK([[NCEditMenuPresentationDelegate alloc] initWithCutMenuItem:nil
                                                        copyMenuItem:copy_item
                                                       pasteMenuItem:paste_item
                                                   selectAllMenuItem:select_all_item] == nil);
    CHECK([[NCEditMenuPresentationDelegate alloc] initWithCutMenuItem:cut_item
                                                        copyMenuItem:nil
                                                       pasteMenuItem:paste_item
                                                   selectAllMenuItem:select_all_item] == nil);
    CHECK([[NCEditMenuPresentationDelegate alloc] initWithCutMenuItem:cut_item
                                                        copyMenuItem:copy_item
                                                       pasteMenuItem:nil
                                                   selectAllMenuItem:select_all_item] == nil);
    CHECK([[NCEditMenuPresentationDelegate alloc] initWithCutMenuItem:cut_item
                                                        copyMenuItem:copy_item
                                                       pasteMenuItem:paste_item
                                                   selectAllMenuItem:nil] == nil);
}

TEST_CASE(PREFIX "recognizes only a matching key equivalent as a shortcut invocation")
{
    REQUIRE(nc::dispatch_is_main_queue());
    NSMenuItem *const item =
        [[NSMenuItem alloc] initWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    item.keyEquivalentModifierMask = NSEventModifierFlagCommand;

    CHECK(CommandPresentationAdapter::IsKeyEquivalentInvocation(item,
                                                                 KeyDown(@"c", NSEventModifierFlagCommand)));
    CHECK(CommandPresentationAdapter::IsKeyEquivalentInvocation(
        item, KeyDown(@"c", NSEventModifierFlagCommand | NSEventModifierFlagCapsLock)));
    CHECK_FALSE(CommandPresentationAdapter::IsKeyEquivalentInvocation(
        item, KeyDown(@"C", NSEventModifierFlagCommand | NSEventModifierFlagShift)));

    NSString *const function_key =
        [NSString stringWithFormat:@"%C", static_cast<unichar>(NSF2FunctionKey)];
    CHECK_FALSE(CommandPresentationAdapter::IsKeyEquivalentInvocation(
        item, KeyDown(function_key, NSEventModifierFlagControl | NSEventModifierFlagFunction)));
    CHECK_FALSE(CommandPresentationAdapter::IsKeyEquivalentInvocation(item, nil));

    NSMenuItem *const cut_item =
        [[NSMenuItem alloc] initWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    cut_item.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    CHECK(CommandPresentationAdapter::IsKeyEquivalentInvocation(cut_item,
                                                                 KeyDown(@"x", NSEventModifierFlagCommand)));
    CHECK_FALSE(CommandPresentationAdapter::IsKeyEquivalentInvocation(cut_item,
                                                                       KeyDown(@"c", NSEventModifierFlagCommand)));
}

TEST_CASE(PREFIX "restores a localized Copy title when an item snapshot becomes empty")
{
    REQUIRE(nc::dispatch_is_main_queue());
    NSMenuItem *const item =
        [[NSMenuItem alloc] initWithTitle:@"Copy 3 Items" action:@selector(copy:) keyEquivalent:@"c"];
    const std::span<const VFSListingItem> no_items;

    nc::panel::actions::UpdateCopyToPasteboardMenuItemTitle(no_items, item, TestBundle());

    CHECK([item.title isEqualToString:@"Localized Copy"]);
}

TEST_CASE(PREFIX "restores a localized Cut title when an item snapshot becomes empty")
{
    REQUIRE(nc::dispatch_is_main_queue());
    NSMenuItem *const item =
        [[NSMenuItem alloc] initWithTitle:@"Cut 3 Items" action:@selector(cut:) keyEquivalent:@"x"];
    const std::span<const VFSListingItem> no_items;

    nc::panel::actions::UpdateCutToPasteboardMenuItemTitle(no_items, item, TestBundle());

    CHECK([item.title isEqualToString:@"Localized Cut"]);
}
