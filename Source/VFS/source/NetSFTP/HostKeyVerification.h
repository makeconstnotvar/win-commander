// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace nc::vfs::sftp {

/** What a server presented to identify itself, as the verifier sees it. */
struct HostKeyPresentation {
    /** The host as configured, so a verifier looks up the pin under the name the user typed. */
    std::string_view server_url;
    long port = 22;
    /** The key algorithm, e.g. "ssh-ed25519". Empty when the algorithm is not one we can name. */
    std::string_view algorithm;
    /** SHA-256 of the host key as lowercase hex. */
    std::string_view fingerprint;
};

/**
 * Decides whether a connection may proceed once the server has identified itself.
 *
 * VFS asks; it does not decide. Pinning, first-use prompts and keychain storage all live above this
 * interface, so this module stays testable without a keychain and without a user to ask.
 */
class HostKeyVerifier
{
public:
    virtual ~HostKeyVerifier() = default;

    /**
     * Returns true when the connection may proceed to authentication.
     *
     * Called on whatever thread opened the connection, and may block - a first-use decision belongs
     * to a user, and there is nothing useful to do with the socket until they answer.
     */
    [[nodiscard]] virtual bool VerifyHostKey(const HostKeyPresentation &_presented) = 0;
};

/**
 * Installs the process-wide host-key policy, replacing any previous one. Passing nullptr removes it,
 * which makes SFTP connections fail rather than fall back to connecting unverified.
 *
 * Process-wide rather than per-host because host-key policy *is* a property of the process - the way
 * one `known_hosts` covers every ssh session a user starts - and because hosts revived from a
 * serialized configuration have no call site that could supply one.
 */
void SetHostKeyVerifier(std::shared_ptr<HostKeyVerifier> _verifier) noexcept;

/** The installed policy, or nullptr when none was installed. */
[[nodiscard]] std::shared_ptr<HostKeyVerifier> HostKeyVerifierOrNull() noexcept;

/**
 * Formats a host key hash as lowercase hex with no separators.
 *
 * That spelling is deliberate: it is the form host-pin comparison normalizes to, so a fingerprint
 * makes the round trip through storage and back without a normalization step that could differ
 * between the writer and the reader.
 */
[[nodiscard]] std::string FormatHostKeyFingerprint(std::span<const unsigned char> _hash);

/**
 * Names a libssh2 `LIBSSH2_HOSTKEY_TYPE_*` value, or returns empty for one it does not know.
 *
 * Takes the raw integer so that naming stays testable here rather than inside the connection path,
 * which needs a server to reach.
 */
[[nodiscard]] std::string_view HostKeyAlgorithmName(int _hostkey_type) noexcept;

} // namespace nc::vfs::sftp
