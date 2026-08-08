// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "KeychainHostPinStore.h"

#include <Utility/KeychainServices.h>

namespace nc::core {

std::string HostPinKeychainService(const std::string_view _provider)
{
    // A fixed prefix keeps these entries apart from the connection passwords the same keychain
    // already holds, so a pin can never be read as a credential or vice versa.
    return std::string{"wincommander.hostpin."} + std::string{_provider};
}

std::optional<std::string> KeychainHostPinStore::LoadPin(const std::string_view _provider,
                                                         const std::string_view _host) const
{
    if( _provider.empty() || _host.empty() )
        return std::nullopt;
    std::string fingerprint;
    if( !KeychainServices::GetPassword(
            HostPinKeychainService(_provider), std::string{_host}, fingerprint) )
        return std::nullopt;
    // An empty stored value is not "no pin": RemoteHostTrust treats an unusable stored pin as a
    // mismatch on purpose, and reporting nullopt here would downgrade it to first-use instead.
    return fingerprint;
}

bool KeychainHostPinStore::StorePin(const std::string_view _provider,
                                    const std::string_view _host,
                                    const std::string_view _fingerprint)
{
    if( _provider.empty() || _host.empty() || _fingerprint.empty() )
        return false;
    return KeychainServices::SetPassword(
        HostPinKeychainService(_provider), std::string{_host}, std::string{_fingerprint});
}

bool KeychainHostPinStore::ErasePin(const std::string_view _provider, const std::string_view _host)
{
    if( _provider.empty() || _host.empty() )
        return false;
    return KeychainServices::ErasePassword(HostPinKeychainService(_provider), std::string{_host});
}

} // namespace nc::core
