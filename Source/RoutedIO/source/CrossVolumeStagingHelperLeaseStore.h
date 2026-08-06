// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CrossVolumeStagingHelperDescriptorSealValidator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>

namespace nc::routedio::cross_volume_staging::helper {

class SourceSnapshotWriter;
class DestinationStageWriter;
class StagingRootAuthority;
class StagingPublicationBarrier;
class StagingPublicationLifecycle;

/** Helper-local identity for one authenticated XPC peer.  Zero is never a valid owner. */
using OwnerID = uint64_t;

/**
 * Bounded, in-memory ownership for V1 Begin descriptor rights.  This class does not create an artifact, select a
 * protected root, or perform a namespace operation.  It only prevents a decoded lease from being used by another
 * peer or more than once before the future helper operation consumes it.
 */
class LeaseStore final
{
private:
    enum class TerminalDisposition : uint8_t {
        Pending,
        Commit,
        Abort,
    };

public:
    static constexpr size_t kMaximumLeases = 16;

    enum class Error : uint8_t {
        InvalidArgument,
        ForkedProcess,
        CapacityExceeded,
        DuplicateCorrelation,
        TokenGenerationFailed,
        UnknownLease,
        OwnerMismatch,
    };

    /** Move-only descriptor-bound claim removed atomically by exactly one Commit or Abort request. */
    class TerminalLease final
    {
    public:
        TerminalLease(const TerminalLease &) = delete;
        TerminalLease &operator=(const TerminalLease &) = delete;
        TerminalLease(TerminalLease &&_other) noexcept;
        TerminalLease &operator=(TerminalLease &&_other) noexcept;
        ~TerminalLease() noexcept = default;

        [[nodiscard]] const BeginRequest &Request() const noexcept { return m_Request; }
        [[nodiscard]] const Lease &Authority() const noexcept { return m_Lease; }

    private:
        TerminalLease(int _creator_pid,
                      OwnerID _owner,
                      BeginRequest _request,
                      xpc_codec::OwnedBeginDescriptors _descriptors,
                      Lease _lease,
                      TerminalDisposition _disposition) noexcept;

        [[nodiscard]] bool HasCurrentProcessCreator() const noexcept;
        [[nodiscard]] bool IsCreatedByCurrentProcess() const noexcept;
        [[nodiscard]] OwnerID Owner() const noexcept { return m_Owner; }
        [[nodiscard]] bool IsCommit() const noexcept { return m_Valid && m_Disposition == TerminalDisposition::Commit; }
        // The future execution seam must revalidate this exact pair immediately before its first namespace mutation.
        // No public caller can obtain descriptor authority merely by completing Take.
        [[nodiscard]] xpc_codec::BorrowedBeginDescriptors Descriptors() const noexcept
        {
            return m_Descriptors.Borrow();
        }

        int m_CreatorPID{-1};
        OwnerID m_Owner;
        BeginRequest m_Request;
        xpc_codec::OwnedBeginDescriptors m_Descriptors;
        Lease m_Lease;
        TerminalDisposition m_Disposition;
        bool m_Valid{true};

        friend class LeaseStore;
        friend class SourceSnapshotWriter;
        friend class DestinationStageWriter;
        friend class StagingRootAuthority;
        friend class StagingPublicationBarrier;
        friend class StagingPublicationLifecycle;
    };

    LeaseStore() noexcept;

    /** Consumes the already validated, duplicated pair of descriptor rights and returns a helper-minted opaque lease.
     */
    [[nodiscard]] std::expected<Lease, Error> Grant(OwnerID _owner, ValidatedBegin _begin) noexcept;

    /** Commit and Abort compete for the same one-use lease.  The successful call removes it before returning. */
    [[nodiscard]] std::expected<TerminalLease, Error> Take(OwnerID _owner, const CommitRequest &_request) noexcept;
    [[nodiscard]] std::expected<TerminalLease, Error> Take(OwnerID _owner, const AbortRequest &_request) noexcept;

    /** Releases all unconsumed descriptor rights belonging to a disconnected peer. */
    size_t RevokeOwner(OwnerID _owner) noexcept;

    [[nodiscard]] size_t ActiveLeaseCount() const noexcept;

private:
    [[nodiscard]] bool IsCreatedByCurrentProcess() const noexcept;

    [[nodiscard]] std::expected<TerminalLease, Error>
    Take(OwnerID _owner, const Header &_header, const Lease &_lease, TerminalDisposition _disposition) noexcept;

    int m_CreatorPID{-1};
    mutable std::mutex m_Mutex;
    std::array<std::optional<TerminalLease>, kMaximumLeases> m_Entries;
};

/**
 * Helper-private one-use lease lifecycle.  Begin is intentionally namespace-mutation-free: it can only retain the
 * descriptor-validated claim and mint an opaque lease.  Until a later Commit-stage staging authority exists, Commit
 * consumes the lease and reports a confirmed NotPublished helper failure; Abort consumes it as NotPublished/Aborted.
 */
class LeaseLifecycle final
{
public:
    explicit LeaseLifecycle(LeaseStore &_leases) noexcept;

    [[nodiscard]] BeginResult Begin(OwnerID _owner, ValidatedBegin _begin) noexcept;
    [[nodiscard]] CompletionResult Commit(OwnerID _owner, const CommitRequest &_request) noexcept;
    [[nodiscard]] CompletionResult Abort(OwnerID _owner, const AbortRequest &_request) noexcept;

    /** Closes every descriptor pair still owned by a disconnected authenticated peer. */
    size_t RevokeOwner(OwnerID _owner) noexcept;

private:
    LeaseStore &m_Leases;
};

} // namespace nc::routedio::cross_volume_staging::helper
