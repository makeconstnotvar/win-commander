// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "RemoteHostTrust.h"

namespace nc::core {

namespace {

bool IsSeparator(const char _character) noexcept
{
    return _character == ':' || _character == ' ' || _character == '-';
}

std::optional<char> HexDigit(const char _character) noexcept
{
    if( _character >= '0' && _character <= '9' )
        return _character;
    if( _character >= 'a' && _character <= 'f' )
        return _character;
    if( _character >= 'A' && _character <= 'F' )
        return static_cast<char>(_character - 'A' + 'a');
    return std::nullopt;
}

} // namespace

std::optional<std::string> NormalizeHostFingerprint(const std::string_view _fingerprint)
{
    std::string normalized;
    normalized.reserve(_fingerprint.size());
    for( const char character : _fingerprint ) {
        if( IsSeparator(character) )
            continue;
        const auto digit = HexDigit(character);
        // Anything that is not hex or a separator makes the whole value unusable. Dropping it
        // instead could map two different inputs onto the same normalized form.
        if( !digit )
            return std::nullopt;
        normalized.push_back(*digit);
    }
    if( normalized.empty() || normalized.size() % 2 != 0 )
        return std::nullopt;
    return normalized;
}

RemoteHostTrustVerdict ClassifyRemoteHost(const std::optional<std::string> &_pinned_fingerprint,
                                          const std::string_view _presented_fingerprint)
{
    const auto presented = NormalizeHostFingerprint(_presented_fingerprint);
    // Nothing was verified, so no amount of pinned state makes this trustworthy - not even when a
    // pin exists, because there is nothing to compare it against.
    if( !presented )
        return RemoteHostTrustVerdict::Unusable;

    if( !_pinned_fingerprint )
        return RemoteHostTrustVerdict::UnknownFirstUse;

    const auto pinned = NormalizeHostFingerprint(*_pinned_fingerprint);
    // A stored pin that cannot be parsed is treated as a mismatch, never as "no pin". Degrading it
    // to first-use would let a corrupted or tampered store silently downgrade an established host
    // into one the user is invited to accept.
    if( !pinned )
        return RemoteHostTrustVerdict::Mismatch;

    return *pinned == *presented ? RemoteHostTrustVerdict::TrustedPinned : RemoteHostTrustVerdict::Mismatch;
}

} // namespace nc::core
