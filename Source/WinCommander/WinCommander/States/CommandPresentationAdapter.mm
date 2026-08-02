// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CommandPresentationAdapter.h"

#include <Base/dispatch_cpp.h>
#include <WinCommander/Core/VisualState/VisualStateMapper.h>

namespace nc::presentation {

namespace {

NSString *const g_GenericDisabledReasonKey = @"commands.disabled.generic";
NSString *const g_GenericDisabledReasonFallback = @"This command is currently unavailable";

[[nodiscard]] NSString *LocalizedValue(const std::string &_key, NSBundle *_bundle)
{
    if( _key.empty() )
        return nil;

    NSString *const key = [[NSString alloc] initWithBytes:_key.data()
                                                   length:_key.size()
                                                 encoding:NSUTF8StringEncoding];
    if( key.length == 0 )
        return nil;

    NSString *const value = [_bundle localizedStringForKey:key value:nil table:nil];
    if( value.length == 0 || [value isEqualToString:key] )
        return nil;
    return value;
}

[[nodiscard]] NSString *GenericDisabledReason(NSBundle *_bundle)
{
    NSString *const value = [_bundle localizedStringForKey:g_GenericDisabledReasonKey
                                                     value:g_GenericDisabledReasonFallback
                                                     table:nil];
    if( value.length == 0 || [value isEqualToString:g_GenericDisabledReasonKey] )
        return g_GenericDisabledReasonFallback;
    return value;
}

[[nodiscard]] NSString *DisabledReason(const core::CommandVisualState &_visual, NSBundle *_bundle)
{
    if( !_visual.visible || _visual.enabled )
        return nil;

    if( _visual.disabled_message ) {
        if( NSString *const localized = LocalizedValue(_visual.disabled_message->user_message_key, _bundle) )
            return localized;
    }
    return GenericDisabledReason(_bundle);
}

[[nodiscard]] NSBundle *EffectiveBundle(NSBundle *_bundle)
{
    return _bundle != nil ? _bundle : NSBundle.mainBundle;
}

[[nodiscard]] NSControlStateValue ControlState(const core::CommandCheckState _state) noexcept
{
    switch( _state ) {
        case core::CommandCheckState::Off:
            return NSControlStateValueOff;
        case core::CommandCheckState::On:
            return NSControlStateValueOn;
        case core::CommandCheckState::Mixed:
            return NSControlStateValueMixed;
    }
    return NSControlStateValueOff;
}

} // namespace

void CommandPresentationAdapter::Apply(const core::CommandState &_state, NSButton *_button)
{
    Apply(_state, _button, NSBundle.mainBundle);
}

void CommandPresentationAdapter::Apply(const core::CommandState &_state, NSButton *_button, NSBundle *_bundle)
{
    dispatch_assert_main_queue();
    if( _button == nil )
        return;

    const core::CommandVisualState visual = core::VisualStateMapper::MapCommand(_state);
    NSString *const disabled_reason = DisabledReason(visual, EffectiveBundle(_bundle));
    _button.hidden = !visual.visible;
    _button.enabled = visual.visible && visual.enabled;
    _button.allowsMixedState = visual.check_state == core::CommandCheckState::Mixed;
    _button.state = ControlState(visual.check_state);
    _button.toolTip = disabled_reason;
    _button.accessibilityHelp = disabled_reason;
}

bool CommandPresentationAdapter::Apply(const core::CommandState &_state, NSMenuItem *_item)
{
    return Apply(_state, _item, NSBundle.mainBundle);
}

bool CommandPresentationAdapter::Apply(const core::CommandState &_state, NSMenuItem *_item, NSBundle *_bundle)
{
    dispatch_assert_main_queue();
    if( _item == nil )
        return false;

    const core::CommandVisualState visual = core::VisualStateMapper::MapCommand(_state);
    NSString *const disabled_reason = DisabledReason(visual, EffectiveBundle(_bundle));
    _item.hidden = !visual.visible;
    _item.state = ControlState(visual.check_state);
    _item.toolTip = disabled_reason;
    _item.accessibilityHelp = disabled_reason;
    return visual.visible && visual.enabled;
}

void CommandPresentationAdapter::Clear(NSButton *_button)
{
    dispatch_assert_main_queue();
    _button.toolTip = nil;
    _button.accessibilityHelp = nil;
}

void CommandPresentationAdapter::Clear(NSMenuItem *_item)
{
    dispatch_assert_main_queue();
    _item.toolTip = nil;
    _item.accessibilityHelp = nil;
}

bool CommandPresentationAdapter::IsKeyEquivalentInvocation(NSMenuItem *_item, NSEvent *_event) noexcept
{
    if( _item == nil || _event.type != NSEventTypeKeyDown || _item.keyEquivalent.length == 0 )
        return false;

    // Keep this normalization aligned with utility::ActionShortcut: Caps Lock, numeric-pad and Fn
    // describe the keyboard event but are not configurable shortcut modifiers.
    constexpr NSEventModifierFlags mask = NSEventModifierFlagDeviceIndependentFlagsMask &
                                          (~NSEventModifierFlagCapsLock & ~NSEventModifierFlagNumericPad &
                                           ~NSEventModifierFlagFunction);
    if( (_event.modifierFlags & mask) != (_item.keyEquivalentModifierMask & mask) )
        return false;

    NSString *event_key = _event.charactersIgnoringModifiers;
    NSString *item_key = _item.keyEquivalent;
    if( event_key.length == 1 && item_key.length == 1 ) {
        const unichar event_character = [event_key characterAtIndex:0];
        const unichar item_character = [item_key characterAtIndex:0];
        if( event_character == NSDeleteCharacter && item_character == NSBackspaceCharacter )
            return true;
        if( event_character == NSDeleteFunctionKey && item_character == NSDeleteCharacter )
            return true;
    }
    return [event_key.lowercaseString isEqualToString:item_key.lowercaseString];
}

} // namespace nc::presentation
