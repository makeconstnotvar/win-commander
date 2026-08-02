// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ProviderConditionalCopyJournalMapper.h"

#include <cerrno>

namespace nc::ops {
namespace {

using MappingError = ProviderConditionalCopyJournalMappingError;
using CommitFailure = vfs::ProviderConditionalCopyCommitFailure;
using PublicationState = vfs::ProviderConditionalCopyPublicationState;
using FilesystemSyncStatus = vfs::ProviderConditionalCopyFilesystemSyncStatus;

bool ProviderConditionalCopyJournalResultIsConsistent(
    const vfs::ProviderConditionalCopyCommitResult &_result) noexcept
{
    if( _result.system_error < 0 || _result.filesystem_sync_system_error < 0 )
        return false;

    switch( _result.filesystem_sync_status ) {
        case FilesystemSyncStatus::NotAttempted:
        case FilesystemSyncStatus::Confirmed:
            if( _result.filesystem_sync_system_error != 0 )
                return false;
            break;
        case FilesystemSyncStatus::Failed:
            if( _result.filesystem_sync_system_error == 0 )
                return false;
            break;
        default:
            return false;
    }

    switch( _result.publication ) {
        case PublicationState::NotPublished:
            if( _result.filesystem_sync_status != FilesystemSyncStatus::NotAttempted )
                return false;
            switch( _result.failure ) {
                case CommitFailure::Aborted:
                case CommitFailure::Cancelled:
                    return _result.system_error == 0;
                case CommitFailure::SourceStale:
                case CommitFailure::DestinationParentStale:
                    return _result.system_error == ESTALE;
                case CommitFailure::DestinationExists:
                    return _result.system_error == EEXIST;
                case CommitFailure::ProviderFailure:
                    return _result.system_error != 0;
                case CommitFailure::None:
                case CommitFailure::MetadataFailed:
                case CommitFailure::FileSystemSyncFailed:
                    return false;
            }
            return false;
        case PublicationState::Unknown:
            return _result.failure == CommitFailure::ProviderFailure && _result.system_error != 0 &&
                   _result.filesystem_sync_status == FilesystemSyncStatus::NotAttempted;
        case PublicationState::Published:
            switch( _result.failure ) {
                case CommitFailure::None:
                    return _result.system_error == 0 &&
                           _result.filesystem_sync_status == FilesystemSyncStatus::Confirmed;
                case CommitFailure::MetadataFailed:
                    return _result.system_error != 0 &&
                           _result.filesystem_sync_status != FilesystemSyncStatus::NotAttempted;
                case CommitFailure::FileSystemSyncFailed:
                    return _result.system_error != 0 &&
                           _result.filesystem_sync_status == FilesystemSyncStatus::Failed &&
                           _result.filesystem_sync_system_error == _result.system_error;
                case CommitFailure::ProviderFailure:
                    return _result.system_error != 0 &&
                           _result.filesystem_sync_status != FilesystemSyncStatus::NotAttempted;
                case CommitFailure::Aborted:
                case CommitFailure::Cancelled:
                case CommitFailure::SourceStale:
                case CommitFailure::DestinationParentStale:
                case CommitFailure::DestinationExists:
                    return false;
            }
            return false;
    }
    return false;
}

std::expected<OperationJournalPublicationState, MappingError>
ProviderConditionalCopyJournalPublication(PublicationState _state) noexcept
{
    switch( _state ) {
        case PublicationState::NotPublished:
            return OperationJournalPublicationState::NotPublished;
        case PublicationState::Published:
            return OperationJournalPublicationState::Published;
        case PublicationState::Unknown:
            return OperationJournalPublicationState::Unknown;
    }
    return std::unexpected(MappingError::InconsistentResult);
}

std::expected<OperationJournalFilesystemSyncStatus, MappingError>
ProviderConditionalCopyJournalFilesystemSync(FilesystemSyncStatus _status) noexcept
{
    switch( _status ) {
        case FilesystemSyncStatus::NotAttempted:
            return OperationJournalFilesystemSyncStatus::NotAttempted;
        case FilesystemSyncStatus::Confirmed:
            return OperationJournalFilesystemSyncStatus::Confirmed;
        case FilesystemSyncStatus::Failed:
            return OperationJournalFilesystemSyncStatus::Failed;
    }
    return std::unexpected(MappingError::InconsistentResult);
}

} // namespace

std::expected<OperationJournalItemResult, ProviderConditionalCopyJournalMappingError>
MapProviderConditionalCopyCommitResultToJournalItemResult(
    const vfs::ProviderConditionalCopyCommitResult &_result,
    ProviderConditionalCopyJournalContext _context) noexcept
{
    if( !ProviderConditionalCopyJournalResultIsConsistent(_result) )
        return std::unexpected(MappingError::InconsistentResult);
    if( _result.failure == CommitFailure::Aborted )
        return std::unexpected(MappingError::NonExecutionTerminal);

    const auto publication = ProviderConditionalCopyJournalPublication(_result.publication);
    const auto filesystem_sync =
        ProviderConditionalCopyJournalFilesystemSync(_result.filesystem_sync_status);
    if( !publication || !filesystem_sync )
        return std::unexpected(MappingError::InconsistentResult);

    OperationJournalItemStatus status{OperationJournalItemStatus::Failed};
    OperationJournalItemError error{OperationJournalItemError::Unknown};
    OperationJournalRecoveryAction recovery{OperationJournalRecoveryAction::Retry};

    switch( _result.failure ) {
        case CommitFailure::None:
            status = OperationJournalItemStatus::Succeeded;
            error = OperationJournalItemError::None;
            recovery = OperationJournalRecoveryAction::None;
            break;
        case CommitFailure::Cancelled:
            status = OperationJournalItemStatus::Cancelled;
            error = OperationJournalItemError::Cancelled;
            recovery = OperationJournalRecoveryAction::None;
            break;
        case CommitFailure::SourceStale:
            error = OperationJournalItemError::SourceChanged;
            break;
        case CommitFailure::DestinationParentStale:
        case CommitFailure::DestinationExists:
            error = OperationJournalItemError::DestinationChanged;
            break;
        case CommitFailure::MetadataFailed:
            error = OperationJournalItemError::Metadata;
            recovery = OperationJournalRecoveryAction::InspectDestination;
            break;
        case CommitFailure::FileSystemSyncFailed:
            error = OperationJournalItemError::Commit;
            recovery = OperationJournalRecoveryAction::InspectDestination;
            break;
        case CommitFailure::ProviderFailure:
            error = _result.publication == PublicationState::NotPublished
                        ? OperationJournalItemError::Unknown
                        : OperationJournalItemError::Commit;
            recovery = _result.publication == PublicationState::NotPublished
                           ? OperationJournalRecoveryAction::Retry
                           : OperationJournalRecoveryAction::InspectDestination;
            break;
        case CommitFailure::Aborted:
            return std::unexpected(MappingError::NonExecutionTerminal);
    }

    const uint64_t bytes = _result.publication == PublicationState::Published
                               ? _context.exact_source_bytes
                               : 0;
    return OperationJournalItemResult{
        .item_index = _context.item_index,
        .status = status,
        .error = error,
        .system_error = _result.system_error,
        .prior_error = OperationJournalItemError::None,
        .prior_system_error = 0,
        .bytes = bytes,
        .destination_publication = *publication,
        .filesystem_sync_status = *filesystem_sync,
        .filesystem_sync_system_error = _result.filesystem_sync_system_error,
        .recovery_action = recovery};
}

} // namespace nc::ops
