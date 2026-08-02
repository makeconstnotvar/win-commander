// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CopyOperationRecoveryCoordinator.h"

namespace nc::core {

class CopyOperationRecoveryCoordinatorTesting final
{
public:
    using Services = CopyOperationRecoveryCoordinator::Services;

    [[nodiscard]] static std::shared_ptr<CopyOperationRecoveryCoordinator>
    Make(std::shared_ptr<nc::ops::OperationJournal> _journal, std::string _state_directory, Services _services)
    {
        return std::shared_ptr<CopyOperationRecoveryCoordinator>{new CopyOperationRecoveryCoordinator{
            std::move(_journal), {}, std::move(_state_directory), std::move(_services)}};
    }
};

} // namespace nc::core
