// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <string_view>

namespace nc::core {

/**
 * Whether `_candidate` is unambiguously a leftover temporary from an interrupted atomic write of
 * `_target`.
 *
 * `nc::base::WriteAtomically` publishes through a `mkstemp` file named `.<target>.nctmp.XXXXXX`. If
 * the process dies between creating that file and renaming it, the temporary survives forever -
 * nothing cleans it up, and it accumulates one per crash in the user's own directory.
 *
 * The rule this encodes is **when in doubt, do not delete**, and the `.nctmp.` marker is what makes
 * that possible. Shape alone cannot decide: `mkstemp`'s suffix is six alphanumerics, which is
 * exactly the shape of names people choose - `notes.txt.backup` and `notes.txt.bak123` both fit it.
 * A matcher relying on shape would delete those. The marker is not a name anyone picks by accident,
 * so the write side was changed to emit it rather than the read side made to guess.
 *
 * A match therefore requires all of: the leading `.`, the exact target name, the `.nctmp.` marker,
 * and exactly six characters from `[A-Za-z0-9]`.
 *
 * The cost of being wrong in the strict direction is one file left on disk; the cost of being wrong
 * the other way is destroying someone's data.
 */
[[nodiscard]] bool IsOrphanedAtomicWriteTemporary(std::string_view _target_name, std::string_view _candidate) noexcept;

} // namespace nc::core
