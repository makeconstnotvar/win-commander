// Copyright (C) 2017-2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "DefaultAction.h"

@class PanelController;

namespace nc::vfs {
class Host;
}

namespace nc::panel::actions {

enum class QuickNewFolderSubmissionResult {
    Submitted,
    PaneUnavailable,
    WindowUnavailable,
    Loading,
    DestinationUnavailable,
    DestinationReadOnly,
    ProviderUnsupported,
    StaleDestination,
    NameUnavailable
};

enum class QuickNewFileSubmissionResult {
    Submitted,
    PaneUnavailable,
    WindowUnavailable,
    Loading,
    DestinationUnavailable,
    DestinationReadOnly,
    ProviderUnsupported,
    StaleDestination,
    NameUnavailable
};

/** Binds and submits the quick-create folder intent to one exact live destination. */
[[nodiscard]] QuickNewFolderSubmissionResult SubmitQuickNewFolder(PanelController *_target);

/**
 * App-local admission for the quick New File mutation. True only for provider implementations whose
 * OF_NoExist path is proven to publish atomically without replacing an existing file.
 */
[[nodiscard]] bool SupportsExclusiveQuickNewFile(const nc::vfs::Host &_host) noexcept;

/** Evaluates one exclusive empty-file creation against an exact live destination. */
[[nodiscard]] QuickNewFileSubmissionResult EvaluateQuickNewFileSubmission(PanelController *_target);
/** Revalidates and submits one admitted exclusive empty-file creation against that destination. */
[[nodiscard]] QuickNewFileSubmissionResult SubmitQuickNewFile(PanelController *_target);

struct MakeNewFile final : PanelAction {
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;
};

struct MakeNewFolder final : PanelAction {
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;
};

struct MakeNewNamedFolder final : PanelAction {
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;
};

struct MakeNewNamedFolderInOppositePanel final : PanelAction {
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;
};

struct MakeNewFolderWithSelection final : PanelAction {
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;
};

}; // namespace nc::panel::actions
