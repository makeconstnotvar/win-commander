// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CommandPaletteFilter.h"

#include <algorithm>
#include <optional>

namespace nc::core {

namespace {

constexpr int g_VerbatimBase = 1000;
constexpr int g_VerbatimAtStart = 400;
constexpr int g_VerbatimAtWordBoundary = 200;
constexpr int g_SubsequenceBase = 100;
constexpr int g_ConsecutiveBonus = 12;
constexpr int g_WordBoundaryBonus = 20;
constexpr int g_FirstCharAtStartBonus = 60;
/** Applied to any subtitle result so it can never overtake a title result. */
constexpr int g_SubtitlePenalty = g_VerbatimBase * 2;

char Fold(const char _character) noexcept
{
    return _character >= 'A' && _character <= 'Z' ? static_cast<char>(_character - 'A' + 'a') : _character;
}

std::string Folded(const std::string_view _text)
{
    std::string folded;
    folded.reserve(_text.size());
    for( const char character : _text )
        folded.push_back(Fold(character));
    return folded;
}

bool IsSeparator(const char _character) noexcept
{
    return _character == ' ' || _character == '\t' || _character == '.' || _character == '-' ||
           _character == '_' || _character == '/' || _character == ':' || _character == '(' || _character == ')';
}

/** A position starts a word when it opens the text, follows a separator, or opens a camelCase hump. */
bool IsWordStart(const std::string_view _original, const size_t _position) noexcept
{
    if( _position == 0 )
        return true;
    if( _position > _original.size() )
        return false;
    const char previous = _original[_position - 1];
    if( IsSeparator(previous) )
        return true;
    const char current = _original[_position];
    return current >= 'A' && current <= 'Z' && previous >= 'a' && previous <= 'z';
}

/**
 * Length normalisation. Two titles containing the same query are ranked by how much of the title the
 * query accounts for, so "Copy" beats "Copy Item Path to Clipboard" for the query "copy".
 */
int LengthPenalty(const size_t _text_length, const size_t _query_length) noexcept
{
    const size_t excess = _text_length > _query_length ? _text_length - _query_length : 0;
    return static_cast<int>(std::min<size_t>(excess, 60));
}

struct FieldMatch {
    int score = 0;
    std::vector<size_t> offsets;
};

/** Best verbatim occurrence of the query, preferring the start, then a word boundary, then earliest. */
std::optional<FieldMatch> MatchVerbatim(const std::string_view _original,
                                        const std::string_view _folded_text,
                                        const std::string_view _folded_query)
{
    std::optional<size_t> best_position;
    int best_bonus = -1;
    for( size_t position = _folded_text.find(_folded_query); position != std::string_view::npos;
         position = _folded_text.find(_folded_query, position + 1) ) {
        int bonus = 0;
        if( position == 0 )
            bonus = g_VerbatimAtStart;
        else if( IsWordStart(_original, position) )
            bonus = g_VerbatimAtWordBoundary;
        if( bonus > best_bonus ) {
            best_bonus = bonus;
            best_position = position;
        }
        if( bonus == g_VerbatimAtStart )
            break; // Nothing can beat the start, and the earliest start is the one we already have.
    }
    if( !best_position )
        return std::nullopt;

    FieldMatch match;
    match.score = g_VerbatimBase + best_bonus - static_cast<int>(std::min<size_t>(*best_position, 40)) -
                  LengthPenalty(_original.size(), _folded_query.size());
    match.offsets.reserve(_folded_query.size());
    for( size_t index = 0; index < _folded_query.size(); ++index )
        match.offsets.push_back(*best_position + index);
    return match;
}

/** Greedy left-to-right subsequence match: predictable, and cheap enough to run on every keystroke. */
std::optional<FieldMatch> MatchSubsequence(const std::string_view _original,
                                           const std::string_view _folded_text,
                                           const std::string_view _folded_query)
{
    FieldMatch match;
    match.offsets.reserve(_folded_query.size());
    size_t position = 0;
    std::optional<size_t> previous;
    for( const char wanted : _folded_query ) {
        while( position < _folded_text.size() && _folded_text[position] != wanted )
            ++position;
        if( position == _folded_text.size() )
            return std::nullopt;
        if( previous && *previous + 1 == position )
            match.score += g_ConsecutiveBonus;
        if( IsWordStart(_original, position) )
            match.score += g_WordBoundaryBonus;
        if( match.offsets.empty() && position == 0 )
            match.score += g_FirstCharAtStartBonus;
        match.offsets.push_back(position);
        previous = position;
        ++position;
    }
    match.score += g_SubsequenceBase - LengthPenalty(_original.size(), _folded_query.size());
    return match;
}

std::optional<FieldMatch> MatchField(const std::string_view _original, const std::string_view _folded_query)
{
    if( _original.empty() )
        return std::nullopt;
    const std::string folded_text = Folded(_original);
    if( auto verbatim = MatchVerbatim(_original, folded_text, _folded_query) )
        return verbatim;
    return MatchSubsequence(_original, folded_text, _folded_query);
}

} // namespace

std::vector<CommandPaletteMatch> FilterCommandPalette(const std::span<const CommandPaletteEntry> _entries,
                                                      const std::string_view _query,
                                                      const CommandPaletteOptions &_options)
{
    std::vector<CommandPaletteMatch> matches;
    if( _options.maximum_results == 0 )
        return matches;

    const std::string folded_query = Folded(_query);
    matches.reserve(std::min(_entries.size(), _options.maximum_results));

    for( size_t index = 0; index < _entries.size(); ++index ) {
        const CommandPaletteEntry &entry = _entries[index];
        if( _options.exclude_disabled && !entry.enabled )
            continue;

        if( folded_query.empty() ) {
            matches.push_back({.entry_index = index, .score = 0, .field = CommandPaletteMatchField::Title});
            continue;
        }
        if( auto title = MatchField(entry.title, folded_query) ) {
            matches.push_back({.entry_index = index,
                               .score = title->score,
                               .field = CommandPaletteMatchField::Title,
                               .offsets = std::move(title->offsets)});
            continue;
        }
        if( auto subtitle = MatchField(entry.subtitle, folded_query) ) {
            matches.push_back({.entry_index = index,
                               .score = subtitle->score - g_SubtitlePenalty,
                               .field = CommandPaletteMatchField::Subtitle,
                               .offsets = std::move(subtitle->offsets)});
        }
    }

    // Score descending, then roster position ascending. std::ranges::sort is not stable, so the
    // index is part of the comparison rather than left to the algorithm - equal-scoring rows must
    // keep one fixed order across keystrokes, or the selection lands on a different command than
    // the one the user was looking at.
    std::ranges::sort(matches, [](const CommandPaletteMatch &_lhs, const CommandPaletteMatch &_rhs) noexcept {
        if( _lhs.score != _rhs.score )
            return _lhs.score > _rhs.score;
        return _lhs.entry_index < _rhs.entry_index;
    });
    if( matches.size() > _options.maximum_results )
        matches.resize(_options.maximum_results);
    return matches;
}

std::vector<CommandPaletteEntry> BuildCommandPaletteRoster(const std::span<const CommandPaletteSource> _sources)
{
    std::vector<CommandPaletteEntry> roster;
    roster.reserve(_sources.size());
    for( const CommandPaletteSource &source : _sources ) {
        // Not visible means the registry says this command does not apply here at all; offering it
        // anyway would route the palette around the registry's own answer. An entry with no id or
        // no title could be listed but never meaningfully chosen, so it is dropped for the same
        // reason rather than shown as a blank row.
        if( !source.visible || source.id.empty() || source.title.empty() )
            continue;
        roster.push_back({.id = source.id,
                          .title = source.title,
                          .subtitle = source.category,
                          .enabled = source.enabled});
    }
    return roster;
}

} // namespace nc::core
