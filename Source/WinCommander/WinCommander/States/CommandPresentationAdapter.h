// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <WinCommander/Core/Commands/Command.h>
#include <Cocoa/Cocoa.h>

namespace nc::presentation {

/** Applies toolkit-independent CommandState to AppKit command surfaces. */
class CommandPresentationAdapter final
{
public:
    /** Uses NSBundle.mainBundle for localization. Must be called on the main queue. */
    static void Apply(const core::CommandState &_state, NSButton *_button);
    /** Explicit bundle overload for deterministic presentation tests. */
    static void Apply(const core::CommandState &_state, NSButton *_button, NSBundle *_bundle);

    /** Returns the value expected by NSMenuItemValidation. Must be called on the main queue. */
    [[nodiscard]] static bool Apply(const core::CommandState &_state, NSMenuItem *_item);
    /** Explicit bundle overload for deterministic presentation tests. */
    [[nodiscard]] static bool Apply(const core::CommandState &_state, NSMenuItem *_item, NSBundle *_bundle);

    /** Clears transient disabled-reason presentation without changing title or enabled state. */
    static void Clear(NSButton *_button);
    static void Clear(NSMenuItem *_item);

    /** True only when the key-down event matches the menu item's configured key equivalent. */
    [[nodiscard]] static bool IsKeyEquivalentInvocation(NSMenuItem *_item, NSEvent *_event) noexcept;
};

} // namespace nc::presentation
