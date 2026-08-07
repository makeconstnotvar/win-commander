// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <Utility/Tags.h>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace nc::vfs {
class ListingItem;
}

namespace nc::core {

/** An owning value copy of one Finder tag already present in a listing. */
struct FileMetadataTag final {
    std::string label;
    utility::Tags::Color color = utility::Tags::Color::None;

    bool operator==(const FileMetadataTag &) const noexcept = default;
};

/**
 * Immediate read-only metadata copied from one exact ListingItem.
 *
 * The snapshot contains no Listing, VFSHost, provider, descriptor, or mutation handle. Optional
 * fields preserve the distinction between an unavailable listing fact and a legitimate zero value.
 */
struct FileMetadataSnapshot final {
    std::string path;
    std::string filename;
    std::optional<std::string> display_name;
    std::optional<std::string> extension;
    mode_t unix_mode = 0;
    uint8_t unix_type = 0;
    bool is_directory = false;
    bool is_regular = false;
    bool is_symlink = false;
    bool is_hidden = false;
    std::optional<uint64_t> size;
    std::optional<uint64_t> inode;
    std::optional<time_t> accessed_time;
    std::optional<time_t> modified_time;
    std::optional<time_t> status_changed_time;
    std::optional<time_t> created_time;
    std::optional<time_t> added_time;
    std::optional<uint32_t> unix_flags;
    std::optional<uid_t> unix_uid;
    std::optional<gid_t> unix_gid;
    std::optional<std::string> symlink_target;
    std::vector<FileMetadataTag> tags;

    bool operator==(const FileMetadataSnapshot &) const noexcept = default;
};

/** Copies listing-backed metadata into an authority-free owning value. */
[[nodiscard]] FileMetadataSnapshot CopyFileMetadataSnapshot(const vfs::ListingItem &_item);

} // namespace nc::core
