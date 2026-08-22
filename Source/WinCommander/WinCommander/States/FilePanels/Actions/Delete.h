// Copyright (C) 2017-2020 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/VFS.h>
#include "DefaultAction.h"
#include <span>
#include <vector>

namespace nc::utility {
class NativeFSManager;
}

namespace nc::panel::actions {

/** Exact-item Registry execution ports over the established nc::ops::Deletion implementation. */
[[nodiscard]] bool SubmitItemsToTrash(std::span<const VFSListingItem> _items, PanelController *_target);
[[nodiscard]] bool PresentPermanentDeletion(std::span<const VFSListingItem> _items,
                                            PanelController *_target);

namespace reviewed_delete {

/**
 * A fresh three-value enum rather than a reuse of `reviewed_copy_as::Selection`: the two are the same
 * three words by coincidence of vocabulary, not by any relationship one policy has to the other, and
 * reusing it would tie a Delete's own eligibility answer to a header this file has no other reason to
 * include.
 */
enum class Selection : unsigned char {
    Legacy,
    Reviewed,
    Reject
};

/**
 * Conservative policy boundary selecting the reviewed permanent-Delete lifecycle. Unlike
 * `reviewed_copy_as::Select`/`reviewed_move::Select`, there is no destination to ask about at all - a
 * Delete plan has none, structurally - so the only question is whether the item itself and its
 * provider are eligible.
 */
[[nodiscard]] Selection Select(const VFSListingItem &_item) noexcept;

/**
 * One answer for a whole selection, for the same reason `reviewed_copy_as::SelectBatch` exists: a set
 * of items cannot be split between the reviewed engine and the legacy one, so a single `Legacy` answer
 * takes the whole set legacy, and `Reject` outranks it even when found after some other item already
 * answered legacy - an eligibility question the provider could not answer must never be quietly
 * downgraded into "delete it the old way". An empty selection is nothing to review.
 */
[[nodiscard]] Selection SelectBatch(const std::vector<VFSListingItem> &_items) noexcept;

} // namespace reviewed_delete

struct Delete final : PanelAction {
    Delete(nc::utility::NativeFSManager &_nat_fsman, bool _permanently = false);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;

private:
    nc::utility::NativeFSManager &m_NativeFSManager;
    bool m_Permanently;
};

struct MoveToTrash final : PanelAction {
    MoveToTrash(nc::utility::NativeFSManager &_nat_fsman);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;

private:
    nc::utility::NativeFSManager &m_NativeFSManager;
};

namespace context {

struct DeletePermanently final : PanelAction {
    DeletePermanently(const std::vector<VFSListingItem> &_items);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;

private:
    const std::vector<VFSListingItem> &m_Items;
    bool m_AllWriteable;
};

struct MoveToTrash final : PanelAction {
    MoveToTrash(const std::vector<VFSListingItem> &_items);
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;

private:
    const std::vector<VFSListingItem> &m_Items;
    bool m_AllAreNative;
};

} // namespace context

} // namespace nc::panel::actions
