// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "FileMetadataSnapshot.h"

#include <VFS/VFS.h>

namespace nc::core {

FileMetadataSnapshot CopyFileMetadataSnapshot(const vfs::ListingItem &_item)
{
    FileMetadataSnapshot snapshot;
    snapshot.path = _item.Path();
    snapshot.filename = _item.Filename();
    if( _item.HasDisplayName() )
        snapshot.display_name = _item.DisplayName();
    if( _item.HasExtension() )
        snapshot.extension = _item.Extension();
    snapshot.unix_mode = _item.UnixMode();
    snapshot.unix_type = _item.UnixType();
    snapshot.is_directory = _item.IsDir();
    snapshot.is_regular = _item.IsReg();
    snapshot.is_symlink = _item.IsSymlink();
    snapshot.is_hidden = _item.IsHidden();
    if( _item.HasSize() )
        snapshot.size = _item.Size();
    if( _item.HasInode() )
        snapshot.inode = _item.Inode();
    if( _item.HasATime() )
        snapshot.accessed_time = _item.ATime();
    if( _item.HasMTime() )
        snapshot.modified_time = _item.MTime();
    if( _item.HasCTime() )
        snapshot.status_changed_time = _item.CTime();
    if( _item.HasBTime() )
        snapshot.created_time = _item.BTime();
    if( _item.HasAddTime() )
        snapshot.added_time = _item.AddTime();
    if( _item.HasUnixFlags() )
        snapshot.unix_flags = _item.UnixFlags();
    if( _item.HasUnixUID() )
        snapshot.unix_uid = _item.UnixUID();
    if( _item.HasUnixGID() )
        snapshot.unix_gid = _item.UnixGID();
    if( _item.HasSymlink() )
        snapshot.symlink_target = _item.Symlink();
    if( _item.HasTags() ) {
        const auto tags = _item.Tags();
        snapshot.tags.reserve(tags.size());
        for( const utility::Tags::Tag &tag : tags )
            snapshot.tags.emplace_back(FileMetadataTag{.label = tag.Label(), .color = tag.Color()});
    }
    return snapshot;
}

} // namespace nc::core
