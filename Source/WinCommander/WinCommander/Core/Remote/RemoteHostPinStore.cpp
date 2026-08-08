// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "RemoteHostPinStore.h"

namespace nc::core {

RemoteHostTrustPolicy::RemoteHostTrustPolicy(RemoteHostPinStore &_store) noexcept : m_Store(&_store)
{
}

RemoteHostTrustVerdict RemoteHostTrustPolicy::Verify(const std::string_view _provider,
                                                     const std::string_view _host,
                                                     const std::string_view _presented_fingerprint) const
{
    return ClassifyRemoteHost(m_Store->LoadPin(_provider, _host), _presented_fingerprint);
}

bool RemoteHostTrustPolicy::TrustOnFirstUse(const std::string_view _provider,
                                            const std::string_view _host,
                                            const std::string_view _presented_fingerprint)
{
    // Exactly UnknownFirstUse, checked against the live store rather than against whatever the
    // caller last saw. Anything else - an existing pin, a mismatch, an unverifiable fingerprint -
    // must not be resolvable by the routine accept path, or a mismatch warning could be undone by
    // clicking the same button that accepts a new host.
    if( Verify(_provider, _host, _presented_fingerprint) != RemoteHostTrustVerdict::UnknownFirstUse )
        return false;
    const auto normalized = NormalizeHostFingerprint(_presented_fingerprint);
    if( !normalized )
        return false;
    return m_Store->StorePin(_provider, _host, *normalized);
}

bool RemoteHostTrustPolicy::ReplacePin(const std::string_view _provider,
                                       const std::string_view _host,
                                       const std::string_view _presented_fingerprint)
{
    // A usable fingerprint is still required - replacing a pin with something unverifiable would
    // leave the host permanently unverifiable rather than merely re-pinned.
    const auto normalized = NormalizeHostFingerprint(_presented_fingerprint);
    if( !normalized )
        return false;
    return m_Store->StorePin(_provider, _host, *normalized);
}

bool RemoteHostTrustPolicy::Forget(const std::string_view _provider, const std::string_view _host)
{
    return m_Store->ErasePin(_provider, _host);
}

} // namespace nc::core
