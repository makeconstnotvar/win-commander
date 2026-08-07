// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandRegistry.h"
#include <functional>

namespace nc::core {

enum class FilePasteAvailability {
    Available,
    PaneUnavailable,
    WindowUnavailable,
    DestinationUnavailable,
    DestinationReadOnly,
    ClipboardUnavailable,
    ClipboardBusy,
    ClipboardChanged,
    SourceUnavailable
};

enum class FileDeletionIntent {
    Trash,
    Permanent
};

enum class FileCreationIntent {
    Folder,
    File
};

enum class FileCreationAvailability {
    Available,
    PaneUnavailable,
    WindowUnavailable,
    Loading,
    DestinationUnavailable,
    DestinationReadOnly,
    ProviderUnsupported,
    StaleDestination,
    NameUnavailable
};

enum class PaneSelectionIntent {
    SelectAll,
    Invert
};

enum class PaneSelectionAvailability {
    Available,
    PaneUnavailable,
    Loading,
    ListingUnavailable,
    Empty
};

enum class ArchiveCreateAvailability {
    Available,
    PaneUnavailable,
    WindowUnavailable,
    Loading,
    SelectionUnavailable,
    ParentEntryUnsupported,
    SourceUnreadable,
    SourceNameCollision,
    DestinationUnavailable,
    DestinationReadOnly,
    ProviderUnsupported,
    StaleContext
};

enum class ArchiveExtractAvailability {
    Available,
    PaneUnavailable,
    WindowUnavailable,
    Loading,
    SelectionUnavailable,
    ParentEntryUnsupported,
    SourceUnsupported,
    SourceUnreadable,
    DestinationUnavailable,
    DestinationReadOnly,
    ProviderUnsupported,
    CaseSensitivityUnavailable,
    StaleContext
};

enum class FileDuplicateAvailability {
    Available,
    PaneUnavailable,
    WindowUnavailable,
    Loading,
    SelectionUnavailable,
    ParentEntryUnsupported,
    SourceUnreadable,
    DestinationUnavailable,
    DestinationReadOnly,
    ProviderUnsupported,
    NameUnavailable,
    StaleContext
};

enum class FileCopyPathAvailability {
    Available,
    PaneUnavailable,
    Loading,
    SelectionUnavailable,
    ParentEntryUnsupported,
    StaleContext,
    ClipboardUnavailable
};

enum class FileCalculateSizesAvailability {
    Available,
    PaneUnavailable,
    Loading,
    ListingUnavailable,
    SelectionUnavailable,
    ParentEntryUnsupported,
    DirectoryRequired,
    SourceUnreadable,
    Busy,
    StaleContext
};

enum class FileBatchRenameAvailability {
    Available,
    PaneUnavailable,
    WindowUnavailable,
    Loading,
    ListingUnavailable,
    SelectionUnavailable,
    ParentEntryUnsupported,
    ProviderUnavailable,
    MixedProviders,
    ProviderUnsupported,
    InvalidPlan,
    DestinationConflict,
    StaleContext
};

using FilePasteAvailabilityProvider = std::function<FilePasteAvailability(void *)>;
using FilePasteExecutor = std::function<FilePasteAvailability(void *, const void *)>;
using FileDeletionExecutor =
    std::function<bool(void *, std::span<const vfs::ListingItem>, FileDeletionIntent, const void *)>;
using FileCreationAvailabilityProvider =
    std::function<FileCreationAvailability(void *, FileCreationIntent)>;
using FileCreationExecutor =
    std::function<FileCreationAvailability(void *, FileCreationIntent, const void *)>;
using PaneSelectionAvailabilityProvider =
    std::function<PaneSelectionAvailability(void *, PaneSelectionIntent)>;
using PaneSelectionExecutor =
    std::function<PaneSelectionAvailability(void *, PaneSelectionIntent, const void *)>;
using ArchiveCreateAvailabilityProvider =
    std::function<ArchiveCreateAvailability(void *, std::span<const vfs::ListingItem>)>;
using ArchiveCreateExecutor =
    std::function<ArchiveCreateAvailability(void *, std::span<const vfs::ListingItem>, const void *)>;
using ArchiveExtractAvailabilityProvider =
    std::function<ArchiveExtractAvailability(void *, std::span<const vfs::ListingItem>)>;
using ArchiveExtractExecutor =
    std::function<ArchiveExtractAvailability(void *, std::span<const vfs::ListingItem>, const void *)>;
using FileDuplicateAvailabilityProvider =
    std::function<FileDuplicateAvailability(void *, std::span<const vfs::ListingItem>)>;
using FileDuplicateExecutor =
    std::function<FileDuplicateAvailability(void *, std::span<const vfs::ListingItem>, const void *)>;
using FileCopyPathAvailabilityProvider =
    std::function<FileCopyPathAvailability(void *, std::span<const vfs::ListingItem>)>;
using FileCopyPathExecutor =
    std::function<FileCopyPathAvailability(void *, std::span<const vfs::ListingItem>, const void *)>;
using FileCalculateSizesAvailabilityProvider =
    std::function<FileCalculateSizesAvailability(void *, std::span<const vfs::ListingItem>)>;
using FileCalculateSizesExecutor =
    std::function<FileCalculateSizesAvailability(void *, std::span<const vfs::ListingItem>, const void *)>;
using FileBatchRenameAvailabilityProvider =
    std::function<FileBatchRenameAvailability(void *, std::span<const vfs::ListingItem>)>;
using FileBatchRenameExecutor =
    std::function<FileBatchRenameAvailability(void *, std::span<const vfs::ListingItem>, const void *)>;

/**
 * Builds the file.paste definition. The composition layer supplies a synchronous pasteboard and
 * destination projection plus a live execution port over the established legacy Copying action.
 */
[[nodiscard]] CommandRegistry::Registration
MakeFilePasteCommand(FilePasteAvailabilityProvider _availability, FilePasteExecutor _executor);

/** Builds the reversible file.trash definition over the established legacy Deletion action. */
[[nodiscard]] CommandRegistry::Registration MakeFileTrashCommand(FileDeletionExecutor _executor);

/**
 * Builds the explicit permanent file.delete definition. Production execution presents the legacy
 * deletion review before an accepted choice can enqueue nc::ops::Deletion.
 */
[[nodiscard]] CommandRegistry::Registration MakeFileDeleteCommand(FileDeletionExecutor _executor);

/** Builds quick file.newFolder over the established legacy DirectoryCreation operation. */
[[nodiscard]] CommandRegistry::Registration
MakeFileNewFolderCommand(FileCreationAvailabilityProvider _availability, FileCreationExecutor _executor);

/** Builds quick file.newFile over the narrow legacy EmptyFileCreation operation. */
[[nodiscard]] CommandRegistry::Registration
MakeFileNewFileCommand(FileCreationAvailabilityProvider _availability, FileCreationExecutor _executor);

/** Builds pane-local selection commands over the current visible pane projection. */
[[nodiscard]] CommandRegistry::Registration
MakePaneSelectAllCommand(PaneSelectionAvailabilityProvider _availability, PaneSelectionExecutor _executor);
[[nodiscard]] CommandRegistry::Registration
MakePaneInvertSelectionCommand(PaneSelectionAvailabilityProvider _availability, PaneSelectionExecutor _executor);

/** Builds the primary archive.create command over the established legacy Compression operation. */
[[nodiscard]] CommandRegistry::Registration
MakeArchiveCreateCommand(ArchiveCreateAvailabilityProvider _availability, ArchiveCreateExecutor _executor);

/** Builds native Extract Here over typed archive acquisition and the established legacy Copying operation. */
[[nodiscard]] CommandRegistry::Registration
MakeArchiveExtractCommand(ArchiveExtractAvailabilityProvider _availability, ArchiveExtractExecutor _executor);

/** Builds file.duplicate over the established legacy Copying operation. */
[[nodiscard]] CommandRegistry::Registration
MakeFileDuplicateCommand(FileDuplicateAvailabilityProvider _availability, FileDuplicateExecutor _executor);

/** Builds the non-mutating file.copyPath clipboard command. */
[[nodiscard]] CommandRegistry::Registration
MakeFileCopyPathCommand(FileCopyPathAvailabilityProvider _availability, FileCopyPathExecutor _executor);

/** Builds the opt-in background file.calculateSizes command for exact current-listing directories. */
[[nodiscard]] CommandRegistry::Registration
MakeFileCalculateSizesCommand(FileCalculateSizesAvailabilityProvider _availability,
                              FileCalculateSizesExecutor _executor);

/** Builds the Queue 1 file.batchRename review surface over the established legacy operation. */
[[nodiscard]] CommandRegistry::Registration
MakeFileBatchRenameCommand(FileBatchRenameAvailabilityProvider _availability, FileBatchRenameExecutor _executor);

} // namespace nc::core
