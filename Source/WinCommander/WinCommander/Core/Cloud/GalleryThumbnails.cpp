// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "GalleryThumbnails.h"

#include <utility>

namespace nc::core {

GalleryThumbnailCache::GalleryThumbnailCache(Generator _generator, const size_t _capacity)
    : m_Generator{std::move(_generator)}, m_Capacity{_capacity == 0 ? 1 : _capacity}
{
}

void GalleryThumbnailCache::TouchLocked(const std::string &_path, Entry &_entry) const
{
    m_Recency.erase(_entry.recency);
    m_Recency.push_front(_path);
    _entry.recency = m_Recency.begin();
}

GalleryThumbnailCache::Entry &GalleryThumbnailCache::InsertOrTouchLocked(const std::string &_path)
{
    const auto [it, inserted] = m_Entries.try_emplace(_path);
    if( inserted ) {
        m_Recency.push_front(_path);
        it->second.recency = m_Recency.begin();
    }
    else {
        TouchLocked(_path, it->second);
    }
    return it->second;
}

void GalleryThumbnailCache::EvictLocked()
{
    // The least recently asked-for entries go first: those are the ones furthest from the viewport,
    // and the ones a scroll is least likely to want back immediately.
    while( m_Entries.size() > m_Capacity && !m_Recency.empty() ) {
        const std::string victim = m_Recency.back();
        m_Recency.pop_back();
        m_Entries.erase(victim);
    }
}

GalleryThumbnailState GalleryThumbnailCache::State(const std::string_view _path) const
{
    const auto lock = std::lock_guard{m_Lock};
    const auto found = m_Entries.find(_path);
    return found == m_Entries.end() ? GalleryThumbnailState::Unknown : found->second.state;
}

GalleryThumbnailCache::Thumbnail GalleryThumbnailCache::Known(const std::string_view _path) const
{
    const auto lock = std::lock_guard{m_Lock};
    const auto found = m_Entries.find(_path);
    if( found == m_Entries.end() || found->second.state != GalleryThumbnailState::Ready )
        return {};
    // This call means "I am drawing this now", which is what makes it the freshest thing in the
    // cache. Ordering by insertion instead would evict exactly the thumbnails on screen during a
    // long scroll back through a folder - the ones certain to be wanted again immediately.
    TouchLocked(found->first, const_cast<Entry &>(found->second)); // NOLINT: recency is mutable state
    return found->second.thumbnail;
}

bool GalleryThumbnailCache::NeedsGeneration(const GalleryRow &_row, const std::string_view _path) const
{
    // The rule GL-1 exists for: generating a thumbnail is what would fetch the bytes, and switching
    // to Gallery is not consent to a download.
    if( _row.eligibility != GalleryEligibility::Thumbnail )
        return false;
    const auto lock = std::lock_guard{m_Lock};
    const auto found = m_Entries.find(_path);
    // Already answered, one way or the other. A failure is an answer: re-attempting a file the
    // generator cannot read would repeat on every redraw.
    return found == m_Entries.end() || found->second.state == GalleryThumbnailState::Unknown;
}

void GalleryThumbnailCache::Generate(const GalleryRow &_row, const std::string &_path)
{
    if( _row.eligibility == GalleryEligibility::PlaceholderOnly ) {
        // Recorded as withheld rather than left unknown, so nothing keeps reconsidering it - and so
        // a surface can say why the tile has no picture.
        const auto lock = std::lock_guard{m_Lock};
        Entry &entry = InsertOrTouchLocked(_path);
        entry.state = GalleryThumbnailState::Withheld;
        entry.thumbnail = {};
        EvictLocked();
        return;
    }
    if( _row.eligibility != GalleryEligibility::Thumbnail || !m_Generator )
        return;

    // Outside the lock: this is the part that reads a file and can take real time, and holding the
    // lock across it would stall every drawing-thread read for exactly that long.
    Thumbnail produced;
    try {
        produced = m_Generator(_path);
    } catch( ... ) {
        produced = {};
    }

    const auto lock = std::lock_guard{m_Lock};
    Entry &entry = InsertOrTouchLocked(_path);
    entry.thumbnail = produced;
    entry.state = produced ? GalleryThumbnailState::Ready : GalleryThumbnailState::Failed;
    EvictLocked();
}

void GalleryThumbnailCache::Clear()
{
    const auto lock = std::lock_guard{m_Lock};
    m_Entries.clear();
    m_Recency.clear();
}

size_t GalleryThumbnailCache::HeldCount() const
{
    const auto lock = std::lock_guard{m_Lock};
    return m_Entries.size();
}

} // namespace nc::core
