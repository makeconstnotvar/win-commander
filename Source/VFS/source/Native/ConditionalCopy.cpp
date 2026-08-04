// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ConditionalCopy.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <sys/acl.h>
#include <sys/clonefile.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace nc::vfs::native {
namespace {

constexpr uint32_t g_ConditionalCopyAllowedUserFlags =
    static_cast<uint32_t>(UF_NODUMP | UF_IMMUTABLE | UF_APPEND | UF_HIDDEN);

ConditionalCopyTimestamp ConditionalCopyTimestampFrom(const timespec &_value) noexcept
{
    return ConditionalCopyTimestamp{.seconds = _value.tv_sec, .nanoseconds = _value.tv_nsec};
}

bool ConditionalCopyStatSealMatches(const struct stat &_before, const struct stat &_after) noexcept
{
    return _before.st_dev == _after.st_dev && _before.st_ino == _after.st_ino && _before.st_uid == _after.st_uid &&
           _before.st_gid == _after.st_gid && _before.st_mode == _after.st_mode &&
           _before.st_flags == _after.st_flags && _before.st_nlink == _after.st_nlink &&
           _before.st_size == _after.st_size && _before.st_atimespec.tv_sec == _after.st_atimespec.tv_sec &&
           _before.st_atimespec.tv_nsec == _after.st_atimespec.tv_nsec &&
           _before.st_mtimespec.tv_sec == _after.st_mtimespec.tv_sec &&
           _before.st_mtimespec.tv_nsec == _after.st_mtimespec.tv_nsec &&
           _before.st_ctimespec.tv_sec == _after.st_ctimespec.tv_sec &&
           _before.st_ctimespec.tv_nsec == _after.st_ctimespec.tv_nsec &&
           _before.st_birthtimespec.tv_sec == _after.st_birthtimespec.tv_sec &&
           _before.st_birthtimespec.tv_nsec == _after.st_birthtimespec.tv_nsec;
}

std::expected<std::vector<std::byte>, int> ConditionalCopyReadACL(int _fd) noexcept
{
    acl_t acl = acl_get_fd_np(_fd, ACL_TYPE_EXTENDED);
    if( acl == nullptr && errno == ENOENT )
        return std::vector<std::byte>{};
    if( acl == nullptr )
        return std::unexpected(errno == 0 ? EIO : errno);
    const auto free_acl = std::unique_ptr<void, decltype(&acl_free)>{acl, &acl_free};

    const ssize_t size = acl_size(acl);
    if( size < 0 )
        return std::unexpected(errno == 0 ? EIO : errno);
    if( static_cast<uint64_t>(size) > ConditionalCopyIO::MaxACLBytes )
        return std::unexpected(EFBIG);

    try {
        std::vector<std::byte> bytes(static_cast<size_t>(size));
        if( size != 0 && acl_copy_ext(bytes.data(), acl, size) != size )
            return std::unexpected(errno == 0 ? EIO : errno);
        return bytes;
    } catch( ... ) {
        return std::unexpected(ENOMEM);
    }
}

std::expected<std::vector<std::pair<std::string, std::vector<std::byte>>>, int>
ConditionalCopyReadExtendedAttributes(int _fd) noexcept
{
    const ssize_t names_size = flistxattr(_fd, nullptr, 0, 0);
    if( names_size < 0 )
        return std::unexpected(errno == 0 ? EIO : errno);
    if( static_cast<uint64_t>(names_size) > ConditionalCopyIO::MaxExtendedAttributeNameBytes )
        return std::unexpected(EFBIG);

    try {
        std::vector<char> names(static_cast<size_t>(names_size));
        if( names_size != 0 ) {
            const ssize_t read_names = flistxattr(_fd, names.data(), names.size(), 0);
            if( read_names != names_size )
                return std::unexpected(errno == 0 ? EAGAIN : errno);
        }

        std::vector<std::pair<std::string, std::vector<std::byte>>> attributes;
        size_t offset = 0;
        size_t total_value_bytes = 0;
        while( offset < names.size() ) {
            const auto end = std::find(names.begin() + static_cast<ptrdiff_t>(offset), names.end(), '\0');
            if( end == names.end() || end == names.begin() + static_cast<ptrdiff_t>(offset) )
                return std::unexpected(EIO);
            std::string name(names.begin() + static_cast<ptrdiff_t>(offset), end);
            offset = static_cast<size_t>(std::distance(names.begin(), end)) + 1;

            const ssize_t value_size = fgetxattr(_fd, name.c_str(), nullptr, 0, 0, 0);
            if( value_size < 0 )
                return std::unexpected(errno == 0 ? EIO : errno);
            if( static_cast<uint64_t>(value_size) > ConditionalCopyIO::MaxExtendedAttributeValueBytes ||
                static_cast<size_t>(value_size) >
                    ConditionalCopyIO::MaxExtendedAttributeValueBytes - total_value_bytes ) {
                return std::unexpected(EFBIG);
            }
            total_value_bytes += static_cast<size_t>(value_size);
            std::vector<std::byte> value(static_cast<size_t>(value_size));
            if( value_size != 0 ) {
                const ssize_t read_value = fgetxattr(_fd, name.c_str(), value.data(), value.size(), 0, 0);
                if( read_value != value_size )
                    return std::unexpected(errno == 0 ? EAGAIN : errno);
            }
            attributes.emplace_back(std::move(name), std::move(value));
        }
        std::ranges::sort(
            attributes, {}, [](const auto &_attribute) -> const std::string & { return _attribute.first; });
        return attributes;
    } catch( ... ) {
        return std::unexpected(ENOMEM);
    }
}

} // namespace

