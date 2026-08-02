// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandRegistry.h"
#include <functional>
#include <stdexcept>

namespace nc::core {

class NavigationUpExecutionError final : public std::runtime_error
{
public:
    NavigationUpExecutionError();
};

class NavigationRefreshExecutionError final : public std::runtime_error
{
public:
    NavigationRefreshExecutionError();
};

using NavigationUpExecutor = std::function<bool(void *native_target)>;
using NavigationRefreshExecutor = std::function<bool(void *native_target)>;

/**
 * Builds pure pane-local navigation commands. Command state is derived from borrowed availability;
 * each injected executor owns the mandatory live validation and synchronous action submission.
 */
[[nodiscard]] CommandRegistry::Registration MakeNavigationUpCommand(NavigationUpExecutor _executor);
[[nodiscard]] CommandRegistry::Registration
MakeNavigationRefreshCommand(NavigationRefreshExecutor _executor);

} // namespace nc::core
