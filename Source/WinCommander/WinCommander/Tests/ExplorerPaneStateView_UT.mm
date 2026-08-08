// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/States/Explorer/NCExplorerPaneStateView.h>

using nc::core::PaneStatusVisualKind;
using nc::core::PaneVisualKind;
using nc::core::PaneVisualState;
using nc::core::VisualMessage;

@interface NCExplorerPaneStateView (ExplorerPaneStateViewTesting)
@property(nonatomic, readonly) PaneVisualKind renderedKindForTesting;
@property(nonatomic, readonly) BOOL skeletonVisibleForTesting;
@property(nonatomic, readonly) BOOL loadingIndicatorVisibleForTesting;
@property(nonatomic, readonly) BOOL iconVisibleForTesting;
@property(nonatomic, readonly) NSString *messageTextForTesting;
@property(nonatomic, readonly) NSColor *backgroundColorForTesting;
@end

namespace {

NCExplorerPaneStateView *View()
{
    return [[NCExplorerPaneStateView alloc] initWithFrame:NSMakeRect(0, 0, 720, 480)];
}

PaneVisualState State(const PaneVisualKind _kind, const bool _content_visible = false)
{
    PaneVisualState state;
    state.kind = _kind;
    state.content_visible = _content_visible;
    return state;
}

PaneVisualState ErrorState(const PaneVisualKind _kind, const char *_message, const bool _content_visible = false)
{
    PaneVisualState state = State(_kind, _content_visible);
    state.status.kind = PaneStatusVisualKind::Error;
    state.status.message = VisualMessage{
        .user_message_key = "test.error",
        .user_message_fallback = _message,
    };
    return state;
}

} // namespace

#define PREFIX "NCExplorerPaneStateView "

TEST_CASE(PREFIX "renders loading as one accessible state with a skeleton")
{
    NCExplorerPaneStateView *const view = View();
    PaneVisualState loading = State(PaneVisualKind::Loading);
    loading.status.kind = PaneStatusVisualKind::Loading;
    loading.status.message = VisualMessage{
        .user_message_key = "visualState.folder.loading",
        .user_message_fallback = "Loading fixture…",
    };

    [view updateWithVisualState:loading];

    CHECK_FALSE(view.hidden);
    CHECK(view.renderedKindForTesting == PaneVisualKind::Loading);
    CHECK(view.skeletonVisibleForTesting);
    CHECK(view.loadingIndicatorVisibleForTesting);
    CHECK_FALSE(view.iconVisibleForTesting);
    CHECK([view.messageTextForTesting isEqualToString:@"Loading fixture…"]);
    CHECK([view.accessibilityIdentifier isEqualToString:@"wincommander.explorer.paneState"]);
    CHECK([view.accessibilityRole isEqualToString:NSAccessibilityGroupRole]);
    CHECK([view.accessibilityValue isEqualToString:@"Loading fixture…"]);
    CHECK(view.isOpaque);
    CHECK([view.backgroundColorForTesting isEqual:NSColor.controlBackgroundColor]);
}

TEST_CASE(PREFIX "keeps Loading semantics while progressive content replaces the skeleton")
{
    NCExplorerPaneStateView *const view = View();
    PaneVisualState loading = State(PaneVisualKind::Loading, true);
    loading.status.kind = PaneStatusVisualKind::Loading;

    [view updateWithVisualState:loading];

    CHECK(view.hidden);
    CHECK(view.renderedKindForTesting == PaneVisualKind::Loading);
    CHECK_FALSE(view.skeletonVisibleForTesting);
    CHECK_FALSE(view.loadingIndicatorVisibleForTesting);
    CHECK_FALSE(view.iconVisibleForTesting);
}

TEST_CASE(PREFIX "keeps the neutral unavailable state hidden")
{
    NCExplorerPaneStateView *const view = View();

    [view updateWithVisualState:State(PaneVisualKind::Unavailable)];

    CHECK(view.hidden);
    CHECK(view.renderedKindForTesting == PaneVisualKind::Unavailable);
    CHECK(view.messageTextForTesting.length == 0);
    CHECK(view.skeletonVisibleForTesting == false);
    CHECK(view.iconVisibleForTesting == false);
}

