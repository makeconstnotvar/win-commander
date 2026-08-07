// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/Core/Commands/CommandRegistry.h>
#include <WinCommander/Core/VisualState/VisualStateMapper.h>

#include <VFS/Host.h>
#include <array>
#include <memory>

using namespace nc::core;

#define PREFIX "nc::core::VisualStateMapper "

namespace {

VFSHostPtr TestHost()
{
    return std::make_shared<VFSHost>("/", nullptr, "visual_state_test");
}

PaneSnapshot Pane(const PaneLoadPhase _phase, const int32_t _item_count = 3)
{
    PaneSnapshot snapshot;
    snapshot.pane_id = PaneId{7};
    snapshot.revision = 11;
    snapshot.state.load_phase = _phase;
    snapshot.state.is_uniform = true;
    snapshot.state.path = "/fixture/";
    snapshot.state.display_title = "Fixture";
    snapshot.state.host = TestHost();
    snapshot.state.item_count = _item_count;
    snapshot.state.selected_count = _item_count > 0 ? 1 : 0;
    snapshot.state.selected_bytes = _item_count > 0 ? 42 : 0;
    return snapshot;
}

FileManagerError Error(const FileManagerErrorCategory _category,
                       const FileManagerErrorSeverity _severity = FileManagerErrorSeverity::BlockingError)
{
    return FileManagerError{
        .category = _category,
        .severity = _severity,
        .user_message_key = "errors.test",
        .user_message = "User-facing failure",
        .technical_message = "sensitive diagnostic details",
        .suggested_actions = {CommandId{"file.open"}},
        .original_error = nc::Error{"VisualStateTest", 1},
    };
}

CommandRegistry::Registration DisabledRegistration()
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{"file.test"};
    descriptor.title_key = "commands.test.title";
    descriptor.description_key = "commands.test.description";

    CommandRegistry::Registration registration;
    registration.descriptor = std::move(descriptor);
    registration.state_provider = [](const CommandContext &) {
        CommandState state;
        state.enabled = false;
        return state;
    };
    registration.handler = [](const CommandContext &) {};
    return registration;
}

} // namespace

TEST_CASE(PREFIX "keeps unloaded and loaded empty states distinct")
{
    const PaneVisualState unavailable = VisualStateMapper::MapPane(Pane(PaneLoadPhase::Empty, 0));
    CHECK(unavailable.kind == PaneVisualKind::Unavailable);
    CHECK_FALSE(unavailable.content_visible);
    CHECK(unavailable.status.kind == PaneStatusVisualKind::Unavailable);

    const PaneVisualState empty_folder = VisualStateMapper::MapPane(Pane(PaneLoadPhase::Loaded, 0));
    CHECK(empty_folder.kind == PaneVisualKind::EmptyFolder);
    CHECK(empty_folder.content_visible);
    CHECK(empty_folder.breadcrumb.kind == BreadcrumbVisualKind::Location);
    CHECK(empty_folder.breadcrumb.editable);
    CHECK(empty_folder.status.kind == PaneStatusVisualKind::Empty);
    REQUIRE(empty_folder.status.message);
    CHECK(empty_folder.status.message->user_message_key == visual_state_messages::FolderEmptyKey);
}

