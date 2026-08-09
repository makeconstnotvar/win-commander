// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "GitBadgePresentation.h"

namespace nc::core {

std::optional<GitBadgePresentation> PresentGitBadge(const GitFileState _state) noexcept
{
    switch( _state ) {
        case GitFileState::Modified:
            return GitBadgePresentation{
                .symbol = "pencil.circle.fill", .is_warning = false, .accessibility_phrase = "modified"};
        case GitFileState::Added:
            return GitBadgePresentation{
                .symbol = "plus.circle.fill", .is_warning = false, .accessibility_phrase = "added"};
        case GitFileState::Deleted:
            return GitBadgePresentation{
                .symbol = "minus.circle.fill", .is_warning = false, .accessibility_phrase = "deleted"};
        case GitFileState::Renamed:
            return GitBadgePresentation{
                .symbol = "arrow.right.circle.fill", .is_warning = false, .accessibility_phrase = "renamed"};
        case GitFileState::Untracked:
            return GitBadgePresentation{
                .symbol = "questionmark.circle.fill", .is_warning = false, .accessibility_phrase = "untracked"};
        case GitFileState::Conflicted:
            // The only state where doing nothing loses work, and so the only one tinted as a
            // warning. If everything is emphasised, nothing is.
            return GitBadgePresentation{
                .symbol = "exclamationmark.triangle.fill", .is_warning = true, .accessibility_phrase = "conflicted"};
        case GitFileState::Unmodified:
        case GitFileState::Ignored:
            // Exactly the states the badge rule refuses. Deciding differently here would let a
            // drawing site decorate rows the rule says carry no news.
            return std::nullopt;
    }
    return std::nullopt;
}

} // namespace nc::core
