// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "../Tests.h"

#include "../../source/NetSFTP/HostKeyVerification.h"

#include <libssh2.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

using nc::vfs::sftp::FormatHostKeyFingerprint;
using nc::vfs::sftp::HostKeyAlgorithmName;
using nc::vfs::sftp::HostKeyPresentation;
using nc::vfs::sftp::HostKeyVerifier;
using nc::vfs::sftp::HostKeyVerifierOrNull;
using nc::vfs::sftp::SetHostKeyVerifier;

#define PREFIX "nc::vfs::sftp::HostKeyVerification "

namespace {

struct RecordingVerifier final : HostKeyVerifier {
    bool answer = true;
    std::vector<std::string> seen;

    bool VerifyHostKey(const HostKeyPresentation &_presented) override
    {
        seen.emplace_back(_presented.fingerprint);
        return answer;
    }
};

/** Restores whatever policy was installed, so one test cannot decide another's outcome. */
struct VerifierGuard {
    VerifierGuard() : previous(HostKeyVerifierOrNull()) {}
    ~VerifierGuard() { SetHostKeyVerifier(previous); }
    std::shared_ptr<HostKeyVerifier> previous;
};

} // namespace

TEST_CASE(PREFIX "formats a fingerprint as the lowercase hex that pin comparison normalizes to")
{
    // The writer and the reader of a pin must agree without a normalization step in between, so the
    // spelling produced here is the spelling comparison expects.
    const std::array<unsigned char, 4> hash{0xAB, 0xCD, 0xEF, 0x01};
    const std::string formatted = FormatHostKeyFingerprint(hash);
    CHECK(formatted == "abcdef01");
    CHECK(std::ranges::none_of(formatted, [](const char c) { return c >= 'A' && c <= 'Z'; }));
}

TEST_CASE(PREFIX "keeps every byte, including the ones that look like nothing")
{
    // A byte of zero is still a byte. Dropping it, or printing it as one digit, would make two
    // different keys share a fingerprint - which is the one thing a fingerprint must never do.
    const std::array<unsigned char, 4> hash{0x00, 0x0F, 0xF0, 0x00};
    CHECK(FormatHostKeyFingerprint(hash) == "000ff000");

    const std::array<unsigned char, 32> sha256{};
    CHECK(FormatHostKeyFingerprint(sha256).size() == 64);

    CHECK(FormatHostKeyFingerprint({}).empty());
}

TEST_CASE(PREFIX "names only the algorithms it actually knows")
{
    CHECK(HostKeyAlgorithmName(LIBSSH2_HOSTKEY_TYPE_RSA) == "ssh-rsa");
    CHECK(HostKeyAlgorithmName(LIBSSH2_HOSTKEY_TYPE_ED25519) == "ssh-ed25519");
    CHECK(HostKeyAlgorithmName(LIBSSH2_HOSTKEY_TYPE_ECDSA_256) == "ecdsa-sha2-nistp256");
    CHECK(HostKeyAlgorithmName(LIBSSH2_HOSTKEY_TYPE_ECDSA_384) == "ecdsa-sha2-nistp384");
    CHECK(HostKeyAlgorithmName(LIBSSH2_HOSTKEY_TYPE_ECDSA_521) == "ecdsa-sha2-nistp521");

    // An unknown algorithm is reported as unknown rather than guessed: this string is shown to a
    // user deciding whether to trust a host, and an invented one would be worse than none.
    CHECK(HostKeyAlgorithmName(LIBSSH2_HOSTKEY_TYPE_UNKNOWN).empty());
    CHECK(HostKeyAlgorithmName(9999).empty());
}

TEST_CASE(PREFIX "has no policy until one is installed")
{
    const VerifierGuard guard;

    SetHostKeyVerifier(nullptr);
    // The connection path reads this as "nobody decided what this process trusts" and fails closed
    // rather than connecting unverified.
    CHECK(HostKeyVerifierOrNull() == nullptr);

    auto verifier = std::make_shared<RecordingVerifier>();
    SetHostKeyVerifier(verifier);
    CHECK(HostKeyVerifierOrNull() == verifier);
}

TEST_CASE(PREFIX "replacing the policy does not disturb a connection already holding one")
{
    const VerifierGuard guard;

    auto first = std::make_shared<RecordingVerifier>();
    SetHostKeyVerifier(first);
    const std::shared_ptr<HostKeyVerifier> held = HostKeyVerifierOrNull();
    REQUIRE(held == first);

    SetHostKeyVerifier(std::make_shared<RecordingVerifier>());
    CHECK(HostKeyVerifierOrNull() != first);
    // The handshake that started under the old policy keeps it alive and usable, rather than
    // dereferencing a policy that was swapped out mid-connection.
    CHECK(held.use_count() >= 2);
    CHECK(held->VerifyHostKey({.server_url = "example.org", .port = 22, .fingerprint = "abcd"}));
    CHECK(first->seen == std::vector<std::string>{"abcd"});

    SetHostKeyVerifier(nullptr);
    CHECK(HostKeyVerifierOrNull() == nullptr);
}

TEST_CASE(PREFIX "passes the host through as configured and reports a refusal")
{
    const VerifierGuard guard;

    auto verifier = std::make_shared<RecordingVerifier>();
    verifier->answer = false;
    SetHostKeyVerifier(verifier);

    // The verifier looks a pin up under the name the user typed, so what it receives is the
    // configured host rather than anything the connection resolved it to.
    const HostKeyPresentation presented{
        .server_url = "example.org", .port = 2222, .algorithm = "ssh-ed25519", .fingerprint = "0011aabb"};
    CHECK_FALSE(HostKeyVerifierOrNull()->VerifyHostKey(presented));
    CHECK(verifier->seen == std::vector<std::string>{"0011aabb"});
}

#undef PREFIX
