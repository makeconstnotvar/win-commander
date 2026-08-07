// Copyright (C) 2017-2020 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "DefaultAction.h"

namespace nc::vfs {
class NativeHost;
}

namespace nc::panel::actions {

// extract additional state from NSPasteboard.generalPasteboard

enum class PasteSubmissionResult {
    Submitted,
    PaneUnavailable,
    WindowUnavailable,
    DestinationUnavailable,
    DestinationReadOnly,
    ClipboardUnavailable,
    ClipboardBusy,
    ClipboardChanged,
    SourceUnavailable
};

struct PasteFromPasteboard final : PanelAction {
    PasteFromPasteboard(nc::vfs::NativeHost &_native_host);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    /** Performs one live paste attempt and reports whether Copying was submitted. */
    [[nodiscard]] PasteSubmissionResult Execute(PanelController *_target, NSPasteboard *_pasteboard = nil) const;
    void Perform(PanelController *_target, id _sender) const override;

private:
    nc::vfs::NativeHost &m_NativeHost;
};

struct MoveFromPasteboard final : PanelAction {
    MoveFromPasteboard(nc::vfs::NativeHost &_native_host);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;

private:
    nc::vfs::NativeHost &m_NativeHost;
};

}; // namespace nc::panel::actions
