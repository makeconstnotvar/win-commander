// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "OrphanedTemporary.h"

#include <string_view>

namespace nc::core {

namespace {

/** mkstemp replaces its six trailing X characters with these, and only these. */
constexpr bool IsMkstempSuffixCharacter(const char _character) noexcept
{
    return (_character >= 'a' && _character <= 'z') || (_character >= 'A' && _character <= 'Z') ||
           (_character >= '0' && _character <= '9');
}

constexpr size_t g_MkstempSuffixLength = 6;

} // namespace

bool IsOrphanedAtomicWriteTemporary(const std::string_view _target_name, const std::string_view _candidate) noexcept
{
    if( _target_name.empty() )
        return false;

    constexpr std::string_view marker = ".nctmp.";
    // "." + "<target>" + ".nctmp." + six characters.
    if( _candidate.size() != 1 + _target_name.size() + marker.size() + g_MkstempSuffixLength )
        return false;
    if( _candidate.front() != '.' )
        return false;
    if( _candidate.substr(1, _target_name.size()) != _target_name )
        return false;
    if( _candidate.substr(1 + _target_name.size(), marker.size()) != marker )
        return false;
    for( const char character : _candidate.substr(1 + _target_name.size() + marker.size()) )
        if( !IsMkstempSuffixCharacter(character) )
            return false;
    return true;
}

} // namespace nc::core
