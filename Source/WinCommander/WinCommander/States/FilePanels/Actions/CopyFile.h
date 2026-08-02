// Copyright (C) 2017-2024 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "DefaultAction.h"

#include <memory>
#include <string>

namespace nc {

namespace config {
class Config;
}
namespace ops {
class Operation;
struct CopyingOptions;
} // namespace ops
namespace vfs {
class Host;
class ListingItem;
} // namespace vfs
} // namespace nc

namespace nc::panel::actions {

namespace reviewed_copy_as {

enum class Selection : unsigned char {
    Legacy,
    Reviewed,
    Reject
};

/** Conservative policy boundary selecting the create-only reviewed CopyAs lifecycle. */
[[nodiscard]] Selection Select(const nc::vfs::ListingItem &_item,
                               const std::string &_destination,
                               const std::shared_ptr<nc::vfs::Host> &_destination_host,
                               const nc::ops::CopyingOptions &_options) noexcept;

} // namespace reviewed_copy_as

class CopyBase
{
public:
    CopyBase(nc::config::Config &_config);

protected:
    void AddDeselectorIfNeeded(nc::ops::Operation &_with_operation, PanelController *_to_target) const;
    [[nodiscard]] bool ShouldAutomaticallyDeselect() const;

private:
    nc::config::Config &m_Config;
};

class CopyTo final : public StateAction, CopyBase
{
public:
    CopyTo(nc::config::Config &_config);
    [[nodiscard]] bool Predicate(MainWindowFilePanelState *_target) const override;
    void Perform(MainWindowFilePanelState *_target, id _sender) const override;
};

class CopyAs final : public StateAction, CopyBase
{
public:
    CopyAs(nc::config::Config &_config);
    [[nodiscard]] bool Predicate(MainWindowFilePanelState *_target) const override;
    void Perform(MainWindowFilePanelState *_target, id _sender) const override;
};

class MoveTo final : public StateAction
{
public:
    [[nodiscard]] bool Predicate(MainWindowFilePanelState *_target) const override;
    void Perform(MainWindowFilePanelState *_target, id _sender) const override;
};

class MoveAs final : public StateAction
{
public:
    [[nodiscard]] bool Predicate(MainWindowFilePanelState *_target) const override;
    void Perform(MainWindowFilePanelState *_target, id _sender) const override;
};

} // namespace nc::panel::actions
