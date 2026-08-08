// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "RemoteConnectionState.h"

#include <Base/Error.h>

namespace nc::core {

/**
 * Classifies an error raised while reaching a remote provider.
 *
 * `RemoteConnectionState` decides what may be retried; until something produces its vocabulary from
 * a real error, none of those decisions can ever be reached. This is that producer.
 *
 * The mapping is deliberately conservative. An error this does not recognise becomes
 * `ProtocolError`, which `IsRetryableRemoteFailure` refuses - an unrecognised failure retried on a
 * timer is how a connection hammers a server for reasons nobody understands. Retrying is the
 * privilege of failures we can name.
 */
[[nodiscard]] RemoteConnectionFailure ClassifyRemoteFailure(const Error &_error) noexcept;

} // namespace nc::core
