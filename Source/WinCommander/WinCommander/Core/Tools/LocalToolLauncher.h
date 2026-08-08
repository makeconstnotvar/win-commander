// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "LocalToolLaunch.h"

#include <cstdint>
#include <functional>

namespace nc::core {

enum class LocalToolLaunchOutcome : uint8_t {
    Started,
    /** The configured application is not installed, or is not an application. */
    ApplicationMissing,
    /** Launch Services refused or the application failed to start. */
    LaunchFailed
};

/** Told the outcome once it is known. Called on an unspecified queue. */
using LocalToolLaunchCompletion = std::function<void(LocalToolLaunchOutcome)>;

/**
 * Performs a request built by `PrepareLocalToolLaunch`.
 *
 * Resolves the application **before** asking for a launch, so "you have not got that editor" is
 * reported as itself rather than as a generic failure - the two send the user to different places.
 * That check is cheap and synchronous, so a misconfigured tool is reported immediately.
 *
 * The launch itself is **not waited for**. An application can take many seconds to start, and
 * blocking for it is the same stall this codebase refuses to accept for an unresponsive network
 * mount. The completion arrives when Launch Services answers.
 *
 * Documents are handed over as URLs, never as a command line, so a filename containing spaces,
 * quotes or newlines stays a filename.
 */
void PerformLocalToolLaunch(const LocalToolLaunchRequest &_request, LocalToolLaunchCompletion _completion = {});

} // namespace nc::core
