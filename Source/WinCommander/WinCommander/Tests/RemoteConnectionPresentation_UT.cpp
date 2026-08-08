// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Remote/RemoteConnectionPresentation.h>

namespace {

using namespace std::chrono_literals;
using nc::core::ApplyRemoteConnected;
using nc::core::ApplyRemoteFailure;
using nc::core::ClassifyRemoteLinkQuality;
using nc::core::PresentRemoteConnection;
using nc::core::RemoteConnectionFailure;
using nc::core::RemoteConnectionState;
using nc::core::RemoteConnectionStatus;
using nc::core::RemoteCredentialsState;
using nc::core::RemoteHostTrustVerdict;
using nc::core::RemoteLinkQuality;
using nc::core::RemoteRetryPolicy;

constexpr RemoteRetryPolicy Policy()
{
    return {.maximum_attempts = 0, .initial_backoff = 100ms, .maximum_backoff = 1000ms, .multiplier = 2.0};
}

} // namespace

#define PREFIX "nc::core::RemoteConnectionPresentation "

TEST_CASE(PREFIX "classifies link quality and refuses to flatter a broken sample")
{
    CHECK(ClassifyRemoteLinkQuality(std::nullopt) == RemoteLinkQuality::Unknown);
    CHECK(ClassifyRemoteLinkQuality(std::optional{10ms}) == RemoteLinkQuality::Good);
    CHECK(ClassifyRemoteLinkQuality(std::optional{119ms}) == RemoteLinkQuality::Good);
    CHECK(ClassifyRemoteLinkQuality(std::optional{120ms}) == RemoteLinkQuality::Fair);
    CHECK(ClassifyRemoteLinkQuality(std::optional{399ms}) == RemoteLinkQuality::Fair);
    CHECK(ClassifyRemoteLinkQuality(std::optional{400ms}) == RemoteLinkQuality::Poor);
    CHECK(ClassifyRemoteLinkQuality(std::optional{5000ms}) == RemoteLinkQuality::Poor);

    // A negative sample is a broken measurement, not a fast link. Reporting Good would be the most
    // misleading answer available.
    CHECK(ClassifyRemoteLinkQuality(std::optional{-1ms}) == RemoteLinkQuality::Unknown);
}

TEST_CASE(PREFIX "reports a rejected credential as its own state, not merely as stored")
{
    // A stored credential the server rejected is worse than none: it keeps failing until replaced,
    // and calling it "stored" leaves the user nothing to act on.
    const auto rejected = ApplyRemoteFailure(
        {}, RemoteConnectionFailure::AuthenticationRejected, 5, "bad password", Policy());
    const auto presentation =
        PresentRemoteConnection(rejected.state, true, RemoteHostTrustVerdict::TrustedPinned);

    CHECK(presentation.credentials == RemoteCredentialsState::Rejected);
    CHECK(presentation.status == RemoteConnectionStatus::Blocked);
    CHECK(presentation.needs_attention);

    // Even with nothing stored, a rejection is what the row must say.
    CHECK(PresentRemoteConnection(rejected.state, false, RemoteHostTrustVerdict::TrustedPinned).credentials ==
          RemoteCredentialsState::Rejected);
}

TEST_CASE(PREFIX "distinguishes a stored credential from a missing one")
{
    const auto connected = ApplyRemoteConnected({}, 10, 50ms, false);
    CHECK(PresentRemoteConnection(connected.state, true, RemoteHostTrustVerdict::TrustedPinned).credentials ==
          RemoteCredentialsState::Stored);
    CHECK(PresentRemoteConnection(connected.state, false, RemoteHostTrustVerdict::TrustedPinned).credentials ==
          RemoteCredentialsState::Missing);
}

TEST_CASE(PREFIX "flags a host that needs the user even while the connection looks fine")
{
    // The reason this flag is not simply status == Blocked: a mismatched pin needs the user while
    // the connection sits idle and has never failed once.
    const auto connected = ApplyRemoteConnected({}, 10, 50ms, false);
    REQUIRE(connected.state.status == RemoteConnectionStatus::Connected);
    REQUIRE(connected.state.history.empty());

    CHECK(PresentRemoteConnection(connected.state, true, RemoteHostTrustVerdict::Mismatch).needs_attention);
    CHECK(PresentRemoteConnection(connected.state, true, RemoteHostTrustVerdict::Unusable).needs_attention);

    // First use is a question the connect flow asks, not a standing alert on an idle row.
    CHECK_FALSE(PresentRemoteConnection(connected.state, true, RemoteHostTrustVerdict::UnknownFirstUse).needs_attention);
    CHECK_FALSE(PresentRemoteConnection(connected.state, true, RemoteHostTrustVerdict::TrustedPinned).needs_attention);
}

TEST_CASE(PREFIX "an offline connection is not an attention case by itself")
{
    // Offline means "try again later"; it is the retryable outcome, so it must not compete for
    // attention with a blocked host the user actually has to resolve.
    const auto offline = ApplyRemoteFailure({}, RemoteConnectionFailure::Unreachable, 5, "no route", Policy());
    REQUIRE(offline.state.status == RemoteConnectionStatus::Offline);
    const auto presentation = PresentRemoteConnection(offline.state, true, RemoteHostTrustVerdict::TrustedPinned);
    CHECK_FALSE(presentation.needs_attention);
    CHECK(presentation.quality == RemoteLinkQuality::Unknown);
    CHECK(presentation.recorded_failures == 1);
}

TEST_CASE(PREFIX "carries through what the manager row lists")
{
    auto state = ApplyRemoteConnected({}, 99, 300ms, true).state;
    state = ApplyRemoteFailure(std::move(state), RemoteConnectionFailure::TimedOut, 100, "slow", Policy()).state;
    state = ApplyRemoteConnected(std::move(state), 101, 300ms, true).state;

    const auto presentation = PresentRemoteConnection(state, true, RemoteHostTrustVerdict::TrustedPinned);
    CHECK(presentation.status == RemoteConnectionStatus::Connected);
    CHECK(presentation.last_successful_connection == 101);
    CHECK(presentation.read_only);
    CHECK(presentation.quality == RemoteLinkQuality::Fair);
    CHECK(presentation.recorded_failures == 1);
    CHECK(presentation.trust == RemoteHostTrustVerdict::TrustedPinned);
    CHECK_FALSE(presentation.needs_attention);
}
