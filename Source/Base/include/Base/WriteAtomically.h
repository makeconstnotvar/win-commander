// Copyright (C) 2021-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <span>
#include <cstddef>
#include <filesystem>
#include <string_view>
#include "Error.h"

namespace nc::base {

/**
 * Marks the temporary file WriteAtomically publishes through, as `.<target>.nctmp.XXXXXX`.
 *
 * Exported because anything cleaning up after an interrupted write has to recognise that name, and
 * a second copy of the convention would silently stop matching the day this one changed. mkstemp's
 * own suffix cannot carry that role: six alphanumerics is exactly the shape of names people choose,
 * so `notes.txt.backup` would be indistinguishable from a leftover.
 */
inline constexpr std::string_view g_AtomicWriteTemporaryMarker = ".nctmp.";
/** Length of the random suffix mkstemp substitutes for its `XXXXXX`. */
inline constexpr size_t g_AtomicWriteTemporarySuffixLength = 6;

// Does write a temp file + rename.
// Path should be an absolute path.
// If _follow_symlink is true, WriteAtomically first follows any symlinks in the existing file path and writes to the
// symlink destination instead of the symlink file itself.
std::expected<void, Error> WriteAtomically(const std::filesystem::path &_path,
                                           std::span<const std::byte> _bytes,
                                           bool _follow_symlink = false) noexcept;

} // namespace nc::base
