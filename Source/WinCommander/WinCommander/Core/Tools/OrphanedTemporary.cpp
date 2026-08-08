// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "OrphanedTemporary.h"

#include <Base/WriteAtomically.h>

#include <string_view>

namespace nc::core {

namespace {

/** mkstemp replaces its six trailing X characters with these, and only these. */
constexpr bool IsMkstempSuffixCharacter(const char _character) noexcept
{
    return (_character >= 'a' && _character <= 'z') || (_character >= 'A' && _character <= 'Z') ||
           (_character >= '0' && _character <= '9');
}

/** Taken from the writer rather than restated, so the two cannot drift apart. */
constexpr size_t g_MkstempSuffixLength = nc::base::g_AtomicWriteTemporarySuffixLength;

} // namespace

bool IsOrphanedAtomicWriteTemporary(const std::string_view _target_name, const std::string_view _candidate) noexcept
{
    if( _target_name.empty() )
        return false;

    constexpr std::string_view marker = nc::base::g_AtomicWriteTemporaryMarker;
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

OrphanedTemporarySweepResult SweepOrphanedTemporaries(const std::filesystem::path &_target) noexcept
{
    OrphanedTemporarySweepResult result;
    try {
        if( _target.empty() || !_target.has_filename() )
            return result;
        const std::filesystem::path directory = _target.parent_path();
        if( directory.empty() )
            return result;
        const std::string target_name = _target.filename().native();

        std::error_code ec;
        // skip_permission_denied: an unreadable directory is not this function's problem to report,
        // and must not stop startup.
        std::filesystem::directory_iterator it{
            directory, std::filesystem::directory_options::skip_permission_denied, ec};
        if( ec )
            return result;

        for( const std::filesystem::directory_entry &entry : it ) {
            if( !IsOrphanedAtomicWriteTemporary(target_name, entry.path().filename().native()) )
                continue;
            // symlink_status, not status: a symlink wearing the name was not written by us, and
            // resolving it would let a planted link redirect the deletion elsewhere.
            const std::filesystem::file_status status = std::filesystem::symlink_status(entry.path(), ec);
            if( ec || status.type() != std::filesystem::file_type::regular )
                continue;
            if( std::filesystem::remove(entry.path(), ec) && !ec )
                result.removed.push_back(entry.path().native());
            else
                result.failed.push_back(entry.path().native());
        }
    } catch( ... ) {
        // Best-effort cleanup: never let tidying up prevent the application from starting.
    }
    return result;
}

} // namespace nc::core
