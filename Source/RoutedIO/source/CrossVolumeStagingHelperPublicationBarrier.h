// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CrossVolumeStagingHelperDestinationStage.h"
#include "CrossVolumeStagingProtectedRootLedger.h"

#include <cstdint>
#include <expected>

namespace nc::routedio::cross_volume_staging::helper {

class StagingPublicationBarrierTestAccess;

/**
 * Helper-private final review gate for a sealed destination stage.  It has no namespace mutation or cleanup
 * operation: success only retains the exact stage and a freshly locked destination protected root for the future
 * publisher.  The returned permit has no public descriptor, pathname or publication surface.
 */
class StagingPublicationBarrier final
{
public:
    enum class CancellationPoint : uint8_t {
        BeforePublication,
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
        InvalidStage,
        ForkedProcess,
        SourceStale,
        DestinationParentStale,
        DestinationExists,
        DestinationLookupFailed,
        DestinationRootInvalid,
        DestinationRootBusy,
        DestinationRootLockFailed,
        DestinationRootBindingFailed,
        StageStale,
        StageManifestFailed,
        Cancelled,
    };

    /**
     * Opaque move-only permit for the next helper-private publisher.  It retains the read-only stage and the exact
     * destination root flock, but exposes neither of them and performs no action on destruction beyond ordinary
     * descriptor/lock release.
     */
    class PublicationPermit final
    {
    public:
        PublicationPermit(const PublicationPermit &) = delete;
        PublicationPermit &operator=(const PublicationPermit &) = delete;
        PublicationPermit(PublicationPermit &&_other) noexcept;
        PublicationPermit &operator=(PublicationPermit &&) = delete;
        ~PublicationPermit() noexcept = default;

    private:
        PublicationPermit(DestinationStageWriter::SealedDestinationStage _stage,
                          ProtectedRootLedger _destination_root) noexcept;

        [[nodiscard]] bool IsCreatedByCurrentProcess() const noexcept;

        int m_CreatorPID{-1};
        DestinationStageWriter::SealedDestinationStage m_Stage;
        ProtectedRootLedger m_DestinationRoot;

        friend class StagingPublicationBarrier;
    };

private:
    /**
     * Consumes one current-process sealed stage.  It reacquires the exact destination-root flock, validates the
     * committed terminal claim, source, destination parent, stage, durable records and absent destination both
     * before and after the sole unlocked cancellation callback, then returns an opaque locked permit.  It never
     * creates, renames, links, unlinks or opens an entry in the user destination namespace.
     */
    [[nodiscard]] static std::expected<PublicationPermit, Error>
    Prepare(DestinationStageWriter::SealedDestinationStage _stage, Cancellation _cancellation) noexcept;

    friend class StagingPublicationBarrierTestAccess;
};

} // namespace nc::routedio::cross_volume_staging::helper
