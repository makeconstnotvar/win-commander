// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Remote/RemoteHostPinStore.h>

#include <map>
#include <string>
#include <utility>

namespace {

using nc::core::RemoteHostPinStore;
using nc::core::RemoteHostTrustPolicy;
using nc::core::RemoteHostTrustVerdict;

class MemoryPinStore final : public RemoteHostPinStore
{
public:
    std::optional<std::string> LoadPin(const std::string_view _provider, const std::string_view _host) const override
    {
        ++loads;
        const auto found = pins.find(Key(_provider, _host));
        return found == pins.end() ? std::nullopt : std::optional{found->second};
    }
    bool StorePin(const std::string_view _provider,
                  const std::string_view _host,
                  const std::string_view _fingerprint) override
    {
        ++stores;
        if( fail_stores )
            return false;
        pins[Key(_provider, _host)] = std::string{_fingerprint};
        return true;
    }
    bool ErasePin(const std::string_view _provider, const std::string_view _host) override
    {
        return pins.erase(Key(_provider, _host)) > 0;
    }

    static std::string Key(const std::string_view _provider, const std::string_view _host)
    {
        return std::string{_provider} + "\n" + std::string{_host};
    }

    std::map<std::string, std::string> pins;
    bool fail_stores = false;
    mutable unsigned loads = 0;
    unsigned stores = 0;
};

} // namespace

#define PREFIX "nc::core::RemoteHostTrustPolicy "

TEST_CASE(PREFIX "pins a first-use host and then trusts it, whatever spelling arrives")
{
    MemoryPinStore store;
    RemoteHostTrustPolicy policy{store};

    CHECK(policy.Verify("sftp", "example.org", "AA:BB:CC:DD") == RemoteHostTrustVerdict::UnknownFirstUse);
    CHECK(policy.TrustOnFirstUse("sftp", "example.org", "AA:BB:CC:DD"));

    // Stored normalized, so a later differently-spelled presentation still matches.
    CHECK(store.pins.at(MemoryPinStore::Key("sftp", "example.org")) == "aabbccdd");
    CHECK(policy.Verify("sftp", "example.org", "aabbccdd") == RemoteHostTrustVerdict::TrustedPinned);
    CHECK(policy.Verify("sftp", "example.org", "aa bb cc dd") == RemoteHostTrustVerdict::TrustedPinned);

    // Pins are per provider and per host, not global.
    CHECK(policy.Verify("ftp", "example.org", "aabbccdd") == RemoteHostTrustVerdict::UnknownFirstUse);
    CHECK(policy.Verify("sftp", "other.org", "aabbccdd") == RemoteHostTrustVerdict::UnknownFirstUse);
}

TEST_CASE(PREFIX "the routine accept path can never overwrite an established pin")
{
    MemoryPinStore store;
    RemoteHostTrustPolicy policy{store};
    REQUIRE(policy.TrustOnFirstUse("sftp", "example.org", "aabbccdd"));
    const auto stores_after_pinning = store.stores;

    SECTION("a mismatched host is not resolvable by accepting it")
    {
        // This is the whole point: otherwise the button that accepts an unknown host would also
        // silently dismiss an interception warning.
        REQUIRE(policy.Verify("sftp", "example.org", "11223344") == RemoteHostTrustVerdict::Mismatch);
        CHECK_FALSE(policy.TrustOnFirstUse("sftp", "example.org", "11223344"));
        CHECK(store.stores == stores_after_pinning); // the store was not even asked
        CHECK(store.pins.at(MemoryPinStore::Key("sftp", "example.org")) == "aabbccdd");
    }
    SECTION("re-accepting the same host is a no-op rather than a rewrite")
    {
        CHECK_FALSE(policy.TrustOnFirstUse("sftp", "example.org", "aabbccdd"));
        CHECK(store.stores == stores_after_pinning);
    }
    SECTION("an unverifiable fingerprint is never recorded")
    {
        CHECK_FALSE(policy.TrustOnFirstUse("sftp", "new.org", "not-a-fingerprint"));
        CHECK_FALSE(store.pins.contains(MemoryPinStore::Key("sftp", "new.org")));
    }
}

TEST_CASE(PREFIX "replacing a pin is the separate, deliberate path")
{
    MemoryPinStore store;
    RemoteHostTrustPolicy policy{store};
    REQUIRE(policy.TrustOnFirstUse("sftp", "example.org", "aabbccdd"));
    REQUIRE(policy.Verify("sftp", "example.org", "11223344") == RemoteHostTrustVerdict::Mismatch);

    // ReplacePin deliberately accepts a mismatch - that is the case it exists for - so the refusal
    // lives in TrustOnFirstUse and the explicit user decision lives in whatever calls this.
    CHECK(policy.ReplacePin("sftp", "example.org", "11:22:33:44"));
    CHECK(policy.Verify("sftp", "example.org", "11223344") == RemoteHostTrustVerdict::TrustedPinned);

    // But it still refuses to pin something unverifiable, which would leave the host permanently
    // unverifiable rather than merely re-pinned.
    CHECK_FALSE(policy.ReplacePin("sftp", "example.org", "garbage"));
    CHECK(policy.Verify("sftp", "example.org", "11223344") == RemoteHostTrustVerdict::TrustedPinned);
}

TEST_CASE(PREFIX "forgetting a host returns it to first use")
{
    MemoryPinStore store;
    RemoteHostTrustPolicy policy{store};
    REQUIRE(policy.TrustOnFirstUse("sftp", "example.org", "aabbccdd"));

    CHECK(policy.Forget("sftp", "example.org"));
    CHECK(policy.Verify("sftp", "example.org", "aabbccdd") == RemoteHostTrustVerdict::UnknownFirstUse);
    // Forgetting a host that was never pinned reports that nothing was removed.
    CHECK_FALSE(policy.Forget("sftp", "example.org"));
}

TEST_CASE(PREFIX "a store that cannot persist reports failure instead of pretending")
{
    MemoryPinStore store;
    store.fail_stores = true;
    RemoteHostTrustPolicy policy{store};

    // A pin that did not become durable must not be reported as trusted, or the next launch would
    // present the host as unknown again with no explanation.
    CHECK_FALSE(policy.TrustOnFirstUse("sftp", "example.org", "aabbccdd"));
    CHECK(policy.Verify("sftp", "example.org", "aabbccdd") == RemoteHostTrustVerdict::UnknownFirstUse);
    CHECK_FALSE(policy.ReplacePin("sftp", "example.org", "aabbccdd"));
}

TEST_CASE(PREFIX "decisions are taken against the live store, not a remembered verdict")
{
    MemoryPinStore store;
    RemoteHostTrustPolicy policy{store};

    // Something else pins the host between a caller's Verify and its accept - TrustOnFirstUse must
    // re-read rather than trust the caller's stale first-use answer.
    REQUIRE(policy.Verify("sftp", "example.org", "aabbccdd") == RemoteHostTrustVerdict::UnknownFirstUse);
    store.pins[MemoryPinStore::Key("sftp", "example.org")] = "11223344";

    CHECK_FALSE(policy.TrustOnFirstUse("sftp", "example.org", "aabbccdd"));
    CHECK(store.pins.at(MemoryPinStore::Key("sftp", "example.org")) == "11223344");
}
