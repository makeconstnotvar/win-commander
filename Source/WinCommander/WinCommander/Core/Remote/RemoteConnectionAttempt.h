// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "RemoteConnectionRegistry.h"

#include <VFS/VFS.h>

#include <functional>
#include <string_view>

namespace nc::core {

/**
 * Runs a connection attempt, records what it did, and reproduces that outcome for the caller.
 *
 * The connection paths were built to return a host or raise; this wraps one so the retry policy
 * finally sees real outcomes without any caller changing how it handles them. Whatever `_spawn`
 * returns is returned, and whatever it raises is re-raised **after** being recorded, so behaviour
 * on every existing path is exactly what it was.
 *
 * Three outcomes, and the middle one is the reason this is a function rather than two lines:
 *
 * - **A host** is a success, recorded with the moment it arrived.
 * - **A null host is not a failure.** It is what a cancelled password prompt returns, and nothing
 *   was ever asked of the server. Recording it as a failure would spend a retry budget - and
 *   eventually block a connection - because somebody pressed Cancel.
 * - **A raise** is classified and recorded, then rethrown. An exception that is not an `Error` is
 *   recorded as a protocol failure, which is never retried: something we cannot name is not
 *   evidence that trying again will help.
 */
[[nodiscard]] VFSHostPtr RecordConnectionAttempt(RemoteConnectionRegistry &_registry,
                                                  std::string_view _key,
                                                  RemoteConnectionRegistry::Instant _now,
                                                  const std::function<VFSHostPtr()> &_spawn);

} // namespace nc::core
