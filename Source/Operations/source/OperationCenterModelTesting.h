// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "OperationCenterModel.h"

namespace nc::ops {

/** Test-only access to the superseded isolated model-allocation foundation. */
class OperationCenterModelTesting final
{
public:
    [[nodiscard]] static std::expected<OperationCenterModel::Reservation, OperationCenterModelError>
    Reserve(OperationCenterModel &_model);

    [[nodiscard]] static std::expected<OperationRecord, OperationCenterModelError>
    Admit(OperationCenterModel &_model,
          OperationCenterModel::Reservation &&_reservation,
          const OperationPlan &_plan,
          OperationPlan::TimePoint _created_at);
};

} // namespace nc::ops