static ConditionalCopyVolumeDecision
EvaluateConditionalCopyVolumeImpl(const nc::utility::NativeFileSystemInfo &_volume, const bool _requires_clone) noexcept
{
    const auto media =
        _volume.mount_flags.internal ? ConditionalCopyVolumeMedia::Internal : ConditionalCopyVolumeMedia::External;
    if( _volume.fs_type_name != "apfs" )
        return {.disposition = ConditionalCopyVolumeDisposition::UnsupportedFilesystem, .media = media};
    if( !_volume.mount_flags.local )
        return {.disposition = ConditionalCopyVolumeDisposition::NonLocal, .media = media};
    // F_FULLFSYNC can only prove the durability contract for media whose flush semantics are controlled by the
    // local system. External, removable and ejectable devices may acknowledge a flush without stable persistence.
    if( !_volume.mount_flags.internal || _volume.mount_flags.removable || _volume.mount_flags.ejectable )
        return {.disposition = ConditionalCopyVolumeDisposition::UnsupportedExternalMedia, .media = media};
    if( _volume.mount_flags.read_only )
        return {.disposition = ConditionalCopyVolumeDisposition::ReadOnly, .media = media};
    if( _volume.mount_flags.unknown_permissions || _volume.format.no_permissions )
        return {.disposition = ConditionalCopyVolumeDisposition::UnknownPermissions, .media = media};
    if( _requires_clone && !_volume.interfaces.clone )
        return {.disposition = ConditionalCopyVolumeDisposition::CloneUnavailable, .media = media};
    if( !_volume.interfaces.attr_list || !_volume.interfaces.extended_attr || !_volume.interfaces.extended_security ) {
        return {.disposition = ConditionalCopyVolumeDisposition::MetadataUnavailable, .media = media};
    }
    return {.disposition = ConditionalCopyVolumeDisposition::Supported, .media = media};
}

ConditionalCopyVolumeDecision EvaluateConditionalCopyVolume(const nc::utility::NativeFileSystemInfo &_volume) noexcept
{
    return EvaluateConditionalCopyVolumeImpl(_volume, true);
}

ConditionalCopyVolumeDecision
EvaluateConditionalCopyStagingVolume(const nc::utility::NativeFileSystemInfo &_volume) noexcept
{
    return EvaluateConditionalCopyVolumeImpl(_volume, false);
}

bool ConditionalCopyVolumesMatch(const nc::utility::NativeFileSystemInfo &_source,
                                 const nc::utility::NativeFileSystemInfo &_destination) noexcept
{
    return _source.mounted_at_path == _destination.mounted_at_path &&
           _source.basic.fs_id.val[0] == _destination.basic.fs_id.val[0] &&
           _source.basic.fs_id.val[1] == _destination.basic.fs_id.val[1];
}

std::expected<void, ConditionalCopyMetadataPolicyError>
ValidateConditionalCopyMetadataPolicy(const ConditionalCopyMetadataSnapshot &_source,
                                      const ConditionalCopyMetadataSnapshot &_destination_parent) noexcept
{
    if( _source.uid != static_cast<uint32_t>(geteuid()) || _source.gid != _destination_parent.gid )
        return std::unexpected(ConditionalCopyMetadataPolicyError::UnsupportedOwnership);
    if( (_source.mode & 07000) != 0 )
        return std::unexpected(ConditionalCopyMetadataPolicyError::UnsupportedMode);
    if( (_source.flags & ~g_ConditionalCopyAllowedUserFlags) != 0 )
        return std::unexpected(ConditionalCopyMetadataPolicyError::UnsupportedFlags);
    if( !_destination_parent.acl.empty() )
        return std::unexpected(ConditionalCopyMetadataPolicyError::DestinationParentACL);
    return {};
}

