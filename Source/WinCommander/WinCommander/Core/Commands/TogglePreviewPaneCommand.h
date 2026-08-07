// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandRegistry.h"

#include <functional>

namespace nc::core {

/**
 * Synchronously updates a borrowed live pane only while its visibility still matches the expected
 * snapshot. Returns false when the target disappeared or the snapshot became stale.
 */
using PreviewPaneVisibilitySetter =
    std::function<bool(void *native_target, bool expected_visible, bool desired_visible)>;

/** Builds the pure view.togglePreviewPane Registry definition. */
[[nodiscard]] CommandRegistry::Registration
MakeViewTogglePreviewPaneCommand(PreviewPaneVisibilitySetter _setter);

} // namespace nc::core
