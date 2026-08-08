// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Remote/RemoteHostTrust.h>

#include <optional>
#include <string>

namespace {

using nc::core::ClassifyRemoteHost;
using nc::core::MayConnectWithoutPrompt;
using nc::core::MayPromptToTrust;
using nc::core::NormalizeHostFingerprint;
using nc::core::RemoteHostTrustVerdict;

} // namespace

#define PREFIX "nc::core::RemoteHostTrust "

TEST_CASE(PREFIX "accepts the same fingerprint however a provider spells it")
{
    // Providers print the same fingerprint several ways. Comparing them literally would warn about
    // a host that never changed, which trains users to click through the one warning that must
    // never become routine.
    const std::string canonical = "aabbccdd";
    for( const auto spelling : {"aabbccdd", "AABBCCDD", "aa:bb:cc:dd", "AA:BB:CC:DD", "aa bb cc dd", "aa-bb-cc-dd"} ) {
        const auto normalized = NormalizeHostFingerprint(spelling);
        REQUIRE(normalized);
        CHECK(*normalized == canonical);
        CHECK(ClassifyRemoteHost(canonical, spelling) == RemoteHostTrustVerdict::TrustedPinned);
    }
}

TEST_CASE(PREFIX "rejects a fingerprint it cannot parse rather than coercing it")
{
    // Dropping unexpected characters could map two different inputs onto one normalized form.
    CHECK_FALSE(NormalizeHostFingerprint("aabbccgg"));
    CHECK_FALSE(NormalizeHostFingerprint("aabb!ccdd"));
    CHECK_FALSE(NormalizeHostFingerprint("SHA256:aabbccdd"));
    // An odd number of hex digits is not a whole fingerprint.
    CHECK_FALSE(NormalizeHostFingerprint("aabbc"));
    CHECK_FALSE(NormalizeHostFingerprint(""));
    CHECK_FALSE(NormalizeHostFingerprint("::::"));
}

TEST_CASE(PREFIX "verifies nothing when the presented fingerprint is unusable")
{
    // Even with a pin on file: there is nothing to compare it against, so this is not a mismatch
    // and certainly not trust - it is a failure to verify.
    CHECK(ClassifyRemoteHost(std::optional<std::string>{"aabbccdd"}, "") == RemoteHostTrustVerdict::Unusable);
    CHECK(ClassifyRemoteHost(std::optional<std::string>{"aabbccdd"}, "not-a-fingerprint") ==
          RemoteHostTrustVerdict::Unusable);
    CHECK(ClassifyRemoteHost(std::nullopt, "") == RemoteHostTrustVerdict::Unusable);

    CHECK_FALSE(MayConnectWithoutPrompt(RemoteHostTrustVerdict::Unusable));
    CHECK_FALSE(MayPromptToTrust(RemoteHostTrustVerdict::Unusable));
}

TEST_CASE(PREFIX "treats an unpinned host as a first-use decision, never as an automatic yes")
{
    const auto verdict = ClassifyRemoteHost(std::nullopt, "aa:bb:cc:dd");
    CHECK(verdict == RemoteHostTrustVerdict::UnknownFirstUse);
    CHECK_FALSE(MayConnectWithoutPrompt(verdict));
    // This is the one verdict a user may resolve with a plain accept.
    CHECK(MayPromptToTrust(verdict));
}

TEST_CASE(PREFIX "never resolves a mismatch by itself and never offers it as a routine accept")
{
    const auto verdict = ClassifyRemoteHost(std::optional<std::string>{"aabbccdd"}, "11223344");
    CHECK(verdict == RemoteHostTrustVerdict::Mismatch);
    CHECK_FALSE(MayConnectWithoutPrompt(verdict));
    // An established pin that suddenly disagrees is the signature of an interception. Offering it
    // as an accept/decline prompt would put a question to the user they cannot honestly answer;
    // replacing such a pin has to be a separate, explicit action.
    CHECK_FALSE(MayPromptToTrust(verdict));

    // Classification is side-effect free: asking twice cannot upgrade the verdict.
    CHECK(ClassifyRemoteHost(std::optional<std::string>{"aabbccdd"}, "11223344") == RemoteHostTrustVerdict::Mismatch);
}

TEST_CASE(PREFIX "treats an unparseable stored pin as a mismatch, not as an absent one")
{
    // Degrading a corrupted or tampered pin to first-use would silently downgrade an established
    // host into one the user is invited to accept - which is the whole attack this guards.
    const auto verdict = ClassifyRemoteHost(std::optional<std::string>{"corrupted-pin"}, "aabbccdd");
    CHECK(verdict == RemoteHostTrustVerdict::Mismatch);
    CHECK_FALSE(MayPromptToTrust(verdict));

    // An empty stored pin is a present-but-unusable pin, and gets the same treatment.
    CHECK(ClassifyRemoteHost(std::optional<std::string>{""}, "aabbccdd") == RemoteHostTrustVerdict::Mismatch);
}

TEST_CASE(PREFIX "only a pinned match may connect without asking anyone")
{
    CHECK(MayConnectWithoutPrompt(RemoteHostTrustVerdict::TrustedPinned));
    CHECK_FALSE(MayConnectWithoutPrompt(RemoteHostTrustVerdict::UnknownFirstUse));
    CHECK_FALSE(MayConnectWithoutPrompt(RemoteHostTrustVerdict::Mismatch));
    CHECK_FALSE(MayConnectWithoutPrompt(RemoteHostTrustVerdict::Unusable));

    CHECK_FALSE(MayPromptToTrust(RemoteHostTrustVerdict::TrustedPinned));
}
