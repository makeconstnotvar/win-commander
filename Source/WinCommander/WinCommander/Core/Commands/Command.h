// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandId.h"
#include "../Pane/PaneNavigationAvailability.h"
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nc::vfs {
class ListingItem;
}

namespace nc::core {

enum class CommandCategory {
    Navigation,
    Pane,
    File,
    Edit,
    View,
    Search,
    Operation,
    Archive,
    Remote,
    Sync,
    Developer,
    Settings,
    Window,
    Help
};

enum class CommandInvocationSource {
    Menu,
    Toolbar,
    ContextMenu,
    Shortcut,
    Palette,
    Programmatic
};

enum class CommandCheckState {
    Off,
    On,
    Mixed
};

struct DisabledReason {
    std::string code;
    std::string user_message_key;
    std::string technical_message;
    std::optional<CommandId> suggested_action;

    friend bool operator==(const DisabledReason &, const DisabledReason &) = default;
};

struct CommandState {
    bool visible = true;
    bool enabled = true;
    std::optional<DisabledReason> disabled_reason;
    std::optional<std::string> title_key_override;
    CommandCheckState check_state = CommandCheckState::Off;
};

struct CommandContext {
    CommandInvocationSource source = CommandInvocationSource::Programmatic;

    // Borrowed migration bridge. It is valid only for the duration of a synchronous Execute() call.
    const void *native_sender = nullptr;

    // Borrowed target bridge for commands that synchronously initiate UI owned by a pane.
    // It is valid only for the duration of QueryState()/Execute() and must never be retained.
    void *native_target = nullptr;

    // Borrowed synchronous snapshot of the pane's hidden-files visibility. Callers refresh it for
    // every QueryState()/Execute() invocation; handlers must not treat it as durable pane state.
    std::optional<bool> shows_hidden_files;

    // Borrowed synchronous history-availability projection. The execution port must still validate
    // the live pane history immediately before moving because this snapshot can become stale.
    std::optional<bool> can_go_back;
    std::optional<bool> can_go_forward;

    // Borrowed synchronous projections for pane-local Up and Refresh presentation. Their narrow
    // execution ports must still validate live controller state immediately before acting.
    std::optional<NavigationUpAvailability> navigation_up_availability;
    std::optional<NavigationRefreshAvailability> navigation_refresh_availability;

    // Typed borrowed file-item context. The caller keeps the items alive for the duration of the
    // synchronous QueryState()/Execute() call; handlers must not retain this span.
    std::span<const vfs::ListingItem> items;
};

struct LegacyCommandMetadata {
    std::optional<std::string> selector_name;
    std::vector<std::string> shortcut_action_names;
    std::optional<int> shortcut_tag;
};

struct CommandDescriptor {
    CommandId id;
    std::string title_key;
    std::string description_key;
    CommandCategory category = CommandCategory::File;
    std::string icon_name;
    bool is_destructive = false;
    bool requires_operation_plan = false;
    bool supports_undo = false;
    std::string analytics_name;
    std::optional<LegacyCommandMetadata> legacy;
};

} // namespace nc::core
