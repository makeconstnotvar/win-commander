// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CrossVolumeStagingHelperDescriptorSealValidator.h"

#include <fcntl.h>
#include <sys/stat.h>

namespace nc::routedio::cross_volume_staging::helper {
namespace {

bool TimestampFrom(const timespec &_value, Timestamp &_timestamp) noexcept
{
    if( _value.tv_nsec < 0 || _value.tv_nsec >= 1'000'000'000 )
        return false;
    _timestamp = Timestamp{
        .seconds = _value.tv_sec,
        .nanoseconds = static_cast<uint32_t>(_value.tv_nsec),
    };
    return true;
}

bool SealFrom(const struct stat &_value, ObjectSeal &_seal) noexcept
{
    if( _value.st_size < 0 || _value.st_nlink <= 0 )
        return false;

    ObjectSeal seal{
        .device = static_cast<uint64_t>(_value.st_dev),
        .inode = static_cast<uint64_t>(_value.st_ino),
        .uid = static_cast<uint32_t>(_value.st_uid),
        .gid = static_cast<uint32_t>(_value.st_gid),
        .mode = static_cast<uint32_t>(_value.st_mode),
        .flags = static_cast<uint32_t>(_value.st_flags),
        .link_count = static_cast<uint64_t>(_value.st_nlink),
        .byte_size = static_cast<uint64_t>(_value.st_size),
    };
    if( !TimestampFrom(_value.st_birthtimespec, seal.birth_time) ||
        !TimestampFrom(_value.st_mtimespec, seal.modification_time) ||
        !TimestampFrom(_value.st_ctimespec, seal.status_change_time) )
        return false;
    _seal = seal;
    return true;
}

enum class DescriptorPreparation : uint8_t {
    Ready,
    InvalidRequest,
    HelperFailure,
};

DescriptorPreparation PrepareReadOnlyDescriptor(const int _fd) noexcept
{
    const int descriptor_flags = fcntl(_fd, F_GETFD);
    if( descriptor_flags < 0 || fcntl(_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0 )
        return DescriptorPreparation::HelperFailure;
    const int verified_descriptor_flags = fcntl(_fd, F_GETFD);
    if( verified_descriptor_flags < 0 || (verified_descriptor_flags & FD_CLOEXEC) == 0 )
        return DescriptorPreparation::HelperFailure;

    const int status_flags = fcntl(_fd, F_GETFL);
    if( status_flags < 0 )
        return DescriptorPreparation::HelperFailure;
    return (status_flags & O_ACCMODE) == O_RDONLY ? DescriptorPreparation::Ready
                                                   : DescriptorPreparation::InvalidRequest;
}

} // namespace

std::expected<ValidatedBegin, BeginDescriptorValidationError>
ValidateBeginDescriptors(xpc_codec::DecodedBegin _begin) noexcept
{
    if( !Validate(_begin.request) )
        return std::unexpected{BeginDescriptorValidationError::InvalidRequest};

    const auto descriptors = _begin.descriptors.Borrow();
    if( descriptors.source_fd < 0 || descriptors.destination_parent_fd < 0 ||
        descriptors.source_fd == descriptors.destination_parent_fd )
        return std::unexpected{BeginDescriptorValidationError::InvalidRequest};
    const auto source_prepared = PrepareReadOnlyDescriptor(descriptors.source_fd);
    const auto destination_parent_prepared = PrepareReadOnlyDescriptor(descriptors.destination_parent_fd);
    if( source_prepared == DescriptorPreparation::InvalidRequest ||
        destination_parent_prepared == DescriptorPreparation::InvalidRequest )
        return std::unexpected{BeginDescriptorValidationError::InvalidRequest};
    if( source_prepared != DescriptorPreparation::Ready || destination_parent_prepared != DescriptorPreparation::Ready )
        return std::unexpected{BeginDescriptorValidationError::HelperFailure};

    struct stat source{};
    if( fstat(descriptors.source_fd, &source) != 0 )
        return std::unexpected{BeginDescriptorValidationError::HelperFailure};
    struct stat destination_parent{};
    if( fstat(descriptors.destination_parent_fd, &destination_parent) != 0 )
        return std::unexpected{BeginDescriptorValidationError::HelperFailure};

    ObjectSeal source_seal;
    if( !SealFrom(source, source_seal) || source_seal != _begin.request.source )
        return std::unexpected{BeginDescriptorValidationError::SourceStale};
    ObjectSeal destination_parent_seal;
    if( !SealFrom(destination_parent, destination_parent_seal) || destination_parent_seal != _begin.request.destination_parent )
        return std::unexpected{BeginDescriptorValidationError::DestinationParentStale};
    return ValidatedBegin{std::move(_begin)};
}

} // namespace nc::routedio::cross_volume_staging::helper
