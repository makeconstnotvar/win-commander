// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace nc::ops {

/**
 * Formats this application can *create*.
 *
 * Deliberately a different, smaller set than the formats it can extract: RAR and several others
 * are extractable but not creatable, because their compressors are proprietary. Modelling only
 * "supported archive" would let a Create menu offer a format that fails at execution.
 */
enum class ArchiveCreationFormat : uint8_t {
    Zip,
    Tar,
    TarGzip,
    TarBzip2
};

struct ArchiveCreationFormatInfo {
    ArchiveCreationFormat format = ArchiveCreationFormat::Zip;
    /** Canonical extension without the leading dot. May contain a dot, as in `tar.gz`. */
    std::string_view extension;
    /** Carries full POSIX ownership and permission metadata through a round trip. */
    bool preserves_posix_metadata = false;
    /** The container itself compresses; false means bytes are stored as-is. */
    bool compresses = false;

    friend bool operator==(const ArchiveCreationFormatInfo &, const ArchiveCreationFormatInfo &) = default;
};

/** Every creatable format, in the order a picker should offer them. */
[[nodiscard]] std::span<const ArchiveCreationFormatInfo> SupportedArchiveCreationFormats() noexcept;

[[nodiscard]] const ArchiveCreationFormatInfo &DescribeArchiveCreationFormat(ArchiveCreationFormat _format) noexcept;

/**
 * Resolves a filename's trailing extension to a creatable format.
 *
 * **Longest match wins**, and that is the point: `backup.tar.gz` must resolve to `TarGzip`, not to
 * a bare gzip stream. Getting it the other way round produces a file named like a tarball that is
 * not one - the archive opens, yields a single unnamed blob, and the directory structure the user
 * thought they had archived is silently absent.
 *
 * Matching is case-insensitive over ASCII and requires the extension to follow a `.` that is not
 * the first character, so a dotfile named `.zip` is a file called `.zip`, not an archive.
 */
[[nodiscard]] std::optional<ArchiveCreationFormat> ArchiveCreationFormatForFilename(std::string_view _filename);

} // namespace nc::ops
