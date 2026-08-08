// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Remote/KeychainHostPinStore.h>

#include <set>
#include <string>
#include <vector>

namespace {

using nc::core::HostPinKeychainService;
using nc::core::KeychainHostPinStore;

} // namespace

#define PREFIX "nc::core::KeychainHostPinStore "

TEST_CASE(PREFIX "files pins under a service that no provider pair can collide on")
{
    // A collision would mean one host silently inheriting another's pin, which is the failure
    // pinning exists to prevent. The provider goes in the service name; the host is the account,
    // so the two never get concatenated into one ambiguous string.
    std::set<std::string> services;
    for( const auto provider : {"sftp", "ftp", "webdav", "smb", "sftp.example", "sftp:example"} )
        CHECK(services.emplace(HostPinKeychainService(provider)).second);

    // The pair ("a", "b.c") and ("a.b", "c") must not land in the same place - the classic
    // concatenation bug.
    CHECK(HostPinKeychainService("a") != HostPinKeychainService("a.b"));
}

TEST_CASE(PREFIX "keeps pins in a namespace of their own")
{
    // These entries share a keychain with connection passwords. A pin must never be readable as a
    // credential, nor a credential as a pin.
    const std::string service = HostPinKeychainService("sftp");
    CHECK(service.starts_with("wincommander.hostpin."));
    CHECK(service.find("sftp") != std::string::npos);
}

TEST_CASE(PREFIX "refuses an incomplete key or an empty pin without touching the keychain")
{
    // Guarded before any keychain call, so these cases are safe to exercise in a test binary that
    // must not read or write the user's real keychain.
    KeychainHostPinStore store;

    CHECK_FALSE(store.LoadPin("", "example.org"));
    CHECK_FALSE(store.LoadPin("sftp", ""));
    CHECK_FALSE(store.StorePin("", "example.org", "aabbccdd"));
    CHECK_FALSE(store.StorePin("sftp", "", "aabbccdd"));
    // An empty fingerprint is not a pin; storing one would create an entry that
    // RemoteHostTrust must then read as a mismatch forever.
    CHECK_FALSE(store.StorePin("sftp", "example.org", ""));
    CHECK_FALSE(store.ErasePin("", "example.org"));
    CHECK_FALSE(store.ErasePin("sftp", ""));
}
