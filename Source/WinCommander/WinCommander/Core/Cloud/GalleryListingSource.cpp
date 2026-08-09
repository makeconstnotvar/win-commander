// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "GalleryListingSource.h"

namespace nc::core {

namespace {

/** Everything after the final dot, or empty when there is none that starts a suffix. */
std::string ExtensionOf(const std::string &_filename)
{
    const size_t dot = _filename.rfind('.');
    // A leading dot is the start of a hidden name, not the start of an extension: ".profile" has
    // none, and treating "profile" as one would put dotfiles in front of extension-driven rules.
    if( dot == std::string::npos || dot == 0 || dot + 1 == _filename.size() )
        return {};
    return _filename.substr(dot + 1);
}

} // namespace

GalleryListingSource BuildGalleryListing(const std::string &_directory,
                                         const std::span<const NativeListingEntry> _entries,
                                         const std::function<NativeCloudProbe(const std::string &)> &_prober)
{
    GalleryListingSource source;
    source.m_Names.reserve(_entries.size());
    source.m_Extensions.reserve(_entries.size());

    std::vector<CloudSyncState> states;
    std::vector<bool> directories;
    std::vector<bool> dot_dots;
    states.reserve(_entries.size());
    directories.reserve(_entries.size());
    dot_dots.reserve(_entries.size());

    for( const NativeListingEntry &entry : _entries ) {
        // Unmasked first, before anything reads the extension. A not-yet-downloaded photograph is on
        // disk as `.holiday.jpg.icloud`; taken at face value its extension is `icloud`, and the one
        // row the user most wants to see would silently vanish from the view.
        const std::optional<std::string> unmasked = UnmaskedCloudPlaceholderName(entry.filename);
        source.m_Names.push_back(unmasked.value_or(entry.filename));
        source.m_Extensions.push_back(ExtensionOf(source.m_Names.back()));
        directories.push_back(entry.is_directory);
        dot_dots.push_back(entry.filename == "..");

        CloudSyncState state = CloudSyncState::NotCloud;
        // A folder is a folder to Gallery whatever its sync state, so asking would spend a
        // filesystem call per row to learn something nothing reads.
        if( !entry.is_directory && _prober ) {
            // Probed under the name on disk - the masked one is what actually exists there.
            const std::string path = _directory.empty() ? entry.filename : _directory + "/" + entry.filename;
            state = ClassifyCloudSyncState(CloudItemFactsFromProbe(_prober(path)));
        }
        states.push_back(state);
    }

    // Built last, once neither vector can reallocate again: every item holds views into them.
    source.m_Items.reserve(_entries.size());
    for( size_t i = 0; i < _entries.size(); ++i ) {
        source.m_Items.push_back(GalleryListingItem{
            .filename = source.m_Names[i],
            .facts = GalleryItemFacts{.extension = source.m_Extensions[i],
                                      .is_directory = directories[i],
                                      .cloud_state = states[i]},
            .is_dot_dot = dot_dots[i],
        });
    }
    return source;
}

} // namespace nc::core
