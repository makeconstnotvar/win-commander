// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Operations/VFSOperationPlanningProbes.h>

namespace nc::panel {
class DirectoryAccessProvider;
}

namespace nc::core {

/**
 * Builds the application permission-check boundary used by synchronous VFS operation preflight.
 * The provider must outlive the returned callback. Permission acquisition remains a separate,
 * explicitly user-initiated application action.
 */
[[nodiscard]] ops::VFSOperationPlanningProbes::AccessChecker
MakeVFSOperationPlanningAccessChecker(panel::DirectoryAccessProvider &_provider);

} // namespace nc::core
