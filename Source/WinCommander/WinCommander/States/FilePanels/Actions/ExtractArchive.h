// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/VFS.h>
#include "DefaultAction.h"
#include <cstddef>
#include <cstdint>
#include <optional>

namespace nc::panel::actions {

class ArchiveExtractionTraversalBudget final
{
public:
    static constexpr size_t MaximumEntries = 1'000'000;
    static constexpr size_t MaximumDepth = 127;
    static constexpr size_t MaximumComponents = 4'000'000;
    static constexpr size_t MaximumNameBytes = 256ULL * 1024ULL * 1024ULL;

    enum class Admission : uint8_t {
        Accepted,
        DepthExceeded,
        CapacityExceeded
    };

    /** Reserves the materialized path before its ancestor component vector is copied. */
    [[nodiscard]] Admission Admit(size_t _parent_depth,
                                  size_t _parent_name_bytes,
                                  size_t _filename_bytes) noexcept
    {
        if( _parent_depth >= MaximumDepth )
            return Admission::DepthExceeded;
        if( m_Entries >= MaximumEntries || _parent_name_bytes > MaximumNameBytes ||
            _filename_bytes > MaximumNameBytes - _parent_name_bytes ) {
            return Admission::CapacityExceeded;
        }

        const size_t entry_components = _parent_depth + 1;
        const size_t entry_name_bytes = _parent_name_bytes + _filename_bytes;
        if( entry_components > MaximumComponents - m_Components ||
            entry_name_bytes > MaximumNameBytes - m_NameBytes ) {
            return Admission::CapacityExceeded;
        }

        ++m_Entries;
        m_Components += entry_components;
        m_NameBytes += entry_name_bytes;
        return Admission::Accepted;
    }

    [[nodiscard]] size_t Entries() const noexcept { return m_Entries; }
    [[nodiscard]] size_t Components() const noexcept { return m_Components; }
    [[nodiscard]] size_t NameBytes() const noexcept { return m_NameBytes; }

private:
    size_t m_Entries{0};
    size_t m_Components{0};
    size_t m_NameBytes{0};
};

/** Exact no-follow source identity required before archive-host acquisition. */
struct ArchiveExtractionSourceIdentity final {
    uint64_t inode{0};
    uint64_t size{0};
    int64_t modification_seconds{0};
    int64_t modification_nanoseconds{0};

    bool operator==(const ArchiveExtractionSourceIdentity &) const noexcept = default;

    [[nodiscard]] static std::optional<ArchiveExtractionSourceIdentity>
    Capture(const VFSListingItem &_source) noexcept;
    [[nodiscard]] bool Matches(const VFSListingItem &_source) const noexcept;
};

enum class ArchiveExtractionSubmissionResult {
    Submitted,
    PaneUnavailable,
    WindowUnavailable,
    Loading,
    SelectionUnavailable,
    ParentEntryUnsupported,
    SourceUnsupported,
    SourceUnreadable,
    DestinationUnavailable,
    DestinationReadOnly,
    ProviderUnsupported,
    CaseSensitivityUnavailable,
    StaleContext
};

/** Synchronous, side-effect-free command admission for one exact Extract Here request. */
[[nodiscard]] ArchiveExtractionSubmissionResult
EvaluateArchiveExtractionSubmission(std::span<const VFSListingItem> _items, PanelController *_target);

/**
 * Starts typed archive acquisition and full namespace validation on the pane loading queue.
 * Returning Submitted means the bounded asynchronous request was accepted, not that extraction completed.
 */
[[nodiscard]] ArchiveExtractionSubmissionResult SubmitArchiveExtraction(std::span<const VFSListingItem> _items,
                                                                        PanelController *_target);

struct ExtractArchiveHere final : PanelAction {
    [[nodiscard]] bool Predicate(PanelController *_target) const override;
    void Perform(PanelController *_target, id _sender) const override;
};

} // namespace nc::panel::actions
