// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "RemoteHostPinStore.h"

#include <optional>
#include <string>
#include <string_view>

namespace nc::core {

/**
 * Derives the keychain service name a host pin is filed under.
 *
 * The provider is embedded in the service name rather than concatenated with the host into one
 * string, so no `(provider, host)` pair can ever collide with a different one - a collision here
 * would mean one host silently inheriting another's pin, which is the failure pinning exists to
 * prevent. Exposed for testing that property directly.
 */
[[nodiscard]] std::string HostPinKeychainService(std::string_view _provider);

/**
 * Host pins kept in the macOS keychain.
 *
 * A pin is not a secret - it is a public fingerprint - so this is not about confidentiality. It is
 * about integrity: the keychain is harder to tamper with than a config file, and a silently edited
 * pin is exactly how an interception warning would be made to disappear.
 *
 * Deliberately holds no cache. A cached pin could answer a verification from memory after the
 * stored one changed underneath it.
 */
class KeychainHostPinStore final : public RemoteHostPinStore
{
public:
    [[nodiscard]] std::optional<std::string> LoadPin(std::string_view _provider,
                                                     std::string_view _host) const override;
    [[nodiscard]] bool StorePin(std::string_view _provider,
                                std::string_view _host,
                                std::string_view _fingerprint) override;
    [[nodiscard]] bool ErasePin(std::string_view _provider, std::string_view _host) override;
};

} // namespace nc::core
