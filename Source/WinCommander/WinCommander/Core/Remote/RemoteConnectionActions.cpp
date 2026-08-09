// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "RemoteConnectionActions.h"

namespace nc::core {

namespace {

RemoteActionAvailability Refuse(const RemoteActionRefusal _refusal) noexcept
{
    return RemoteActionAvailability{.refusal = _refusal, .needs_confirmation = false};
}

bool IsConnected(const RemoteConnectionStatus _status) noexcept
{
    return _status == RemoteConnectionStatus::Connected;
}

} // namespace

RemoteActionAvailability EvaluateRemoteConnectionAction(const RemoteConnectionAction _action,
                                                        const RemoteConnectionPresentation &_row)
{
    switch( _action ) {
        case RemoteConnectionAction::Connect:
            if( IsConnected(_row.status) )
                return Refuse(RemoteActionRefusal::NothingToDo);
            // A changed host key is not something connecting again resolves. It needs the deliberate
            // act, which is kept separate precisely so a routine button cannot quietly perform it.
            if( _row.trust == RemoteHostTrustVerdict::Mismatch )
                return Refuse(RemoteActionRefusal::BlockedFirst);
            // A rejected credential does not block: connecting prompts, and prompting is how the
            // user supplies a new one. Refusing here would leave no way in from this row.
            return RemoteActionAvailability{};

        case RemoteConnectionAction::Disconnect:
            return IsConnected(_row.status) ? RemoteActionAvailability{}
                                            : Refuse(RemoteActionRefusal::NothingToDo);

        case RemoteConnectionAction::ReplaceCredential:
            // Offered whenever something is stored - a working credential may still be replaced on
            // purpose - and refused when there is nothing to replace.
            return _row.credentials == RemoteCredentialsState::Missing
                       ? Refuse(RemoteActionRefusal::NothingStored)
                       : RemoteActionAvailability{};

        case RemoteConnectionAction::ReplaceHostKey:
            // Only for a mismatch. On a host that never changed there is nothing to accept, and an
            // always-available "trust this key" button invites clicking through the one warning that
            // must never become routine.
            if( _row.trust != RemoteHostTrustVerdict::Mismatch )
                return Refuse(RemoteActionRefusal::NoKeyChange);
            return RemoteActionAvailability{.refusal = RemoteActionRefusal::None, .needs_confirmation = true};

        case RemoteConnectionAction::Forget:
            // Always available and always confirmed: it discards a stored credential and a pin the
            // user cannot get back by pressing the button again.
            return RemoteActionAvailability{.refusal = RemoteActionRefusal::None, .needs_confirmation = true};
    }
    return Refuse(RemoteActionRefusal::NothingToDo);
}

} // namespace nc::core
