// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "VFSOperationPlanningAccessChecker.h"

#include <WinCommander/States/FilePanels/PanelController.h>

#include <string>
#include <string_view>

namespace nc::core {
namespace {

bool IsValid(const ops::OperationPlanningRequiredAccess _required) noexcept
{
    switch( _required ) {
        case ops::OperationPlanningRequiredAccess::Read:
        case ops::OperationPlanningRequiredAccess::Write:
        case ops::OperationPlanningRequiredAccess::Rename:
        case ops::OperationPlanningRequiredAccess::ReplaceFile:
        case ops::OperationPlanningRequiredAccess::ReplaceDirectory:
        case ops::OperationPlanningRequiredAccess::Delete:
            return true;
    }
    return false;
}

bool IsValidIdentity(const std::string_view _identity) noexcept
{
    return !_identity.empty() && _identity.find('\0') == std::string_view::npos;
}

bool IsValidAbsolutePath(const std::string_view _path) noexcept
{
    return !_path.empty() && _path.front() == '/' && _path.find('\0') == std::string_view::npos;
}

std::string ParentDirectory(std::string_view _path)
{
    while( _path.size() > 1 && _path.back() == '/' )
        _path.remove_suffix(1);
    if( _path == "/" )
        return "/";

    const auto separator = _path.rfind('/');
    if( separator == 0 )
        return "/";

    std::string parent{_path.substr(0, separator)};
    while( parent.size() > 1 && parent.back() == '/' )
        parent.pop_back();
    return parent;
}

ops::OperationPlanningAccessEvidence PermissionRequired() noexcept
{
    return {ops::OperationPlanningAccessState::PermissionRequired};
}

} // namespace

ops::VFSOperationPlanningProbes::AccessChecker
MakeVFSOperationPlanningAccessChecker(panel::DirectoryAccessProvider &_provider)
{
    return [&_provider](const ops::OperationPlanningPath &_path,
                        const ops::OperationPlanningRequiredAccess _required,
                        vfs::Host &_host) -> ops::OperationPlanningProbeResult<ops::OperationPlanningAccessEvidence> {
        if( !IsValidIdentity(_path.provider_id) || !IsValidAbsolutePath(_path.absolute_path) ||
            !IsValid(_required) )
            return PermissionRequired();

        const std::string access_directory = _required == ops::OperationPlanningRequiredAccess::Write ||
                                                     _required == ops::OperationPlanningRequiredAccess::Rename
                                                 ? _path.absolute_path
                                                 : ParentDirectory(_path.absolute_path);
        try {
            const bool granted = _provider.HasAccess(nullptr, access_directory, _host);
            return ops::OperationPlanningAccessEvidence{
                granted ? ops::OperationPlanningAccessState::Granted
                        : ops::OperationPlanningAccessState::PermissionRequired};
        }
        catch( ... ) {
            return PermissionRequired();
        }
    };
}

} // namespace nc::core
