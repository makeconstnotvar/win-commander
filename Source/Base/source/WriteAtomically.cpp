// Copyright (C) 2021-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#include "WriteAtomically.h"
#include "StackAllocator.h"
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace nc::base {

std::expected<void, Error>
WriteAtomically(const std::filesystem::path &_path, std::span<const std::byte> _bytes, bool _follow_symlink) noexcept
{
    if( _path.empty() || !_path.is_absolute() ) {
        return std::unexpected(Error{Error::POSIX, EINVAL});
    }

    nc::StackAllocator alloc;
    std::pmr::string target_path(_path.c_str(), &alloc);

    if( _follow_symlink ) {
        // Try the read the real target path
        char actualpath[PATH_MAX + 1];
        if( realpath(_path.c_str(), actualpath) ) {
            target_path = actualpath;
        }
        else {
            // Non-existing entries are ok
            if( errno != ENOENT ) {
                return std::unexpected(Error{Error::POSIX, errno});
            }
        }
    }

    // Open a temporary file next to the destination
    std::pmr::string temp_path(target_path, &alloc);
    temp_path += ".XXXXXX";
    const auto fd = mkstemp(temp_path.data());
    if( fd < 0 )
        return std::unexpected(Error{Error::POSIX, errno});

    // Write the data into the temporary file
    ssize_t left = _bytes.size();
    const std::byte *ptr = _bytes.data();
    while( left > 0 ) {
        const auto write_rc = write(fd, ptr, left);
        if( write_rc >= 0 ) {
            left -= write_rc;
            ptr += write_rc;
        }
        else {
            const int err = errno;
            close(fd);
            unlink(temp_path.c_str());
            return std::unexpected(Error{Error::POSIX, err});
        }
    }

    // Flush the contents before the rename can publish the name. Without this the directory entry
    // may reach the disk while the data blocks have not, so a power loss can leave a correctly
    // named file holding garbage - which is worse than losing the write outright, because nothing
    // downstream can tell that it happened.
    if( fsync(fd) != 0 ) {
        const int err = errno;
        close(fd);
        unlink(temp_path.c_str());
        return std::unexpected(Error{Error::POSIX, err});
    }
    close(fd);

    // Rename into the destination atomically
    if( rename(temp_path.c_str(), target_path.c_str()) != 0 ) {
        const int err = errno;
        unlink(temp_path.c_str());
        return std::unexpected(Error{Error::POSIX, err});
    }

    // The rename itself is a directory mutation, and it is durable only once the directory is
    // flushed: otherwise a crash can roll the entry back and the file silently reverts to its
    // previous contents, with the call having already reported success.
    //
    // A failure here is deliberately NOT reported as an error and the file is not rolled back. The
    // rename has already succeeded, so the new contents are visible and correct to everything
    // running now; only durability across an unclean shutdown is in doubt, and undoing a completed
    // publication would turn a durability question into certain data loss.
    const std::filesystem::path parent = std::filesystem::path{target_path.c_str()}.parent_path();
    if( const int dir_fd = open(parent.c_str(), O_RDONLY | O_CLOEXEC); dir_fd >= 0 ) {
        fsync(dir_fd);
        close(dir_fd);
    }
    return {};
}

} // namespace nc::base
