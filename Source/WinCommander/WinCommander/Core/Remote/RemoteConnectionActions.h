// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "RemoteConnectionPresentation.h"

#include <cstdint>

namespace nc::core {

/** The actions a Connection Manager row offers (spec §46). */
enum class RemoteConnectionAction : uint8_t {
    Connect,
    Disconnect,
    /** Replace a stored credential that the server refused. */
    ReplaceCredential,
    /** Accept a host whose key changed - the deliberate, separate act RC-3 requires. */
    ReplaceHostKey,
    /** Remove the connection, its credential and its pin together. */
    Forget
};

enum class RemoteActionRefusal : uint8_t {
    None,
    /** It is already in that state. */
    NothingToDo,
    /**
     * The connection is blocked for a reason this action would not resolve. Offering it would let a
     * user press it repeatedly against a wall the button cannot move.
     */
    BlockedFirst,
    /** Nothing is stored, so there is nothing to replace. */
    NothingStored,
    /** The host's key has not changed, so there is nothing to accept. */
    NoKeyChange
};

struct RemoteActionAvailability {
    RemoteActionRefusal refusal = RemoteActionRefusal::None;
    /**
     * The action goes ahead only after the user confirms. Set for anything that discards something
     * they cannot get back by pressing the button again.
     */
    bool needs_confirmation = false;

    [[nodiscard]] bool Enabled() const noexcept { return refusal == RemoteActionRefusal::None; }

    friend bool operator==(const RemoteActionAvailability &, const RemoteActionAvailability &) = default;
};

/**
 * Whether a row may offer an action, and why not when it may not.
 *
 * The decisions worth naming:
 *
 * - **A host whose key changed cannot simply be connected to.** `Connect` is refused while the
 *   trust verdict is `Mismatch`, because the thing standing in the way is not something connecting
 *   again will resolve - it needs the deliberate `ReplaceHostKey`, which RC-3 keeps as a separate
 *   act precisely so a routine button can never quietly perform it.
 * - **A rejected credential does not block connecting.** Connecting will prompt, and prompting is
 *   how the user supplies a new one - refusing here would leave them no way in from this row.
 * - **`ReplaceHostKey` is offered only for a mismatch.** On a host that never changed there is
 *   nothing to accept, and an always-available "trust this key" button is an invitation to click
 *   through the one warning that must never become routine.
 * - **`Forget` always needs confirmation and always takes the pin with it.** Leaving the pin behind
 *   would keep a host silently trusted after the user believed they had removed it - and a pin that
 *   outlives the connection it belonged to is exactly the state nobody would think to check.
 */
[[nodiscard]] RemoteActionAvailability EvaluateRemoteConnectionAction(RemoteConnectionAction _action,
                                                                       const RemoteConnectionPresentation &_row);

} // namespace nc::core
