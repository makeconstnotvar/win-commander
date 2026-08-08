// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NetworkVolumeState.h"

namespace nc::core {

NetworkVolumeState ClassifyNetworkVolume(const NetworkVolumeFacts &_facts) noexcept
{
    // A local volume is never any of the network states, whatever stale flags an adapter carries.
    if( !_facts.is_network_mount )
        return NetworkVolumeState::Local;
    if( !_facts.is_mounted )
        return NetworkVolumeState::Unmounted;

    // Unresponsive outranks a rejected export: if the server is not answering, "the export is
    // gone" is a conclusion we cannot have reached, and treating it as Stale would invite the
    // optimistic fast-failing path into a mount that actually blocks.
    if( !_facts.last_probe_answered )
        return NetworkVolumeState::Unresponsive;
    if( _facts.export_rejected )
        return NetworkVolumeState::Stale;
    return NetworkVolumeState::Responsive;
}

} // namespace nc::core
