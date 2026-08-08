// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "HostKeyVerification.h"

#include <libssh2.h>

#include <mutex>
#include <utility>

namespace nc::vfs::sftp {

namespace {

struct VerifierSlot {
    std::mutex lock;
    std::shared_ptr<HostKeyVerifier> verifier;
};

VerifierSlot &Slot() noexcept
{
    // Deliberately never destroyed. A connection can still be handshaking while the process tears
    // down, and a policy that destroyed itself first would leave that handshake reading freed state.
    [[clang::no_destroy]] static VerifierSlot slot;
    return slot;
}

} // namespace

void SetHostKeyVerifier(std::shared_ptr<HostKeyVerifier> _verifier) noexcept
{
    VerifierSlot &slot = Slot();
    std::shared_ptr<HostKeyVerifier> previous;
    {
        const auto lock = std::lock_guard{slot.lock};
        previous = std::exchange(slot.verifier, std::move(_verifier));
    }
    // The replaced policy is released outside the lock: its destructor is somebody else's code, and
    // running it here would let it deadlock by installing a policy of its own.
}

std::shared_ptr<HostKeyVerifier> HostKeyVerifierOrNull() noexcept
{
    // Returns a copy rather than a reference: a connection in flight must keep the policy it started
    // with alive, even if another thread replaces it mid-handshake.
    VerifierSlot &slot = Slot();
    const auto lock = std::lock_guard{slot.lock};
    return slot.verifier;
}

std::string FormatHostKeyFingerprint(const std::span<const unsigned char> _hash)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string formatted;
    formatted.reserve(_hash.size() * 2);
    for( const unsigned char byte : _hash ) {
        formatted.push_back(digits[byte >> 4]);
        formatted.push_back(digits[byte & 0x0F]);
    }
    return formatted;
}

std::string_view HostKeyAlgorithmName(const int _hostkey_type) noexcept
{
    switch( _hostkey_type ) {
        case LIBSSH2_HOSTKEY_TYPE_RSA:
            return "ssh-rsa";
        case LIBSSH2_HOSTKEY_TYPE_DSS:
            return "ssh-dss";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
            return "ecdsa-sha2-nistp256";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
            return "ecdsa-sha2-nistp384";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
            return "ecdsa-sha2-nistp521";
        case LIBSSH2_HOSTKEY_TYPE_ED25519:
            return "ssh-ed25519";
        default:
            // Deliberately empty rather than a guess: an algorithm name shown to a user making a
            // trust decision must not be invented.
            return {};
    }
}

} // namespace nc::vfs::sftp
