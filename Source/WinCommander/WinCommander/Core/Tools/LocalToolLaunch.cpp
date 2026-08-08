// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "LocalToolLaunch.h"

#include <algorithm>

namespace nc::core {

namespace {

bool IsBlank(const std::string_view _text)
{
    return std::ranges::all_of(_text, [](const unsigned char _c) { return _c == ' ' || _c == '\t' || _c == '\n'; });
}

/** True when `_path` is `_directory` itself or something inside it, by component. */
bool IsWithin(const std::string_view _directory, const std::string_view _path)
{
    if( !_path.starts_with(_directory) )
        return false;
    if( _path.size() == _directory.size() )
        return true;
    // The next character must be a separator, so `/tmp/abc` is not taken for something in `/tmp/ab`.
    if( _directory == "/" )
        return true;
    return _path[_directory.size()] == '/';
}

} // namespace

std::expected<LocalToolLaunchRequest, LocalToolLaunchRefusal>
PrepareLocalToolLaunch(const LocalToolRole _role,
                       const PaneLocationFacts &_location,
                       const std::string_view _application,
                       const std::vector<std::string> &_documents)
{
    // Location first, deliberately. A missing application is an obvious failure the user can fix; a
    // path inside an archive or on a remote host resolves silently against the real filesystem and
    // opens the wrong place. The refusal that would otherwise be silent is the one to reach.
    const LocalWorkingDirectory directory = ResolveLocalWorkingDirectory(_location);
    if( !directory.Usable() )
        return std::unexpected(LocalToolLaunchRefusal::LocationUnusable);

    if( _application.empty() )
        return std::unexpected(LocalToolLaunchRefusal::ApplicationNotConfigured);
    if( IsBlank(_application) )
        return std::unexpected(LocalToolLaunchRefusal::ApplicationUnusable);

    LocalToolLaunchRequest request{
        .application = std::string{_application}, .working_directory = directory.path, .documents = {}};

    // A terminal is opened *at* a directory, not on files. Handing it the selection would make
    // "open terminal here" mean something different depending on what happened to be selected.
    if( _role == LocalToolRole::Editor ) {
        for( const std::string &document : _documents ) {
            if( document.empty() || document.front() != '/' )
                continue;
            // A selection can outlive the listing it came from. Passing a path from somewhere else
            // is how a stale selection ends up editing the wrong file.
            if( IsWithin(directory.path, document) )
                request.documents.push_back(document);
        }
    }

    return request;
}

} // namespace nc::core
