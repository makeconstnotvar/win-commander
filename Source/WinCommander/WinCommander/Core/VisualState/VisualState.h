// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <WinCommander/Core/Commands/Command.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nc::core {

namespace visual_state_messages {

inline constexpr std::string_view FolderLoadingKey = "visualState.folder.loading";
inline constexpr std::string_view FolderLoadingFallback = "Loading...";
inline constexpr std::string_view FolderEmptyKey = "visualState.folder.empty";
inline constexpr std::string_view FolderEmptyFallback = "Folder is empty.";

} // namespace visual_state_messages

enum class PaneVisualKind : uint8_t {
    Unavailable,
    Loading,
    Refreshing,
    Ready,
    EmptyFolder,
    PermissionBlocked,
    PathNotFound,
    ProviderUnavailable,
    Unsupported,
    Error
};

/** Ordered from the least to the most visually significant pane signal. */
enum class VisualPriority : uint8_t {
    Normal,
    Selection,
    Warning,
    Activity,
    Blocking,
    Critical
};

struct VisualMessage {
    std::string user_message_key;
    std::string user_message_fallback;
    std::vector<CommandId> suggested_actions;

    bool operator==(const VisualMessage &) const = default;
};

enum class BreadcrumbVisualKind : uint8_t {
    Unavailable,
    Location,
    MultipleLocations
};

struct BreadcrumbVisualState {
    BreadcrumbVisualKind kind = BreadcrumbVisualKind::Unavailable;
    bool editable = false;
    bool shows_activity = false;
    bool shows_error = false;

    bool operator==(const BreadcrumbVisualState &) const = default;
};

enum class PaneStatusVisualKind : uint8_t {
    Unavailable,
    Counts,
    Empty,
    Loading,
    Error
};

struct PaneStatusVisualState {
    PaneStatusVisualKind kind = PaneStatusVisualKind::Unavailable;
    int32_t item_count = 0;
    int32_t selected_count = 0;
    int64_t selected_bytes = 0;
    bool shows_activity = false;
    std::optional<VisualMessage> message;

    bool operator==(const PaneStatusVisualState &) const = default;
};

struct PaneVisualState {
    PaneVisualKind kind = PaneVisualKind::Unavailable;
    VisualPriority priority = VisualPriority::Normal;
    bool content_visible = false;
    BreadcrumbVisualState breadcrumb;
    PaneStatusVisualState status;
    std::optional<VisualMessage> nonblocking_notice;

    bool operator==(const PaneVisualState &) const = default;
};

struct CommandVisualState {
    bool visible = true;
    bool enabled = true;
    CommandCheckState check_state = CommandCheckState::Off;
    std::optional<VisualMessage> disabled_message;

    bool operator==(const CommandVisualState &) const = default;
};

} // namespace nc::core
