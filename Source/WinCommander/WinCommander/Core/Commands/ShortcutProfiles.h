// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nc::core {

/** One action's binding, as the shortcut tables express it: an action name and its key equivalent. */
struct ShortcutBinding {
    std::string action;
    /** Persisted key-equivalent form, e.g. "⇧⌘p". An empty string means deliberately unbound. */
    std::string shortcut;

    friend bool operator==(const ShortcutBinding &, const ShortcutBinding &) = default;
};

/**
 * Two or more actions in the same domain claiming one key equivalent.
 *
 * Domains matter: the shortcut lookup is domain-filtered, so a `menu.*` and a `panel.*` binding may
 * legitimately share a key and are not a conflict. Reporting those would make the detector noise,
 * and a detector nobody trusts is worse than none.
 */
struct ShortcutConflict {
    std::string domain;
    std::string shortcut;
    /** Colliding action names, in the order the bindings were supplied. */
    std::vector<std::string> actions;

    friend bool operator==(const ShortcutConflict &, const ShortcutConflict &) = default;
};

/**
 * Reports every same-domain collision in a binding set.
 *
 * Unbound actions never collide - any number of actions may have no shortcut. A domain is the
 * action name's first dot-separated segment (`menu`, `panel`, `viewer`); a name with no dot forms
 * its own domain rather than being silently grouped with everything else.
 *
 * Results are ordered by first appearance so a report reads in the same order as the table it came
 * from, and repeated runs over the same input are byte-identical.
 */
[[nodiscard]] std::vector<ShortcutConflict> DetectShortcutConflicts(std::span<const ShortcutBinding> _bindings);

/** Named alternative keyboard layouts (spec §26.2 P1). */
enum class ShortcutProfileKind : uint8_t {
    /** The application's own defaults; selecting it clears every profile override. */
    Default,
    /** Finder-like: familiar macOS bindings for users coming from the system file manager. */
    MacOSNative,
    /** Windows Explorer-like: F2 rename, Delete to trash, Alt+arrows for history. */
    WindowsExplorer,
    /** Total Commander-like: the function-key row drives copy/move/mkdir/delete. */
    Commander
};

struct ShortcutProfile {
    ShortcutProfileKind kind = ShortcutProfileKind::Default;
    /** Stable identifier for persistence; never localized. */
    std::string id;
    /** Overrides this profile applies on top of the defaults. Empty for Default. */
    std::vector<ShortcutBinding> bindings;
};

/** Every selectable profile, Default first. */
[[nodiscard]] std::vector<ShortcutProfile> AllShortcutProfiles();

/** The profile for a persisted id, or nothing when the id is unknown. */
[[nodiscard]] std::vector<ShortcutProfile>::const_iterator FindShortcutProfile(const std::vector<ShortcutProfile> &_profiles,
                                                                               std::string_view _id);

} // namespace nc::core
