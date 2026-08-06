// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CrossVolumeStagingHelperSourceSnapshot.h"

#include <cstdint>
#include <expected>

namespace nc::routedio::cross_volume_staging::helper {

class StagingPublicationBarrier;
class StagingPublicationLifecycle;

/**
 * Helper-private continuation for an immutable destination-volume stage.  It retains the source continuation and
 * holds only private descriptor authority; no user path, artifact name, root FD or publication method escapes.
 */
class DestinationStageWriter final
{
public:
    enum class CancellationPoint : uint8_t {
        BeforeReservation,
        BeforeArtifactCreate,
        BeforeCopyChunk,
        AfterCopy,
        BeforeSealManifest,
    };

    struct Cancellation final {
        using Probe = bool (*)(CancellationPoint, void *) noexcept;

        Probe probe{nullptr};
        void *context{nullptr};

        [[nodiscard]] bool IsCancelled(const CancellationPoint _point) const noexcept
        {
            return probe != nullptr && probe(_point, context);
        }
    };

    enum class Error : uint8_t {
        Cancelled,
        InvalidSourceSnapshot,
        CrossVolumeBindingFailed,
        SourceStale,
        DestinationParentStale,
        ReservationFailed,
        StageCreateFailed,
        SnapshotReadFailed,
        StageWriteFailed,
        StageValidationFailed,
        StageSyncFailed,
        StageCloseFailed,
        SealManifestFailed,
        PostSealValidationFailed,
    };

    class SealedDestinationStage final
    {
    public:
        SealedDestinationStage(const SealedDestinationStage &) = delete;
        SealedDestinationStage &operator=(const SealedDestinationStage &) = delete;
        SealedDestinationStage(SealedDestinationStage &&_other) noexcept;
        SealedDestinationStage &operator=(SealedDestinationStage &&) = delete;
        ~SealedDestinationStage() noexcept;

        [[nodiscard]] const Header &Correlation() const noexcept { return m_SourceSnapshot.Correlation(); }
        [[nodiscard]] const ObjectSeal &SourceSeal() const noexcept { return m_SourceSnapshot.SourceSeal(); }
        [[nodiscard]] const ObjectSeal &SourceSnapshotSeal() const noexcept { return m_SourceSnapshot.ArtifactSeal(); }
        [[nodiscard]] const ObjectSeal &StageSeal() const noexcept { return m_StageSeal; }

    private:
        SealedDestinationStage(SourceSnapshotWriter::SealedSourceSnapshot _source_snapshot,
                               ArtifactID _id,
                               ObjectSeal _stage_seal,
                               int _stage_fd,
                               int _root_fd,
                               uint64_t _root_device,
                               uint64_t _root_inode) noexcept;

        [[nodiscard]] bool IsCreatedByCurrentProcess() const noexcept;

        int m_CreatorPID{-1};
        SourceSnapshotWriter::SealedSourceSnapshot m_SourceSnapshot;
        ArtifactID m_ID;
        ObjectSeal m_StageSeal;
        int m_StageFD{-1};
        int m_RootFD{-1};
        uint64_t m_RootDevice{0};
        uint64_t m_RootInode{0};

        friend class DestinationStageWriter;
        friend class StagingPublicationBarrier;
        friend class StagingPublicationLifecycle;
    };

    /**
     * Consumes a sealed source continuation and produces one immutable helper-owned stage on the exact distinct
     * destination volume.  Every failure after reservation leaves retained private state without a continuation or
     * any mutation in the user destination namespace.
     */
    [[nodiscard]] static std::expected<SealedDestinationStage, Error>
    Create(ProtectedRootLedger &_destination_ledger,
           SourceSnapshotWriter::SealedSourceSnapshot _source_snapshot,
           Cancellation _cancellation) noexcept;
};

} // namespace nc::routedio::cross_volume_staging::helper
