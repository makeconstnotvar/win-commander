// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "SFTPHostKeyPolicy.h"

#include "KeychainHostPinStore.h"

#include <WinCommander/Core/Alert.h>

#include <Base/dispatch_cpp.h>
#include <Utility/StringExtras.h>

#include <fmt/format.h>

#include <memory>
#include <utility>

namespace nc::core {

static constexpr std::string_view g_SFTPPinProvider = "sftp";

std::string RemoteHostPinKey(const std::string_view _host, const long _port)
{
    return fmt::format("[{}]:{}", _host, _port);
}

SFTPHostKeyPolicy::SFTPHostKeyPolicy(RemoteHostPinStore &_store, RemoteHostTrustPrompt _prompt)
    : m_Trust(_store), m_Prompt(std::move(_prompt))
{
}

bool SFTPHostKeyPolicy::VerifyHostKey(const nc::vfs::sftp::HostKeyPresentation &_presented)
{
    const std::string pin_key = RemoteHostPinKey(_presented.server_url, _presented.port);
    const RemoteHostTrustVerdict verdict = m_Trust.Verify(g_SFTPPinProvider, pin_key, _presented.fingerprint);

    if( MayConnectWithoutPrompt(verdict) )
        return true;

    // Mismatch and Unusable end here without ever reaching the user. The prompt exists to introduce
    // a host, not to dismiss the warning that says an established one changed underneath us.
    if( !MayPromptToTrust(verdict) )
        return false;

    if( !m_Prompt )
        return false;

    if( !m_Prompt(_presented.server_url, _presented.port, _presented.algorithm, _presented.fingerprint) )
        return false;

    // A pin that did not become durable is reported as a refusal rather than connected anyway: the
    // user accepted this host once, and silently forgetting it would ask them again next launch with
    // no way to tell an unremembered host from a changed one.
    return m_Trust.TrustOnFirstUse(g_SFTPPinProvider, pin_key, _presented.fingerprint);
}

static bool AskToTrustHost(const std::string_view _host,
                           const long _port,
                           const std::string_view _algorithm,
                           const std::string_view _fingerprint)
{
    // Same contract the password prompt on this path already relies on: the connection runs on a
    // background queue while the main thread keeps its run loop, so the question can be asked there.
    if( !nc::dispatch_is_main_queue() ) {
        bool accepted = false;
        dispatch_sync(dispatch_get_main_queue(),
                      [&] { accepted = AskToTrustHost(_host, _port, _algorithm, _fingerprint); });
        return accepted;
    }

    NSString *const host = [NSString stringWithUTF8StdStringView:_host];
    NSString *const fingerprint = [NSString stringWithUTF8StdStringView:_fingerprint];

    Alert *const alert = [[Alert alloc] init];
    alert.messageText = [NSString
        localizedStringWithFormat:NSLocalizedString(@"Verify the identity of “%@”?",
                                                    "Title of the SFTP host key verification alert"),
                                  host];
    // An unnamed algorithm gets its own sentence rather than a localized word for "host" dropped
    // into a slot: a translator needs the whole sentence to put it into decent grammar.
    alert.informativeText =
        _algorithm.empty()
            ? [NSString localizedStringWithFormat:NSLocalizedString(
                                                      @"This server has not been connected to before. Its host key "
                                                      @"fingerprint is:\n\n%@\n\nAccept it only if it matches the "
                                                      @"fingerprint published by the server’s administrator.",
                                                      "Body of the SFTP host key verification alert, unnamed "
                                                      "algorithm"),
                                                  fingerprint]
            : [NSString localizedStringWithFormat:NSLocalizedString(
                                                      @"This server has not been connected to before. Its %@ key "
                                                      @"fingerprint is:\n\n%@\n\nAccept it only if it matches the "
                                                      @"fingerprint published by the server’s administrator.",
                                                      "Body of the SFTP host key verification alert"),
                                                  [NSString stringWithUTF8StdStringView:_algorithm],
                                                  fingerprint];
    // Cancel comes first so it is the default button: an unread fingerprint must not be accepted by
    // pressing Return on a dialog the user did not expect.
    [alert addButtonWithTitle:NSLocalizedString(@"Cancel", "Cancel button in the SFTP host key verification alert")];
    [alert addButtonWithTitle:NSLocalizedString(@"Trust", "Accept button in the SFTP host key verification alert")];
    alert.alertStyle = NSAlertStyleCritical;
    return [alert runModal] == NSAlertSecondButtonReturn;
}

void InstallSFTPHostKeyPolicy()
{
    // Both live for the process: a connection may verify a host at any time, and the policy must
    // outlive every host that could still be handshaking during shutdown.
    [[clang::no_destroy]] static KeychainHostPinStore store;
    [[clang::no_destroy]] static auto policy = std::make_shared<SFTPHostKeyPolicy>(store, AskToTrustHost);
    nc::vfs::sftp::SetHostKeyVerifier(policy);
}

} // namespace nc::core
