// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandRegistry.h"
#include <functional>
#include <stdexcept>

namespace nc::core {

enum class NavigationHistoryDirection {
    Back,
    Forward
};

class NavigationHistoryExecutionError final : public std::runtime_error
{
public:
    NavigationHistoryExecutionError();
};

using NavigationHistoryExecutor =
    std::function<bool(void *native_target, NavigationHistoryDirection direction)>;

/**
 * Builds pane-history navigation commands. Command state uses the borrowed availability projection;
 * the injected executor owns the mandatory live-history validation and synchronous move.
 */
[[nodiscard]] CommandRegistry::Registration MakeNavigationBackCommand(NavigationHistoryExecutor _executor);
[[nodiscard]] CommandRegistry::Registration MakeNavigationForwardCommand(NavigationHistoryExecutor _executor);

} // namespace nc::core
