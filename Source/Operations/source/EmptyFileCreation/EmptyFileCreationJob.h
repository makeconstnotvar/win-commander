// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "../Job.h"
#include <VFS/VFS.h>

namespace nc::ops {

struct EmptyFileCreationJobCallbacks {
    enum class ErrorResolution : uint8_t {
        Stop,
        Retry
    };
    std::function<ErrorResolution(Error _err, const std::string &_path, VFSHost &_vfs)> m_OnError =
        [](Error, const std::string &, VFSHost &) { return ErrorResolution::Stop; };
};

class EmptyFileCreationJob final : public Job, public EmptyFileCreationJobCallbacks
{
public:
    EmptyFileCreationJob(std::string _filename, std::string _root_folder, VFSHostPtr _vfs);
    ~EmptyFileCreationJob() override;

private:
    void Perform() override;
    [[nodiscard]] bool Create();

    std::string m_Filename;
    std::string m_RootFolder;
    VFSHostPtr m_VFS;
};

} // namespace nc::ops
