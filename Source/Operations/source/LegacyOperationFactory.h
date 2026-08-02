// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "VFSOperationPlanningProbes.h"

#include <Base/Error.h>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

namespace nc::ops {

class Operation;

enum class LegacyOperationFactoryErrorCode : uint8_t {
    UnsupportedPlanType,
    MissingBindings,
    ProviderUnavailable,
    UnsupportedProviderScope,
    EmptyAcceptedPlan,
    BatchUnsupported,
    UnsupportedConflictPolicy,
    SourceMaterializationFailed,
    SourceMaterializationInvalid,
    Cancelled,
    ConstructionFailed
};

struct LegacyOperationFactoryError final {
    LegacyOperationFactoryErrorCode code;
    std::optional<OperationPlanningPath> path;
    std::optional<Error> cause;
};

/** Maps an explicitly reviewed, provider-bound plan to the current execution engine. */
class LegacyOperationFactory final
{
public:
    using CancelChecker = std::function<bool()>;

    [[nodiscard]] static std::expected<std::shared_ptr<Operation>, LegacyOperationFactoryError>
    Create(ReviewedVFSOperationPreflight _preflight, CancelChecker _cancel_checker = {}) noexcept;
};

} // namespace nc::ops
