// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "RemoteHostPinStore.h"

#include <VFS/NetSFTP.h>

#include <functional>
#include <string>
#include <string_view>

namespace nc::core {

/**
 * The name a host's pin is filed under, in OpenSSH's `[host]:port` spelling.
 *
 * The port is part of the identity, not decoration: two services on one machine can present
 * different keys, and one silently inheriting the other's pin would either accept a host nobody
 * verified or warn about one that never changed. The brackets are what make that unambiguous for
 * IPv6 literals, which contain colons of their own.
 */
[[nodiscard]] std::string RemoteHostPinKey(std::string_view _host, long _port);

/** Asks the user whether to trust a host presenting an unrecognized key. Blocks until answered. */
using RemoteHostTrustPrompt = std::function<bool(
    std::string_view _host, long _port, std::string_view _algorithm, std::string_view _fingerprint)>;

/**
 * The application's SFTP host-key policy.
 *
 * A pinned host connects; an unknown one asks the user once and is pinned on acceptance; a
 * mismatched or unverifiable one is refused outright and never prompts - the prompt exists to
 * introduce a host, not to wave away the warning that says one changed.
 */
class SFTPHostKeyPolicy final : public nc::vfs::sftp::HostKeyVerifier
{
public:
    /**
     * The prompt is injected rather than called directly so this class - where the refusals live -
     * is testable without a user, and so a test cannot accidentally block on a modal window.
     */
    SFTPHostKeyPolicy(RemoteHostPinStore &_store, RemoteHostTrustPrompt _prompt);

    bool VerifyHostKey(const nc::vfs::sftp::HostKeyPresentation &_presented) override;

private:
    RemoteHostTrustPolicy m_Trust;
    RemoteHostTrustPrompt m_Prompt;
};

/** Installs the application's policy as the process-wide one. Called once during startup. */
void InstallSFTPHostKeyPolicy();

} // namespace nc::core
