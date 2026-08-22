// Copyright (C) 2017-2024 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "DefaultAction.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

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

/**
 * The same policy for one item landing in a destination *directory* under its own name - the shape a
 * `Copy To` produces, as opposed to `Copy As`, which names its destination exactly.
 *
 * The one deliberate difference is that this does not require the destination to be the item's own
 * directory. That restriction was never the provider's: `ConditionalCopyPathSupport` answers about a
 * source and a destination parent on the same volume, the Native transaction anchors the two
 * independently, and its own tests have always run with the source and the destination in different
 * directories. It was `CopyAs` declining to claim more than the shape it needed, and a `Copy To`
 * needs the other one.
 */
[[nodiscard]] Selection SelectIntoDirectory(const nc::vfs::ListingItem &_item,
                                            const std::string &_destination_directory,
                                            const std::shared_ptr<nc::vfs::Host> &_destination_host,
                                            const nc::ops::CopyingOptions &_options) noexcept;

/**
 * One answer for a whole selection, because one user action has to become one operation.
 *
 * A set of items cannot be split between the reviewed engine and the legacy one: that would show two
 * operations where the user asked for one, and give half the files a journal and the other half none.
 * So a single `Legacy` answer takes the whole set legacy. `Reject` outranks it and is returned even
 * when some other item is already legacy - an eligibility question the provider could not answer must
 * never be quietly downgraded into "copy it the old way", which is exactly the silent fallback the
 * single-item rule exists to prevent. An empty selection is nothing to review.
 */
[[nodiscard]] Selection SelectBatch(const std::vector<nc::vfs::ListingItem> &_items,
                                    const std::string &_destination_directory,
                                    const std::shared_ptr<nc::vfs::Host> &_destination_host,
                                    const nc::ops::CopyingOptions &_options) noexcept;

} // namespace reviewed_copy_as

namespace reviewed_move {

using Selection = reviewed_copy_as::Selection;

/**
 * The Move counterpart of `reviewed_copy_as::Select`, for the single-item exact-destination shape
 * `Move As` has.
 */
[[nodiscard]] Selection Select(const nc::vfs::ListingItem &_item,
                               const std::string &_destination,
                               const std::shared_ptr<nc::vfs::Host> &_destination_host,
                               const nc::ops::CopyingOptions &_options) noexcept;

/**
 * The Move counterpart of `reviewed_copy_as::SelectIntoDirectory`, for one item landing in a
 * destination directory under its own name - the shape `Move To` produces.
 */
[[nodiscard]] Selection SelectIntoDirectory(const nc::vfs::ListingItem &_item,
                                            const std::string &_destination_directory,
                                            const std::shared_ptr<nc::vfs::Host> &_destination_host,
                                            const nc::ops::CopyingOptions &_options) noexcept;

/** The Move counterpart of `reviewed_copy_as::SelectBatch`. */
[[nodiscard]] Selection SelectBatch(const std::vector<nc::vfs::ListingItem> &_items,
                                    const std::string &_destination_directory,
                                    const std::shared_ptr<nc::vfs::Host> &_destination_host,
                                    const nc::ops::CopyingOptions &_options) noexcept;

} // namespace reviewed_move

/**
 * The Delete counterpart of the reviewed submission machinery this file already builds for Copy and
 * Move - declared here, not in `Actions/Delete.h`, because it is defined in `CopyFile.mm` alongside
 * the description/presentation helpers it reuses (`ReviewedOperationNoun`, `PlanningBlockerDescription`,
 * `PresentDurableCopyOutcome`, ...), and duplicating those into `Delete.mm` instead would be the one
 * part of this producer actually worth not sharing. `_intent_is_current` is supplied by the caller: the
 * staleness check a Delete needs is `DeletionContextIsCurrent`, which belongs next to the legacy dialog
 * it was written for, in `Delete.mm`.
 */
void SubmitReviewedDelete(MainWindowFilePanelState *_target,
                          PanelController *_panel,
                          std::vector<nc::vfs::ListingItem> _items,
                          std::function<bool()> _intent_is_current,
                          std::function<void()> _refresh_panel);

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

class MoveBase
{
public:
    MoveBase(nc::config::Config &_config);

protected:
    void AddDeselectorIfNeeded(nc::ops::Operation &_with_operation, PanelController *_to_target) const;
    [[nodiscard]] bool ShouldAutomaticallyDeselect() const;

private:
    nc::config::Config &m_Config;
};

class MoveTo final : public StateAction, MoveBase
{
public:
    MoveTo(nc::config::Config &_config);
    [[nodiscard]] bool Predicate(MainWindowFilePanelState *_target) const override;
    void Perform(MainWindowFilePanelState *_target, id _sender) const override;
};

class MoveAs final : public StateAction, MoveBase
{
public:
    MoveAs(nc::config::Config &_config);
    [[nodiscard]] bool Predicate(MainWindowFilePanelState *_target) const override;
    void Perform(MainWindowFilePanelState *_target, id _sender) const override;
};

} // namespace nc::panel::actions
