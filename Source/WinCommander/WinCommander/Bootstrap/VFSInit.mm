// Copyright (C) 2017-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#include "VFSInit.h"
#include <VFS/Native.h>
#include <VFS/ArcLA.h>
#include <VFS/ArcLARaw.h>
#include <VFS/PS.h>
#include <VFS/XAttr.h>
#include <VFS/NetFTP.h>
#include <VFS/NetSFTP.h>
#include <VFS/NetWebDAV.h>
#include <WinCommander/Bootstrap/AppDelegate.h>
#include <WinCommander/Core/Remote/SFTPHostKeyPolicy.h>

namespace nc::bootstrap {

void RegisterAvailableVFS()
{
    auto native_meta = VFSNativeHost::Meta();
    native_meta.SpawnWithConfig = [](const VFSHostPtr &, const VFSConfiguration &, VFSCancelChecker) {
        return NCAppDelegate.me.nativeHostPtr;
    };

    VFSFactory::Instance().RegisterVFS(std::move(native_meta));
    VFSFactory::Instance().RegisterVFS(vfs::PSHost::Meta());
    VFSFactory::Instance().RegisterVFS(vfs::SFTPHost::Meta());
    VFSFactory::Instance().RegisterVFS(vfs::FTPHost::Meta());
    VFSFactory::Instance().RegisterVFS(vfs::ArchiveHost::Meta());
    VFSFactory::Instance().RegisterVFS(vfs::ArchiveRawHost::Meta());
    VFSFactory::Instance().RegisterVFS(vfs::XAttrHost::Meta());
    VFSFactory::Instance().RegisterVFS(vfs::WebDAVHost::Meta());

    // Before anything can spawn an SFTP host, not after: a connection made without a policy in place
    // is refused outright, which is the correct answer but a confusing one to hand a user.
    core::InstallSFTPHostKeyPolicy();
}

} // namespace nc::bootstrap