TEST_CASE(PREFIX "projects loading and refreshing without discarding committed refresh content")
{
    const PaneVisualState loading = VisualStateMapper::MapPane(Pane(PaneLoadPhase::Loading));
    CHECK(loading.kind == PaneVisualKind::Loading);
    CHECK(loading.priority == VisualPriority::Activity);
    CHECK_FALSE(loading.content_visible);
    CHECK_FALSE(loading.breadcrumb.editable);
    CHECK(loading.breadcrumb.shows_activity);
    CHECK(loading.status.kind == PaneStatusVisualKind::Loading);
    CHECK(loading.status.shows_activity);

    PaneSnapshot refreshing_snapshot = Pane(PaneLoadPhase::Refreshing);
    const PaneVisualState malformed_refreshing = VisualStateMapper::MapPane(refreshing_snapshot);
    CHECK(malformed_refreshing.kind == PaneVisualKind::Loading);
    CHECK_FALSE(malformed_refreshing.content_visible);
    CHECK_FALSE(malformed_refreshing.breadcrumb.editable);
    CHECK(malformed_refreshing.breadcrumb.shows_activity);
    CHECK(malformed_refreshing.status.kind == PaneStatusVisualKind::Loading);

    refreshing_snapshot.state.listing = VFSListing::EmptyListing();
    const PaneVisualState refreshing = VisualStateMapper::MapPane(refreshing_snapshot);
    CHECK(refreshing.kind == PaneVisualKind::Refreshing);
    CHECK(refreshing.priority == VisualPriority::Activity);
    CHECK(refreshing.content_visible);
    CHECK(refreshing.breadcrumb.editable);
    CHECK(refreshing.breadcrumb.shows_activity);
    CHECK(refreshing.status.kind == PaneStatusVisualKind::Counts);
    CHECK(refreshing.status.shows_activity);
    CHECK(refreshing.status.item_count == 3);
    CHECK(refreshing.status.selected_count == 1);
    CHECK(refreshing.status.selected_bytes == 42);
}

TEST_CASE(PREFIX "projects ready counts, selection priority, and non-uniform locations")
{
    PaneSnapshot snapshot = Pane(PaneLoadPhase::Loaded);
    const PaneVisualState ready = VisualStateMapper::MapPane(snapshot);
    CHECK(ready.kind == PaneVisualKind::Ready);
    CHECK(ready.priority == VisualPriority::Selection);
    CHECK(ready.content_visible);
    CHECK(ready.status.kind == PaneStatusVisualKind::Counts);
    CHECK(ready.status.item_count == 3);
    CHECK(ready.status.selected_count == 1);
    CHECK(ready.status.selected_bytes == 42);

    snapshot.state.is_uniform = false;
    snapshot.state.host.reset();
    snapshot.state.path.clear();
    const PaneVisualState multiple_locations = VisualStateMapper::MapPane(snapshot);
    CHECK(multiple_locations.breadcrumb.kind == BreadcrumbVisualKind::MultipleLocations);
    CHECK_FALSE(multiple_locations.breadcrumb.editable);
}

TEST_CASE(PREFIX "maps blocking errors by category and lets them dominate loading")
{
    struct Case {
        FileManagerErrorCategory category;
        PaneVisualKind expected_kind;
    };
    constexpr auto cases = std::to_array<Case>({
        {FileManagerErrorCategory::PermissionError, PaneVisualKind::PermissionBlocked},
        {FileManagerErrorCategory::PathNotFoundError, PaneVisualKind::PathNotFound},
        {FileManagerErrorCategory::VolumeUnavailableError, PaneVisualKind::VolumeDisconnected},
        {FileManagerErrorCategory::NetworkError, PaneVisualKind::RemoteUnavailable},
        {FileManagerErrorCategory::TimeoutError, PaneVisualKind::RemoteUnavailable},
        {FileManagerErrorCategory::AuthenticationError, PaneVisualKind::RemoteUnavailable},
        {FileManagerErrorCategory::RateLimitError, PaneVisualKind::RemoteUnavailable},
        {FileManagerErrorCategory::ProviderUnsupportedError, PaneVisualKind::Unsupported},
        {FileManagerErrorCategory::ConflictError, PaneVisualKind::Error},
        {FileManagerErrorCategory::ValidationError, PaneVisualKind::Error},
        {FileManagerErrorCategory::InsufficientSpaceError, PaneVisualKind::Error},
        {FileManagerErrorCategory::FileBusyError, PaneVisualKind::Error},
        {FileManagerErrorCategory::ReadOnlyError, PaneVisualKind::Error},
        {FileManagerErrorCategory::ChecksumMismatchError, PaneVisualKind::Error},
        {FileManagerErrorCategory::PartialFailureError, PaneVisualKind::Error},
        {FileManagerErrorCategory::UnknownError, PaneVisualKind::Error},
    });

    const PaneSnapshot loading = Pane(PaneLoadPhase::Loading);
    for( const auto &test_case : cases ) {
        CAPTURE(test_case.category);
        const FileManagerError error = Error(test_case.category);
        const PaneVisualState state = VisualStateMapper::MapPane(loading, &error);
        CHECK(state.kind == test_case.expected_kind);
        CHECK(state.priority == VisualPriority::Blocking);
        CHECK_FALSE(state.content_visible);
        CHECK(state.breadcrumb.shows_error);
        CHECK_FALSE(state.breadcrumb.shows_activity);
        CHECK(state.status.kind == PaneStatusVisualKind::Error);
        REQUIRE(state.status.message);
        CHECK(state.status.message->user_message_key == "errors.test");
        CHECK(state.status.message->user_message_fallback == "User-facing failure");
        CHECK(state.status.message->suggested_actions == error.suggested_actions);
        CHECK(state.status.message->user_message_fallback != error.technical_message);
    }

    FileManagerError fatal = Error(FileManagerErrorCategory::UnknownError, FileManagerErrorSeverity::FatalError);
    CHECK(VisualStateMapper::MapPane(loading, &fatal).priority == VisualPriority::Critical);
}

