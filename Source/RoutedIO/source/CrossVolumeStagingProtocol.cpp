// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include <RoutedIO/CrossVolumeStagingProtocol.h>

#include <cerrno>
#include <sys/stat.h>

namespace nc::routedio::cross_volume_staging {
namespace {

constexpr uint32_t kNanosecondsPerSecond = 1'000'000'000;

bool IsAllZero(std::span<const uint8_t> _bytes) noexcept
{
    for( const uint8_t byte : _bytes ) {
        if( byte != 0 )
            return false;
    }
    return true;
}

bool IsValidTimestamp(const Timestamp &_timestamp) noexcept
{
    return _timestamp.nanoseconds < kNanosecondsPerSecond;
}

bool IsValidObjectSeal(const ObjectSeal &_seal, const mode_t _expected_type) noexcept
{
    return (_seal.mode & S_IFMT) == _expected_type && _seal.inode != 0 && _seal.link_count != 0 &&
           IsValidTimestamp(_seal.birth_time) && IsValidTimestamp(_seal.modification_time) &&
           IsValidTimestamp(_seal.status_change_time);
}

bool IsValidDestinationComponentBytes(std::span<const uint8_t> _bytes) noexcept
{
    if( _bytes.empty() || _bytes.size() > kMaximumDestinationComponentBytes )
        return false;

    if( _bytes.size() == 1 && _bytes.front() == '.' )
        return false;
    if( _bytes.size() == 2 && _bytes[0] == '.' && _bytes[1] == '.' )
        return false;

    for( const uint8_t byte : _bytes ) {
        if( byte == 0 || byte == '/' )
            return false;
    }
    return true;
}

bool IsZeroLease(const Lease &_lease) noexcept
{
    return IsAllZero(_lease.token.bytes);
}

std::expected<void, ValidationError> ValidateRequestWithLease(const Header &_header, const Lease &_lease) noexcept
{
    if( const auto header = Validate(_header); !header )
        return std::unexpected{header.error()};
    if( const auto lease = Validate(_lease); !lease )
        return std::unexpected{lease.error()};
    if( _header != _lease.header )
        return std::unexpected{ValidationError::InconsistentLease};
    return {};
}

} // namespace

std::expected<DestinationComponent, ValidationError>
DestinationComponent::Create(std::span<const uint8_t> _bytes) noexcept
{
    if( !IsValidDestinationComponentBytes(_bytes) )
        return std::unexpected{ValidationError::InvalidDestinationComponent};

    DestinationComponent component;
    for( size_t index = 0; index < _bytes.size(); ++index )
        component.m_Bytes[index] = _bytes[index];
    component.m_Size = static_cast<uint16_t>(_bytes.size());
    return component;
}

std::expected<void, ValidationError> Validate(const Header &_header) noexcept
{
    if( _header.version != kProtocolVersion )
        return std::unexpected{ValidationError::UnsupportedVersion};
    if( IsAllZero(_header.correlation) )
        return std::unexpected{ValidationError::InvalidCorrelation};
    return {};
}

std::expected<void, ValidationError> Validate(const BeginRequest &_request) noexcept
{
    if( const auto header = Validate(_request.header); !header )
        return std::unexpected{header.error()};
    if( !IsValidDestinationComponentBytes(_request.destination_name.Bytes()) )
        return std::unexpected{ValidationError::InvalidDestinationComponent};
    if( !IsValidObjectSeal(_request.source, S_IFREG) ) {
        if( !IsValidTimestamp(_request.source.birth_time) || !IsValidTimestamp(_request.source.modification_time) ||
            !IsValidTimestamp(_request.source.status_change_time) )
            return std::unexpected{ValidationError::InvalidTimestamp};
        return std::unexpected{ValidationError::InvalidSourceSeal};
    }
    if( !IsValidObjectSeal(_request.destination_parent, S_IFDIR) ) {
        if( !IsValidTimestamp(_request.destination_parent.birth_time) ||
            !IsValidTimestamp(_request.destination_parent.modification_time) ||
            !IsValidTimestamp(_request.destination_parent.status_change_time) )
            return std::unexpected{ValidationError::InvalidTimestamp};
        return std::unexpected{ValidationError::InvalidDestinationParentSeal};
    }
    return {};
}

std::expected<void, ValidationError> Validate(const Lease &_lease) noexcept
{
    if( const auto header = Validate(_lease.header); !header )
        return std::unexpected{header.error()};
    if( IsAllZero(_lease.token.bytes) )
        return std::unexpected{ValidationError::InvalidLease};
    return {};
}

std::expected<void, ValidationError> Validate(const CommitRequest &_request) noexcept
{
    return ValidateRequestWithLease(_request.header, _request.lease);
}

std::expected<void, ValidationError> Validate(const AbortRequest &_request) noexcept
{
    return ValidateRequestWithLease(_request.header, _request.lease);
}

std::expected<void, ValidationError> Validate(const BeginResult &_result) noexcept
{
    if( const auto header = Validate(_result.header); !header )
        return std::unexpected{header.error()};

    if( _result.disposition == BeginDisposition::Granted ) {
        if( _result.failure != BeginFailure::None )
            return std::unexpected{ValidationError::InconsistentBeginResult};
        if( !ValidateRequestWithLease(_result.header, _result.lease) )
            return std::unexpected{ValidationError::InconsistentBeginResult};
        return {};
    }

    if( _result.disposition == BeginDisposition::Rejected && _result.failure != BeginFailure::None &&
        IsZeroLease(_result.lease) )
        return {};
    return std::unexpected{ValidationError::InconsistentBeginResult};
}

std::expected<void, ValidationError> Validate(const CompletionResult &_result) noexcept
{
    if( const auto header = Validate(_result.header); !header )
        return std::unexpected{header.error()};

    if( _result.system_error < 0 || _result.filesystem_sync_system_error < 0 )
        return std::unexpected{ValidationError::InconsistentCompletionResult};

    if( _result.filesystem_sync == FilesystemSync::Failed ) {
        if( _result.filesystem_sync_system_error == 0 )
            return std::unexpected{ValidationError::InconsistentCompletionResult};
    }
    else if( _result.filesystem_sync_system_error != 0 ) {
        return std::unexpected{ValidationError::InconsistentCompletionResult};
    }

    if( _result.publication == Publication::Published ) {
        if( _result.failure == CompletionFailure::None ) {
            if( _result.system_error == 0 && _result.filesystem_sync == FilesystemSync::Confirmed )
                return {};
        }
        else if( _result.failure == CompletionFailure::MetadataFailed &&
                 _result.system_error != 0 && _result.filesystem_sync != FilesystemSync::NotAttempted ) {
            return {};
        }
        else if( _result.failure == CompletionFailure::FileSystemSyncFailed &&
                 _result.system_error != 0 && _result.filesystem_sync == FilesystemSync::Failed &&
                 _result.filesystem_sync_system_error == _result.system_error ) {
            return {};
        }
        else if( _result.failure == CompletionFailure::HelperFailure && _result.system_error != 0 &&
                 _result.filesystem_sync != FilesystemSync::NotAttempted ) {
            return {};
        }
        return std::unexpected{ValidationError::InconsistentCompletionResult};
    }

    if( _result.publication == Publication::Unknown ) {
        if( _result.failure == CompletionFailure::HelperFailure && _result.system_error != 0 &&
            _result.filesystem_sync == FilesystemSync::NotAttempted )
            return {};
        return std::unexpected{ValidationError::InconsistentCompletionResult};
    }

    if( _result.filesystem_sync != FilesystemSync::NotAttempted )
        return std::unexpected{ValidationError::InconsistentCompletionResult};

    switch( _result.failure ) {
        case CompletionFailure::Aborted:
        case CompletionFailure::Cancelled:
            if( _result.system_error == 0 )
                return {};
            break;
        case CompletionFailure::SourceStale:
        case CompletionFailure::DestinationParentStale:
            if( _result.system_error == ESTALE )
                return {};
            break;
        case CompletionFailure::DestinationExists:
            if( _result.system_error == EEXIST )
                return {};
            break;
        case CompletionFailure::HelperFailure:
            if( _result.system_error != 0 )
                return {};
            break;
        case CompletionFailure::None:
        case CompletionFailure::MetadataFailed:
        case CompletionFailure::FileSystemSyncFailed:
            break;
    }
    return std::unexpected{ValidationError::InconsistentCompletionResult};
}

} // namespace nc::routedio::cross_volume_staging
