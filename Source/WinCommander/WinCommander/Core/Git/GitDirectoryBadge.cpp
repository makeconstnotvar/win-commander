// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "GitDirectoryBadge.h"

#include <map>

namespace nc::core {

std::optional<GitFileState> AggregateGitDirectoryBadge(const std::span<const GitFileState> _below)
{
    bool anything = false;
    for( const GitFileState state : _below ) {
        // An unresolved merge is the one state where doing nothing is wrong. Burying it under a
        // milder summary is how it gets missed, so it wins outright and immediately.
        if( state == GitFileState::Conflicted )
            return GitFileState::Conflicted;
        // Ignored paths can number in the thousands and their contents are ignored too, so a
        // directory does not become interesting for containing them.
        if( state == GitFileState::Unmodified || state == GitFileState::Ignored )
            continue;
        anything = true;
    }
    // Everything else summarises as Modified: a directory holding untracked files is not itself
    // untracked, and claiming it was added or deleted would be false.
    return anything ? std::optional{GitFileState::Modified} : std::nullopt;
}

namespace {

/** The part of `_path` directly under `_directory`, and whether anything follows it. */
struct Child {
    std::string_view name;
    bool is_directory = false;
};

std::optional<Child> ChildOf(const std::string_view _directory, const std::string_view _path)
{
    std::string_view remainder = _path;
    if( !_directory.empty() ) {
        if( !_path.starts_with(_directory) )
            return std::nullopt;
        if( _path.size() <= _directory.size() || _path[_directory.size()] != '/' )
            // Matched as characters but not as a path component: `src` must not collect what belongs
            // to `src-vendor`, which would badge a folder for changes it does not contain.
            return std::nullopt;
        remainder = _path.substr(_directory.size() + 1);
    }
    if( remainder.empty() )
        return std::nullopt;
    const size_t slash = remainder.find('/');
    return slash == std::string_view::npos ? Child{.name = remainder, .is_directory = false}
                                           : Child{.name = remainder.substr(0, slash), .is_directory = true};
}

} // namespace

std::vector<GitBadgeAssignment> BadgeGitDirectoryChildren(const std::string_view _directory,
                                                          const std::span<const GitStatusEntry> _status)
{
    // Insertion-ordered would need a second structure; the caller is matching these against a
    // listing by name, so an ordered map keeps repeated runs identical without one.
    std::map<std::string_view, std::vector<GitFileState>> below;
    std::map<std::string_view, GitFileState> files;
    std::vector<std::string_view> order;

    for( const GitStatusEntry &entry : _status ) {
        const auto child = ChildOf(_directory, entry.path);
        if( !child )
            continue;
        if( child->is_directory ) {
            if( below.find(child->name) == below.end() )
                order.push_back(child->name);
            below[child->name].push_back(entry.state);
        }
        else {
            if( files.find(child->name) == files.end() )
                order.push_back(child->name);
            files[child->name] = entry.state;
        }
    }

    std::vector<GitBadgeAssignment> assignments;
    assignments.reserve(order.size());
    for( const std::string_view name : order ) {
        if( const auto file = files.find(name); file != files.end() ) {
            if( ShouldBadgeGitFileState(file->second) )
                assignments.push_back(GitBadgeAssignment{.path = std::string{name}, .state = file->second});
            continue;
        }
        const auto directory = below.find(name);
        if( directory == below.end() )
            continue;
        if( const auto badge = AggregateGitDirectoryBadge(directory->second) )
            assignments.push_back(GitBadgeAssignment{.path = std::string{name}, .state = *badge});
    }
    return assignments;
}

} // namespace nc::core
