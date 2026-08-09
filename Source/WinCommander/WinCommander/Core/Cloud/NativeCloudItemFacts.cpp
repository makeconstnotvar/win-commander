// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "NativeCloudItemFacts.h"

namespace nc::core {

std::optional<std::string> UnmaskedCloudPlaceholderName(const std::string_view _filename)
{
    static constexpr std::string_view suffix = ".icloud";
    // The leading dot is as much a part of the convention as the suffix: a file genuinely named
    // "notes.icloud" is not a placeholder, and unmasking it would invent a file that is not there.
    if( _filename.size() <= suffix.size() + 1 )
        return std::nullopt;
    if( _filename.front() != '.' || !_filename.ends_with(suffix) )
        return std::nullopt;

    const std::string_view inner = _filename.substr(1, _filename.size() - suffix.size() - 1);
    // ".icloud" alone unmasks to nothing, which is not a filename.
    if( inner.empty() )
        return std::nullopt;
    return std::string{inner};
}

CloudItemFacts CloudItemFactsFromProbe(const NativeCloudProbe &_probe) noexcept
{
    return CloudItemFacts{
        .in_cloud_container = _probe.in_cloud_container,
        // A placeholder is precisely "the provider knows this file and its bytes are not here".
        .has_local_copy = !_probe.is_dataless_placeholder,
        .download_in_progress = _probe.download_in_progress,
        .upload_in_progress = _probe.upload_in_progress,
        .has_conflict = _probe.has_conflict,
        .excluded_from_sync = _probe.excluded_from_sync,
    };
}

} // namespace nc::core
