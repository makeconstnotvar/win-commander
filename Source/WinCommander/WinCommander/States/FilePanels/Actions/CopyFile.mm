// Copyright (C) 2017-2024 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CopyFile.h"
#include "../MainWindowFilePanelState.h"
#include "../PanelController.h"
#include <Panel/PanelData.h>
#include <Panel/PanelDataStatistics.h>
#include "../PanelView.h"
#include "../PanelAux.h"
#include "Helpers.h"
#include <Operations/CopyOperationOrchestrator.h>
#include <Operations/OperationCenterCoordinator.h>
#include <Operations/Copying.h>
#include <Operations/CopyingDialog.h>
#include <Operations/OperationPlan.h>
#include <Operations/VFSOperationPlanningProbes.h>
#include <WinCommander/Bootstrap/AppDelegate.h>
#include <WinCommander/Bootstrap/AppDelegate+MainWindowCreation.h>
#include <WinCommander/Core/Alert.h>
#include <WinCommander/Core/Operations/CopyOperationRecoveryCoordinator.h>
#include <WinCommander/Core/Operations/OperationSubmissionGate.h>
#include <WinCommander/Core/Operations/ReviewedCopyAsApplicationBoundary.h>
#include <WinCommander/Core/Operations/ReviewedCopyTerminalPresentation.h>
#include <WinCommander/Core/VFSOperationPlanningAccessChecker.h>
#include <WinCommander/States/MainWindowController.h>
#include <Base/dispatch_cpp.h>
#include <Config/Config.h>
#include <Utility/StringExtras.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <variant>

