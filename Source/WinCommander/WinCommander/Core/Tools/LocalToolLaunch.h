// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "LocalWorkingDirectory.h"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace nc::core {

/** Why nothing can be launched for this pane. */
enum class LocalToolLaunchRefusal : uint8_t {
    /** The pane has no usable local directory - see `LocalWorkingDirectoryRefusal` for which. */
    LocationUnusable,
    /** No application is configured for this role. */
    ApplicationNotConfigured,
    /** The configured application is not something that can be launched. */
    ApplicationUnusable
};

/** Which tool a pane is asking for. They differ in what they are handed. */
enum class LocalToolRole : uint8_t {
    /** Opened *at* a directory: the directory is the thing to show. */
    Terminal,
    /** Opened *on* a selection, with the directory only as context. */
    Editor
};

/**
 * A launch, reduced to what the platform call needs.
 *
 * An application identifier plus explicit arguments, never a command line. A directory or filename
 * may contain spaces, quotes and newlines, and a command line would let all of them be re-read as
 * syntax - the same reason the git integration spawns with an argument vector.
 */
struct LocalToolLaunchRequest {
    /** Bundle identifier, or an absolute path to an application. */
    std::string application;
    /** Directory the tool should treat as its location. Never empty. */
    std::string working_directory;
    /** Absolute paths the tool should open. Empty for a terminal. */
    std::vector<std::string> documents;

    friend bool operator==(const LocalToolLaunchRequest &, const LocalToolLaunchRequest &) = default;
};

/**
 * Builds the launch for a pane, or refuses.
 *
 * The location is checked **first**, before the application is even looked at. That ordering is the
 * point: a missing editor is a visible, obvious failure the user can fix, while a path inside an
 * archive or on a remote host resolves silently against the real filesystem and opens the wrong
 * place. The refusal that would otherwise be silent is the one that must be reached.
 *
 * `_documents` are the pane's selected items, as absolute paths. They are dropped for a terminal,
 * which is opened at a directory rather than on files, and any that fall outside the resolved
 * working directory are dropped as well: a selection can outlive the listing it came from, and
 * handing an editor a path from somewhere else is how a stale selection edits the wrong file.
 */
[[nodiscard]] std::expected<LocalToolLaunchRequest, LocalToolLaunchRefusal>
PrepareLocalToolLaunch(LocalToolRole _role,
                       const PaneLocationFacts &_location,
                       std::string_view _application,
                       const std::vector<std::string> &_documents = {});

} // namespace nc::core