TEST_CASE(PREFIX "uses an embedded failed error and gives an explicit request error precedence")
{
    PaneSnapshot failed = Pane(PaneLoadPhase::Failed);
    const FileManagerError embedded = Error(FileManagerErrorCategory::PathNotFoundError);
    failed.state.visible_error = embedded;
    failed.state.listing = VFSListing::EmptyListing();

    const PaneVisualState embedded_state = VisualStateMapper::MapPane(failed);
    CHECK(embedded_state.kind == PaneVisualKind::PathNotFound);
    CHECK(embedded_state.priority == VisualPriority::Blocking);
    CHECK(embedded_state.content_visible);
    CHECK(embedded_state.breadcrumb.kind == BreadcrumbVisualKind::Location);
    CHECK(embedded_state.breadcrumb.editable);
    CHECK(embedded_state.breadcrumb.shows_error);
    REQUIRE(embedded_state.status.message);
    CHECK(embedded_state.status.message->user_message_fallback == embedded.user_message);

    FileManagerError explicit_error = Error(FileManagerErrorCategory::PermissionError);
    explicit_error.user_message = "Explicit request failure";
    const PaneVisualState explicit_state = VisualStateMapper::MapPane(failed, &explicit_error);
    CHECK(explicit_state.kind == PaneVisualKind::PermissionBlocked);
    REQUIRE(explicit_state.status.message);
    CHECK(explicit_state.status.message->user_message_fallback == "Explicit request failure");
}

TEST_CASE(PREFIX "keeps recoverable errors as notices while preserving pane content")
{
    PaneSnapshot snapshot = Pane(PaneLoadPhase::Loaded);
    const FileManagerError warning =
        Error(FileManagerErrorCategory::NetworkError, FileManagerErrorSeverity::RecoverableError);
    snapshot.state.visible_error = warning;
    const PaneVisualState state = VisualStateMapper::MapPane(snapshot);

    CHECK(state.kind == PaneVisualKind::Ready);
    CHECK(state.priority == VisualPriority::Warning);
    CHECK(state.content_visible);
    CHECK(state.status.kind == PaneStatusVisualKind::Counts);
    REQUIRE(state.nonblocking_notice);
    CHECK(state.nonblocking_notice->user_message_key == "errors.test");
    CHECK(state.nonblocking_notice->suggested_actions == warning.suggested_actions);
}

