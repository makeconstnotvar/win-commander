// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CrossVolumeStagingHelperLeaseStore.h"
#include "CrossVolumeStagingProtectedRootLedger.h"

#include <cstdint>
#include <expected>
#include <optional>

namespace nc::routedio::cross_volume_staging::helper {

class StagingRootAuthorityTestAccess;
class StagingSessionRunner;

/**
 * Helper-private admission for the pair of protected roots required by a future cross-volume operation.  It only
 * binds the committed descriptor claim to two exact, distinct-volume root locks; it neither reserves an artifact
 * nor exposes a root, path, descriptor or mutation operation.
 */
class StagingRootAuthority final
{
public:
    enum class Error : uint8_t {
        InvalidTerminalLease,
        ForkedProcess,
        SourceRootInvalid,
        DestinationRootInvalid,
        SourceRootDeviceMismatch,
        DestinationRootDeviceMismatch,
        RootsOnSameDevice,
        SourceRootBusy,
        DestinationRootBusy,
        SourceRootLockFailed,
        DestinationRootLockFailed,
    };

    /**
     * Move-only helper-private continuation.  It retains the committed terminal lease and the exact source and
     * destination root ledgers until its destruction.  It deliberately exposes no writer, descriptor or pathname.
     */
    class LockedSession final
    {
    public:
        LockedSession(const LockedSession &) = delete;
        LockedSession &operator=(const LockedSession &) = delete;
        LockedSession(LockedSession &&_other) noexcept;
        LockedSession &operator=(LockedSession &&) = delete;
        ~LockedSession() noexcept = default;

    private:
        LockedSession(LeaseStore::TerminalLease _terminal_lease,
                      ProtectedRootLedger _source_root,
                      ProtectedRootLedger _destination_root) noexcept;

        [[nodiscard]] bool IsCreatedByCurrentProcess() const noexcept;

        int m_CreatorPID{-1};
        std::optional<LeaseStore::TerminalLease> m_TerminalLease;
        std::optional<ProtectedRootLedger> m_SourceRoot;
        std::optional<ProtectedRootLedger> m_DestinationRoot;

        friend class StagingRootAuthority;
        friend class StagingSessionRunner;
        friend class StagingRootAuthorityTestAccess;
    };

private:
    /**
     * Consumes a current-process committed terminal lease and borrows two caller-owned root FDs.  Both root FDs are
     * validated before lock acquisition; their exact ProtectedRootLedger instances are then opened in deterministic
     * (device, inode) order.  Any second-root failure destroys the first temporary ledger and releases its lock.
     */
    [[nodiscard]] static std::expected<LockedSession, Error> Acquire(LeaseStore::TerminalLease _terminal_lease,
                                                                     int _borrowed_source_root_fd,
                                                                     int _borrowed_destination_root_fd) noexcept;

    friend class StagingRootAuthorityTestAccess;
};

} // namespace nc::routedio::cross_volume_staging::helper
