// Copyright (C) 2017 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "DefaultAction.h"
#include <VFS/VFS.h>

#include <span>

@class PanelController;

namespace nc::panel::actions {

enum class CalculateSizesSubmissionResult {
    Submitted,
    PaneUnavailable,
    Loading,
    ListingUnavailable,
    SelectionUnavailable,
    ParentEntryUnsupported,
    StaleContext,
    NoDirectories,
    SourceUnreadable,
    CalculationBusy
};

[[nodiscard]] CalculateSizesSubmissionResult
EvaluateCalculateSizesSubmission(std::span<const VFSListingItem> _items, PanelController *_target);
[[nodiscard]] CalculateSizesSubmissionResult
SubmitCalculateSizes(std::span<const VFSListingItem> _items, PanelController *_target);

struct CalculateSizes final : PanelAction {
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;
};

struct CalculateAllSizes final : PanelAction {
    void Perform(PanelController *_target, id _sender) const override;
};

} // namespace nc::panel::actions
