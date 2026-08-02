// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <string_view>

namespace nc::core::command_ids {

inline constexpr std::string_view FileOpen = "file.open";
inline constexpr std::string_view NavigationBack = "navigation.back";
inline constexpr std::string_view NavigationForward = "navigation.forward";
inline constexpr std::string_view NavigationUp = "navigation.up";
inline constexpr std::string_view NavigationRefresh = "navigation.refresh";
inline constexpr std::string_view FileCopy = "file.copy";
inline constexpr std::string_view FileCut = "file.cut";
inline constexpr std::string_view FileRename = "file.rename";
inline constexpr std::string_view ViewToggleHiddenFiles = "view.toggleHiddenFiles";
inline constexpr std::string_view OperationCancel = "operation.cancel";
inline constexpr std::string_view OperationCenterOpen = "operationCenter.open";

} // namespace nc::core::command_ids