TEST_CASE(PREFIX "provides a generic failed-state message and treats cancellation as nonblocking")
{
    const PaneVisualState failed = VisualStateMapper::MapPane(Pane(PaneLoadPhase::Failed));
    CHECK(failed.kind == PaneVisualKind::Error);
    CHECK(failed.priority == VisualPriority::Blocking);
    REQUIRE(failed.status.message);
    CHECK(failed.status.message->user_message_key == file_manager_error_messages::UnknownErrorKey);
    CHECK(failed.status.message->user_message_fallback == file_manager_error_messages::UnknownErrorFallback);

    const FileManagerError cancelled =
        Error(FileManagerErrorCategory::OperationCancelledError, FileManagerErrorSeverity::Info);
    const PaneVisualState cancelled_state = VisualStateMapper::MapPane(Pane(PaneLoadPhase::Failed), &cancelled);
    CHECK(cancelled_state.kind == PaneVisualKind::Unavailable);
    CHECK(cancelled_state.priority == VisualPriority::Normal);
    CHECK_FALSE(cancelled_state.breadcrumb.shows_error);
    REQUIRE(cancelled_state.nonblocking_notice);

    const FileManagerError info = Error(FileManagerErrorCategory::UnknownError, FileManagerErrorSeverity::Info);
    const PaneVisualState info_state = VisualStateMapper::MapPane(Pane(PaneLoadPhase::Failed), &info);
    CHECK(info_state.kind == PaneVisualKind::Unavailable);
    CHECK(info_state.priority == VisualPriority::Normal);
    REQUIRE(info_state.nonblocking_notice);
}

TEST_CASE(PREFIX "projects normalized command state without diagnostics")
{
    CHECK(VisualStateMapper::MapCommand(CommandState{}) == CommandVisualState{});

    CommandState hidden;
    hidden.visible = false;
    hidden.enabled = true;
    hidden.check_state = CommandCheckState::On;
    hidden.disabled_reason = DisabledReason{
        .code = "hidden",
        .user_message_key = "commands.hidden",
        .technical_message = "must not escape",
    };
    const CommandVisualState hidden_visual = VisualStateMapper::MapCommand(hidden);
    CHECK_FALSE(hidden_visual.visible);
    CHECK_FALSE(hidden_visual.enabled);
    CHECK(hidden_visual.check_state == CommandCheckState::On);
    CHECK_FALSE(hidden_visual.disabled_message);

    CommandState disabled;
    disabled.enabled = false;
    disabled.check_state = CommandCheckState::Mixed;
    disabled.disabled_reason = DisabledReason{
        .code = "selection.empty",
        .user_message_key = "commands.disabled.selectionEmpty",
        .technical_message = "selection count was zero",
        .suggested_action = CommandId{"file.open"},
    };
    const CommandVisualState disabled_visual = VisualStateMapper::MapCommand(disabled);
    CHECK(disabled_visual.visible);
    CHECK_FALSE(disabled_visual.enabled);
    CHECK(disabled_visual.check_state == CommandCheckState::Mixed);
    REQUIRE(disabled_visual.disabled_message);
    CHECK(disabled_visual.disabled_message->user_message_key == "commands.disabled.selectionEmpty");
    CHECK(disabled_visual.disabled_message->user_message_fallback.empty());
    CHECK(disabled_visual.disabled_message->suggested_actions ==
          std::vector<CommandId>{CommandId{"file.open"}});

    CommandRegistry registry;
    REQUIRE(registry.Register(DisabledRegistration()) == CommandRegistry::RegisterResult::Registered);
    const auto normalized = registry.QueryState(CommandId{"file.test"}, {});
    REQUIRE(normalized.state.disabled_reason);
    const CommandVisualState fallback = VisualStateMapper::MapCommand(normalized.state);
    REQUIRE(fallback.disabled_message);
    CHECK(fallback.disabled_message->user_message_key == "commands.disabled.generic");

    const auto unknown = registry.QueryState(CommandId{"file.unknown"}, {});
    CHECK(VisualStateMapper::MapCommand(unknown.state) ==
          CommandVisualState{.visible = false, .enabled = false});
}

TEST_CASE(PREFIX "preserves command check state for every visibility and enablement combination")
{
    for( const bool visible : {false, true} ) {
        for( const bool enabled : {false, true} ) {
            for( const CommandCheckState check_state :
                 {CommandCheckState::Off, CommandCheckState::On, CommandCheckState::Mixed} ) {
                CommandState state;
                state.visible = visible;
                state.enabled = enabled;
                state.check_state = check_state;

                const CommandVisualState visual = VisualStateMapper::MapCommand(state);
                CHECK(visual.visible == visible);
                CHECK(visual.enabled == (visible && enabled));
                CHECK(visual.check_state == check_state);
            }
        }
    }
}

#undef PREFIX
