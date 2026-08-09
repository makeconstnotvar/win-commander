// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Remote/RemoteConnectionActions.h>

namespace {

using nc::core::EvaluateRemoteConnectionAction;
using nc::core::RemoteActionRefusal;
using nc::core::RemoteConnectionAction;
using nc::core::RemoteConnectionPresentation;
using nc::core::RemoteConnectionStatus;
using nc::core::RemoteCredentialsState;
using nc::core::RemoteHostTrustVerdict;

RemoteConnectionPresentation Row(const RemoteConnectionStatus _status = RemoteConnectionStatus::Disconnected,
                                 const RemoteHostTrustVerdict _trust = RemoteHostTrustVerdict::TrustedPinned,
                                 const RemoteCredentialsState _credentials = RemoteCredentialsState::Stored)
{
    RemoteConnectionPresentation row;
    row.status = _status;
    row.trust = _trust;
    row.credentials = _credentials;
    return row;
}

} // namespace

#define PREFIX "nc::core::EvaluateRemoteConnectionAction "

TEST_CASE(PREFIX "will not offer to connect to a host whose key changed")
{
    // Connecting again does not resolve a changed host key. It needs the deliberate act, kept
    // separate precisely so a routine button cannot quietly perform it.
    const auto mismatched = Row(RemoteConnectionStatus::Disconnected, RemoteHostTrustVerdict::Mismatch);
    const auto connect = EvaluateRemoteConnectionAction(RemoteConnectionAction::Connect, mismatched);
    CHECK_FALSE(connect.Enabled());
    CHECK(connect.refusal == RemoteActionRefusal::BlockedFirst);

    // And the act that does resolve it is offered, with a confirmation.
    const auto replace = EvaluateRemoteConnectionAction(RemoteConnectionAction::ReplaceHostKey, mismatched);
    CHECK(replace.Enabled());
    CHECK(replace.needs_confirmation);
}

TEST_CASE(PREFIX "still lets a rejected credential be dealt with by connecting")
{
    // Connecting prompts, and prompting is how the user supplies a new one. Refusing here would
    // leave them no way in from this row.
    const auto rejected = Row(RemoteConnectionStatus::Blocked, RemoteHostTrustVerdict::TrustedPinned,
                              RemoteCredentialsState::Rejected);
    CHECK(EvaluateRemoteConnectionAction(RemoteConnectionAction::Connect, rejected).Enabled());
    CHECK(EvaluateRemoteConnectionAction(RemoteConnectionAction::ReplaceCredential, rejected).Enabled());
}

TEST_CASE(PREFIX "offers connect and disconnect as exact opposites")
{
    const auto connected = Row(RemoteConnectionStatus::Connected);
    CHECK(EvaluateRemoteConnectionAction(RemoteConnectionAction::Connect, connected).refusal ==
          RemoteActionRefusal::NothingToDo);
    CHECK(EvaluateRemoteConnectionAction(RemoteConnectionAction::Disconnect, connected).Enabled());

    for( const auto status : {RemoteConnectionStatus::Disconnected,
                              RemoteConnectionStatus::Offline,
                              RemoteConnectionStatus::Blocked} ) {
        const auto row = Row(status);
        CHECK(EvaluateRemoteConnectionAction(RemoteConnectionAction::Connect, row).Enabled());
        CHECK(EvaluateRemoteConnectionAction(RemoteConnectionAction::Disconnect, row).refusal ==
              RemoteActionRefusal::NothingToDo);
    }
}

TEST_CASE(PREFIX "does not offer to replace a credential that is not there")
{
    const auto missing = Row(RemoteConnectionStatus::Disconnected, RemoteHostTrustVerdict::TrustedPinned,
                             RemoteCredentialsState::Missing);
    CHECK(EvaluateRemoteConnectionAction(RemoteConnectionAction::ReplaceCredential, missing).refusal ==
          RemoteActionRefusal::NothingStored);

    // A working credential may still be replaced on purpose.
    CHECK(EvaluateRemoteConnectionAction(RemoteConnectionAction::ReplaceCredential, Row()).Enabled());
}

TEST_CASE(PREFIX "offers to accept a host key only where one actually changed")
{
    // An always-available "trust this key" button is an invitation to click through the one warning
    // that must never become routine.
    for( const auto trust : {RemoteHostTrustVerdict::TrustedPinned,
                             RemoteHostTrustVerdict::UnknownFirstUse,
                             RemoteHostTrustVerdict::Unusable} ) {
        const auto row = Row(RemoteConnectionStatus::Disconnected, trust);
        CHECK(EvaluateRemoteConnectionAction(RemoteConnectionAction::ReplaceHostKey, row).refusal ==
              RemoteActionRefusal::NoKeyChange);
    }
}

TEST_CASE(PREFIX "always offers to forget, and always asks first")
{
    // It discards a stored credential and a pin the user cannot get back by pressing the button
    // again - and leaving the pin behind would keep a host silently trusted after they believed they
    // had removed it.
    for( const auto status : {RemoteConnectionStatus::Connected,
                              RemoteConnectionStatus::Disconnected,
                              RemoteConnectionStatus::Offline,
                              RemoteConnectionStatus::Blocked} ) {
        const auto forget = EvaluateRemoteConnectionAction(RemoteConnectionAction::Forget, Row(status));
        CHECK(forget.Enabled());
        CHECK(forget.needs_confirmation);
    }
}

TEST_CASE(PREFIX "asks before nothing else")
{
    // Confirmation is reserved for what cannot be undone by pressing the button again. Asking on
    // every action is how a confirmation stops being read.
    const auto row = Row();
    CHECK_FALSE(EvaluateRemoteConnectionAction(RemoteConnectionAction::Connect, row).needs_confirmation);
    CHECK_FALSE(EvaluateRemoteConnectionAction(RemoteConnectionAction::ReplaceCredential, row).needs_confirmation);
    CHECK_FALSE(
        EvaluateRemoteConnectionAction(RemoteConnectionAction::Disconnect, Row(RemoteConnectionStatus::Connected))
            .needs_confirmation);
}

#undef PREFIX
