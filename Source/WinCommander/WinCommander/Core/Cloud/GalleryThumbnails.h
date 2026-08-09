// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "GalleryContents.h"

#include <Base/UnorderedUtil.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nc::core {

/** What is known about one row's thumbnail. */
enum class GalleryThumbnailState : uint8_t {
    /** Never asked for, or the answer aged out. */
    Unknown,
    Ready,
    /**
     * Asked for and refused. Remembered rather than retried, because a folder holding one file the
     * generator cannot read would otherwise re-attempt it on every single redraw.
     */
    Failed,
    /**
     * Never asked for, and never will be from here: the bytes are not local, and generating a
     * thumbnail is what would fetch them. Switching to Gallery is not consent to a download.
     */
    Withheld
};

/**
 * Thumbnails already produced for the current folder.
 *
 * Deliberately the same shape as the network-volume probe cache, and for the same reason: the
 * expensive answer is taken off the drawing thread once and remembered, and the drawing thread only
 * ever asks what is already known.
 */
class GalleryThumbnailCache final
{
public:
    /** An opaque handle to whatever the caller uses for an image. Null means the load failed. */
    using Thumbnail = std::shared_ptr<void>;
    /** Produces a thumbnail for a path. May block; never called on the drawing thread. */
    using Generator = std::function<Thumbnail(const std::string &_path)>;

    /**
     * `_capacity` bounds how many thumbnails are held. A folder can hold fifty thousand photographs,
     * and an unbounded cache of them is a memory problem the user did not ask for; the least
     * recently asked-for entries are dropped first, since those are the ones furthest from the
     * viewport.
     */
    GalleryThumbnailCache(Generator _generator, size_t _capacity = 512);

    /** What is known. **Never blocks and never generates** - this is the drawing thread's call. */
    [[nodiscard]] GalleryThumbnailState State(std::string_view _path) const;

    /**
     * The image, or nothing when it is not ready. Never blocks.
     *
     * Asking marks it as the most recently wanted, because this call means "I am drawing this now" -
     * which is exactly what must survive eviction.
     */
    [[nodiscard]] Thumbnail Known(std::string_view _path) const;

    /**
     * Whether a row is worth generating for.
     *
     * False for a row whose bytes are not local, which is the rule GL-1 exists for; false for one
     * already answered, whether it succeeded or failed.
     */
    [[nodiscard]] bool NeedsGeneration(const GalleryRow &_row, std::string_view _path) const;

    /** Generates and records. Call off the drawing thread. */
    void Generate(const GalleryRow &_row, const std::string &_path);

    /** Forgets everything - for a folder change, where none of it applies any more. */
    void Clear();

    [[nodiscard]] size_t HeldCount() const;

private:
    struct Entry {
        GalleryThumbnailState state = GalleryThumbnailState::Unknown;
        Thumbnail thumbnail;
        std::list<std::string>::iterator recency;
    };

    void TouchLocked(const std::string &_path, Entry &_entry) const;
    [[nodiscard]] Entry &InsertOrTouchLocked(const std::string &_path);
    void EvictLocked();

    mutable std::mutex m_Lock;
    Generator m_Generator;
    size_t m_Capacity;
    /** Most recently asked-for at the front. */
    mutable std::list<std::string> m_Recency;
    std::unordered_map<std::string, Entry, UnorderedStringHashEqual, UnorderedStringHashEqual> m_Entries;
};

} // namespace nc::core