namespace nc::panel::actions {

static std::function<void()> RefreshCurrentActiveControllerLambda(MainWindowFilePanelState *_target);
static std::function<void()> RefreshBothCurrentControllersLambda(MainWindowFilePanelState *_target);

namespace reviewed_copy_as {
namespace {

constexpr std::string_view g_ReviewedCopyProviderId = "native";

bool EqualCopyingOptions(const nc::ops::CopyingOptions &_lhs, const nc::ops::CopyingOptions &_rhs) noexcept
{
    return _lhs.docopy == _rhs.docopy && _lhs.preserve_symlinks == _rhs.preserve_symlinks &&
           _lhs.copy_xattrs == _rhs.copy_xattrs && _lhs.copy_file_times == _rhs.copy_file_times &&
           _lhs.copy_unix_flags == _rhs.copy_unix_flags && _lhs.copy_unix_owners == _rhs.copy_unix_owners &&
           _lhs.disable_system_caches == _rhs.disable_system_caches &&
           _lhs.reject_final_component_symlinks == _rhs.reject_final_component_symlinks &&
           _lhs.verification == _rhs.verification && _lhs.exist_behavior == _rhs.exist_behavior &&
           _lhs.locked_items_behaviour == _rhs.locked_items_behaviour &&
           _lhs.destination_path_interpretation == _rhs.destination_path_interpretation;
}

bool IsReviewedCopyOptions(const nc::ops::CopyingOptions &_options) noexcept
{
    if( _options.verification == nc::ops::CopyingOptions::ChecksumVerification::Always )
        return false;

    nc::ops::CopyingOptions required;
    required.docopy = true;
    // "WhenMoves" requests no checksum for a Copy and is equivalent to "Never" in this scope.
    required.verification = _options.verification;
    return EqualCopyingOptions(_options, required);
}

bool IsSameLexicalPath(const std::string &_lhs, const std::string &_rhs)
{
    if( _lhs.empty() || _rhs.empty() )
        return false;

    const auto normalize = [](const std::string &_path) {
        std::string normalized = std::filesystem::path{_path}.lexically_normal().native();
        while( normalized.size() > 1 && normalized.back() == '/' )
            normalized.pop_back();
        return normalized;
    };
    return normalize(_lhs) == normalize(_rhs);
}

/** Everything both selection shapes agree on, before either asks its own remaining question. */
bool IsReviewedCopyShape(const VFSListingItem &_item,
                         const VFSHostPtr &_destination_host,
                         const nc::ops::CopyingOptions &_options)
{
    return _item && _item.IsReg() && _item.Host() && _item.Host()->IsNativeFS() && _destination_host &&
           _destination_host.get() == _item.Host().get() && IsReviewedCopyOptions(_options);
}

/**
 * What the user typed, if and only if it is a folder that already exists - trailing slashes removed.
 *
 * `Copy To` accepts a destination the legacy operation is free to interpret: an existing folder to
 * land in, a name to create, or a path whose parent it may have to make. The reviewed engine means
 * exactly one of those, so the ambiguity is resolved before a route is chosen rather than inside it -
 * anything that is not already a folder stays with the operation that knows what to do with it, and
 * an answer that cannot be read counts as not a folder.
 */
std::optional<std::string> TrimmedExistingDirectory(const VFSHostPtr &_host, const std::string &_path) noexcept
{
    try {
        // Asked before the `stat`, not after: this runs on the main thread from the dialog handler,
        // and a remote provider can take as long as its own timeout to answer. The reviewed engine
        // needs a Native host anyway, so a provider it could never accept is never waited on.
        if( !_host || !_host->IsNativeFS() || _path.empty() || _path.front() != '/' )
            return std::nullopt;
        auto trimmed = _path;
        while( trimmed.size() > 1 && trimmed.back() == '/' )
            trimmed.pop_back();
        const auto stat = _host->Stat(trimmed, 0);
        if( !stat || !stat->meaning.mode || !S_ISDIR(stat->mode) )
            return std::nullopt;
        return trimmed;
    } catch( ... ) {
        return std::nullopt;
    }
}

} // namespace

Selection Select(const VFSListingItem &_item,
                 const std::string &_destination,
                 const VFSHostPtr &_destination_host,
                 const nc::ops::CopyingOptions &_options) noexcept
{
    try {
        if( !IsReviewedCopyShape(_item, _destination_host, _options) || _destination.empty() ||
            _destination.front() != '/' )
            return Selection::Legacy;

        const auto destination_parent = std::filesystem::path{_destination}.parent_path().native();
        switch( _destination_host->ConditionalCopyPathSupport(_item.Path(), destination_parent) ) {
            case nc::vfs::ProviderConditionalCopyPathSupport::SameVolumeClone:
                return IsSameLexicalPath(destination_parent, _item.Directory()) ? Selection::Reviewed : Selection::Legacy;
            case nc::vfs::ProviderConditionalCopyPathSupport::CrossVolumeStaged:
                return Selection::Reviewed;
            case nc::vfs::ProviderConditionalCopyPathSupport::Unsupported:
                return Selection::Legacy;
            case nc::vfs::ProviderConditionalCopyPathSupport::Unavailable:
                return Selection::Reject;
        }
    } catch( ... ) {
        return Selection::Reject;
    }
    return Selection::Reject;
}

Selection SelectIntoDirectory(const VFSListingItem &_item,
                              const std::string &_destination_directory,
                              const VFSHostPtr &_destination_host,
                              const nc::ops::CopyingOptions &_options) noexcept
{
    try {
        if( !IsReviewedCopyShape(_item, _destination_host, _options) || _destination_directory.empty() ||
            _destination_directory.front() != '/' )
            return Selection::Legacy;

        // Copying an item into the directory it already occupies cannot create anything - the derived
        // destination is the source itself, which the planner refuses as `SamePath`. Answered legacy
        // here so the reviewed engine is never handed a plan that cannot exist, rather than left to
        // be refused later as a blocked preflight the user would have to read.
        if( IsSameLexicalPath(_destination_directory, _item.Directory()) )
            return Selection::Legacy;

        switch( _destination_host->ConditionalCopyPathSupport(_item.Path(), _destination_directory) ) {
            case nc::vfs::ProviderConditionalCopyPathSupport::SameVolumeClone:
            case nc::vfs::ProviderConditionalCopyPathSupport::CrossVolumeStaged:
                return Selection::Reviewed;
            case nc::vfs::ProviderConditionalCopyPathSupport::Unsupported:
                return Selection::Legacy;
            case nc::vfs::ProviderConditionalCopyPathSupport::Unavailable:
                return Selection::Reject;
        }
    } catch( ... ) {
        return Selection::Reject;
    }
    return Selection::Reject;
}

Selection SelectBatch(const std::vector<VFSListingItem> &_items,
                      const std::string &_destination_directory,
                      const VFSHostPtr &_destination_host,
                      const nc::ops::CopyingOptions &_options) noexcept
{
    if( _items.empty() )
        return Selection::Legacy;

    // Every item is asked, even once one has already answered legacy: a `Reject` further down the
    // selection is an eligibility question the provider could not answer, and stopping early would
    // turn it into a silent legacy copy - the one outcome the single-item rule exists to refuse.
    bool any_legacy = false;
    for( const auto &item : _items ) {
        switch( SelectIntoDirectory(item, _destination_directory, _destination_host, _options) ) {
            case Selection::Reject:
                return Selection::Reject;
            case Selection::Legacy:
                any_legacy = true;
                break;
            case Selection::Reviewed:
                break;
        }
    }
    return any_legacy ? Selection::Legacy : Selection::Reviewed;
}

} // namespace reviewed_copy_as

namespace reviewed_move {
namespace {

bool IsReviewedMoveOptions(const nc::ops::CopyingOptions &_options) noexcept
{
    if( _options.verification == nc::ops::CopyingOptions::ChecksumVerification::Always )
        return false;

    nc::ops::CopyingOptions required;
    required.docopy = false;
    required.verification = _options.verification;
    return reviewed_copy_as::EqualCopyingOptions(_options, required);
}

/** Everything both Move selection shapes agree on - the Move counterpart of `IsReviewedCopyShape`. */
bool IsReviewedMoveShape(const VFSListingItem &_item,
                         const VFSHostPtr &_destination_host,
                         const nc::ops::CopyingOptions &_options)
{
    return _item && _item.IsReg() && _item.Host() && _item.Host()->IsNativeFS() && _destination_host &&
           _destination_host.get() == _item.Host().get() && IsReviewedMoveOptions(_options);
}

} // namespace

/**
 * The Move counterpart of `reviewed_copy_as::Select`, and it keeps the same narrower-than-the-provider
 * scope `Copy As` keeps: reviewed only when the destination names the item's own directory under a new
 * name, exactly the shape `Move As` has today. There is no `CrossVolumeStaged` case here, because there
 * is no cross-volume Move at all - `ConditionalMovePathSupport` answers `SameVolumeRename` or nothing.
 */
Selection Select(const VFSListingItem &_item,
                 const std::string &_destination,
                 const VFSHostPtr &_destination_host,
                 const nc::ops::CopyingOptions &_options) noexcept
{
    try {
        if( !IsReviewedMoveShape(_item, _destination_host, _options) || _destination.empty() ||
            _destination.front() != '/' )
            return Selection::Legacy;

        const auto destination_parent = std::filesystem::path{_destination}.parent_path().native();
        switch( _destination_host->ConditionalMovePathSupport(_item.Path(), destination_parent) ) {
            case nc::vfs::ProviderConditionalMovePathSupport::SameVolumeRename:
                return reviewed_copy_as::IsSameLexicalPath(destination_parent, _item.Directory()) ? Selection::Reviewed
                                                                                                   : Selection::Legacy;
            case nc::vfs::ProviderConditionalMovePathSupport::Unsupported:
                return Selection::Legacy;
            case nc::vfs::ProviderConditionalMovePathSupport::Unavailable:
                return Selection::Reject;
        }
    } catch( ... ) {
        return Selection::Reject;
    }
    return Selection::Reject;
}

} // namespace reviewed_move

namespace {

NSString *PlanningBlockerDescription(const nc::ops::OperationPlanningBlockerCode _code)
{
    using enum nc::ops::OperationPlanningBlockerCode;
    switch( _code ) {
        case UnsupportedPlanType:
            return @"This copy plan is not supported.";
        case ProviderCapabilityUnsupported:
            return @"The storage provider cannot perform this reviewed copy.";
        case ProviderUnavailable:
            return @"The storage provider is unavailable.";
        case SourceMissing:
            return @"The source file no longer exists.";
        case SourceUnreadable:
            return @"The source file cannot be read.";
        case DestinationMissing:
            return @"The destination directory no longer exists.";
        case DestinationNotDirectory:
            return @"The destination parent is not a directory.";
        case DestinationNotWritable:
            return @"The destination directory is not writable.";
        case PermissionRequired:
            return @"Access to the source or destination must be granted first.";
        case PermissionDenied:
            return @"Access to the source or destination was denied.";
        case InvalidSourceName:
            return @"The source name is invalid.";
        case InvalidDestinationName:
            return @"The destination name is invalid.";
        case DestinationNameEvidenceUnavailable:
            return @"The destination name could not be validated.";
        case PathIdentityUnavailable:
            return @"The provider cannot prove path identity for this copy.";
        case SamePath:
            return @"The source and destination identify the same file.";
        case RecursiveDestination:
            return @"The destination is inside the source.";
        case DuplicateDestination:
            return @"More than one item resolves to the same destination.";
        case ConflictDecisionRequired:
            return @"The destination already exists. This reviewed path is create-only.";
        case ConflictPolicyUnsupported:
            return @"The selected conflict policy is not supported.";
        case InsufficientSpace:
            return @"There is not enough free space at the destination.";
        case EstimateOverflow:
            return @"The copy size could not be represented safely.";
        case NothingToDo:
            return @"There is nothing to copy.";
        case ProbeCancelled:
            return @"Copy validation was cancelled.";
        case ProbeFailed:
            return @"Copy validation failed.";
    }
    return @"Copy validation failed.";
}

NSString *ReviewedFactoryErrorDescription(const nc::ops::ReviewedOperationFactoryErrorCode _code)
{
    using enum nc::ops::ReviewedOperationFactoryErrorCode;
    switch( _code ) {
        case UnsupportedPlanType:
            return @"The reviewed request is not a supported Copy plan.";
        case MissingBindings:
            return @"The reviewed request lost its provider bindings.";
        case ProviderUnavailable:
            return @"A reviewed storage provider is unavailable.";
        case UnsupportedProviderScope:
            return @"The reviewed provider scope is no longer supported.";
        case ConditionalCommitAuthorityUnavailable:
            return @"The provider could not establish conditional publication authority.";
        case ConditionalCommitIntegrationUnavailable:
            return @"The provider transaction could not be integrated with the operation lifecycle.";
        case EmptyAcceptedPlan:
            return @"The reviewed plan contains no copy item.";
        case IncompleteAcceptedPlan:
            return @"The reviewed plan does not cover every source it names.";
        case UnsupportedConflictPolicy:
            return @"The reviewed conflict policy is no longer supported.";
        case UnexpectedConflictEvidence:
            return @"Destination conflict evidence changed after review.";
        case InvalidReviewedPlan:
            return @"The reviewed approval token is invalid or already consumed.";
        case UnsupportedSourceKind:
            return @"The source is no longer a supported regular file.";
        case MissingEvidence:
            return @"Required validation evidence is missing.";
        case InvalidEvidence:
            return @"Validation evidence is internally inconsistent.";
        case InvalidPath:
            return @"A reviewed path is invalid.";
        case UnsupportedAccessRoute:
            return @"The reviewed direct-access route is unavailable.";
        case StaleSource:
            return @"The source changed after review.";
        case StaleSourceParent:
            return @"The folder containing the source changed after review.";
        case StaleDestination:
            return @"The destination or its parent changed after review.";
        case Cancelled:
            return @"The copy was cancelled during runtime revalidation.";
        case OpenFailed:
            return @"A reviewed source or destination descriptor could not be opened.";
        case ConstructionFailed:
            return @"The reviewed operation could not be constructed.";
    }
    return @"The reviewed operation factory rejected the request.";
}

NSString *SubmissionErrorDescription(const nc::ops::CopyOperationOrchestratorError &_error)
{
    using enum nc::ops::CopyOperationOrchestratorErrorCode;
    NSString *description = nil;
    switch( _error.code ) {
        case MissingJournal:
            description = @"The durable operation journal is unavailable.";
            break;
        case MissingPool:
            description = @"The operation queue is unavailable.";
            break;
        case MissingExecutionFactory:
            description = @"The reviewed copy execution factory is unavailable.";
            break;
        case MissingRunReceiptCustodian:
            description = @"The durable run-receipt custodian is unavailable.";
            break;
        case UnsupportedReviewedPlan:
            description = @"The provider rejected this reviewed copy at runtime revalidation.";
            break;
        case Cancelled:
            description = @"The copy was cancelled before it entered the operation queue.";
            break;
        case JournalAdmissionFailed:
            description = @"The copy could not be admitted to the durable operation journal.";
            break;
        case InvalidJournalAdmissionReceipt:
            description = @"The reviewed copy no longer matches its durable admission.";
            break;
        case ExecutionFactoryFailed:
            description = @"The reviewed copy operation could not be constructed.";
            break;
        case InvalidExecutionProduct:
            description = @"The reviewed copy execution product was invalid.";
            break;
        case OperationConfigurationFailed:
            description = @"The operation lifecycle callbacks could not be installed safely.";
            break;
        case AdmissionFinalizationFailed:
            description = @"The rejected copy could not be finalized in the durable journal.";
            break;
        case RunReceiptReservationFailed:
            description = @"Durable run-receipt capacity is exhausted.";
            break;
        case RunningTransitionFailed:
            description = @"The copy could not enter the durable running state.";
            break;
        case RunReceiptArmFailed:
            description = @"The durable run receipt could not be armed.";
            break;
        case PreEnqueuePreparationFailed:
            description = @"The reviewed copy could not be prepared for safe queue admission.";
            break;
        case RunningFinalizationFailed:
            description = @"The copy stopped, but its terminal state still requires recovery.";
            break;
        case EnqueueRejected:
            description = @"The operation queue rejected the reviewed copy.";
            break;
    }

    NSMutableString *const details =
        [NSMutableString stringWithString:description ?: @"The reviewed copy could not be submitted."];
    if( _error.reviewed_factory_error ) {
        const auto &factory = *_error.reviewed_factory_error;
        [details appendFormat:@"\n\nFactory: %@", ReviewedFactoryErrorDescription(factory.code)];
        if( factory.path ) {
            [details appendFormat:@"\nPath: %@", [NSString stringWithUTF8StdString:factory.path->absolute_path]];
        }
        if( factory.cause ) {
            const auto reason = factory.cause->LocalizedFailureReason();
            if( !reason.empty() )
                [details appendFormat:@"\nCause: %@", [NSString stringWithUTF8StdString:reason]];
        }
    }
    if( _error.recovery_disposition ) {
        switch( *_error.recovery_disposition ) {
            case nc::ops::CopyOperationRunReceiptRecoveryDisposition::RetryRequired:
                [details appendString:@"\n\nRecovery: retry the durable finalization before starting another copy."];
                break;
            case nc::ops::CopyOperationRunReceiptRecoveryDisposition::ReconcileRequired:
                [details appendString:@"\n\nRecovery: reconcile the journal and run receipt before retrying."];
                break;
        }
    }
    if( _error.enqueue_result ) {
        switch( *_error.enqueue_result ) {
            case nc::ops::PoolEnqueueResult::Accepted:
                break;
            case nc::ops::PoolEnqueueResult::ShuttingDown:
                [details appendString:@"\n\nQueue state: the window is shutting down."];
                break;
            case nc::ops::PoolEnqueueResult::NotCold:
                [details appendString:@"\n\nQueue state: the operation was no longer cold."];
                break;
            case nc::ops::PoolEnqueueResult::Duplicate:
                [details appendString:@"\n\nQueue state: the operation was already present."];
                break;
        }
    }
    return details;
}

NSString *CoordinatorSubmissionErrorDescription(const nc::ops::OperationCenterSubmissionError &_error)
{
    using enum nc::ops::OperationCenterSubmissionErrorCode;
    switch( _error.code ) {
        case HookPreparationFailed:
            return @"The operation lifecycle projection could not be prepared safely.";
        case AdmissionStagingFailed:
            return @"The operation queue could not prepare a durable admission.";
        case AdmissionCommitFailed:
            return @"The operation queue could not commit the durable admission.";
        case OrchestratorRejected:
            return @"The reviewed copy could not enter the operation queue.";
    }
    return @"The reviewed copy could not enter the operation queue.";
}

NSString *RecoveryServiceDescription(const nc::core::CopyOperationRecoveryServiceResult &_result)
{
    using enum nc::core::CopyOperationRecoveryServiceError;
    switch( _result.error ) {
        case None:
            break;
        case RuntimeUnavailable:
            return @"The recovery runtime is unavailable.";
        case CoordinatorBusy:
            return @"Another recovery action is already in progress.";
        case JournalInUse:
            return @"The operation journal is still in use. Retry recovery after the active submission finishes.";
        case JournalReopenFailed:
            return @"The operation journal could not be reopened safely.";
        case UnexpectedFailure:
            return @"Recovery stopped after an unexpected internal failure.";
    }

    if( _result.release ) {
        switch( *_result.release ) {
            case nc::ops::CopyOperationRunReceiptPoolReleaseStatus::Released:
                return @"Recovery confirmed the durable result and released the retained operation.";
            case nc::ops::CopyOperationRunReceiptPoolReleaseStatus::Retained:
                return @"The durable result was confirmed, but the retained operation still requires release.";
            case nc::ops::CopyOperationRunReceiptPoolReleaseStatus::InProgress:
                return @"The retained operation is still finalizing.";
            case nc::ops::CopyOperationRunReceiptPoolReleaseStatus::NotFound:
                return @"The retained operation could not be found for release.";
            case nc::ops::CopyOperationRunReceiptPoolReleaseStatus::Busy:
                return @"The retained operation is busy. Retry recovery later.";
            case nc::ops::CopyOperationRunReceiptPoolReleaseStatus::ContractViolation:
                return @"The retained operation does not match its recovery record.";
        }
    }
    if( _result.reconciliation ) {
        switch( _result.reconciliation->status ) {
            case nc::ops::CopyOperationRunReceiptReconciliationStatus::TerminalConfirmed:
                return @"The journal confirmed the terminal result.";
            case nc::ops::CopyOperationRunReceiptReconciliationStatus::InterruptedConfirmed:
                return @"The journal confirmed that the operation was interrupted.";
            case nc::ops::CopyOperationRunReceiptReconciliationStatus::Mismatch:
                return @"The reopened journal does not match the retained recovery record.";
            case nc::ops::CopyOperationRunReceiptReconciliationStatus::RetryRequired:
                return @"Journal reconciliation could not be persisted. Retry recovery.";
            case nc::ops::CopyOperationRunReceiptReconciliationStatus::ContractViolation:
                return @"The journal result violates the retained recovery contract.";
            case nc::ops::CopyOperationRunReceiptReconciliationStatus::NotFound:
                return @"The reopened journal does not contain this operation.";
            case nc::ops::CopyOperationRunReceiptReconciliationStatus::Busy:
                return @"The recovery record is busy. Retry recovery later.";
        }
    }
    if( _result.retry ) {
        switch( _result.retry->status ) {
            case nc::ops::CopyOperationRunReceiptCustodyStatus::Finalized:
                return @"The durable operation result was finalized.";
            case nc::ops::CopyOperationRunReceiptCustodyStatus::RetryRequired:
                return @"Durable finalization still requires a retry.";
            case nc::ops::CopyOperationRunReceiptCustodyStatus::ReconcileRequired:
                return @"The operation journal must be reopened and reconciled.";
            case nc::ops::CopyOperationRunReceiptCustodyStatus::ContractViolation:
                return @"The retained receipt does not match its terminal result.";
            case nc::ops::CopyOperationRunReceiptCustodyStatus::NotFound:
                return @"No retained recovery record exists for this operation.";
            case nc::ops::CopyOperationRunReceiptCustodyStatus::Busy:
                return @"The recovery record is busy. Retry recovery later.";
        }
    }
    return @"Recovery completed without a retained operation result.";
}

void ShowCopyAlert(NCMainWindowController *_window_controller,
                   NSString *_title,
                   NSString *_message,
                   NSAlertStyle _style);

NSString *RecoveryHistoryRefreshDescription(const nc::core::CopyOperationRecoveryHistoryRefreshResult &_result)
{
    switch( _result.history_refresh ) {
        case nc::core::CopyOperationRecoveryHistoryRefreshStatus::NotRequired:
        case nc::core::CopyOperationRecoveryHistoryRefreshStatus::Refreshed:
            return @"";
        case nc::core::CopyOperationRecoveryHistoryRefreshStatus::CoordinatorUnavailable:
            return @"\n\nThe durable recovery completed, but operation history is unavailable in this application session.";
        case nc::core::CopyOperationRecoveryHistoryRefreshStatus::JournalUnavailable:
            return @"\n\nThe durable recovery completed, but its operation history could not be reopened for display.";
        case nc::core::CopyOperationRecoveryHistoryRefreshStatus::Deferred:
            return @"\n\nThe durable recovery completed, but operation history is busy. You can refresh it once.";
        case nc::core::CopyOperationRecoveryHistoryRefreshStatus::ProjectionFailed:
            return @"\n\nThe durable recovery completed, but operation history could not be refreshed.";
        case nc::core::CopyOperationRecoveryHistoryRefreshStatus::RetryExhausted:
            return @"\n\nThe durable recovery completed, but operation history is still busy after its one refresh attempt.";
    }
    return @"";
}

void RefreshDeferredRecoveryHistoryInUI(
    const std::shared_ptr<nc::core::CopyOperationRecoveryCoordinator> &_recovery_coordinator,
    const std::shared_ptr<nc::ops::OperationCenterCoordinator> &_operation_center,
    const nc::core::CopyOperationRecoveryHistoryRefreshResult &_prior,
    const std::shared_ptr<std::atomic_bool> &_durable_outcome_delivered,
    __weak NCMainWindowController *_weak_window_controller)
{
    dispatch_to_default([recovery_coordinator = _recovery_coordinator,
                         operation_center = _operation_center,
                         prior = _prior,
                         durable_outcome_delivered = _durable_outcome_delivered,
                         weak_window_controller = _weak_window_controller] {
        if( !durable_outcome_delivered || durable_outcome_delivered->load(std::memory_order_acquire) )
            return;
        const auto result = nc::core::RetryDeferredHistoryProjection(recovery_coordinator, operation_center, prior);
        if( durable_outcome_delivered->load(std::memory_order_acquire) )
            return;
        dispatch_to_main_queue([result, weak_window_controller] {
            if( NCMainWindowController *const controller = weak_window_controller ) {
                ShowCopyAlert(controller,
                              result.recovery.error == nc::core::CopyOperationRecoveryServiceError::None
                                  ? @"Copy recovery result"
                                  : @"Copy recovery incomplete",
                              [RecoveryServiceDescription(result.recovery)
                                  stringByAppendingString:RecoveryHistoryRefreshDescription(result)],
                              result.recovery.error == nc::core::CopyOperationRecoveryServiceError::None
                                  ? NSAlertStyleInformational
                                  : NSAlertStyleCritical);
            }
        });
    });
}

void PresentRecoveryHistoryRefreshResult(
    NCMainWindowController *_window_controller,
    const nc::core::CopyOperationRecoveryHistoryRefreshResult &_result,
    std::shared_ptr<nc::core::CopyOperationRecoveryCoordinator> _recovery_coordinator,
    std::shared_ptr<nc::ops::OperationCenterCoordinator> _operation_center,
    std::shared_ptr<std::atomic_bool> _durable_outcome_delivered)
{
    dispatch_assert_main_queue();
    const bool can_refresh = _result.history_refresh == nc::core::CopyOperationRecoveryHistoryRefreshStatus::Deferred &&
                             _result.HasDeferredHistoryProjection();
    if( !can_refresh ) {
        ShowCopyAlert(_window_controller,
                      _result.recovery.error == nc::core::CopyOperationRecoveryServiceError::None
                          ? @"Copy recovery result"
                          : @"Copy recovery incomplete",
                      [RecoveryServiceDescription(_result.recovery)
                          stringByAppendingString:RecoveryHistoryRefreshDescription(_result)],
                      _result.recovery.error == nc::core::CopyOperationRecoveryServiceError::None ? NSAlertStyleInformational
                                                                                                   : NSAlertStyleCritical);
        return;
    }

    Alert *const alert = [[Alert alloc] init];
    alert.alertStyle = NSAlertStyleInformational;
    alert.messageText = @"Copy recovery result";
    alert.informativeText = [RecoveryServiceDescription(_result.recovery)
        stringByAppendingString:RecoveryHistoryRefreshDescription(_result)];
    [alert addButtonWithTitle:NSLocalizedString(@"Refresh history", "Refresh deferred copy recovery history once")];
    [alert addButtonWithTitle:NSLocalizedString(@"Dismiss", "Dismiss deferred copy recovery history refresh")];

    const auto prior = _result;
    __weak NCMainWindowController *weak_window_controller = _window_controller;
    if( _window_controller.window ) {
        [alert beginSheetModalForWindow:_window_controller.window
                      completionHandler:^(NSModalResponse response) {
                        if( response == NSAlertFirstButtonReturn )
                            RefreshDeferredRecoveryHistoryInUI(_recovery_coordinator,
                                                              _operation_center,
                                                              prior,
                                                              _durable_outcome_delivered,
                                                              weak_window_controller);
                      }];
    }
    else if( [alert runModal] == NSAlertFirstButtonReturn ) {
        RefreshDeferredRecoveryHistoryInUI(
            _recovery_coordinator, _operation_center, prior, _durable_outcome_delivered, weak_window_controller);
    }
}

void PresentSubmissionFailure(NCMainWindowController *_window_controller,
                              const nc::ops::CopyOperationOrchestratorError &_error,
                              std::string _plan_id,
                              std::shared_ptr<nc::core::CopyOperationRecoveryCoordinator> _recovery_coordinator,
                              std::shared_ptr<nc::ops::OperationCenterCoordinator> _operation_center,
                              std::shared_ptr<std::atomic_bool> _durable_outcome_delivered)
{
    dispatch_assert_main_queue();
    if( !_error.recovery_disposition || !_recovery_coordinator || _plan_id.empty() ) {
        ShowCopyAlert(
            _window_controller, @"Copy submission failed", SubmissionErrorDescription(_error), NSAlertStyleCritical);
        return;
    }

    Alert *const alert = [[Alert alloc] init];
    alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = @"Copy submission requires recovery";
    alert.informativeText = SubmissionErrorDescription(_error);
    [alert addButtonWithTitle:NSLocalizedString(@"Recover", "Recover a retained copy submission")];
    [alert addButtonWithTitle:NSLocalizedString(@"Dismiss", "Dismiss copy recovery")];

    __weak NCMainWindowController *weak_window_controller = _window_controller;
    [alert beginSheetModalForWindow:_window_controller.window
                  completionHandler:^(NSModalResponse response) {
                    if( response != NSAlertFirstButtonReturn )
                        return;
                    dispatch_to_default([recovery_coordinator = std::move(_recovery_coordinator),
                                         operation_center = std::move(_operation_center),
                                         plan_id = std::move(_plan_id),
                                         durable_outcome_delivered = std::move(_durable_outcome_delivered),
                                         weak_window_controller] {
                        const auto result = nc::core::ServiceCopyRecoveryAndRefreshHistory(
                            recovery_coordinator, operation_center, plan_id);
                        if( durable_outcome_delivered->load(std::memory_order_acquire) )
                            return;
                        dispatch_to_main_queue([result,
                                                recovery_coordinator,
                                                operation_center,
                                                durable_outcome_delivered,
                                                weak_window_controller] {
                            if( NCMainWindowController *const controller = weak_window_controller ) {
                                PresentRecoveryHistoryRefreshResult(controller,
                                                                    result,
                                                                    recovery_coordinator,
                                                                    operation_center,
                                                                    durable_outcome_delivered);
                            }
                        });
                    });
                  }];
}

void PresentCoordinatorSubmissionFailure(
    NCMainWindowController *_window_controller,
    const nc::ops::OperationCenterSubmissionError &_error,
    std::string _plan_id,
    std::shared_ptr<nc::core::CopyOperationRecoveryCoordinator> _recovery_coordinator,
    std::shared_ptr<nc::ops::OperationCenterCoordinator> _operation_center,
    std::shared_ptr<std::atomic_bool> _durable_outcome_delivered)
{
    dispatch_assert_main_queue();
    if( _error.orchestrator_error ) {
        PresentSubmissionFailure(
            _window_controller,
            *_error.orchestrator_error,
            std::move(_plan_id),
            std::move(_recovery_coordinator),
            std::move(_operation_center),
            std::move(_durable_outcome_delivered));
        return;
    }
    ShowCopyAlert(
        _window_controller, @"Copy submission failed", CoordinatorSubmissionErrorDescription(_error), NSAlertStyleCritical);
}

void ShowCopyAlert(NCMainWindowController *_window_controller,
                   NSString *_title,
                   NSString *_message,
                   const NSAlertStyle _style = NSAlertStyleWarning)
{
    dispatch_assert_main_queue();
    Alert *const alert = [[Alert alloc] init];
    alert.alertStyle = _style;
    alert.messageText = _title;
    alert.informativeText = _message;
    [alert addButtonWithTitle:NSLocalizedString(@"OK", "Acknowledge a copy lifecycle message")];
    if( _window_controller.window )
        [alert beginSheetModalForWindow:_window_controller.window completionHandler:nil];
    else
        [alert runModal];
}

bool ReviewedCopyPaneIsCurrent(MainWindowFilePanelState *_state,
                               PanelController *_panel,
                               const nc::core::PaneId _pane_id,
                               const unsigned long _data_generation)
{
    dispatch_assert_main_queue();
    return _state && _panel && _state.activePanelController == _panel && _panel.paneId == _pane_id &&
           _panel.dataGeneration == _data_generation;
}

bool ReviewedCopyIntentIsCurrent(MainWindowFilePanelState *_state,
                                 PanelController *_panel,
                                 const nc::core::PaneId _pane_id,
                                 const unsigned long _data_generation,
                                 const VFSListingItem &_item)
{
    return ReviewedCopyPaneIsCurrent(_state, _panel, _pane_id, _data_generation) && _panel.view.item == _item;
}

/**
 * The same question asked about a selection, which is what a `Copy To` acts on.
 *
 * It is not the focused-item question with a loop around it, and substituting one for the other would
 * be wrong in both directions: `Copy As` acts on the focused item while a selection may exist and name
 * something else entirely, and `Copy To` acts on the selection, which the user can change without the
 * focus or the data generation moving at all. A listing item compares as listing identity plus index,
 * so a reloaded pane invalidates the intent even where the same filenames come back - which is the
 * answer this needs, since the plan was reviewed against the objects, not against their names.
 */
bool ReviewedCopyIntentIsCurrent(MainWindowFilePanelState *_state,
                                 PanelController *_panel,
                                 const nc::core::PaneId _pane_id,
                                 const unsigned long _data_generation,
                                 const std::vector<VFSListingItem> &_items)
{
    return ReviewedCopyPaneIsCurrent(_state, _panel, _pane_id, _data_generation) &&
           _panel.selectedEntriesOrFocusedEntry == _items;
}

/** Everything one reviewed submission needs to carry across its two asynchronous hops. */
struct ReviewedCopyRequest final {
    std::vector<VFSListingItem> items;
    /** Exactly what the user named: one destination file, or the folder to land in. */
    std::string destination;
    nc::ops::OperationPlanDestinationKind destination_kind{nc::ops::OperationPlanDestinationKind::ExactItem};
    nc::ops::OperationPlanType plan_type{nc::ops::OperationPlanType::Copy};
    nc::core::PaneId pane_id;
    unsigned long data_generation{0};
    /** `Copy To` acts on the selection; `Copy As` acts on the focused item. Different questions. */
    bool intent_follows_selection{false};
    bool deselect{false};
};

/** "Copy" or "Move", for user-visible wording - the only difference the two plan types read as here. */
NSString *ReviewedOperationNoun(const nc::ops::OperationPlanType _plan_type)
{
    return _plan_type == nc::ops::OperationPlanType::Move ? @"Move" : @"Copy";
}

bool ReviewedCopyIntentIsCurrent(MainWindowFilePanelState *_state,
                                 PanelController *_panel,
                                 const ReviewedCopyRequest &_request)
{
    if( _request.items.empty() )
        return false;
    return _request.intent_follows_selection
               ? ReviewedCopyIntentIsCurrent(_state, _panel, _request.pane_id, _request.data_generation, _request.items)
               : ReviewedCopyIntentIsCurrent(
                     _state, _panel, _request.pane_id, _request.data_generation, _request.items.front());
}

void SubmitLegacyCopyAs(MainWindowFilePanelState *_target,
                        PanelController *_panel,
                        const std::vector<VFSListingItem> &_entries,
                        const std::string &_path,
                        const VFSHostPtr &_host,
                        const nc::ops::CopyingOptions &_options,
                        const bool _deselect)
{
    const auto op = std::make_shared<nc::ops::Copying>(_entries, _path, _host, _options);

    const auto update = RefreshCurrentActiveControllerLambda(_target);
    op->ObserveUnticketed(nc::ops::Operation::NotifyAboutFinish, update);

    __weak PanelController *weak_panel = _panel;
    op->ObserveUnticketed(nc::ops::Operation::NotifyAboutCompletion, [weak_panel, path = _path] {
        dispatch_to_main_queue([weak_panel, path] {
            if( PanelController *const panel = weak_panel ) {
                if( panel.isUniform &&
                    reviewed_copy_as::IsSameLexicalPath(panel.currentDirectoryPath,
                                                        std::filesystem::path{path}.parent_path().native()) ) {
                    nc::panel::DelayedFocusing request;
                    request.filename = std::filesystem::path{path}.filename().native();
                    [panel scheduleDelayedFocusing:request];
                }
            }
        });
    });

    if( _deselect ) {
        const auto deselector = std::make_shared<const DeselectorViaOpNotification>(_panel);
        op->SetItemStatusCallback([deselector](nc::ops::ItemStateReport _report) { deselector->Handle(_report); });
    }

    [_target.mainWindowController enqueueOperation:op];
}

void SubmitLegacyMoveAs(MainWindowFilePanelState *_target,
                        PanelController *_panel,
                        const std::vector<VFSListingItem> &_entries,
                        const std::string &_path,
                        const VFSHostPtr &_host,
                        const nc::ops::CopyingOptions &_options,
                        const bool _deselect)
{
    const auto op = std::make_shared<nc::ops::Copying>(_entries, _path, _host, _options);

    const auto update = RefreshCurrentActiveControllerLambda(_target);
    op->ObserveUnticketed(nc::ops::Operation::NotifyAboutFinish, update);

    __weak PanelController *weak_panel = _panel;
    op->ObserveUnticketed(nc::ops::Operation::NotifyAboutCompletion, [weak_panel, path = _path] {
        dispatch_to_main_queue([weak_panel, path] {
            if( PanelController *const panel = weak_panel ) {
                if( panel.isUniform &&
                    reviewed_copy_as::IsSameLexicalPath(panel.currentDirectoryPath,
                                                        std::filesystem::path{path}.parent_path().native()) ) {
                    nc::panel::DelayedFocusing request;
                    request.filename = std::filesystem::path{path}.filename().native();
                    [panel scheduleDelayedFocusing:request];
                }
            }
        });
    });

    if( _deselect ) {
        const auto deselector = std::make_shared<const DeselectorViaOpNotification>(_panel);
        op->SetItemStatusCallback([deselector](nc::ops::ItemStateReport _report) { deselector->Handle(_report); });
    }

    [_target.mainWindowController enqueueOperation:op];
}

NSString *JournalItemErrorDescription(const nc::ops::OperationJournalItemError _error)
{
    using enum nc::ops::OperationJournalItemError;
    switch( _error ) {
        case None:
            return @"none";
        case SourceChanged:
            return @"the source changed";
        case DestinationChanged:
            return @"the destination changed";
        case PermissionDenied:
            return @"access was denied";
        case Read:
            return @"the source could not be read";
        case Write:
            return @"the destination could not be written";
        case Metadata:
            return @"metadata verification failed";
        case Commit:
            return @"conditional publication failed";
        case Cleanup:
            return @"cleanup failed";
        case Cancelled:
            return @"the operation was cancelled";
        case Unknown:
            return @"the failure could not be classified";
    }
    return @"the failure could not be classified";
}

NSString *JournalRecoveryDescription(const nc::ops::OperationJournalRecoveryAction _action)
{
    using enum nc::ops::OperationJournalRecoveryAction;
    switch( _action ) {
        case None:
            return nil;
        case Retry:
            return @"Retry is safe after revalidation.";
        case InspectDestination:
            return @"Inspect the destination before retrying.";
        case RemoveTemporaryItem:
            return @"Remove the temporary item before retrying.";
        case RestoreSource:
            return @"Restore the source before continuing.";
    }
    return @"Inspect the journal state before continuing.";
}

void AppendSystemError(NSMutableString *_message, NSString *_label, const int _error)
{
    if( _error == 0 )
        return;
    const char *const description = std::strerror(_error);
    if( description )
        [_message appendFormat:@"\n%@: %@ (%d)", _label, [NSString stringWithUTF8String:description], _error];
    else
        [_message appendFormat:@"\n%@: %d", _label, _error];
}

/** Everything one result has to say, appended under whatever heading names the item it belongs to. */
void AppendDurableItemDetail(NSMutableString *_message,
                             const nc::ops::OperationJournalItemResult &_result,
                             NSString **_title,
                             NSString *_noun)
{
    switch( _result.destination_publication ) {
        case nc::ops::OperationJournalPublicationState::NotPublished:
            [_message appendString:@"The durable journal confirms that the destination was not published."];
            break;
        case nc::ops::OperationJournalPublicationState::Published:
            [_message appendFormat:@"The destination was published, but the %@ did not satisfy its terminal contract.",
                                   [_noun lowercaseString]];
            break;
        case nc::ops::OperationJournalPublicationState::Unknown:
            [_message appendString:@"The durable journal cannot confirm whether the destination was published."];
            break;
    }

    if( _result.error != nc::ops::OperationJournalItemError::None )
        [_message appendFormat:@"\nFailure: %@.", JournalItemErrorDescription(_result.error)];
    AppendSystemError(_message, @"System error", _result.system_error);
    if( _result.prior_error != nc::ops::OperationJournalItemError::None ) {
        [_message appendFormat:@"\nPrior failure: %@.", JournalItemErrorDescription(_result.prior_error)];
        AppendSystemError(_message, @"Prior system error", _result.prior_system_error);
    }

    switch( _result.filesystem_sync_status ) {
        case nc::ops::OperationJournalFilesystemSyncStatus::NotAttempted:
            [_message appendString:@"\nFilesystem durability: not attempted."];
            break;
        case nc::ops::OperationJournalFilesystemSyncStatus::Confirmed:
            [_message appendString:@"\nFilesystem durability: confirmed."];
            break;
        case nc::ops::OperationJournalFilesystemSyncStatus::Failed:
            [_message appendString:@"\nFilesystem durability: failed."];
            AppendSystemError(_message, @"Filesystem sync error", _result.filesystem_sync_system_error);
            break;
    }
    if( NSString *const recovery = JournalRecoveryDescription(_result.recovery_action) ) {
        [_message appendFormat:@"\nRecovery: %@", recovery];
        *_title = [NSString stringWithFormat:@"%@ requires recovery", _noun];
    }
}

/**
 * `_focus_panel` is the pane the files land in, which is not always the pane that was acted on: a
 * `Copy To` writes into the opposite one. `_refresh_panels` is likewise the caller's to decide -
 * whether anything needs refreshing is a fact about the outcome and belongs to the classifier, but
 * *which* panes show the destination is a fact about the command.
 */
void PresentDurableCopyOutcome(const nc::ops::CopyOperationDurableTerminalOutcome &_outcome,
                               PanelController *_focus_panel,
                               NCMainWindowController *_window_controller,
                               const std::string &_focus_destination,
                               const std::function<void()> &_refresh_panels,
                               NSString *_noun)
{
    dispatch_assert_main_queue();
    // Decided over the whole set of results rather than over the one this surface used to demand. A
    // batch in which every item published reached here as "no terminal item result" and was announced
    // as a copy needing reconciliation; the rules are in the classifier, tested without AppKit.
    const auto presentation = reviewed_copy_as::ClassifyDurableCopyOutcome(_outcome);

    if( presentation.refresh_panel && _refresh_panels )
        _refresh_panels();

    if( presentation.kind == reviewed_copy_as::DurableCopyOutcomeKind::Published ) {
        // Only a lone publication has one destination to reveal, and the classifier is what says so.
        if( _focus_panel && presentation.focus_single_publication && _focus_panel.isUniform &&
            reviewed_copy_as::IsSameLexicalPath(_focus_panel.currentDirectoryPath,
                                                std::filesystem::path{_focus_destination}.parent_path().native()) ) {
            nc::panel::DelayedFocusing request;
            request.filename = std::filesystem::path{_focus_destination}.filename().native();
            [_focus_panel scheduleDelayedFocusing:request];
        }
        return;
    }

    if( presentation.kind == reviewed_copy_as::DurableCopyOutcomeKind::Silent )
        return;

    NSMutableString *const message = [NSMutableString string];
    NSString *title = [NSString stringWithFormat:@"%@ did not complete", _noun];
    if( presentation.without_item_results ) {
        [message appendString:@"The durable journal has no terminal item result. Inspect the destination and reconcile "
                              @"the journal before retrying."];
        title = [NSString stringWithFormat:@"%@ requires reconciliation", _noun];
    }
    else if( presentation.total_items == 1 ) {
        // One item still reads exactly as it always did - no count, no item heading, nothing that
        // would make a single copy look like a batch of one.
        AppendDurableItemDetail(message, _outcome.item_results.front(), &title, _noun);
    }
    else {
        // A set has to say how much of it landed before it says what went wrong with the rest,
        // because "which files exist now" is the question the user actually has.
        [message
            appendFormat:@"%zu of %zu items were published.", presentation.published_items, presentation.total_items];
        for( const size_t index : presentation.attention_indices ) {
            const auto &result = _outcome.item_results[index];
            [message appendFormat:@"\n\nItem %zu: ", result.item_index + 1];
            AppendDurableItemDetail(message, result, &title, _noun);
        }
        if( presentation.attention_indices.empty() ) {
            [message appendString:@"\nThe durable journal reports no successful terminal state for the operation as a "
                                  @"whole. Inspect the destination before retrying."];
            title = [NSString stringWithFormat:@"%@ requires reconciliation", _noun];
        }
    }

    ShowCopyAlert(_window_controller, title, message, NSAlertStyleCritical);
}

std::expected<nc::ops::VFSBoundOperationPreflight, NSString *>
BuildReviewedCopyPreflight(const std::vector<VFSListingItem> &_items,
                           const std::string &_destination,
                           const nc::ops::OperationPlanDestinationKind _destination_kind,
                           const nc::ops::OperationPlanType _plan_type,
                           nc::panel::DirectoryAccessProvider &_access_provider)
{
    NSString *const noun = ReviewedOperationNoun(_plan_type);
    if( _items.empty() )
        return std::unexpected([NSString stringWithFormat:@"The %@ request names no items.", [noun lowercaseString]]);

    std::vector<nc::ops::OperationPlanSourceInput> sources;
    sources.reserve(_items.size());
    for( const auto &item : _items ) {
        // One binding for the whole plan, which the selection policy has already made true: every item
        // it accepts shares the destination host, and the destination host is the source host.
        if( item.Host() != _items.front().Host() )
            return std::unexpected([NSString
                stringWithFormat:@"The %@ request spans more than one storage provider.", [noun lowercaseString]]);
        sources.emplace_back(
            nc::ops::OperationPlanSourceInput{std::string{reviewed_copy_as::g_ReviewedCopyProviderId}, item.Path()});
    }

    auto plan = nc::ops::OperationPlan::Create({
        .plan_id = NSUUID.UUID.UUIDString.UTF8String,
        .type = _plan_type,
        .sources = std::move(sources),
        .destination = nc::ops::OperationPlanDestinationInput{std::string{reviewed_copy_as::g_ReviewedCopyProviderId},
                                                              _destination,
                                                              _destination_kind},
        .conflict_policy = nc::ops::OperationPlanConflictPolicy{nc::ops::OperationPlanConflictDecision::Ask,
                                                                nc::ops::OperationPlanConflictScope::ThisItem},
        .created_at = nc::ops::OperationPlan::Clock::now(),
    });
    if( !plan )
        return std::unexpected(
            [NSString stringWithFormat:@"The %@ request is structurally invalid.", [noun lowercaseString]]);

    auto bindings = nc::ops::VFSOperationPlanningBindings::Create(
        {{std::string{reviewed_copy_as::g_ReviewedCopyProviderId}, _items.front().Host()}});
    if( !bindings )
        return std::unexpected(
            [NSString stringWithFormat:@"The source provider could not be bound to the %@ request.", [noun lowercaseString]]);

    auto probes = nc::ops::VFSOperationPlanningProbes::Create(
        *bindings, nc::core::MakeVFSOperationPlanningAccessChecker(_access_provider));
    if( !probes )
        return std::unexpected(@"The provider validation boundary could not be created.");

    return probes->Preflight(std::move(*plan));
}

NSString *ReviewedCopyWarningDescription(const nc::ops::OperationPlanningWarningCode _code)
{
    using enum nc::ops::OperationPlanningWarningCode;
    switch( _code ) {
        case EstimateUnavailable:
            return @"The size estimate is unavailable.";
        case SpaceUnknown:
            return @"Available destination space is unknown.";
        case DestructiveReplacement:
            return @"The destination would be replaced.";
        case RuntimeRevalidationRequired:
            return @"Provider eligibility and object identity will be revalidated immediately before publication.";
    }
    return @"The copy requires additional runtime validation.";
}

NSString *ReviewedCopyDetails(const reviewed_copy_as::ReviewPresentation &_presentation)
{
    NSMutableString *const details = [NSMutableString string];
    if( _presentation.items.size() == 1 ) {
        // One item still names both its ends outright: for a single copy that is the whole review,
        // and a list of one under a folder heading would say less about the same thing.
        [details appendFormat:@"Source: %@\nDestination: %@\n\nScope: one item, create only",
                              [NSString stringWithUTF8StdString:_presentation.items.front().source_path],
                              [NSString stringWithUTF8StdString:_presentation.items.front().destination_path]];
    }
    else {
        // A set names the folder once and then the files, because the folder is the part the user
        // chose and the destinations are all derived from it.
        [details appendFormat:@"Destination folder: %@\n\nSources:",
                              [NSString stringWithUTF8StdString:_presentation.destination_root]];
        for( const auto &item : _presentation.items )
            [details appendFormat:@"\n• %@", [NSString stringWithUTF8StdString:item.source_path]];
        [details appendFormat:@"\n\nScope: %zu items, create only", _presentation.items.size()];
    }
    [details appendString:@"\nConflict policy: ask for this item"];

    if( _presentation.estimated_files )
        [details
            appendFormat:@"\nEstimated files: %llu", static_cast<unsigned long long>(*_presentation.estimated_files)];
    else
        [details appendString:@"\nEstimated files: unknown"];
    if( _presentation.estimated_bytes )
        [details
            appendFormat:@"\nEstimated bytes: %llu", static_cast<unsigned long long>(*_presentation.estimated_bytes)];
    else
        [details appendString:@"\nEstimated bytes: unknown"];

    if( _presentation.available_bytes ) {
        [details appendFormat:@"\nDestination available bytes: %llu",
                              static_cast<unsigned long long>(*_presentation.available_bytes)];
    }
    else {
        [details appendString:@"\nDestination available bytes: unknown"];
    }
    [details appendFormat:@"\nAccess checks granted: %llu",
                          static_cast<unsigned long long>(_presentation.access_evidence_count)];

    if( !_presentation.warnings.empty() ) {
        [details appendString:@"\n\nValidation notes:"];
        for( const auto warning : _presentation.warnings )
            [details appendFormat:@"\n• %@", ReviewedCopyWarningDescription(warning)];
    }
    return details;
}

/**
 * `_panel` is the pane acted on - it owns the intent and the deselection. `_focus_panel` is the pane
 * the files land in, and for a `Copy To` those are two different panes. `_refresh_panels` is what the
 * command wants brought up to date once the outcome says something may exist on disk.
 */
void SubmitReviewedCopy(MainWindowFilePanelState *_target,
                        PanelController *_panel,
                        PanelController *_focus_panel,
                        std::function<void()> _refresh_panels,
                        ReviewedCopyRequest _request)
{
    dispatch_assert_main_queue();
    if( _request.items.empty() )
        return;
    NSString *const noun = ReviewedOperationNoun(_request.plan_type);
    NCAppDelegate *const app = NCAppDelegate.me;
    const auto journal = app.operationJournal;
    const auto custodian = app.copyOperationRunReceiptCustodian;
    const auto operation_center = app.operationCenterCoordinator;
    const auto recovery_coordinator = app.copyOperationRecoveryCoordinator;
    if( !journal || !custodian || !operation_center || !recovery_coordinator ) {
        ShowCopyAlert(_target.mainWindowController,
                      [NSString stringWithFormat:@"%@ unavailable", noun],
                      [NSString stringWithFormat:@"The durable operation journal could not be opened. The %@ was not "
                                                 @"started.",
                                                 [noun lowercaseString]],
                      NSAlertStyleCritical);
        return;
    }

    const auto pool = _target.operationsPool.shared_from_this();
    auto *const access_provider = &app.directoryAccessProvider;
    __weak PanelController *weak_panel = _panel;
    __weak PanelController *weak_focus_panel = _focus_panel;
    __weak NCMainWindowController *weak_window_controller = _target.mainWindowController;
    __weak MainWindowFilePanelState *weak_state = _target;

    dispatch_to_default([request = std::move(_request),
                         refresh_panels = std::move(_refresh_panels),
                         journal,
                         custodian,
                         operation_center,
                         recovery_coordinator,
                         pool,
                         access_provider,
                         noun,
                         weak_panel,
                         weak_focus_panel,
                         weak_window_controller,
                         weak_state]() mutable {
        auto preflight = BuildReviewedCopyPreflight(
            request.items, request.destination, request.destination_kind, request.plan_type, *access_provider);
        dispatch_to_main_queue([request = std::move(request),
                                refresh_panels = std::move(refresh_panels),
                                journal,
                                custodian,
                                operation_center,
                                recovery_coordinator,
                                pool,
                                noun,
                                weak_panel,
                                weak_focus_panel,
                                weak_window_controller,
                                weak_state,
                                preflight = std::move(preflight)]() mutable {
            PanelController *const panel = weak_panel;
            NCMainWindowController *const window_controller = weak_window_controller;
            MainWindowFilePanelState *const state = weak_state;
            if( !panel || !window_controller || !state )
                return;
            if( !preflight ) {
                ShowCopyAlert(
                    window_controller, [NSString stringWithFormat:@"%@ validation failed", noun], preflight.error());
                return;
            }

            auto prepared = reviewed_copy_as::PrepareReviewedCopyApplicationBoundary(
                std::move(*preflight), true, ReviewedCopyIntentIsCurrent(state, panel, request));
            if( !prepared ) {
                switch( prepared.error().code ) {
                    case reviewed_copy_as::PreparationErrorCode::StaleIntent:
                        ShowCopyAlert(window_controller,
                                      [NSString stringWithFormat:@"%@ request expired", noun],
                                      [NSString stringWithFormat:@"The active pane or its selection changed while "
                                                                 @"the %@ request was being validated.",
                                                                 [noun lowercaseString]]);
                        return;
                    case reviewed_copy_as::PreparationErrorCode::BlockedPreflight:
                        ShowCopyAlert(window_controller,
                                      [NSString stringWithFormat:@"%@ blocked", noun],
                                      prepared.error().blocker
                                          ? PlanningBlockerDescription(*prepared.error().blocker)
                                          : [NSString stringWithFormat:@"The reviewed %@ is blocked.",
                                                                       [noun lowercaseString]]);
                        return;
                    case reviewed_copy_as::PreparationErrorCode::UnsupportedScope:
                        ShowCopyAlert(window_controller,
                                      [NSString stringWithFormat:@"%@ blocked", noun],
                                      [NSString stringWithFormat:@"The validated %@ no longer satisfies the "
                                                                 @"create-only reviewed scope.",
                                                                 [noun lowercaseString]]);
                        return;
                    case reviewed_copy_as::PreparationErrorCode::UnpersistedRuntime:
                        ShowCopyAlert(window_controller,
                                      [NSString stringWithFormat:@"%@ unavailable", noun],
                                      [NSString stringWithFormat:@"The durable operation journal could not be "
                                                                 @"opened. The %@ was not started.",
                                                                 [noun lowercaseString]],
                                      NSAlertStyleCritical);
                        return;
                }
            }

            NSString *const details = ReviewedCopyDetails(prepared->Presentation());

            // Where the one file lands, taken from the projection the boundary already checked rather
            // than derived a second time here. A second derivation could disagree with the first, and
            // the way it would fail is silently: a focus request for a path that was never written.
            // Empty for a set, which is the same answer the terminal classifier gives.
            std::string focus_destination;
            if( prepared->Presentation().items.size() == 1 )
                focus_destination = prepared->Presentation().items.front().destination_path;

            Alert *const review = [[Alert alloc] init];
            review.alertStyle = NSAlertStyleInformational;
            review.messageText = [NSString stringWithFormat:@"Review %@", noun];
            review.informativeText = details;
            [review addButtonWithTitle:NSLocalizedString(noun, "Approve a reviewed copy or move")];
            [review addButtonWithTitle:NSLocalizedString(@"Cancel", "Cancel a reviewed copy or move")];

            auto prepared_review = std::make_shared<reviewed_copy_as::PreparedReview>(std::move(*prepared));
            [review
                beginSheetModalForWindow:window_controller.window
                       completionHandler:^(NSModalResponse response) {
                         MainWindowFilePanelState *const current_state = weak_state;
                         if( !current_state ) {
                             if( response != NSAlertFirstButtonReturn )
                                 return;
                             ShowCopyAlert(window_controller,
                                           [NSString stringWithFormat:@"%@ request expired", noun],
                                           @"The active pane or focused item changed before approval.");
                             return;
                         }

                         std::shared_ptr<const DeselectorViaOpNotification> deselector;
                         if( request.deselect )
                             deselector = std::make_shared<const DeselectorViaOpNotification>(panel);

                         nc::ops::ItemStateReportCallback item_status_observer;
                         if( deselector ) {
                             item_status_observer = [deselector](nc::ops::ItemStateReport _report) {
                                 deselector->Handle(_report);
                             };
                         }

                         const auto approval = prepared_review->Approve(
                             response == NSAlertFirstButtonReturn,
                             [weak_state, panel, request] {
                                 MainWindowFilePanelState *const current_state = weak_state;
                                 return current_state && ReviewedCopyIntentIsCurrent(current_state, panel, request);
                             },
                             current_state.operationSubmissionGate,
                             {
                                 .dispatch_to_ui =
                                     [](std::function<void()> _task) { dispatch_to_main_queue(std::move(_task)); },
                                 .present_durable_outcome =
                                     [weak_focus_panel,
                                      weak_window_controller,
                                      focus = focus_destination,
                                      refresh_panels,
                                      noun](nc::ops::CopyOperationDurableTerminalOutcome _outcome) {
                                         PresentDurableCopyOutcome(_outcome,
                                                                   weak_focus_panel,
                                                                   weak_window_controller,
                                                                   focus,
                                                                   refresh_panels,
                                                                   noun);
                                     },
                                 .item_status_observer = std::move(item_status_observer),
                                 .submit =
                                     [journal,
                                      custodian,
                                      operation_center,
                                      recovery_coordinator,
                                      pool,
                                      weak_window_controller](
                                         nc::ops::ReviewedVFSOperationPreflight _reviewed,
                                         std::shared_ptr<nc::core::OperationSubmissionGate::Ticket> _submission_ticket,
                                         nc::ops::CopyOperationSubmissionHooks _hooks,
                                         std::shared_ptr<std::atomic_bool> _durable_outcome_delivered) mutable {
                                         const auto plan_id = std::string{_reviewed.AcceptedPlan().Plan().Id().Value()};
                                         dispatch_to_default([reviewed = std::move(_reviewed),
                                                              journal,
                                                              custodian,
                                                              operation_center,
                                                              recovery_coordinator,
                                                              pool,
                                                              plan_id,
                                                              hooks = std::move(_hooks),
                                                              durable_outcome_delivered =
                                                                  std::move(_durable_outcome_delivered),
                                                              submission_ticket = std::move(_submission_ticket),
                                                              weak_window_controller]() mutable {
                                             nc::ops::CopyOperationOrchestrator orchestrator{journal, pool, custodian};
                                             auto submitted = operation_center->SubmitReviewedCopy(
                                                 *journal,
                                                 orchestrator,
                                                 std::move(reviewed),
                                                 [submission_ticket] { return submission_ticket->IsCancelled(); },
                                                 std::move(hooks));
                                             if( !submitted ) {
                                                 const auto error = submitted.error();
                                                 if( (error.orchestrator_error &&
                                                      error.orchestrator_error->code ==
                                                          nc::ops::CopyOperationOrchestratorErrorCode::Cancelled) ||
                                                     durable_outcome_delivered->load(std::memory_order_acquire) )
                                                     return;
                                                 dispatch_to_main_queue([weak_window_controller,
                                                                         error,
                                                                         plan_id,
                                                                         recovery_coordinator,
                                                                         operation_center,
                                                                         durable_outcome_delivered] {
                                                     if( NCMainWindowController *const controller =
                                                             weak_window_controller ) {
                                                         PresentCoordinatorSubmissionFailure(controller,
                                                                                             error,
                                                                                             plan_id,
                                                                                             recovery_coordinator,
                                                                                             operation_center,
                                                                                             durable_outcome_delivered);
                                                     }
                                                 });
                                             }
                                         });
                                     },
                             });

                         switch( approval ) {
                             case reviewed_copy_as::ApprovalResult::Submitted:
                             case reviewed_copy_as::ApprovalResult::Declined:
                                 return;
                             case reviewed_copy_as::ApprovalResult::StaleIntent:
                                 ShowCopyAlert(window_controller,
                                               [NSString stringWithFormat:@"%@ request expired", noun],
                                               @"The active pane or focused item changed before approval.");
                                 return;
                             case reviewed_copy_as::ApprovalResult::Cancelled:
                                 ShowCopyAlert(window_controller,
                                               [NSString stringWithFormat:@"%@ submission cancelled", noun],
                                               [NSString stringWithFormat:@"The window is closing. The %@ was not "
                                                                          @"started.",
                                                                          [noun lowercaseString]]);
                                 return;
                             case reviewed_copy_as::ApprovalResult::ReviewFailed:
                             case reviewed_copy_as::ApprovalResult::AlreadyConsumed:
                             case reviewed_copy_as::ApprovalResult::MissingSubmissionPort:
                             case reviewed_copy_as::ApprovalResult::SubmissionPortFailed:
                                 ShowCopyAlert(window_controller,
                                               [NSString stringWithFormat:@"%@ approval failed", noun],
                                               [NSString stringWithFormat:@"The bound %@ request could not be "
                                                                          @"approved safely.",
                                                                          [noun lowercaseString]]);
                                 return;
                         }
                       }];
        });
    });
}

} // namespace

static const auto g_DeselectConfigFlag = "filePanel.general.deselectItemsAfterFileOperations";

CopyBase::CopyBase(nc::config::Config &_config) : m_Config(_config)
{
}

void CopyBase::AddDeselectorIfNeeded(nc::ops::Operation &_operation, PanelController *_target) const
{
    if( !ShouldAutomaticallyDeselect() )
        return;

    const auto deselector = std::make_shared<const DeselectorViaOpNotification>(_target);
    _operation.SetItemStatusCallback([deselector](nc::ops::ItemStateReport _report) { deselector->Handle(_report); });
}

bool CopyBase::ShouldAutomaticallyDeselect() const
{
    return m_Config.GetBool(g_DeselectConfigFlag);
}

MoveBase::MoveBase(nc::config::Config &_config) : m_Config(_config)
{
}

void MoveBase::AddDeselectorIfNeeded(nc::ops::Operation &_operation, PanelController *_target) const
{
    if( !ShouldAutomaticallyDeselect() )
        return;

    const auto deselector = std::make_shared<const DeselectorViaOpNotification>(_target);
    _operation.SetItemStatusCallback([deselector](nc::ops::ItemStateReport _report) { deselector->Handle(_report); });
}

bool MoveBase::ShouldAutomaticallyDeselect() const
{
    return m_Config.GetBool(g_DeselectConfigFlag);
}

CopyTo::CopyTo(nc::config::Config &_config) : CopyBase(_config)
{
}

bool CopyTo::Predicate(MainWindowFilePanelState *_target) const
{
    const auto act_pc = _target.activePanelController;
    const auto opp_pc = _target.self.oppositePanelController;
    if( !act_pc || !opp_pc )
        return false;

    const auto i = act_pc.view.item;
    if( !i )
        return false;

    if( i.IsDotDot() && act_pc.data.Stats().selected_entries_amount == 0 )
        return false;

    if( opp_pc.isUniform && !opp_pc.vfs->IsWritable() )
        return false;

    return true;
}

void CopyTo::Perform(MainWindowFilePanelState *_target, id /*_sender*/) const
{
    const auto act_pc = _target.activePanelController;
    const auto opp_pc = _target.oppositePanelController;
    if( !act_pc || !opp_pc )
        return;

    auto entries = _target.activePanelController.selectedEntriesOrFocusedEntry;
    if( entries.empty() )
        return;

    const auto act_uniform = act_pc.isUniform;
    const auto opp_uniform = opp_pc.isUniform;
    const auto pane_id = act_pc.paneId;
    const auto data_generation = act_pc.dataGeneration;

    const auto cd = [[NCOpsCopyingDialog alloc] initWithItems:entries
                                                    sourceVFS:act_uniform ? act_pc.vfs : nullptr
                                              sourceDirectory:act_uniform ? act_pc.currentDirectoryPath : ""
                                           initialDestination:opp_uniform ? opp_pc.currentDirectoryPath : ""
                                               destinationVFS:opp_uniform ? opp_pc.vfs : nullptr
                                             operationOptions:MakeDefaultFileCopyOptions()];

    const auto handler = ^(NSModalResponse returnCode) {
      if( returnCode != NSModalResponseOK )
          return;

      auto path = cd.resultDestination;
      auto host = cd.resultHost;
      auto opts = cd.resultOptions;
      if( !host || path.empty() )
          return; // ui invariant is broken

      const auto submit_legacy = [&] {
          const auto op = std::make_shared<nc::ops::Copying>(entries, path, host, opts);

          const auto update_both_panels = RefreshBothCurrentControllersLambda(_target);
          op->ObserveUnticketed(nc::ops::Operation::NotifyAboutFinish, update_both_panels);

          AddDeselectorIfNeeded(*op, act_pc);

          [_target.mainWindowController enqueueOperation:op];
      };

      // The destination the dialog returns is whatever was typed, and the legacy operation is free to
      // read it as a folder, a new name, or a path whose parent it must create. The reviewed engine
      // means exactly one of those, so an existing folder is the only shape it is offered.
      const auto folder = reviewed_copy_as::TrimmedExistingDirectory(host, path);
      if( !folder ) {
          submit_legacy();
          return;
      }

      switch( reviewed_copy_as::SelectBatch(entries, *folder, host, opts) ) {
          case reviewed_copy_as::Selection::Legacy:
              submit_legacy();
              return;
          case reviewed_copy_as::Selection::Reject:
              ShowCopyAlert(
                  _target.mainWindowController,
                  @"Copy validation unavailable",
                  @"The storage provider could not establish whether this copy is eligible. The copy was not started.",
                  NSAlertStyleCritical);
              return;
          case reviewed_copy_as::Selection::Reviewed:
              break;
      }

      if( !ReviewedCopyIntentIsCurrent(_target, act_pc, pane_id, data_generation, entries) ) {
          ShowCopyAlert(_target.mainWindowController,
                        @"Copy request expired",
                        @"The active pane or its selection changed while the copy dialog was open.");
          return;
      }

      // The files land in the opposite pane, so that is the one to reveal them in - and both panes
      // are refreshed, exactly as the legacy route does, because the source pane's selection state
      // moves too. Refreshing only the pane acted on would leave the copied files invisible, which
      // is the whole thing the user asked for.
      SubmitReviewedCopy(_target,
                         act_pc,
                         opp_pc,
                         RefreshBothCurrentControllersLambda(_target),
                         ReviewedCopyRequest{
                             .items = entries,
                             .destination = *folder,
                             .destination_kind = nc::ops::OperationPlanDestinationKind::Directory,
                             .pane_id = pane_id,
                             .data_generation = data_generation,
                             .intent_follows_selection = true,
                             .deselect = ShouldAutomaticallyDeselect(),
                         });
    };

    [_target.mainWindowController beginSheet:cd.window completionHandler:handler];
}

CopyAs::CopyAs(nc::config::Config &_config) : CopyBase(_config)
{
}

bool CopyAs::Predicate(MainWindowFilePanelState *_target) const
{
    const auto act_pc = _target.activePanelController;
    if( !act_pc )
        return false;

    const auto i = act_pc.view.item;
    if( !i || i.IsDotDot() )
        return false;

    if( !i.Host()->IsWritable() )
        return false;

    return true;
}

void CopyAs::Perform(MainWindowFilePanelState *_target, id /*_sender*/) const
{
    const auto act_pc = _target.activePanelController;
    if( !act_pc )
        return;

    // process only currently focused item
    const auto item = act_pc.view.item;
    if( !item || item.IsDotDot() )
        return;

    const auto entries = std::vector<VFSListingItem>({item});
    const auto pane_id = act_pc.paneId;
    const auto data_generation = act_pc.dataGeneration;

    const auto initial_options = MakeDefaultFileCopyOptions();
    const auto cd = [[NCOpsCopyingDialog alloc] initWithItems:entries
                                                    sourceVFS:item.Host()
                                              sourceDirectory:item.Directory()
                                           initialDestination:item.Filename()
                                               destinationVFS:item.Host()
                                             operationOptions:initial_options];

    const auto handler = ^(NSModalResponse returnCode) {
      if( returnCode != NSModalResponseOK )
          return;

      auto path = cd.resultDestination;
      auto host = cd.resultHost;
      auto opts = cd.resultOptions;
      if( !host || path.empty() )
          return; // ui invariant is broken

      const bool deselect = ShouldAutomaticallyDeselect();
      switch( reviewed_copy_as::Select(item, path, host, opts) ) {
          case reviewed_copy_as::Selection::Legacy:
              SubmitLegacyCopyAs(_target, act_pc, entries, path, host, opts, deselect);
              return;
          case reviewed_copy_as::Selection::Reject:
              ShowCopyAlert(
                  _target.mainWindowController,
                  @"Copy validation unavailable",
                  @"The storage provider could not establish whether this copy is eligible. The copy was not started.",
                  NSAlertStyleCritical);
              return;
          case reviewed_copy_as::Selection::Reviewed:
              break;
      }

      if( !ReviewedCopyIntentIsCurrent(_target, act_pc, pane_id, data_generation, item) ) {
          ShowCopyAlert(_target.mainWindowController,
                        @"Copy request expired",
                        @"The active pane or focused item changed while the copy dialog was open.");
          return;
      }

      // Copy As writes a sibling into the pane it was invoked from, so the pane acted on, the pane
      // the file lands in and the only pane worth refreshing are all the same one.
      __weak PanelController *weak_act_pc = act_pc;
      SubmitReviewedCopy(
          _target,
          act_pc,
          act_pc,
          [weak_act_pc] {
              if( PanelController *const panel = weak_act_pc )
                  [panel refreshPanel];
          },
          ReviewedCopyRequest{
              .items = entries,
              .destination = path,
              .destination_kind = nc::ops::OperationPlanDestinationKind::ExactItem,
              .pane_id = pane_id,
              .data_generation = data_generation,
              .intent_follows_selection = false,
              .deselect = deselect,
          });
    };

    [_target.mainWindowController beginSheet:cd.window completionHandler:handler];
}

MoveTo::MoveTo(nc::config::Config &_config) : MoveBase(_config)
{
}

bool MoveTo::Predicate(MainWindowFilePanelState *_target) const
{
    const auto act_pc = _target.activePanelController;
    const auto opp_pc = _target.self.oppositePanelController;
    if( !act_pc || !opp_pc )
        return false;

    const auto i = act_pc.view.item;
    if( !i )
        return false;

    if( i.IsDotDot() && act_pc.data.Stats().selected_entries_amount == 0 )
        return false;

    if( (act_pc.isUniform && !act_pc.vfs->IsWritable()) || (opp_pc.isUniform && !opp_pc.vfs->IsWritable()) )
        return false;

    return true;
}

void MoveTo::Perform(MainWindowFilePanelState *_target, id /*_sender*/) const
{
    const auto act_pc = _target.activePanelController;
    const auto opp_pc = _target.oppositePanelController;
    if( !act_pc || !opp_pc )
        return;

    const auto act_uniform = act_pc.isUniform;
    const auto opp_uniform = opp_pc.isUniform;

    if( act_uniform && !act_pc.vfs->IsWritable() )
        return;

    auto entries = act_pc.selectedEntriesOrFocusedEntry;
    if( entries.empty() )
        return;

    const auto cd = [[NCOpsCopyingDialog alloc] initWithItems:entries
                                                    sourceVFS:act_uniform ? act_pc.vfs : nullptr
                                              sourceDirectory:act_uniform ? act_pc.currentDirectoryPath : ""
                                           initialDestination:opp_uniform ? opp_pc.currentDirectoryPath : ""
                                               destinationVFS:opp_uniform ? opp_pc.vfs : nullptr
                                             operationOptions:MakeDefaultFileMoveOptions()];

    const auto handler = ^(NSModalResponse returnCode) {
      if( returnCode != NSModalResponseOK )
          return;

      auto path = cd.resultDestination;
      auto host = cd.resultHost;
      auto opts = cd.resultOptions;
      if( !host || path.empty() )
          return; // ui invariant is broken

      const auto op = std::make_shared<nc::ops::Copying>(entries, path, host, opts);

      const auto update_both_panels = RefreshBothCurrentControllersLambda(_target);
      op->ObserveUnticketed(nc::ops::Operation::NotifyAboutFinish, update_both_panels);
      AddDeselectorIfNeeded(*op, act_pc);

      [_target.mainWindowController enqueueOperation:op];
    };

    [_target.mainWindowController beginSheet:cd.window completionHandler:handler];
}

MoveAs::MoveAs(nc::config::Config &_config) : MoveBase(_config)
{
}

bool MoveAs::Predicate(MainWindowFilePanelState *_target) const
{
    const auto act_pc = _target.activePanelController;
    if( !act_pc )
        return false;

    const auto i = act_pc.view.item;
    if( !i || i.IsDotDot() )
        return false;

    if( !i.Host()->IsWritable() )
        return false;

    return true;
}

void MoveAs::Perform(MainWindowFilePanelState *_target, id /*_sender*/) const
{
    const auto act_pc = _target.activePanelController;
    if( !act_pc )
        return;

    // process only current cursor item
    const auto item = act_pc.view.item;
    if( !item || item.IsDotDot() || !item.Host()->IsWritable() )
        return;

    const auto entries = std::vector<VFSListingItem>({item});
    const auto pane_id = act_pc.paneId;
    const auto data_generation = act_pc.dataGeneration;

    const auto cd = [[NCOpsCopyingDialog alloc] initWithItems:entries
                                                    sourceVFS:item.Host()
                                              sourceDirectory:item.Directory()
                                           initialDestination:item.Filename()
                                               destinationVFS:item.Host()
                                             operationOptions:MakeDefaultFileMoveOptions()];

    const auto handler = ^(NSModalResponse returnCode) {
      if( returnCode != NSModalResponseOK )
          return;

      auto path = cd.resultDestination;
      auto host = cd.resultHost;
      auto opts = cd.resultOptions;
      if( !host || path.empty() )
          return; // ui invariant is broken

      const bool deselect = ShouldAutomaticallyDeselect();
      switch( reviewed_move::Select(item, path, host, opts) ) {
          case reviewed_move::Selection::Legacy:
              SubmitLegacyMoveAs(_target, act_pc, entries, path, host, opts, deselect);
              return;
          case reviewed_move::Selection::Reject:
              ShowCopyAlert(
                  _target.mainWindowController,
                  @"Move validation unavailable",
                  @"The storage provider could not establish whether this move is eligible. The move was not started.",
                  NSAlertStyleCritical);
              return;
          case reviewed_move::Selection::Reviewed:
              break;
      }

      if( !ReviewedCopyIntentIsCurrent(_target, act_pc, pane_id, data_generation, item) ) {
          ShowCopyAlert(_target.mainWindowController,
                        @"Move request expired",
                        @"The active pane or focused item changed while the move dialog was open.");
          return;
      }

      // Move As writes into the pane it was invoked from, so the pane acted on, the pane the file
      // lands in and the only pane worth refreshing are all the same one.
      __weak PanelController *weak_act_pc = act_pc;
      SubmitReviewedCopy(
          _target,
          act_pc,
          act_pc,
          [weak_act_pc] {
              if( PanelController *const panel = weak_act_pc )
                  [panel refreshPanel];
          },
          ReviewedCopyRequest{
              .items = entries,
              .destination = path,
              .destination_kind = nc::ops::OperationPlanDestinationKind::ExactItem,
              .plan_type = nc::ops::OperationPlanType::Move,
              .pane_id = pane_id,
              .data_generation = data_generation,
              .intent_follows_selection = false,
              .deselect = deselect,
          });
    };

    [_target.mainWindowController beginSheet:cd.window completionHandler:handler];
}

static std::function<void()> RefreshCurrentActiveControllerLambda(MainWindowFilePanelState *_target)
{
    __weak PanelController *cur = _target.activePanelController;
    auto update_current = [=] { dispatch_to_main_queue([=] { [static_cast<PanelController *>(cur) refreshPanel]; }); };
    return update_current;
}

static std::function<void()> RefreshBothCurrentControllersLambda(MainWindowFilePanelState *_target)
{
    __weak PanelController *cur = _target.activePanelController;
    __weak PanelController *opp = _target.oppositePanelController;
    auto update_both_panels = [=] {
        dispatch_to_main_queue([=] {
            [static_cast<PanelController *>(cur) refreshPanel];
            [static_cast<PanelController *>(opp) refreshPanel];
        });
    };
    return update_both_panels;
}

} // namespace nc::panel::actions
