// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "RemoteHostTrust.h"

#include <optional>
#include <string>
#include <string_view>

namespace nc::core {

/**
 * Where host pins live.
 *
 * An interface rather than a concrete Keychain call so the trust decisions above it are testable
 * without a real keychain, and so a failing store is a case the policy has to answer rather than
 * something that only shows up on a device.
 */
class RemoteHostPinStore
{
public:
    virtual ~RemoteHostPinStore() = default;

    /** The pinned fingerprint for a host, or nothing when none is pinned. */
    [[nodiscard]] virtual std::optional<std::string> LoadPin(std::string_view _provider,
                                                             std::string_view _host) const = 0;
    /** Returns false when the pin could not be made durable. */
    [[nodiscard]] virtual bool StorePin(std::string_view _provider,
                                        std::string_view _host,
                                        std::string_view _fingerprint) = 0;
    [[nodiscard]] virtual bool ErasePin(std::string_view _provider, std::string_view _host) = 0;
};

/**
 * The host-trust decisions with their store attached.
 *
 * Pinning is split into two operations on purpose. `TrustOnFirstUse` is the routine one a
 * connection flow may call after the user accepts an unknown host; `ReplacePin` is the deliberate
 * one for a host whose key legitimately changed. Keeping them apart is what stops the routine path
 * from ever overwriting an established pin - which would silently undo a mismatch warning.
 */
class RemoteHostTrustPolicy final
{
public:
    explicit RemoteHostTrustPolicy(RemoteHostPinStore &_store) noexcept;

    [[nodiscard]] RemoteHostTrustVerdict Verify(std::string_view _provider,
                                                std::string_view _host,
                                                std::string_view _presented_fingerprint) const;

    /**
     * Pins a host seen for the first time. Refuses - without touching the store - unless the
     * current verdict is exactly `UnknownFirstUse`, so it can never overwrite an existing pin,
     * replace a mismatched one, or record something that failed to verify.
     */
    [[nodiscard]] bool TrustOnFirstUse(std::string_view _provider,
                                       std::string_view _host,
                                       std::string_view _presented_fingerprint);

    /**
     * Replaces the pin for a host whose key legitimately changed. Requires a usable fingerprint but
     * deliberately accepts a `Mismatch`, because that is the case it exists for. Callers must have
     * an explicit user decision behind this; the policy cannot check that for them.
     */
    [[nodiscard]] bool ReplacePin(std::string_view _provider,
                                  std::string_view _host,
                                  std::string_view _presented_fingerprint);

    /** Forgets a host, returning it to first-use. */
    [[nodiscard]] bool Forget(std::string_view _provider, std::string_view _host);

private:
    RemoteHostPinStore *m_Store;
};

} // namespace nc::core
