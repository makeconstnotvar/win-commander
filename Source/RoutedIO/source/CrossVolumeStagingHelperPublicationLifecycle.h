// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CrossVolumeStagingHelperDestinationStage.h"
#include "CrossVolumeStagingProtectedRootLedger.h"

#include <cstdint>
#include <expected>

namespace nc::routedio::cross_volume_staging::helper {

class StagingPublicationLifecycleTestAccess;
class StagingSessionRunner;

/**
 * Helper-private durable pairing for a staged source snapshot and destination artifact.  This is deliberately a
 * retention/recovery contract: it creates an immutable private lifecycle primary plus append-only sealed companion
 * on each root and classifies them read-only after a restart.  It neither publishes an artifact nor grants cleanup
 * authority.
 */
class StagingPublicationLifecycle final
{
public:
    enum class Error : uint8_t {
        InvalidStage,
        ForkedProcess,
        SourceRootInvalid,
        DestinationRootInvalid,
        RootsOnSameDevice,
        SourceManifestFailed,
        DestinationManifestFailed,
        InvalidHeader,
        SourceRootBusy,
        DestinationRootBusy,
        RootLockFailed,
    };

    enum class State : uint8_t {
        /** Both immutable records and both sealed private artifacts exactly cross-bind the requested correlation. */
        ExactPending,
        /** Neither protected root has a lifecycle record for the requested correlation. */
        Absent,
        /** A valid record exists on only one side, or its linked private artifact is incomplete/stale. */
        Incomplete,
        /** Both sides exist but do not exactly cross-bind one another. */
        Mismatched,
        /** A matching lifecycle name exists but its private record is malformed or unsafe. */
        Malformed,
    };

    struct Inspection final {
        State state{State::Absent};
    };

private:
    /**
     * Writes one root-private primary plus sealed lifecycle companion on each already flock-protected root.  A
     * partial write is retained and reported as failure; no compensating deletion is attempted.
     */
    [[nodiscard]] static std::expected<void, Error>
    RecordStaged(ProtectedRootLedger &_source_root,
                 ProtectedRootLedger &_destination_root,
                 const DestinationStageWriter::SealedDestinationStage &_stage) noexcept;

    /**
     * Acquires the two roots in canonical order and performs a strictly read-only exact classification.  It exposes
     * neither private artifact identifiers nor paths, and never calls `Reconcile()` because reconciliation may remove
     * an empty reservation.
     */
    [[nodiscard]] static std::expected<Inspection, Error>
    Inspect(int _borrowed_source_root_fd, int _borrowed_destination_root_fd, const Header &_header) noexcept;

    friend class StagingPublicationLifecycleTestAccess;
    friend class StagingSessionRunner;
};

} // namespace nc::routedio::cross_volume_staging::helper
