// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "EmptyFileCreation.h"
#include "EmptyFileCreationJob.h"
#include <Operations/Localizable.h>
#include "../AsyncDialogResponse.h"
#include "../Internal.h"
#include <Utility/StringExtras.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"

namespace nc::ops {

EmptyFileCreation::EmptyFileCreation(std::string _filename, std::string _root_folder, VFSHost &_vfs)
    : m_Filename(std::move(_filename))
{
    m_Job = std::make_unique<EmptyFileCreationJob>(m_Filename, std::move(_root_folder), _vfs.shared_from_this());
    m_Job->m_OnError = [this](Error _err, const std::string &_path, VFSHost &_host) {
        return static_cast<Callbacks::ErrorResolution>(OnError(_err, _path, _host));
    };

    const auto title = [NSString localizedStringWithFormat:localizable::EmptyFileCreatingTitle(),
                                                           [NSString stringWithUTF8StdString:m_Filename]];
    SetTitle(title.UTF8String);
}

EmptyFileCreation::~EmptyFileCreation()
{
    Wait();
}

const std::string &EmptyFileCreation::Filename() const noexcept
{
    return m_Filename;
}

Job *EmptyFileCreation::GetJob() noexcept
{
    return m_Job.get();
}

int EmptyFileCreation::OnError(Error _err, const std::string &_path, VFSHost &_vfs)
{
    if( !IsInteractive() )
        return (int)Callbacks::ErrorResolution::Stop;

    const auto context = std::make_shared<AsyncDialogResponse>();
    ShowGenericDialog(
        GenericDialog::AbortRetry, localizable::EmptyFileFailedToCreateMessage(), _err, {_vfs, _path}, context);
    WaitForDialogResponse(context);
    return context->response == NSModalResponseRetry ? (int)Callbacks::ErrorResolution::Retry
                                                     : (int)Callbacks::ErrorResolution::Stop;
}

} // namespace nc::ops

#pragma clang diagnostic pop
