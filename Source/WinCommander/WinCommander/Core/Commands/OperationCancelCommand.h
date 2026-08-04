// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandRegistry.h"

#include <Operations/OperationCenterCoordinator.h>
#include <functional>

namespace nc::core {

using OperationCancelExecutor =
    std::function<ops::OperationCenterCancelResult(ops::OperationId, uint64_t expected_revision)>;

/** Returns the only value target accepted by operation.cancel from one immutable model record. */
[[nodiscard]] OperationCancelTarget OperationCancelTargetFromRecord(const ops::OperationRecord &_record) noexcept;

/**
 * Builds a synchronous value-only command context from one immutable OperationCenter record.
 * The Registry repeats all lifecycle checks through its sealed control port at execution time.
 */
[[nodiscard]] CommandContext OperationCancelContextFromRecord(const ops::OperationRecord &_record,
                                                              CommandInvocationSource _source) noexcept;

/**
 * Projects the Registry lookup result for an Operation Center cancel surface. A missing Registry
 * definition remains visibly disabled with the same user-facing reason as an unavailable control
 * port, instead of becoming an executable fallback.
 */
[[nodiscard]] CommandState
OperationCancelPresentationState(const CommandRegistry::StateResult &_registry_state);

/**
 * Builds the operation.cancel Registry definition. The supplied executor is the only bridge to
 * the sealed OperationCenterCoordinator control port and receives no model, Pool or executor.
 */
[[nodiscard]] CommandRegistry::Registration MakeOperationCancelCommand(OperationCancelExecutor _executor);

} // namespace nc::core
