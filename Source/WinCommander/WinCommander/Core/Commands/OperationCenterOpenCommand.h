// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandRegistry.h"

#include <Operations/OperationCenterModel.h>
#include <functional>
#include <optional>
#include <vector>

namespace nc::core {

/**
 * Supplies one immutable value snapshot from the process-owned Operation Center model. The
 * command never receives the coordinator, Journal, Pool, or any executor authority.
 */
using OperationCenterOpenSnapshotProvider = std::function<std::optional<std::vector<ops::OperationRecord>>()>;

/**
 * Reports whether the snapshot provider can currently serve a presentation request. State
 * projection uses this bounded probe so a vanished app-owned coordinator is disabled before the
 * user invokes the command; execution repeats it before obtaining a snapshot.
 */
using OperationCenterOpenSnapshotAvailability = std::function<bool()>;

/**
 * Presents one copied immutable Operation Center snapshot on the borrowed synchronous UI target.
 * The presenter must not retain the target or derive an engine authority from the records.
 */
using OperationCenterOpenPresenter =
    std::function<bool(void *native_target, std::vector<ops::OperationRecord> snapshot)>;

/**
 * Projects a missing Registry definition into a visible disabled presentation. This prevents a
 * menu surface from treating an absent operationCenter.open definition as an executable fallback.
 */
[[nodiscard]] CommandState
OperationCenterOpenPresentationState(const CommandRegistry::StateResult &_registry_state);

/**
 * Builds the value-only operationCenter.open Registry definition. Execution obtains one snapshot
 * through the injected provider, copies it, then supplies that copy to the injected presenter.
 * An omitted availability probe preserves the generic provider contract; application composition
 * supplies its weak-coordinator probe.
 */
[[nodiscard]] CommandRegistry::Registration MakeOperationCenterOpenCommand(
    OperationCenterOpenSnapshotProvider _snapshot_provider,
    OperationCenterOpenPresenter _presenter,
    OperationCenterOpenSnapshotAvailability _snapshot_available = {});

} // namespace nc::core
