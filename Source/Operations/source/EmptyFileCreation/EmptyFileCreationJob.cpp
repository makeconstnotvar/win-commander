// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "EmptyFileCreationJob.h"
#include <VFS/VFSEasyOps.h>
#include <cerrno>

namespace nc::ops {

EmptyFileCreationJob::EmptyFileCreationJob(std::string _filename,
                                           std::string _root_folder,
                                           VFSHostPtr _vfs)
    : m_Filename(std::move(_filename)), m_RootFolder(std::move(_root_folder)), m_VFS(std::move(_vfs))
{
    Statistics().SetPreferredSource(Statistics::SourceType::Items);
}

EmptyFileCreationJob::~EmptyFileCreationJob() = default;

void EmptyFileCreationJob::Perform()
{
    Statistics().CommitEstimated(Statistics::SourceType::Items, 1);
    if( BlockIfPaused(); IsStopped() )
        return;

    if( !Create() )
        return;

    Statistics().CommitProcessed(Statistics::SourceType::Items, 1);
}

bool EmptyFileCreationJob::Create()
{
    const std::filesystem::path relative{m_Filename};
    const bool is_single_component = !m_Filename.empty() && m_Filename.find('\0') == std::string::npos &&
                                     !relative.is_absolute() && !relative.has_parent_path() && m_Filename != "." &&
                                     m_Filename != "..";
    if( !is_single_component ) {
        Stop();
        return false;
    }

    const std::string path = (std::filesystem::path{m_RootFolder} / relative).native();
    while( true ) {
        if( IsStopped() )
            return false;
        const std::expected<void, Error> result = vfs::easy::VFSEasyCreateEmptyFile(path, m_VFS);
        if( result )
            return true;
        if( m_OnError(result.error(), path, *m_VFS) != ErrorResolution::Retry ) {
            Stop();
            return false;
        }
    }
}

} // namespace nc::ops
