// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CommandRegistry.h"
#include "../Metadata/FileMetadataSnapshot.h"

#include <functional>
#include <vector>

namespace nc::core {

/** Copied read-only payload supplied to the Properties presenter. */
struct FileGetInfoPresentation final {
    CommandInvocationSource source = CommandInvocationSource::Programmatic;
    std::vector<FileMetadataSnapshot> items;

    bool operator==(const FileGetInfoPresentation &) const noexcept = default;
};

/**
 * Presents one copied Properties value payload through a borrowed synchronous admitted pane target.
 * The target is valid only for the call and must not be retained. Returns whether the live target
 * accepted the presentation after revalidating its pane authority.
 */
using FileGetInfoPresenter = std::function<bool(void *native_target, FileGetInfoPresentation presentation)>;

/**
 * Builds the pure read-only file.getInfo Registry definition.
 *
 * State requires a live admitted pane target and a nonempty exact item payload without the
 * synthetic parent entry. Execution copies listing metadata into an authority-free value snapshot
 * before invoking the presenter. Provider readability and uniformity do not constrain already
 * available listing metadata.
 */
[[nodiscard]] CommandRegistry::Registration MakeFileGetInfoCommand(FileGetInfoPresenter _presenter);

} // namespace nc::core
