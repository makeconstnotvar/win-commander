// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <string_view>

namespace nc::core::command_ids {

inline constexpr std::string_view FileOpen = "file.open";
inline constexpr std::string_view FilePreview = "file.preview";
inline constexpr std::string_view FileGetInfo = "file.getInfo";
inline constexpr std::string_view NavigationBack = "navigation.back";
inline constexpr std::string_view NavigationForward = "navigation.forward";
inline constexpr std::string_view NavigationUp = "navigation.up";
inline constexpr std::string_view NavigationRefresh = "navigation.refresh";
inline constexpr std::string_view FileCopy = "file.copy";
inline constexpr std::string_view FileCut = "file.cut";
inline constexpr std::string_view FilePaste = "file.paste";
inline constexpr std::string_view FileNewFolder = "file.newFolder";
inline constexpr std::string_view FileNewFile = "file.newFile";
inline constexpr std::string_view FileDuplicate = "file.duplicate";
inline constexpr std::string_view FileCopyPath = "file.copyPath";
inline constexpr std::string_view FileCalculateSizes = "file.calculateSizes";
inline constexpr std::string_view FileBatchRename = "file.batchRename";
inline constexpr std::string_view FileRename = "file.rename";
inline constexpr std::string_view FileTrash = "file.trash";
inline constexpr std::string_view FileDelete = "file.delete";
inline constexpr std::string_view PaneSelectAll = "pane.selectAll";
inline constexpr std::string_view PaneInvertSelection = "pane.invertSelection";
inline constexpr std::string_view ViewToggleHiddenFiles = "view.toggleHiddenFiles";
inline constexpr std::string_view ViewTogglePreviewPane = "view.togglePreviewPane";
inline constexpr std::string_view OperationCancel = "operation.cancel";
inline constexpr std::string_view OperationCenterOpen = "operationCenter.open";
inline constexpr std::string_view ArchiveCreate = "archive.create";
inline constexpr std::string_view ArchiveExtract = "archive.extract";

} // namespace nc::core::command_ids
