// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nc::core {

/** Why a pane cannot supply a working directory to a local tool. */
enum class LocalWorkingDirectoryRefusal : uint8_t {
    None,
    /** The listing is not one directory - "open here" has no single answer. */
    NotUniform,
    /**
     * The pane is on a provider whose paths only exist inside this application: an archive, or a
     * remote host. A local shell or editor would resolve such a path against the real filesystem.
     */
    NotLocalFilesystem,
    /** No committed location yet. */
    NoLocation
};

/** What a pane can tell us about its current location, reduced to what this decision needs. */
struct PaneLocationFacts {
    /** True only for the native filesystem. Archives and remote hosts are not. */
    bool is_native_filesystem = false;
    bool is_uniform = false;
    /** Absolute directory path as the provider reports it. */
    std::string_view path;

    friend bool operator==(const PaneLocationFacts &, const PaneLocationFacts &) = default;
};

struct LocalWorkingDirectory {
    /** Empty exactly when `refusal != None`. */
    std::string path;
    LocalWorkingDirectoryRefusal refusal = LocalWorkingDirectoryRefusal::None;

    [[nodiscard]] bool Usable() const noexcept { return refusal == LocalWorkingDirectoryRefusal::None; }

    friend bool operator==(const LocalWorkingDirectory &, const LocalWorkingDirectory &) = default;
};

/**
 * Derives the directory a local tool - a shell, an editor - should start in.
 *
 * Fails closed, and the reason it must is worth stating: a path inside an archive or on a remote
 * host looks like an ordinary absolute path, but it only means anything to this application. Handed
 * to a local shell it resolves against the real filesystem, so `/Users/me/backup.zip/etc` silently
 * becomes whatever happens to exist at that name locally - or nothing. "Open terminal here" would
 * then land somewhere the user was not looking, which is worse than declining.
 *
 * A trailing slash is stripped except for the root, because shells and editors accept both while
 * some tools echo the path back and the doubled separator reads as a bug.
 */
[[nodiscard]] LocalWorkingDirectory ResolveLocalWorkingDirectory(const PaneLocationFacts &_facts);

} // namespace nc::core
