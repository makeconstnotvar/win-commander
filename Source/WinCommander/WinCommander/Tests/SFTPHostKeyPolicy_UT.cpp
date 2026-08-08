// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Remote/SFTPHostKeyPolicy.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using nc::core::RemoteHostPinKey;
using nc::core::RemoteHostPinStore;
using nc::core::SFTPHostKeyPolicy;
using nc::vfs::sftp::HostKeyPresentation;

class MemoryPinStore final : public RemoteHostPinStore
{
public:
    std::optional<std::string> LoadPin(const std::string_view _provider, const std::string_view _host) const override
    {
        const auto found = pins.find(Key(_provider, _host));
        return found == pins.end() ? std::nullopt : std::optional{found->second};
    }
    bool StorePin(const std::string_view _provider,
                  const std::string_view _host,
                  const std::string_view _fingerprint) override
    {
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
};

/** Records what it was asked, and answers whatever the test told it to. */
struct RecordingPrompt {
    std::shared_ptr<std::vector<std::string>> asked = std::make_shared<std::vector<std::string>>();
    bool answer = true;

    nc::core::RemoteHostTrustPrompt Handler() const
    {
        auto asked_copy = asked;
        const bool reply = answer;
        return [asked_copy, reply](const std::string_view _host,
                                   const long _port,
                                   const std::string_view _algorithm,
                                   const std::string_view _fingerprint) {
            asked_copy->push_back(std::string{_host} + "|" + std::to_string(_port) + "|" + std::string{_algorithm} +
                                  "|" + std::string{_fingerprint});
            return reply;
        };
    }
};

HostKeyPresentation Presented(const std::string_view _host, const long _port, const std::string_view _fingerprint)
{
    return HostKeyPresentation{
        .server_url = _host, .port = _port, .algorithm = "ssh-ed25519", .fingerprint = _fingerprint};
}

} // namespace

#define PREFIX "nc::core::SFTPHostKeyPolicy "

TEST_CASE(PREFIX "files a pin under a name that cannot be confused with another host's")
{
    CHECK(RemoteHostPinKey("example.org", 22) == "[example.org]:22");
    CHECK(RemoteHostPinKey("example.org", 2222) == "[example.org]:2222");

    // An IPv6 literal is full of colons of its own, which is exactly why the host is bracketed
    // rather than joined to the port with one.
    CHECK(RemoteHostPinKey("::1", 22) == "[::1]:22");
    CHECK(RemoteHostPinKey("fe80::1", 2222) == "[fe80::1]:2222");

    // The pairs a naive host+":"+port would collapse onto one name stay apart. A collision here
    // would mean one host silently inheriting another's pin.
    CHECK(RemoteHostPinKey("a", 22) != RemoteHostPinKey("a:22", 22));
    CHECK(RemoteHostPinKey("example.org", 22) != RemoteHostPinKey("example.org", 2222));
}

TEST_CASE(PREFIX "asks once about an unknown host, then never again")
{
    MemoryPinStore store;
    RecordingPrompt prompt;
    SFTPHostKeyPolicy policy{store, prompt.Handler()};

    CHECK(policy.VerifyHostKey(Presented("example.org", 22, "aabbccdd")));
    REQUIRE(prompt.asked->size() == 1);
    // The user is shown the host, port, algorithm and fingerprint - everything a decision needs.
    CHECK(prompt.asked->front() == "example.org|22|ssh-ed25519|aabbccdd");

    // The accepted host is now pinned, so the second connection proceeds without a question.
    CHECK(policy.VerifyHostKey(Presented("example.org", 22, "aabbccdd")));
    CHECK(prompt.asked->size() == 1);

    // Including when the server spells the same fingerprint differently.
    CHECK(policy.VerifyHostKey(Presented("example.org", 22, "AA:BB:CC:DD")));
    CHECK(prompt.asked->size() == 1);
}

