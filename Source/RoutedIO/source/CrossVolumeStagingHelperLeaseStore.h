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

/** Helper-local identity for one authenticated XPC peer.  Zero is never a valid owner. */
using OwnerID = uint64_t;

/**
 * Bounded, in-memory ownership for V1 Begin descriptor rights.  This class does not create an artifact, select a
 * protected root, or perform a namespace operation.  It only prevents a decoded lease from being used by another
 * peer or more than once before the future helper operation consumes it.
 */
class LeaseStore final
{
public:
    static constexpr size_t kMaximumLeases = 16;

    enum class Error : uint8_t {
        InvalidArgument,
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
        TerminalLease(TerminalLease &&) noexcept = default;
        TerminalLease &operator=(TerminalLease &&) noexcept = default;
        ~TerminalLease() noexcept = default;

        [[nodiscard]] const BeginRequest &Request() const noexcept { return m_Request; }
        [[nodiscard]] const Lease &Authority() const noexcept { return m_Lease; }

    private:
        TerminalLease(OwnerID _owner,
                      BeginRequest _request,
                      xpc_codec::OwnedBeginDescriptors _descriptors,
                      Lease _lease) noexcept;

        [[nodiscard]] OwnerID Owner() const noexcept { return m_Owner; }
        // The future execution seam must revalidate this exact pair immediately before its first namespace mutation.
        // No public caller can obtain descriptor authority merely by completing Take.
        [[nodiscard]] xpc_codec::BorrowedBeginDescriptors Descriptors() const noexcept
        {
            return m_Descriptors.Borrow();
        }

        OwnerID m_Owner;
        BeginRequest m_Request;
        xpc_codec::OwnedBeginDescriptors m_Descriptors;
        Lease m_Lease;

        friend class LeaseStore;
    };

    /** Consumes the already validated, duplicated pair of descriptor rights and returns a helper-minted opaque lease. */
    [[nodiscard]] std::expected<Lease, Error> Grant(OwnerID _owner, ValidatedBegin _begin) noexcept;

    /** Commit and Abort compete for the same one-use lease.  The successful call removes it before returning. */
    [[nodiscard]] std::expected<TerminalLease, Error> Take(OwnerID _owner,
                                                            const CommitRequest &_request) noexcept;
    [[nodiscard]] std::expected<TerminalLease, Error> Take(OwnerID _owner,
                                                            const AbortRequest &_request) noexcept;

    /** Releases all unconsumed descriptor rights belonging to a disconnected peer. */
    size_t RevokeOwner(OwnerID _owner) noexcept;

    [[nodiscard]] size_t ActiveLeaseCount() const noexcept;

private:
    [[nodiscard]] std::expected<TerminalLease, Error> Take(OwnerID _owner,
                                                            const Header &_header,
                                                            const Lease &_lease) noexcept;

    mutable std::mutex m_Mutex;
    std::array<std::optional<TerminalLease>, kMaximumLeases> m_Entries;
};

} // namespace nc::routedio::cross_volume_staging::helper
