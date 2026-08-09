// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CloudSyncState.h"

#include <optional>
#include <string>
#include <string_view>

namespace nc::core {

/**
 * What the filesystem says about one item's cloud status, without opening it.
 *
 * Reading the *content* of a placeholder is what triggers a download, so nothing here does: every
 * answer comes from the item's name and its metadata. That is not a performance choice - it is the
 * same rule GL-1 and GL-4 enforce, applied at the layer that could most easily break it by
 * accident.
 */
struct NativeCloudProbe {
    /** True when the path lies inside a provider's managed container. */
    bool in_cloud_container = false;
    /** macOS names a not-yet-downloaded file `.<name>.icloud` and hides it. */
    bool is_dataless_placeholder = false;
    bool download_in_progress = false;
    bool upload_in_progress = false;
    bool has_conflict = false;
    bool excluded_from_sync = false;

    friend bool operator==(const NativeCloudProbe &, const NativeCloudProbe &) = default;
};

/**
 * The filename a user should see for a cloud placeholder, or nothing when the name is already it.
 *
 * A not-yet-downloaded file is stored as `.name.ext.icloud`. Showing that verbatim would present a
 * hidden file with a wrong extension in place of the photograph the user is looking for - and every
 * extension-driven decision above, Gallery eligibility included, would then be made about
 * `.icloud` rather than about `.jpg`.
 */
[[nodiscard]] std::optional<std::string> UnmaskedCloudPlaceholderName(std::string_view _filename);

/** Turns what the filesystem said into the facts `ClassifyCloudSyncState` reads. */
[[nodiscard]] CloudItemFacts CloudItemFactsFromProbe(const NativeCloudProbe &_probe) noexcept;

/**
 * Asks the filesystem about one native path.
 *
 * Reads resource values only - metadata the system already holds. It does not open the file, which
 * is the whole point: opening a placeholder is what fetches it.
 *
 * A path that cannot be read answers "not in a container" rather than guessing. Reporting an
 * unreadable item as a cloud placeholder would badge it and, worse, would tell every surface above
 * that its bytes are elsewhere when nobody knows that.
 */
[[nodiscard]] NativeCloudProbe ProbeNativeCloudItem(const std::string &_path);

} // namespace nc::core
