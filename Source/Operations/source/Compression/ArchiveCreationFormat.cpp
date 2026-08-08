// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ArchiveCreationFormat.h"

#include <array>
#include <string>

namespace nc::ops {

namespace {

// Ordered longest-extension-first so that resolution can take the first match and be correct by
// construction rather than by a separate length comparison.
constexpr std::array g_Formats{
    ArchiveCreationFormatInfo{
        .format = ArchiveCreationFormat::TarBzip2, .extension = "tar.bz2", .preserves_posix_metadata = true, .compresses = true, .supports_encryption = false},
    ArchiveCreationFormatInfo{
        .format = ArchiveCreationFormat::TarGzip, .extension = "tar.gz", .preserves_posix_metadata = true, .compresses = true, .supports_encryption = false},
    ArchiveCreationFormatInfo{
        .format = ArchiveCreationFormat::Zip, .extension = "zip", .preserves_posix_metadata = false, .compresses = true, .supports_encryption = true},
    ArchiveCreationFormatInfo{
        .format = ArchiveCreationFormat::Tar, .extension = "tar", .preserves_posix_metadata = true, .compresses = false, .supports_encryption = false},
};

std::string AsciiLowered(const std::string_view _text)
{
    std::string lowered;
    lowered.reserve(_text.size());
    for( const char character : _text )
        lowered.push_back(character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character);
    return lowered;
}

} // namespace

std::span<const ArchiveCreationFormatInfo> SupportedArchiveCreationFormats() noexcept
{
    return g_Formats;
}

const ArchiveCreationFormatInfo &DescribeArchiveCreationFormat(const ArchiveCreationFormat _format) noexcept
{
    for( const ArchiveCreationFormatInfo &info : g_Formats )
        if( info.format == _format )
            return info;
    return g_Formats.front();
}

std::optional<ArchiveCreationFormat> ArchiveCreationFormatForFilename(const std::string_view _filename)
{
    if( _filename.empty() )
        return std::nullopt;
    const std::string lowered = AsciiLowered(_filename);

    for( const ArchiveCreationFormatInfo &info : g_Formats ) {
        // +1 for the separating dot: the extension must be preceded by one, and that dot must not
        // be the first character, so a dotfile named ".zip" stays a file called ".zip".
        const size_t needed = info.extension.size() + 1;
        if( lowered.size() <= needed )
            continue;
        const std::string_view tail{lowered.data() + lowered.size() - needed, needed};
        if( tail.front() != '.' )
            continue;
        if( tail.substr(1) == info.extension )
            return info.format;
    }
    return std::nullopt;
}

ArchiveCreationRequestVerdict EvaluateArchiveCreationRequest(const ArchiveCreationFormat _format,
                                                             const bool _has_destination,
                                                             const bool _protect_with_password,
                                                             const bool _has_password) noexcept
{
    if( !_has_destination )
        return ArchiveCreationRequestVerdict::DestinationMissing;
    if( _protect_with_password ) {
        // The unsupported format is reported before the missing password: typing one would not help,
        // and saying "enter a password" to someone who cannot use one is the more misleading answer.
        if( !DescribeArchiveCreationFormat(_format).supports_encryption )
            return ArchiveCreationRequestVerdict::PasswordUnsupported;
        if( !_has_password )
            return ArchiveCreationRequestVerdict::PasswordMissing;
    }
    return ArchiveCreationRequestVerdict::Submittable;
}

} // namespace nc::ops
