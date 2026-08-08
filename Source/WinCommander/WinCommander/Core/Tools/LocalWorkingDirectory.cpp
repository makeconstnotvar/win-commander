// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "LocalWorkingDirectory.h"

namespace nc::core {

LocalWorkingDirectory ResolveLocalWorkingDirectory(const PaneLocationFacts &_facts)
{
    // Order matters only for which reason is reported; all three are refusals. Provider first,
    // because it is the one that would otherwise resolve to a real but wrong local path.
    if( !_facts.is_native_filesystem )
        return {.refusal = LocalWorkingDirectoryRefusal::NotLocalFilesystem};
    if( !_facts.is_uniform )
        return {.refusal = LocalWorkingDirectoryRefusal::NotUniform};
    if( _facts.path.empty() || _facts.path.front() != '/' )
        return {.refusal = LocalWorkingDirectoryRefusal::NoLocation};

    std::string path{_facts.path};
    // Keep "/" itself; strip a trailing slash anywhere else so a tool that echoes the path back
    // does not show a doubled separator.
    while( path.size() > 1 && path.back() == '/' )
        path.pop_back();
    return {.path = std::move(path), .refusal = LocalWorkingDirectoryRefusal::None};
}

} // namespace nc::core
