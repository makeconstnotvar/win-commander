// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "VisualStateMapper.h"

#include <VFS/Host.h>
#include <algorithm>
#include <memory>

namespace nc::core {

namespace {

struct PrimaryVisualState {
    PaneVisualKind kind = PaneVisualKind::Unavailable;
    VisualPriority priority = VisualPriority::Normal;
    std::optional<VisualMessage> message;
};

VisualPriority PriorityForSeverity(const FileManagerErrorSeverity _severity) noexcept
{
    switch( _severity ) {
        case FileManagerErrorSeverity::Info:
            return VisualPriority::Normal;
        case FileManagerErrorSeverity::Warning:
        case FileManagerErrorSeverity::RecoverableError:
            return VisualPriority::Warning;
        case FileManagerErrorSeverity::BlockingError:
            return VisualPriority::Blocking;
        case FileManagerErrorSeverity::DestructiveRisk:
        case FileManagerErrorSeverity::FatalError:
            return VisualPriority::Critical;
    }
}

PaneVisualKind KindForError(const FileManagerErrorCategory _category) noexcept
{
    switch( _category ) {
        case FileManagerErrorCategory::PermissionError:
            return PaneVisualKind::PermissionBlocked;
        case FileManagerErrorCategory::PathNotFoundError:
            return PaneVisualKind::PathNotFound;
        case FileManagerErrorCategory::VolumeUnavailableError:
        case FileManagerErrorCategory::NetworkError:
        case FileManagerErrorCategory::TimeoutError:
        case FileManagerErrorCategory::AuthenticationError:
        case FileManagerErrorCategory::RateLimitError:
            return PaneVisualKind::ProviderUnavailable;
        case FileManagerErrorCategory::ProviderUnsupportedError:
            return PaneVisualKind::Unsupported;
        case FileManagerErrorCategory::ConflictError:
        case FileManagerErrorCategory::ValidationError:
        case FileManagerErrorCategory::InsufficientSpaceError:
        case FileManagerErrorCategory::FileBusyError:
        case FileManagerErrorCategory::ReadOnlyError:
        case FileManagerErrorCategory::ChecksumMismatchError:
        case FileManagerErrorCategory::OperationCancelledError:
        case FileManagerErrorCategory::PartialFailureError:
        case FileManagerErrorCategory::UnknownError:
            return PaneVisualKind::Error;
    }
}

VisualMessage MessageForError(const FileManagerError &_error)
{
    return VisualMessage{
        .user_message_key = _error.user_message_key.empty()
                                ? std::string{file_manager_error_messages::UnknownErrorKey}
                                : _error.user_message_key,
        .user_message_fallback = _error.user_message.empty()
                                     ? std::string{file_manager_error_messages::UnknownErrorFallback}
                                     : _error.user_message,
        .suggested_actions = _error.suggested_actions,
    };
}

VisualMessage GenericErrorMessage()
{
    return VisualMessage{
        .user_message_key = std::string{file_manager_error_messages::UnknownErrorKey},
        .user_message_fallback = std::string{file_manager_error_messages::UnknownErrorFallback},
    };
}

bool IsBlockingSignal(const FileManagerError &_error) noexcept
{
    if( _error.category == FileManagerErrorCategory::OperationCancelledError )
        return false;
    return PriorityForSeverity(_error.severity) >= VisualPriority::Blocking;
}

bool IsPaneError(const PaneVisualKind _kind) noexcept
{
    switch( _kind ) {
        case PaneVisualKind::PermissionBlocked:
        case PaneVisualKind::PathNotFound:
        case PaneVisualKind::ProviderUnavailable:
        case PaneVisualKind::Unsupported:
        case PaneVisualKind::Error:
            return true;
        case PaneVisualKind::Unavailable:
        case PaneVisualKind::Loading:
        case PaneVisualKind::Refreshing:
        case PaneVisualKind::Ready:
        case PaneVisualKind::EmptyFolder:
            return false;
    }
}

PrimaryVisualState ResolvePrimary(const PaneSnapshot &_snapshot, const FileManagerError *_error)
{
    if( _error != nullptr && IsBlockingSignal(*_error) ) {
        return PrimaryVisualState{
            .kind = KindForError(_error->category),
            .priority = PriorityForSeverity(_error->severity),
            .message = MessageForError(*_error),
        };
    }

    if( _snapshot.state.load_phase == PaneLoadPhase::Failed ) {
        // Informational outcomes, including cancellation, are not failed locations to present.
        if( _error != nullptr && (_error->severity == FileManagerErrorSeverity::Info ||
                                  _error->category == FileManagerErrorCategory::OperationCancelledError) )
            return {};

        if( _error != nullptr ) {
            return PrimaryVisualState{
                .kind = KindForError(_error->category),
                .priority = std::max(VisualPriority::Blocking, PriorityForSeverity(_error->severity)),
                .message = MessageForError(*_error),
            };
        }

        return PrimaryVisualState{
            .kind = PaneVisualKind::Error,
            .priority = VisualPriority::Blocking,
            .message = GenericErrorMessage(),
        };
    }

    switch( _snapshot.state.load_phase ) {
        case PaneLoadPhase::Empty:
            return {};
        case PaneLoadPhase::Loading:
            return PrimaryVisualState{.kind = PaneVisualKind::Loading, .priority = VisualPriority::Activity};
        case PaneLoadPhase::Refreshing:
            return PrimaryVisualState{.kind = PaneVisualKind::Refreshing, .priority = VisualPriority::Activity};
        case PaneLoadPhase::Loaded:
            if( _snapshot.state.item_count == 0 )
                return PrimaryVisualState{.kind = PaneVisualKind::EmptyFolder};
            return PrimaryVisualState{
                .kind = PaneVisualKind::Ready,
                .priority = _snapshot.state.selected_count > 0 ? VisualPriority::Selection : VisualPriority::Normal,
            };
        case PaneLoadPhase::Failed:
            break;
    }
    return {};
}

BreadcrumbVisualState MapBreadcrumb(const PaneSnapshot &_snapshot, const PaneVisualKind _primary_kind)
{
    const auto &state = _snapshot.state;
    const bool has_location = state.is_uniform && state.host != nullptr && state.host != VFSHost::DummyHost() &&
                              !state.path.empty();
    const bool has_multiple_locations = !state.is_uniform &&
                                        (state.load_phase == PaneLoadPhase::Loaded ||
                                         state.load_phase == PaneLoadPhase::Refreshing);

    BreadcrumbVisualState result;
    if( has_location )
        result.kind = BreadcrumbVisualKind::Location;
    else if( has_multiple_locations )
        result.kind = BreadcrumbVisualKind::MultipleLocations;

    // A failed navigation retains the last committed location and must remain recoverable through
    // the address editor. Only an initial/unavailable pane and an in-flight location change lock it.
    result.editable = has_location && _snapshot.state.load_phase != PaneLoadPhase::Empty &&
                      _snapshot.state.load_phase != PaneLoadPhase::Loading;
    result.shows_activity = _primary_kind == PaneVisualKind::Loading || _primary_kind == PaneVisualKind::Refreshing;
    result.shows_error = IsPaneError(_primary_kind);
    return result;
}

PaneStatusVisualState MapStatus(const PaneSnapshot &_snapshot, const PrimaryVisualState &_primary)
{
    const auto copy_counts = [&_snapshot](PaneStatusVisualState &_result) {
        _result.item_count = _snapshot.state.item_count;
        _result.selected_count = _snapshot.state.selected_count;
        _result.selected_bytes = _snapshot.state.selected_bytes;
    };

    PaneStatusVisualState result;
    if( IsPaneError(_primary.kind) ) {
        result.kind = PaneStatusVisualKind::Error;
        result.message = _primary.message;
        return result;
    }

    switch( _primary.kind ) {
        case PaneVisualKind::Unavailable:
            break;
        case PaneVisualKind::Loading:
            result.kind = PaneStatusVisualKind::Loading;
            result.shows_activity = true;
            result.message = VisualMessage{
                .user_message_key = std::string{visual_state_messages::FolderLoadingKey},
                .user_message_fallback = std::string{visual_state_messages::FolderLoadingFallback},
            };
            break;
        case PaneVisualKind::Refreshing:
            result.kind = PaneStatusVisualKind::Counts;
            result.shows_activity = true;
            copy_counts(result);
            break;
        case PaneVisualKind::Ready:
            result.kind = PaneStatusVisualKind::Counts;
            copy_counts(result);
            break;
        case PaneVisualKind::EmptyFolder:
            result.kind = PaneStatusVisualKind::Empty;
            copy_counts(result);
            result.message = VisualMessage{
                .user_message_key = std::string{visual_state_messages::FolderEmptyKey},
                .user_message_fallback = std::string{visual_state_messages::FolderEmptyFallback},
            };
            break;
        case PaneVisualKind::PermissionBlocked:
        case PaneVisualKind::PathNotFound:
        case PaneVisualKind::ProviderUnavailable:
        case PaneVisualKind::Unsupported:
        case PaneVisualKind::Error:
            break;
    }
    return result;
}

} // namespace

PaneVisualState VisualStateMapper::MapPane(const PaneSnapshot &_snapshot,
                                           const FileManagerError *_current_navigation_error)
{
    const FileManagerError *const error = _current_navigation_error != nullptr
                                              ? _current_navigation_error
                                              : _snapshot.state.visible_error
                                                    ? std::addressof(*_snapshot.state.visible_error)
                                                    : nullptr;
    const PrimaryVisualState primary = ResolvePrimary(_snapshot, error);

    PaneVisualState result;
    result.kind = primary.kind;
    result.priority = primary.priority;
    result.content_visible = primary.kind == PaneVisualKind::Ready || primary.kind == PaneVisualKind::EmptyFolder ||
                             primary.kind == PaneVisualKind::Refreshing ||
                             (IsPaneError(primary.kind) && _snapshot.state.listing != nullptr);
    result.breadcrumb = MapBreadcrumb(_snapshot, primary.kind);
    result.status = MapStatus(_snapshot, primary);

    if( error != nullptr && !IsBlockingSignal(*error) &&
        !(_snapshot.state.load_phase == PaneLoadPhase::Failed &&
          error->severity != FileManagerErrorSeverity::Info &&
          error->category != FileManagerErrorCategory::OperationCancelledError) ) {
        result.nonblocking_notice = MessageForError(*error);
        result.priority = std::max(result.priority, PriorityForSeverity(error->severity));
    }

    return result;
}

CommandVisualState VisualStateMapper::MapCommand(const CommandState &_state)
{
    if( !_state.visible )
        return CommandVisualState{
            .visible = false,
            .enabled = false,
            .check_state = _state.check_state,
        };
    if( _state.enabled )
        return CommandVisualState{.check_state = _state.check_state};

    CommandVisualState result{
        .visible = true,
        .enabled = false,
        .check_state = _state.check_state,
    };
    if( _state.disabled_reason ) {
        result.disabled_message = VisualMessage{
            .user_message_key = _state.disabled_reason->user_message_key,
            .user_message_fallback = {},
        };
        if( _state.disabled_reason->suggested_action )
            result.disabled_message->suggested_actions.emplace_back(*_state.disabled_reason->suggested_action);
    }
    return result;
}

} // namespace nc::core
