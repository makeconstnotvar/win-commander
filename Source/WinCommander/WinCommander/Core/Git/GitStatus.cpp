// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "GitStatus.h"

namespace nc::core {

namespace {

bool IsConflictPair(const char _index, const char _worktree) noexcept
{
    // Porcelain v1 spells an unresolved merge with U on either side, or with the AA / DD pairs.
    return _index == 'U' || _worktree == 'U' || (_index == 'A' && _worktree == 'A') ||
           (_index == 'D' && _worktree == 'D');
}

} // namespace

GitFileState ClassifyGitStatusCode(const char _index, const char _worktree) noexcept
{
    if( _index == '?' && _worktree == '?' )
        return GitFileState::Untracked;
    if( _index == '!' && _worktree == '!' )
        return GitFileState::Ignored;

    // A conflict is the one state where doing nothing is wrong, so it outranks every other reading
    // of the same pair - notably AA and DD, which would otherwise look like an ordinary add/delete.
    if( IsConflictPair(_index, _worktree) )
        return GitFileState::Conflicted;

    // The worktree column is what the user is looking at on disk, so it decides when the two
    // columns disagree. A file staged as added but since edited reads as Modified, which is what
    // its row actually shows.
    switch( _worktree ) {
        case 'M':
            return GitFileState::Modified;
        case 'D':
            return GitFileState::Deleted;
        case 'A':
            return GitFileState::Added;
        case 'R':
            return GitFileState::Renamed;
        default:
            break;
    }
    switch( _index ) {
        case 'M':
            return GitFileState::Modified;
        case 'A':
            return GitFileState::Added;
        case 'D':
            return GitFileState::Deleted;
        case 'R':
            return GitFileState::Renamed;
        case 'C':
            return GitFileState::Added; // a copy is a new path in the tree
        default:
            break;
    }
    return GitFileState::Unmodified;
}

std::optional<std::vector<GitStatusEntry>> ParseGitPorcelainV1(const std::string_view _output)
{
    std::vector<GitStatusEntry> entries;
    size_t position = 0;
    while( position < _output.size() ) {
        // Each record is "XY <path>\0", and a rename adds a second "<original>\0" record.
        const size_t terminator = _output.find('\0', position);
        if( terminator == std::string_view::npos )
            return std::nullopt;
        const std::string_view record = _output.substr(position, terminator - position);
        position = terminator + 1;
        if( record.empty() )
            continue;
        // "XY " plus at least one path character.
        if( record.size() < 4 || record[2] != ' ' )
            return std::nullopt;

        GitStatusEntry entry;
        entry.state = ClassifyGitStatusCode(record[0], record[1]);
        entry.path = std::string{record.substr(3)};

        if( entry.state == GitFileState::Renamed ) {
            // The original path is its own NUL-terminated record. Its absence means the output was
            // truncated mid-record, which is exactly the case that must not be salvaged.
            const size_t original_end = _output.find('\0', position);
            if( original_end == std::string_view::npos )
                return std::nullopt;
            entry.original_path = std::string{_output.substr(position, original_end - position)};
            position = original_end + 1;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace nc::core
