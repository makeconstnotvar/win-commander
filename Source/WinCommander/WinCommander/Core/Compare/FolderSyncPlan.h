// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "FolderComparison.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nc::core {

/** Which side feeds the other. The destination is the only side a sync ever mutates. */
enum class FolderSyncDirection : uint8_t {
    LeftToRight,
    RightToLeft
};

enum class FolderSyncActionKind : uint8_t {
    /** The destination has no such name; the source entry is copied over as-is. */
    Create,
    /** Both sides have the name and the values differ; the destination copy is replaced. */
    Overwrite,
    /** Only the destination has the name, and removing extraneous entries was requested. */
    Delete,
    /** Nothing will happen to this name. `skip_reason` says why. */
    Skip
};

enum class FolderSyncSkipReason : uint8_t {
    None,
    /** Already identical under the comparison criteria. */
    Identical,
    /** A directory on one side faces a file on the other; no direction is safe. */
    TypeConflict,
    /**
     * The name is a directory on both sides. CompareFolders judges such a pair by presence only, so
     * this plan cannot claim its contents match - a recursive comparison must supply that.
     */
    DirectoryContentsNotCompared,
    /** Present only on the destination, and removing extraneous entries was not requested. */
    DeletionNotRequested,
    /** The values differ, but replacing changed destination entries was not requested. */
    OverwriteNotRequested
};

struct FolderSyncOptions {
    /** Replace a destination entry whose value differs from the source. */
    bool overwrite_changed = true;
    /**
     * Remove destination entries the source does not have. Off by default: this is the one
     * genuinely destructive part of a one-way sync, and the spec requires it be previewed and
     * chosen, never assumed.
     */
    bool delete_extraneous = false;
};

struct FolderSyncAction {
    std::string name;
    FolderSyncActionKind kind = FolderSyncActionKind::Skip;
    FolderSyncSkipReason skip_reason = FolderSyncSkipReason::None;
    bool is_directory = false;
    /** Index into the source side's listing, when this action reads one. */
    std::optional<size_t> source_index;
    /** Index into the destination side's listing, when this action touches one. */
    std::optional<size_t> destination_index;
    /**
     * True when an Overwrite would replace a destination copy that is *newer* than the source. The
     * sync is still one-way and still proceeds, but this is the case a preview must call out - the
     * user is about to lose the more recent of the two.
     */
    bool overwrites_newer_destination = false;

    friend bool operator==(const FolderSyncAction &, const FolderSyncAction &) = default;
};

struct FolderSyncSummary {
    size_t create = 0;
    size_t overwrite = 0;
    size_t remove = 0;
    size_t skip = 0;
    /** Subset of `overwrite` that would replace a newer destination copy. */
    size_t overwrite_newer_destination = 0;

    friend bool operator==(const FolderSyncSummary &, const FolderSyncSummary &) = default;
};

/**
 * A dry run. Building the plan never touches a filesystem, so this value *is* the preview the spec
 * requires before any destructive sync (§15 "sync deletion always previewed in dry-run", §45
 * "dry-run is mandatory before destructive sync"). Execution consumes this plan; it must not
 * re-derive its own.
 */
struct FolderSyncPlan {
    FolderSyncDirection direction = FolderSyncDirection::LeftToRight;
    /** Comparison order, so a preview lists actions next to the rows the user just compared. */
    std::vector<FolderSyncAction> actions;

    [[nodiscard]] FolderSyncSummary Summarize() const noexcept;
    /** True when the plan would remove anything - the gate a destructive-confirmation step reads. */
    [[nodiscard]] bool HasDeletions() const noexcept;
    /** True when the plan would change nothing on the destination. */
    [[nodiscard]] bool IsEmpty() const noexcept;
    /** Every action that would remove a destination entry, for the deletion preview. */
    [[nodiscard]] std::vector<const FolderSyncAction *> Deletions() const;
};

/**
 * Projects a comparison onto one direction. Total: every comparison entry produces exactly one
 * action, so a preview can never omit a name the comparison saw. Nothing is silently resolved -
 * a pair this level cannot decide becomes a Skip carrying its reason rather than a guess.
 */
[[nodiscard]] FolderSyncPlan
PlanOneWaySync(const FolderComparison &_comparison, FolderSyncDirection _direction, const FolderSyncOptions &_options = {});

/** Listing positions a plan resolves to, ready to be turned into concrete items for submission. */
struct FolderSyncSubmission {
    /** Source-side positions to copy over the destination (the Create and Overwrite actions). */
    std::vector<size_t> copy_source_indices;
    /** Destination-side positions to remove (the Delete actions). */
    std::vector<size_t> delete_destination_indices;

    friend bool operator==(const FolderSyncSubmission &, const FolderSyncSubmission &) = default;
};

/**
 * The gate between a reviewed plan and execution.
 *
 * A plan is built from listings sampled at compare time, but the user reviews it before anything
 * runs, and either side can change underneath in between. Binding therefore re-reads the names
 * currently at every position the plan references and requires each to still be the name the plan
 * decided about. Anything else - a position now out of range, or a different name at it - means the
 * reviewed plan no longer describes reality, so binding fails **atomically** rather than submitting
 * the subset that still matches. Submitting a partial plan would mutate files the user never
 * reviewed, which is precisely the failure this exists to prevent.
 *
 * Names must be supplied in the same index space the comparison was built from.
 */
[[nodiscard]] std::optional<FolderSyncSubmission> BindSyncPlan(const FolderSyncPlan &_plan,
                                                               std::span<const std::string> _source_names,
                                                               std::span<const std::string> _destination_names);

} // namespace nc::core
