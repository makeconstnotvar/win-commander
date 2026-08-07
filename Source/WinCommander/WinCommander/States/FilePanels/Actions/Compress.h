// Copyright (C) 2017-2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/VFS.h>
#include "DefaultAction.h"

namespace nc::config {
class Config;
}

namespace nc::ops {
class Operation;
}

namespace nc::panel::actions {

enum class ArchiveCreateSubmissionResult {
    Presented,
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

[[nodiscard]] ArchiveCreateSubmissionResult
EvaluateArchiveCreateSubmission(std::span<const VFSListingItem> _items, PanelController *_target);
[[nodiscard]] ArchiveCreateSubmissionResult PresentArchiveCreate(std::span<const VFSListingItem> _items,
                                                                PanelController *_target,
                                                                nc::config::Config &_config);

class CompressBase
{
public:
    CompressBase(nc::config::Config &_config);
    void AddDeselectorIfNeeded(nc::ops::Operation &_with_operation, PanelController *_to_target) const;
    [[nodiscard]] nc::config::Config &Config() const noexcept { return m_Config; }

private:
    [[nodiscard]] bool ShouldAutomaticallyDeselect() const;

    nc::config::Config &m_Config;
};

struct CompressHere final : PanelAction, CompressBase {
    CompressHere(nc::config::Config &_config);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;
};

struct CompressToOpposite final : PanelAction, CompressBase {
    CompressToOpposite(nc::config::Config &_config);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;
};

namespace context {

struct CompressHere final : PanelAction, CompressBase {
    CompressHere(nc::config::Config &_config, const std::vector<VFSListingItem> &_items);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    [[nodiscard]] bool ValidateMenuItem(PanelController *_target, NSMenuItem *_item) const override;
    void Perform(PanelController *_target, id _sender) const override;

private:
    const std::vector<VFSListingItem> &m_Items;
};

struct CompressToOpposite final : PanelAction, CompressBase {
    CompressToOpposite(nc::config::Config &_config, const std::vector<VFSListingItem> &_items);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    [[nodiscard]] bool ValidateMenuItem(PanelController *_target, NSMenuItem *_item) const override;
    void Perform(PanelController *_target, id _sender) const override;

private:
    const std::vector<VFSListingItem> &m_Items;
};

} // namespace context

} // namespace nc::panel::actions
