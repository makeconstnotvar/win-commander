// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CrossVolumeStagingHelperDestinationStage.h"
#include "CrossVolumeStagingHelperPublicationLifecycle.h"
#include "CrossVolumeStagingHelperStagingRoots.h"

#include <cstdint>
#include <expected>
#include <variant>

namespace nc::routedio::cross_volume_staging::helper {

class StagingSessionRunnerTestAccess;

/**
 * Helper-private composition of a detached locked-root session into the existing source-snapshot and destination-stage
 * writers and the paired retained-lifecycle record.  It owns the session for the entire copy-to-stage sequence and
 * releases both root locks on return.  It returns only the existing read-only stage continuation; it does not select
 * a destination name, publish, clean up, or expose root/path/descriptor authority.
 */
class StagingSessionRunner final
{
public:
    enum class SessionError : uint8_t {
        InvalidSession,
        ForkedProcess,
    };

    enum class Phase : uint8_t {
        Session,
        SourceSnapshot,
        DestinationStage,
        Lifecycle,
    };

    struct Error final {
        Phase phase{Phase::Session};
        std::variant<SessionError,
                     SourceSnapshotWriter::Error,
                     DestinationStageWriter::Error,
                     StagingPublicationLifecycle::Error>
            cause;
    };

private:
    /**
     * Consumes a locked session by value.  The source and destination ledgers remain flock-protected throughout both
     * writer calls and immutable lifecycle-record admission.  Every failure preserves its exact phase and typed
     * writer/lifecycle error; session destruction releases both locks before the error is returned.
     */
    [[nodiscard]] static std::expected<DestinationStageWriter::SealedDestinationStage, Error>
    Run(StagingRootAuthority::LockedSession _session,
        SourceSnapshotWriter::Cancellation _source_cancellation,
        DestinationStageWriter::Cancellation _destination_cancellation) noexcept;

    friend class StagingSessionRunnerTestAccess;
};

} // namespace nc::routedio::cross_volume_staging::helper
