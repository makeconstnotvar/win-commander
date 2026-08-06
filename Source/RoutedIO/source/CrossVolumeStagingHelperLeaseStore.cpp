// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CrossVolumeStagingHelperLeaseStore.h"

#include <Security/SecRandom.h>
#include <algorithm>
#include <cerrno>
#include <unistd.h>
#include <utility>

namespace nc::routedio::cross_volume_staging::helper {
namespace {

constexpr size_t kTokenGenerationAttempts = 4;

std::expected<LeaseToken, LeaseStore::Error> GenerateLeaseToken() noexcept
{
    LeaseToken token;
    if( SecRandomCopyBytes(kSecRandomDefault, token.bytes.size(), token.bytes.data()) != errSecSuccess ||
        std::all_of(token.bytes.begin(), token.bytes.end(), [](const uint8_t byte) { return byte == 0; }) )
        return std::unexpected{LeaseStore::Error::TokenGenerationFailed};
    return token;
}

} // namespace

LeaseStore::LeaseStore() noexcept : m_CreatorPID{static_cast<int>(::getpid())}
{
}

bool LeaseStore::IsCreatedByCurrentProcess() const noexcept
{
    return m_CreatorPID == static_cast<int>(::getpid());
}

LeaseStore::TerminalLease::TerminalLease(const int _creator_pid,
                                         OwnerID _owner,
                                         BeginRequest _request,
                                         xpc_codec::OwnedBeginDescriptors _descriptors,
                                         Lease _lease,
                                         const TerminalDisposition _disposition) noexcept
    : m_CreatorPID{_creator_pid}, m_Owner{_owner}, m_Request{std::move(_request)},
      m_Descriptors{std::move(_descriptors)}, m_Lease{std::move(_lease)}, m_Disposition{_disposition}
{
}

LeaseStore::TerminalLease::TerminalLease(TerminalLease &&_other) noexcept
    : m_CreatorPID{_other.m_CreatorPID}, m_Owner{_other.m_Owner}, m_Request{std::move(_other.m_Request)},
      m_Descriptors{std::move(_other.m_Descriptors)}, m_Lease{std::move(_other.m_Lease)},
      m_Disposition{_other.m_Disposition}, m_Valid{std::exchange(_other.m_Valid, false)}
{
}

LeaseStore::TerminalLease &LeaseStore::TerminalLease::operator=(TerminalLease &&_other) noexcept
{
    if( this == &_other )
        return *this;
    m_CreatorPID = _other.m_CreatorPID;
    m_Owner = _other.m_Owner;
    m_Request = std::move(_other.m_Request);
    m_Descriptors = std::move(_other.m_Descriptors);
    m_Lease = std::move(_other.m_Lease);
    m_Disposition = _other.m_Disposition;
    m_Valid = std::exchange(_other.m_Valid, false);
    return *this;
}

bool LeaseStore::TerminalLease::HasCurrentProcessCreator() const noexcept
{
    return m_CreatorPID == static_cast<int>(::getpid());
}

bool LeaseStore::TerminalLease::IsCreatedByCurrentProcess() const noexcept
{
    return m_Valid && HasCurrentProcessCreator();
}

std::expected<Lease, LeaseStore::Error> LeaseStore::Grant(OwnerID _owner, ValidatedBegin _begin) noexcept
{
    if( !IsCreatedByCurrentProcess() )
        return std::unexpected{Error::ForkedProcess};
    if( !_begin.IsValid() )
        return std::unexpected{Error::InvalidArgument};
    if( !_begin.IsCreatedByCurrentProcess() )
        return std::unexpected{Error::ForkedProcess};
    if( _owner == 0 || !Validate(_begin.m_Begin.request) )
        return std::unexpected{Error::InvalidArgument};

    std::lock_guard lock{m_Mutex};
    std::optional<TerminalLease> *free_entry = nullptr;
    for( const auto &entry : m_Entries ) {
        if( entry && entry->Authority().header == _begin.m_Begin.request.header )
            return std::unexpected{Error::DuplicateCorrelation};
    }
    for( auto &entry : m_Entries ) {
        if( !entry && free_entry == nullptr )
            free_entry = &entry;
    }
    if( free_entry == nullptr )
        return std::unexpected{Error::CapacityExceeded};

    for( size_t attempt = 0; attempt != kTokenGenerationAttempts; ++attempt ) {
        const auto token = GenerateLeaseToken();
        if( !token )
            return std::unexpected{token.error()};
        const Lease lease{
            .header = _begin.m_Begin.request.header,
            .token = *token,
        };
        bool collision = false;
        for( const auto &entry : m_Entries ) {
            if( entry && entry->Authority().token == lease.token ) {
                collision = true;
                break;
            }
        }
        if( collision )
            continue;

        free_entry->emplace(TerminalLease{m_CreatorPID,
                                          _owner,
                                          std::move(_begin.m_Begin.request),
                                          std::move(_begin.m_Begin.descriptors),
                                          lease,
                                          TerminalDisposition::Pending});
        _begin.m_Valid = false;
        return lease;
    }
    return std::unexpected{Error::TokenGenerationFailed};
}

std::expected<LeaseStore::TerminalLease, LeaseStore::Error> LeaseStore::Take(OwnerID _owner,
                                                                             const CommitRequest &_request) noexcept
{
    if( !IsCreatedByCurrentProcess() )
        return std::unexpected{Error::ForkedProcess};
    if( !Validate(_request) )
        return std::unexpected{Error::InvalidArgument};
    return Take(_owner, _request.header, _request.lease, TerminalDisposition::Commit);
}

std::expected<LeaseStore::TerminalLease, LeaseStore::Error> LeaseStore::Take(OwnerID _owner,
                                                                             const AbortRequest &_request) noexcept
{
    if( !IsCreatedByCurrentProcess() )
        return std::unexpected{Error::ForkedProcess};
    if( !Validate(_request) )
        return std::unexpected{Error::InvalidArgument};
    return Take(_owner, _request.header, _request.lease, TerminalDisposition::Abort);
}

std::expected<LeaseStore::TerminalLease, LeaseStore::Error>
LeaseStore::Take(OwnerID _owner,
                 const Header &_header,
                 const Lease &_lease,
                 const TerminalDisposition _disposition) noexcept
{
    if( !IsCreatedByCurrentProcess() )
        return std::unexpected{Error::ForkedProcess};
    if( _owner == 0 || _disposition == TerminalDisposition::Pending || !Validate(_header) || !Validate(_lease) ||
        _header != _lease.header )
        return std::unexpected{Error::InvalidArgument};

    std::lock_guard lock{m_Mutex};
    for( auto &entry : m_Entries ) {
        if( !entry || entry->Authority().token != _lease.token )
            continue;
        if( entry->Authority().header != _header )
            return std::unexpected{Error::UnknownLease};
        if( entry->Owner() != _owner )
            return std::unexpected{Error::OwnerMismatch};

        TerminalLease terminal = std::move(*entry);
        entry.reset();
        terminal.m_Disposition = _disposition;
        return terminal;
    }
    return std::unexpected{Error::UnknownLease};
}

size_t LeaseStore::RevokeOwner(OwnerID _owner) noexcept
{
    if( !IsCreatedByCurrentProcess() )
        return 0;
    if( _owner == 0 )
        return 0;

    std::lock_guard lock{m_Mutex};
    size_t revoked = 0;
    for( auto &entry : m_Entries ) {
        if( entry && entry->Owner() == _owner ) {
            entry.reset();
            ++revoked;
        }
    }
    return revoked;
}

size_t LeaseStore::ActiveLeaseCount() const noexcept
{
    if( !IsCreatedByCurrentProcess() )
        return 0;
    std::lock_guard lock{m_Mutex};
    size_t active = 0;
    for( const auto &entry : m_Entries ) {
        if( entry )
            ++active;
    }
    return active;
}

LeaseLifecycle::LeaseLifecycle(LeaseStore &_leases) noexcept : m_Leases{_leases} {}

BeginResult LeaseLifecycle::Begin(const OwnerID _owner, ValidatedBegin _begin) noexcept
{
    const Header header = _begin.Request().header;
    const auto granted = m_Leases.Grant(_owner, std::move(_begin));
    if( granted ) {
        return BeginResult{
            .header = header,
            .disposition = BeginDisposition::Granted,
            .failure = BeginFailure::None,
            .lease = *granted,
        };
    }

    const BeginFailure failure = granted.error() == LeaseStore::Error::InvalidArgument
                                     ? BeginFailure::InvalidRequest
                                     : BeginFailure::HelperFailure;
    return BeginResult{
        .header = header,
        .disposition = BeginDisposition::Rejected,
        .failure = failure,
        .lease = {.header = header},
    };
}

CompletionResult LeaseLifecycle::Commit(const OwnerID _owner, const CommitRequest &_request) noexcept
{
    const auto terminal = m_Leases.Take(_owner, _request);
    if( !terminal ) {
        return CompletionResult{
            .header = _request.header,
            .publication = Publication::Unknown,
            .failure = CompletionFailure::HelperFailure,
            .system_error = EIO,
            .filesystem_sync = FilesystemSync::NotAttempted,
            .filesystem_sync_system_error = 0,
        };
    }

    return CompletionResult{
        .header = _request.header,
        .publication = Publication::NotPublished,
        .failure = CompletionFailure::HelperFailure,
        .system_error = EOPNOTSUPP,
        .filesystem_sync = FilesystemSync::NotAttempted,
        .filesystem_sync_system_error = 0,
    };
}

CompletionResult LeaseLifecycle::Abort(const OwnerID _owner, const AbortRequest &_request) noexcept
{
    const auto terminal = m_Leases.Take(_owner, _request);
    if( !terminal ) {
        return CompletionResult{
            .header = _request.header,
            .publication = Publication::Unknown,
            .failure = CompletionFailure::HelperFailure,
            .system_error = EIO,
            .filesystem_sync = FilesystemSync::NotAttempted,
            .filesystem_sync_system_error = 0,
        };
    }

    return CompletionResult{
        .header = _request.header,
        .publication = Publication::NotPublished,
        .failure = CompletionFailure::Aborted,
        .system_error = 0,
        .filesystem_sync = FilesystemSync::NotAttempted,
        .filesystem_sync_system_error = 0,
    };
}

size_t LeaseLifecycle::RevokeOwner(const OwnerID _owner) noexcept
{
    return m_Leases.RevokeOwner(_owner);
}

} // namespace nc::routedio::cross_volume_staging::helper