TEST_CASE(PREFIX "renders empty and blocking kinds with stable icon and fallback text")
{
    NCExplorerPaneStateView *const view = View();

    struct Example {
        PaneVisualKind kind;
        NSString *message;
    };
    const Example examples[] = {
        {PaneVisualKind::EmptyFolder, @"Folder is empty."},
        {PaneVisualKind::PermissionBlocked, @"Permission denied."},
        {PaneVisualKind::PathNotFound, @"Folder was not found."},
        {PaneVisualKind::VolumeDisconnected, @"Drive is disconnected."},
        {PaneVisualKind::RemoteUnavailable, @"Remote location is unavailable."},
        {PaneVisualKind::Unsupported, @"This location is not supported."},
        {PaneVisualKind::Error, @"Unable to show this folder."},
    };

    for( const auto &example : examples ) {
        [view updateWithVisualState:State(example.kind, example.kind == PaneVisualKind::EmptyFolder)];
        INFO(static_cast<int>(example.kind));
        CHECK_FALSE(view.hidden);
        CHECK(view.renderedKindForTesting == example.kind);
        CHECK_FALSE(view.skeletonVisibleForTesting);
        CHECK_FALSE(view.loadingIndicatorVisibleForTesting);
        CHECK(view.iconVisibleForTesting);
        CHECK([view.messageTextForTesting isEqualToString:example.message]);
    }
}

TEST_CASE(PREFIX "preserves typed provider and error messages for VoiceOver")
{
    NCExplorerPaneStateView *const view = View();

    const auto disconnected = ErrorState(PaneVisualKind::VolumeDisconnected, "Network volume disconnected.");
    [view updateWithVisualState:disconnected];
    CHECK([view.messageTextForTesting isEqualToString:@"Network volume disconnected."]);
    CHECK([view.accessibilityValue isEqualToString:@"Network volume disconnected."]);

    const auto remote = ErrorState(PaneVisualKind::RemoteUnavailable, "Remote location is unavailable.");
    [view updateWithVisualState:remote];
    CHECK([view.messageTextForTesting isEqualToString:@"Remote location is unavailable."]);
    CHECK([view.accessibilityValue isEqualToString:@"Remote location is unavailable."]);

    const auto denied = ErrorState(PaneVisualKind::PermissionBlocked, "Full Disk Access is required.");
    [view updateWithVisualState:denied];
    CHECK([view.messageTextForTesting isEqualToString:@"Full Disk Access is required."]);
    CHECK([view.accessibilityValue isEqualToString:@"Full Disk Access is required."]);
}

TEST_CASE(PREFIX "uses the fallback when the localization key is absent")
{
    NCExplorerPaneStateView *const view = View();
    PaneVisualState state = State(PaneVisualKind::Error);
    state.status.kind = PaneStatusVisualKind::Error;
    state.status.message = VisualMessage{
        .user_message_key = "wincommander.tests.missing-pane-state-localization",
        .user_message_fallback = "Stable fallback.",
    };

    [view updateWithVisualState:state];

    CHECK_FALSE(view.hidden);
    CHECK([view.messageTextForTesting isEqualToString:@"Stable fallback."]);
    CHECK([view.accessibilityValue isEqualToString:@"Stable fallback."]);
}

TEST_CASE(PREFIX "does not cover a retained committed listing")
{
    NCExplorerPaneStateView *const view = View();

    [view updateWithVisualState:ErrorState(PaneVisualKind::Error, "Refresh failed.", true)];
    CHECK(view.hidden);
    CHECK(view.renderedKindForTesting == PaneVisualKind::Error);
    CHECK([view.messageTextForTesting isEqualToString:@"Refresh failed."]);

    [view updateWithVisualState:State(PaneVisualKind::Ready, true)];
    CHECK(view.hidden);

    [view updateWithVisualState:State(PaneVisualKind::Refreshing, true)];
    CHECK(view.hidden);

    [view updateWithVisualState:ErrorState(PaneVisualKind::PathNotFound, "Folder disappeared.")];
    CHECK_FALSE(view.hidden);
    CHECK([view.messageTextForTesting isEqualToString:@"Folder disappeared."]);
}
