// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "FileManagerError.h"
#include <functional>

namespace nc::core {

class FileManagerErrorAdapter final
{
public:
    using Timestamp = std::chrono::system_clock::time_point;
    using ActionValidator = std::function<bool(const CommandId &)>;

    /**
     * Captures the supplied error and context as a product-facing value.
     *
     * The baseline maps only POSIX categories whose meaning is unambiguous without operation or
     * provider state. All other values are preserved as UnknownError. Recovery behavior is accepted
     * only as an explicit disposition. Every suggested action must be accepted by the injected
     * validator supplied by the owning command composition layer.
     *
     * Throws std::invalid_argument when a supplied recovery disposition is inconsistent.
     */
    [[nodiscard]] static FileManagerError FromError(nc::Error _error,
                                                    FileManagerErrorContext _context = {},
                                                    Timestamp _timestamp = std::chrono::system_clock::now(),
                                                    ActionValidator _action_validator = {});
};

} // namespace nc::core