bool ConditionalCopyMetadataMatchesClone(const ConditionalCopyMetadataSnapshot &_source,
                                         const ConditionalCopyMetadataSnapshot &_destination) noexcept
{
    return (_destination.mode & S_IFMT) == S_IFREG && _destination.device == _source.device &&
           _destination.inode != _source.inode && _destination.uid == _source.uid && _destination.gid == _source.gid &&
           (_destination.mode & 07777) == (_source.mode & 07777) && _destination.flags == _source.flags &&
           _destination.link_count == 1 && _destination.size == _source.size &&
           _destination.access_time == _source.access_time &&
           _destination.modification_time == _source.modification_time &&
           _destination.birth_time == _source.birth_time && _destination.acl == _source.acl &&
           _destination.extended_attributes == _source.extended_attributes;
}

ConditionalCopyIO::~ConditionalCopyIO() = default;

int ConditionalCopyIO::Open(const char *_path, int _flags) noexcept
{
    return open(_path, _flags);
}

int ConditionalCopyIO::OpenAt(int _directory_fd, const char *_name, int _flags) noexcept
{
    return openat(_directory_fd, _name, _flags);
}

int ConditionalCopyIO::FStat(int _fd, struct stat *_stat) noexcept
{
    return fstat(_fd, _stat);
}

int ConditionalCopyIO::FStatAt(int _directory_fd, const char *_name, struct stat *_stat, int _flags) noexcept
{
    return fstatat(_directory_fd, _name, _stat, _flags);
}

int ConditionalCopyIO::Clone(int _source_fd, int _destination_parent_fd, const char *_name, uint32_t _flags) noexcept
{
    return fclonefileat(_source_fd, _destination_parent_fd, _name, _flags);
}

int ConditionalCopyIO::FSync(int _fd) noexcept
{
    return fsync(_fd);
}

int ConditionalCopyIO::FullFSync(int _fd) noexcept
{
    return fcntl(_fd, F_FULLFSYNC);
}

int ConditionalCopyIO::Close(int _fd) noexcept
{
    return close(_fd);
}

std::expected<ConditionalCopyMetadataSnapshot, int> ConditionalCopyIO::CaptureMetadata(int _fd) noexcept
{
    try {
        struct stat value{};
        if( FStat(_fd, &value) != 0 )
            return std::unexpected(errno == 0 ? EIO : errno);
        auto acl = ConditionalCopyReadACL(_fd);
        if( !acl )
            return std::unexpected(acl.error());
        auto extended_attributes = ConditionalCopyReadExtendedAttributes(_fd);
        if( !extended_attributes )
            return std::unexpected(extended_attributes.error());
        struct stat seal{};
        if( FStat(_fd, &seal) != 0 )
            return std::unexpected(errno == 0 ? EIO : errno);
        if( !ConditionalCopyStatSealMatches(value, seal) )
            return std::unexpected(EAGAIN);
        return ConditionalCopyMetadataSnapshot{
            .device = static_cast<uint64_t>(seal.st_dev),
            .inode = static_cast<uint64_t>(seal.st_ino),
            .uid = static_cast<uint32_t>(seal.st_uid),
            .gid = static_cast<uint32_t>(seal.st_gid),
            .mode = static_cast<uint32_t>(seal.st_mode),
            .flags = static_cast<uint32_t>(seal.st_flags),
            .link_count = static_cast<uint64_t>(seal.st_nlink),
            .size = static_cast<uint64_t>(seal.st_size),
            .access_time = ConditionalCopyTimestampFrom(seal.st_atimespec),
            .modification_time = ConditionalCopyTimestampFrom(seal.st_mtimespec),
            .change_time = ConditionalCopyTimestampFrom(seal.st_ctimespec),
            .birth_time = ConditionalCopyTimestampFrom(seal.st_birthtimespec),
            .acl = std::move(*acl),
            .extended_attributes = std::move(*extended_attributes),
        };
    } catch( ... ) {
        return std::unexpected(ENOMEM);
    }
}

} // namespace nc::vfs::native
