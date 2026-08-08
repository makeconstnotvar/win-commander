// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Cloud/NetworkVolumeState.h>

namespace {

using nc::core::ClassifyNetworkVolume;
using nc::core::MayTouchSynchronously;
using nc::core::NetworkVolumeFacts;
using nc::core::NetworkVolumeState;
using nc::core::ShouldRefuseBeforeAttempting;

NetworkVolumeFacts Network()
{
    return {.is_network_mount = true, .is_mounted = true, .last_probe_answered = true, .export_rejected = false};
}

} // namespace

#define PREFIX "nc::core::NetworkVolumeState "

TEST_CASE(PREFIX "never reads a local volume as a network state")
{
    NetworkVolumeFacts facts{.is_network_mount = false,
                             .is_mounted = false,
                             .last_probe_answered = false,
                             .export_rejected = true};
    CHECK(ClassifyNetworkVolume(facts) == NetworkVolumeState::Local);
    CHECK(MayTouchSynchronously(NetworkVolumeState::Local));
    CHECK_FALSE(ShouldRefuseBeforeAttempting(NetworkVolumeState::Local));
}

TEST_CASE(PREFIX "lets an unresponsive server outrank a rejected export")
{
    // If the server is not answering, "the export is gone" is a conclusion we cannot have reached.
    // Calling it Stale would send the optimistic fast-failing path into a mount that blocks.
    NetworkVolumeFacts facts = Network();
    facts.last_probe_answered = false;
    facts.export_rejected = true;
    CHECK(ClassifyNetworkVolume(facts) == NetworkVolumeState::Unresponsive);
}

TEST_CASE(PREFIX "distinguishes the mount that blocks from the one that fails fast")
{
    NetworkVolumeFacts blocking = Network();
    blocking.last_probe_answered = false;
    CHECK(ClassifyNetworkVolume(blocking) == NetworkVolumeState::Unresponsive);

    NetworkVolumeFacts failing = Network();
    failing.export_rejected = true;
    CHECK(ClassifyNetworkVolume(failing) == NetworkVolumeState::Stale);

    // They need opposite handling, which is the whole reason they are separate states.
    CHECK_FALSE(MayTouchSynchronously(NetworkVolumeState::Unresponsive));
    CHECK_FALSE(ShouldRefuseBeforeAttempting(NetworkVolumeState::Unresponsive));
    CHECK(ShouldRefuseBeforeAttempting(NetworkVolumeState::Stale));
}

TEST_CASE(PREFIX "reports an unmounted volume before anything else about it")
{
    NetworkVolumeFacts facts = Network();
    facts.is_mounted = false;
    facts.last_probe_answered = false;
    CHECK(ClassifyNetworkVolume(facts) == NetworkVolumeState::Unmounted);
    CHECK(ShouldRefuseBeforeAttempting(NetworkVolumeState::Unmounted));
}

TEST_CASE(PREFIX "keeps the drawing thread away from anything that can block")
{
    // A stat() under a dead NFS or SMB mount does not fail - it blocks until the kernel's own
    // timeout, tens of seconds. Doing that while drawing is how a file manager becomes a beachball,
    // which is why the state must be known before the access rather than discovered by making it.
    CHECK(MayTouchSynchronously(NetworkVolumeState::Local));
    CHECK(MayTouchSynchronously(NetworkVolumeState::Responsive));

    CHECK_FALSE(MayTouchSynchronously(NetworkVolumeState::Unresponsive));
    CHECK_FALSE(MayTouchSynchronously(NetworkVolumeState::Stale));
    CHECK_FALSE(MayTouchSynchronously(NetworkVolumeState::Unmounted));
}

TEST_CASE(PREFIX "calls a healthy network mount responsive")
{
    CHECK(ClassifyNetworkVolume(Network()) == NetworkVolumeState::Responsive);
    CHECK(MayTouchSynchronously(NetworkVolumeState::Responsive));
    CHECK_FALSE(ShouldRefuseBeforeAttempting(NetworkVolumeState::Responsive));
}
