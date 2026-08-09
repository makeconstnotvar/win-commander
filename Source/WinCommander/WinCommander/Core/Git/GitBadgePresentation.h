// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "GitStatus.h"

#include <optional>
#include <string_view>

namespace nc::core {

/** How a row draws one git state. */
struct GitBadgePresentation {
    /** SF Symbol name. */
    std::string_view symbol;
    /**
     * Whether the badge should be tinted as a warning rather than as ordinary information. Only the
     * state where doing nothing loses work earns it: if everything is emphasised, nothing is.
     */
    bool is_warning = false;
    /**
     * Spoken by a screen reader. A badge that is only a coloured dot conveys nothing to a user who
     * cannot see it, which would make git status a sighted-only feature.
     */
    std::string_view accessibility_phrase;

    friend bool operator==(const GitBadgePresentation &, const GitBadgePresentation &) = default;
};

/**
 * How to draw a state, or nothing when it earns no badge.
 *
 * Returns nothing for exactly the states `ShouldBadgeGitFileState` refuses, so a drawing site cannot
 * decide differently from the rule by accident - the two would then disagree about which rows are
 * decorated, and only one of them would be right.
 *
 * Every badged state gets a **distinct** symbol. Two states sharing one would make the badge say
 * less than the status it came from, which is the only thing it is for.
 */
[[nodiscard]] std::optional<GitBadgePresentation> PresentGitBadge(GitFileState _state) noexcept;

} // namespace nc::core
