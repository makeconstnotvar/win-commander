// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CrossVolumeStagingHelperLeaseStore.h"
#include "CrossVolumeStagingProtectedRootLedger.h"

#include <cstdint>
#include <expected>

namespace nc::routedio::cross_volume_staging::helper {

class DestinationStageWriter;
class StagingPublicationBarrier;
class StagingPublicationLifecycle;

/**
 * Helper-private, descriptor-bound source snapshot authority.  It is deliberately disconnected from XPC dispatch,
 * destination-stage creation and namespace publication.  Its result is process-owned and cannot be reconstructed
 * from the V1 on-disk companion manifest after restart.
 */
class SourceSnapshotWriter final
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
        InvalidTerminalLease,
        SourceRootBindingFailed,
        SourceStale,
        SourceTooLarge,
        ReservationFailed,
        ArtifactCreateFailed,
        SourceReadFailed,
        ArtifactWriteFailed,
        ArtifactValidationFailed,
        ArtifactSyncFailed,
        ArtifactCloseFailed,
        SealManifestFailed,
        PostSealValidationFailed,
    };

    /**
     * Move-only continuation holding the exact source/destination-parent descriptor pair and a read-only sealed
     * artifact FD.  It exposes claims only; no caller receives a user path, root FD, artifact FD or write authority.
     */
    class SealedSourceSnapshot final
    {
    public:
        SealedSourceSnapshot(const SealedSourceSnapshot &) = delete;
        SealedSourceSnapshot &operator=(const SealedSourceSnapshot &) = delete;
        SealedSourceSnapshot(SealedSourceSnapshot &&_other) noexcept;
        SealedSourceSnapshot &operator=(SealedSourceSnapshot &&) = delete;
        ~SealedSourceSnapshot() noexcept;

        [[nodiscard]] const Header &Correlation() const noexcept { return m_Header; }
        [[nodiscard]] const ObjectSeal &SourceSeal() const noexcept { return m_SourceSeal; }
        [[nodiscard]] const ObjectSeal &ArtifactSeal() const noexcept { return m_ArtifactSeal; }

    private:
        SealedSourceSnapshot(Header _header,
                             ArtifactID _id,
                             ObjectSeal _source_seal,
                             ObjectSeal _artifact_seal,
                             LeaseStore::TerminalLease _terminal_lease,
                             int _artifact_fd) noexcept;

        [[nodiscard]] bool IsCreatedByCurrentProcess() const noexcept;

        int m_CreatorPID{-1};
        Header m_Header;
        ArtifactID m_ID;
        ObjectSeal m_SourceSeal;
        ObjectSeal m_ArtifactSeal;
        LeaseStore::TerminalLease m_TerminalLease;
        int m_ArtifactFD{-1};

        friend class SourceSnapshotWriter;
        friend class DestinationStageWriter;
        friend class StagingPublicationBarrier;
        friend class StagingPublicationLifecycle;
    };

    /**
     * Consumes an exact committed terminal lease.  Cancellation before reservation leaves no ledger state; every later
     * failure retains the primary reservation and any private incomplete artifact without a sealed companion manifest.
     */
    [[nodiscard]] static std::expected<SealedSourceSnapshot, Error> Create(ProtectedRootLedger &_ledger,
                                                                           LeaseStore::TerminalLease _terminal_lease,
                                                                           Cancellation _cancellation) noexcept;
};

} // namespace nc::routedio::cross_volume_staging::helper
