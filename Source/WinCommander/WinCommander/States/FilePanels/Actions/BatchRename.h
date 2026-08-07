// Copyright (C) 2017 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "DefaultAction.h"
#include <VFS/VFS.h>
#include <cstdint>
#include <span>

@class PanelController;

namespace nc::panel::actions {

enum class BatchRenameSubmissionResult : uint8_t {
    Presented,
    PaneUnavailable,
    WindowUnavailable,
    Loading,
    ListingUnavailable,
    SelectionUnavailable,
    ParentEntryUnsupported,
    MixedProviders,
    ProviderUnsupported,
    StaleContext,
    InvalidPlan,
    DestinationConflict
};

[[nodiscard]] BatchRenameSubmissionResult
EvaluateBatchRenameSubmission(std::span<const VFSListingItem> _items, PanelController *_target);

[[nodiscard]] BatchRenameSubmissionResult
PresentBatchRename(std::span<const VFSListingItem> _items, PanelController *_target);

struct BatchRename final : PanelAction {
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;
};

} // namespace nc::panel::actions
