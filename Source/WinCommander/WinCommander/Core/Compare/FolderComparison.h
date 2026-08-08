// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nc::core {

/** Per-name outcome of comparing two folder listings at one directory level. */
enum class FolderCompareStatus : uint8_t {
    Same,
    LeftOnly,
    RightOnly,
    Changed,
    /** One side holds a directory and the other a file under the same name. */
    Conflict
};

/** Which side of a matched pair carries the newer modification time. */
enum class FolderCompareNewerSide : uint8_t {
    Neither,
    Left,
    Right
};

enum class FolderCompareFailure : uint8_t {
    /** A listing cannot contain the same name twice; matching would be ambiguous. */
    DuplicateName,
    EmptyName,
    /** "." and ".." are navigation entries, never comparison subjects. */
    ReservedName,
    NegativeTolerance
};

/** One listing entry reduced to exactly what the comparison judges. */
struct FolderCompareItem {
    std::string name;
    uint64_t size = 0;
    /** Whole seconds since the epoch. Compared only when the criteria ask for it. */
    int64_t modification_time = 0;
    bool is_directory = false;

    friend bool operator==(const FolderCompareItem &, const FolderCompareItem &) = default;
};

/**
 * P0 comparison criteria (spec §24.1: by name/size/date). Disabling both value criteria makes every
 * matched pair Same, which is the exact "compare by presence only" mode.
 */
struct FolderCompareOptions {
    bool compare_size = true;
    bool compare_modification_time = true;
    /**
     * Whole seconds of slack allowed before two modification times count as different. Exists for
     * filesystems with coarser timestamp granularity than APFS (exFAT stores 2-second resolution),
     * where a byte-identical copy would otherwise report Changed. Must not be negative.
     */
    int64_t modification_time_tolerance = 0;

    friend bool operator==(const FolderCompareOptions &, const FolderCompareOptions &) = default;
};

struct FolderCompareEntry {
    std::string name;
    FolderCompareStatus status = FolderCompareStatus::Same;
    /**
     * Derived from the modification times of a matched file pair whenever they differ by more than
     * the tolerance - including when the pair is Same because only size was compared. Always
     * Neither for an unmatched entry, a directory pair or a Conflict.
     */
    FolderCompareNewerSide newer_side = FolderCompareNewerSide::Neither;
    /** Index into the corresponding input span; empty when that side has no such name. */
    std::optional<size_t> left_index;
    std::optional<size_t> right_index;
    /** True only when both sides agree the name is a directory. */
    bool is_directory = false;

    friend bool operator==(const FolderCompareEntry &, const FolderCompareEntry &) = default;
};

struct FolderCompareSummary {
    size_t same = 0;
    size_t left_only = 0;
    size_t right_only = 0;
    size_t changed = 0;
    size_t conflict = 0;

    friend bool operator==(const FolderCompareSummary &, const FolderCompareSummary &) = default;
};

struct FolderComparison {
    /** Left input order first, then the right-only names in right input order. */
    std::vector<FolderCompareEntry> entries;

    [[nodiscard]] FolderCompareSummary Summarize() const noexcept;
    /** True when every name matched with status Same - nothing for a sync to do at this level. */
    [[nodiscard]] bool Identical() const noexcept;
};

using FolderComparisonResult = std::expected<FolderComparison, FolderCompareFailure>;

/**
 * Compares two folder listings at one directory level, by exact name.
 *
 * Two deliberate P0 limits, both chosen so that an unknown case is reported rather than silently
 * resolved, and both owned by later increments (spec §24.2/§24.3):
 *
 * - Names are matched byte-exactly. Two names differing only in case, or in Unicode normalization
 *   form, therefore report as LeftOnly plus RightOnly rather than as one matched pair. That is the
 *   conservative direction: a later sync sees two distinct names and cannot silently overwrite one
 *   with the other. Case-insensitive and normalization-aware matching needs real Unicode folding
 *   plus HFS+/APFS normalization awareness and is not attempted here.
 * - A name that is a directory on both sides is compared by presence only and reports Same. Nothing
 *   is claimed about its contents, so a consumer must not read Same on a directory as "nothing to
 *   do" - recursive comparison is a later increment.
 *
 * Rejects atomically rather than guessing when an input is not a valid listing.
 */
[[nodiscard]] FolderComparisonResult CompareFolders(std::span<const FolderCompareItem> _left,
                                                    std::span<const FolderCompareItem> _right,
                                                    const FolderCompareOptions &_options = {});

} // namespace nc::core
