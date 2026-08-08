// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nc::core {

/** One selectable palette row: a stable identity plus the text the user types against. */
struct CommandPaletteEntry {
    /** Stable command identity, carried through untouched so the caller can execute the choice. */
    std::string id;
    /** What the user sees and primarily searches. */
    std::string title;
    /** Secondary searchable text (menu path, shortcut, synonyms). Never outranks a title match. */
    std::string subtitle;
    /** An entry the roster currently offers but cannot run; kept visible unless filtered out. */
    bool enabled = true;

    friend bool operator==(const CommandPaletteEntry &, const CommandPaletteEntry &) = default;
};

/** Where a match landed, so a row can highlight exactly the characters that matched. */
enum class CommandPaletteMatchField : uint8_t {
    Title,
    Subtitle
};

struct CommandPaletteMatch {
    size_t entry_index = 0;
    int score = 0;
    CommandPaletteMatchField field = CommandPaletteMatchField::Title;
    /** Byte offsets within the matched field, ascending. Empty for an empty query. */
    std::vector<size_t> offsets;

    friend bool operator==(const CommandPaletteMatch &, const CommandPaletteMatch &) = default;
};

struct CommandPaletteOptions {
    /** Upper bound on returned rows. The palette is a picker, not a report. */
    size_t maximum_results = 50;
    /** Drop entries that cannot currently run instead of showing them greyed out. */
    bool exclude_disabled = false;
};

/**
 * Ranks a command roster against a query.
 *
 * Matching is case-insensitive over ASCII letters only: command titles in this roster are ASCII,
 * and a half-correct Unicode case folding would be worse than a predictable one - the same call
 * made in CompareFolders. Non-ASCII bytes still match exactly, so a localized title is findable by
 * typing it, just not by typing it in a different case.
 *
 * Ranking, highest first:
 *
 * 1. the query appears verbatim in the title, at its start;
 * 2. verbatim in the title at a word boundary;
 * 3. verbatim in the title anywhere;
 * 4. the query's characters appear in the title in order, rewarded for landing consecutively and on
 *    word boundaries;
 * 5. the same, in the subtitle - always below any title match, because a palette that reorders the
 *    obvious answer below a coincidental subtitle hit is worse than useless.
 *
 * Ties break by roster position, never by anything incidental, so the list cannot reshuffle between
 * two keystrokes that score the same - a moving target under the selection is a real way to run the
 * wrong command.
 *
 * An empty query returns the roster in its own order, which is the roster's job to make meaningful.
 */
[[nodiscard]] std::vector<CommandPaletteMatch> FilterCommandPalette(std::span<const CommandPaletteEntry> _entries,
                                                                    std::string_view _query,
                                                                    const CommandPaletteOptions &_options = {});

} // namespace nc::core