TEST_CASE(PREFIX "declining an unknown host refuses the connection and remembers nothing")
{
    MemoryPinStore store;
    RecordingPrompt prompt;
    prompt.answer = false;
    SFTPHostKeyPolicy policy{store, prompt.Handler()};

    CHECK_FALSE(policy.VerifyHostKey(Presented("example.org", 22, "aabbccdd")));
    CHECK(prompt.asked->size() == 1);
    // Declining is not a decision to record: the next attempt must ask again rather than treat the
    // refusal as an answer about this host's identity.
    CHECK(store.pins.empty());
}

TEST_CASE(PREFIX "never puts a changed host key to the user as a question")
{
    MemoryPinStore store;
    RecordingPrompt prompt;
    SFTPHostKeyPolicy policy{store, prompt.Handler()};

    REQUIRE(policy.VerifyHostKey(Presented("example.org", 22, "aabbccdd")));
    prompt.asked->clear();

    // A pinned host that suddenly presents a different key is the signature of an interception. It
    // is refused outright - offering an accept/decline here would train users to click through the
    // one warning that must never become routine.
    CHECK_FALSE(policy.VerifyHostKey(Presented("example.org", 22, "11223344")));
    CHECK(prompt.asked->empty());
    // And the pin is left as it was, so the next attempt warns just the same.
    CHECK(store.pins.at(MemoryPinStore::Key("sftp", "[example.org]:22")) == "aabbccdd");
}

TEST_CASE(PREFIX "refuses a fingerprint it cannot make sense of, without asking")
{
    MemoryPinStore store;
    RecordingPrompt prompt;
    SFTPHostKeyPolicy policy{store, prompt.Handler()};

    // Nothing was verified, so there is no question worth asking - and certainly nothing to pin.
    CHECK_FALSE(policy.VerifyHostKey(Presented("example.org", 22, "")));
    CHECK_FALSE(policy.VerifyHostKey(Presented("example.org", 22, "not-a-fingerprint")));
    CHECK(prompt.asked->empty());
    CHECK(store.pins.empty());
}

TEST_CASE(PREFIX "treats two ports on one machine as two hosts")
{
    MemoryPinStore store;
    RecordingPrompt prompt;
    SFTPHostKeyPolicy policy{store, prompt.Handler()};

    REQUIRE(policy.VerifyHostKey(Presented("example.org", 22, "aabbccdd")));
    prompt.asked->clear();

    // Different services on one machine can legitimately present different keys, so the second port
    // is introduced on its own rather than inheriting the first one's pin.
    CHECK(policy.VerifyHostKey(Presented("example.org", 2222, "11223344")));
    CHECK(prompt.asked->size() == 1);
    CHECK(store.pins.at(MemoryPinStore::Key("sftp", "[example.org]:22")) == "aabbccdd");
    CHECK(store.pins.at(MemoryPinStore::Key("sftp", "[example.org]:2222")) == "11223344");
}

TEST_CASE(PREFIX "reports a refusal when an accepted host could not be remembered")
{
    MemoryPinStore store;
    store.fail_stores = true;
    RecordingPrompt prompt;
    SFTPHostKeyPolicy policy{store, prompt.Handler()};

    // The user said yes, but the pin did not become durable. Connecting anyway would ask them again
    // next launch with no way to tell an unremembered host from one whose key changed.
    CHECK_FALSE(policy.VerifyHostKey(Presented("example.org", 22, "aabbccdd")));
    CHECK(prompt.asked->size() == 1);
    CHECK(store.pins.empty());
}

TEST_CASE(PREFIX "without a way to ask, an unknown host is refused rather than assumed")
{
    MemoryPinStore store;
    SFTPHostKeyPolicy policy{store, nullptr};

    CHECK_FALSE(policy.VerifyHostKey(Presented("example.org", 22, "aabbccdd")));
    CHECK(store.pins.empty());

    // A host that was pinned earlier still connects: no question needs asking.
    REQUIRE(store.StorePin("sftp", "[example.org]:22", "aabbccdd"));
    CHECK(policy.VerifyHostKey(Presented("example.org", 22, "aabbccdd")));
}

#undef PREFIX
