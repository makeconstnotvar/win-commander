// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "../Operation.h"
#include <VFS/VFS.h>

namespace nc::ops {

class EmptyFileCreationJob;
struct EmptyFileCreationJobCallbacks;

/** A narrow legacy operation that requests one no-overwrite empty regular-file creation. */
class EmptyFileCreation final : public Operation
{
public:
    EmptyFileCreation(std::string _filename, std::string _root_folder, VFSHost &_vfs);
    ~EmptyFileCreation() override;

    [[nodiscard]] const std::string &Filename() const noexcept;

private:
    using Callbacks = EmptyFileCreationJobCallbacks;

    Job *GetJob() noexcept override;
    int OnError(Error _err, const std::string &_path, VFSHost &_vfs);

    std::string m_Filename;
    std::unique_ptr<EmptyFileCreationJob> m_Job;
};

} // namespace nc::ops
