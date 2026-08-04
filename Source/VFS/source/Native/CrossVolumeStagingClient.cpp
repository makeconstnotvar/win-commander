// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CrossVolumeStagingClient.h"

#include <Base/algo.h>

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <unistd.h>
#include <utility>

namespace nc::vfs::native {
namespace {

using Transport = CrossVolumeStagingTransport;
using TransportError = CrossVolumeStagingTransportError;
namespace protocol = staging_protocol;

bool IsCancelled(const ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) noexcept
{
    try {
        return _cancel_checker && _cancel_checker();
    } catch( ... ) {
        return true;
    }
}

protocol::CorrelationID NewCorrelationID() noexcept
{
    protocol::CorrelationID correlation{};
    arc4random_buf(correlation.data(), correlation.size());
    bool all_zero = true;
    for( const uint8_t value : correlation ) {
        if( value != 0 ) {
            all_zero = false;
            break;
        }
    }
    if( all_zero )
        correlation[0] = 1;
    return correlation;
}

std::expected<protocol::ObjectSeal, ProviderConditionalCopyTransactionBeginError>
ProtocolSeal(const CrossVolumeStagingObjectSeal &_seal) noexcept
{
    if( _seal.birth_time.nanoseconds < 0 || _seal.modification_time.nanoseconds < 0 ||
        _seal.status_change_time.nanoseconds < 0 )
        return std::unexpected{ProviderConditionalCopyTransactionBeginError::InvalidRequest};
    if( static_cast<uint64_t>(_seal.birth_time.nanoseconds) > std::numeric_limits<uint32_t>::max() ||
        static_cast<uint64_t>(_seal.modification_time.nanoseconds) > std::numeric_limits<uint32_t>::max() ||
        static_cast<uint64_t>(_seal.status_change_time.nanoseconds) > std::numeric_limits<uint32_t>::max() )
        return std::unexpected{ProviderConditionalCopyTransactionBeginError::InvalidRequest};

    return protocol::ObjectSeal{
        .device = _seal.device,
        .inode = _seal.inode,
        .uid = _seal.uid,
        .gid = _seal.gid,
        .mode = _seal.mode,
        .flags = _seal.flags,
        .link_count = _seal.link_count,
        .byte_size = _seal.byte_size,
        .birth_time = {.seconds = _seal.birth_time.seconds,
                       .nanoseconds = static_cast<uint32_t>(_seal.birth_time.nanoseconds)},
        .modification_time = {.seconds = _seal.modification_time.seconds,
                              .nanoseconds = static_cast<uint32_t>(_seal.modification_time.nanoseconds)},
        .status_change_time = {.seconds = _seal.status_change_time.seconds,
                               .nanoseconds = static_cast<uint32_t>(_seal.status_change_time.nanoseconds)},
    };
}

std::expected<protocol::BeginRequest, ProviderConditionalCopyTransactionBeginError>
ProtocolBeginRequest(const CrossVolumeStagingRequest &_request) noexcept
{
    const auto source_seal = ProtocolSeal(_request.SourceSeal());
    const auto destination_parent_seal = ProtocolSeal(_request.DestinationParentSeal());
    if( !source_seal || !destination_parent_seal )
        return std::unexpected{!source_seal ? source_seal.error() : destination_parent_seal.error()};
    const auto &name = _request.DestinationName();
    const auto destination_name = protocol::DestinationComponent::Create(std::span<const uint8_t>{
        reinterpret_cast<const uint8_t *>(name.data()), name.size()});
    if( !destination_name )
        return std::unexpected{ProviderConditionalCopyTransactionBeginError::InvalidRequest};

    protocol::BeginRequest result{
        .header = {.version = protocol::kProtocolVersion, .correlation = NewCorrelationID()},
        .source = *source_seal,
        .destination_parent = *destination_parent_seal,
        .destination_name = *destination_name,
    };
    if( !protocol::Validate(result) )
        return std::unexpected{ProviderConditionalCopyTransactionBeginError::InvalidRequest};
    return result;
}

ProviderConditionalCopyTransactionBeginError MapBeginFailure(const protocol::BeginFailure _failure) noexcept
{
    switch( _failure ) {
        case protocol::BeginFailure::Unsupported:
            return ProviderConditionalCopyTransactionBeginError::Unsupported;
        case protocol::BeginFailure::InvalidRequest:
            return ProviderConditionalCopyTransactionBeginError::InvalidRequest;
        case protocol::BeginFailure::SourceStale:
            return ProviderConditionalCopyTransactionBeginError::SourceStale;
        case protocol::BeginFailure::DestinationParentStale:
            return ProviderConditionalCopyTransactionBeginError::DestinationParentStale;
        case protocol::BeginFailure::DestinationExists:
            return ProviderConditionalCopyTransactionBeginError::DestinationExists;
        case protocol::BeginFailure::Cancelled:
            return ProviderConditionalCopyTransactionBeginError::Cancelled;
        case protocol::BeginFailure::None:
        case protocol::BeginFailure::HelperFailure:
            return ProviderConditionalCopyTransactionBeginError::ProviderFailure;
    }
    return ProviderConditionalCopyTransactionBeginError::ProviderFailure;
}

ProviderConditionalCopyCommitResult UnknownResult() noexcept
{
    return ProviderConditionalCopyCommitResult{
        .publication = ProviderConditionalCopyPublicationState::Unknown,
        .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
        .system_error = EIO,
        .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
        .filesystem_sync_system_error = 0,
    };
}

ProviderConditionalCopyCommitResult MapCompletion(const protocol::CompletionResult &_result) noexcept
{
    if( !protocol::Validate(_result) )
        return UnknownResult();

    const auto publication = [&] {
        switch( _result.publication ) {
            case protocol::Publication::NotPublished:
                return ProviderConditionalCopyPublicationState::NotPublished;
            case protocol::Publication::Published:
                return ProviderConditionalCopyPublicationState::Published;
            case protocol::Publication::Unknown:
                return ProviderConditionalCopyPublicationState::Unknown;
        }
        return ProviderConditionalCopyPublicationState::Unknown;
    }();
    const auto failure = [&] {
        switch( _result.failure ) {
            case protocol::CompletionFailure::None:
                return ProviderConditionalCopyCommitFailure::None;
            case protocol::CompletionFailure::Aborted:
                return ProviderConditionalCopyCommitFailure::Aborted;
            case protocol::CompletionFailure::Cancelled:
                return ProviderConditionalCopyCommitFailure::Cancelled;
            case protocol::CompletionFailure::SourceStale:
                return ProviderConditionalCopyCommitFailure::SourceStale;
            case protocol::CompletionFailure::DestinationParentStale:
                return ProviderConditionalCopyCommitFailure::DestinationParentStale;
            case protocol::CompletionFailure::DestinationExists:
                return ProviderConditionalCopyCommitFailure::DestinationExists;
            case protocol::CompletionFailure::MetadataFailed:
                return ProviderConditionalCopyCommitFailure::MetadataFailed;
            case protocol::CompletionFailure::FileSystemSyncFailed:
                return ProviderConditionalCopyCommitFailure::FileSystemSyncFailed;
            case protocol::CompletionFailure::HelperFailure:
                return ProviderConditionalCopyCommitFailure::ProviderFailure;
        }
        return ProviderConditionalCopyCommitFailure::ProviderFailure;
    }();
    const auto sync = [&] {
        switch( _result.filesystem_sync ) {
            case protocol::FilesystemSync::NotAttempted:
                return ProviderConditionalCopyFilesystemSyncStatus::NotAttempted;
            case protocol::FilesystemSync::Confirmed:
                return ProviderConditionalCopyFilesystemSyncStatus::Confirmed;
            case protocol::FilesystemSync::Failed:
                return ProviderConditionalCopyFilesystemSyncStatus::Failed;
        }
        return ProviderConditionalCopyFilesystemSyncStatus::NotAttempted;
    }();
    return ProviderConditionalCopyCommitResult{
        .publication = publication,
        .failure = failure,
        .system_error = _result.system_error,
        .filesystem_sync_status = sync,
        .filesystem_sync_system_error = _result.filesystem_sync_system_error,
    };
}

bool Matches(const protocol::Header &_expected, const protocol::Header &_actual) noexcept
{
    return _expected == _actual;
}

ProviderConditionalCopyPublicationState AbortLease(const std::shared_ptr<Transport> &_transport,
                                                   const protocol::AbortRequest &_request) noexcept
{
    if( !_transport )
        return ProviderConditionalCopyPublicationState::Unknown;
    try {
        const auto reply = _transport->Abort(_request);
        if( reply && Matches(_request.header, reply->header) && protocol::Validate(*reply) &&
            reply->publication == protocol::Publication::NotPublished )
            return ProviderConditionalCopyPublicationState::NotPublished;
    } catch( ... ) {
    }
    return ProviderConditionalCopyPublicationState::Unknown;
}

class ClientTransaction final : public CrossVolumeStagingTransaction
{
public:
    ClientTransaction(std::shared_ptr<Transport> _transport, protocol::CommitRequest _commit_request) noexcept
        : m_Transport{std::move(_transport)}, m_CommitRequest{std::move(_commit_request)}
    {
    }

    ~ClientTransaction() override { (void)Abort(); }

    [[nodiscard]] ProviderConditionalCopyCommitResult
    Commit(const ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) noexcept override
    {
        const auto lock = std::lock_guard{m_Mutex};
        if( m_CommitResult )
            return *m_CommitResult;
        if( m_AbortPublication )
            return UnknownResult();
        if( IsCancelled(_cancel_checker) ) {
            m_CommitResult = ProviderConditionalCopyCommitResult{
                .publication = ProviderConditionalCopyPublicationState::NotPublished,
                .failure = ProviderConditionalCopyCommitFailure::Cancelled,
            };
            return *m_CommitResult;
        }

        std::expected<protocol::CompletionResult, TransportError> reply = std::unexpected(TransportError::Failure);
        try {
            m_CommitDispatched = true;
            reply = m_Transport->Commit(m_CommitRequest);
        } catch( ... ) {
            m_CommitResult = UnknownResult();
            return *m_CommitResult;
        }
        if( !reply || !Matches(m_CommitRequest.header, reply->header) ) {
            m_CommitResult = UnknownResult();
            return *m_CommitResult;
        }
        m_CommitResult = MapCompletion(*reply);
        return *m_CommitResult;
    }

    [[nodiscard]] ProviderConditionalCopyPublicationState Abort() noexcept override
    {
        const auto lock = std::lock_guard{m_Mutex};
        if( m_AbortPublication )
            return *m_AbortPublication;
        if( m_CommitDispatched ) {
            m_AbortPublication = m_CommitResult ? m_CommitResult->publication
                                                 : ProviderConditionalCopyPublicationState::Unknown;
            return *m_AbortPublication;
        }

        const protocol::AbortRequest request{
            .header = m_CommitRequest.header,
            .lease = m_CommitRequest.lease,
        };
        m_AbortPublication = AbortLease(m_Transport, request);
        return *m_AbortPublication;
    }

private:
    std::shared_ptr<Transport> m_Transport;
    protocol::CommitRequest m_CommitRequest;
    std::mutex m_Mutex;
    std::optional<ProviderConditionalCopyCommitResult> m_CommitResult;
    std::optional<ProviderConditionalCopyPublicationState> m_AbortPublication;
    bool m_CommitDispatched{false};
};

} // namespace

CrossVolumeStagingTransportBegin::CrossVolumeStagingTransportBegin(staging_protocol::BeginRequest _request,
                                                                   const int _source_fd,
                                                                   const int _destination_parent_fd) noexcept
    : m_Request{std::move(_request)}, m_SourceFD{_source_fd}, m_DestinationParentFD{_destination_parent_fd}
{
}

CrossVolumeStagingTransportBegin::CrossVolumeStagingTransportBegin(CrossVolumeStagingTransportBegin &&_rhs) noexcept
    : m_Request{std::move(_rhs.m_Request)}, m_SourceFD{std::exchange(_rhs.m_SourceFD, -1)},
      m_DestinationParentFD{std::exchange(_rhs.m_DestinationParentFD, -1)}
{
}

CrossVolumeStagingTransportBegin &
CrossVolumeStagingTransportBegin::operator=(CrossVolumeStagingTransportBegin &&_rhs) noexcept
{
    if( this == &_rhs )
        return *this;
    if( m_SourceFD >= 0 )
        close(m_SourceFD);
    if( m_DestinationParentFD >= 0 )
        close(m_DestinationParentFD);
    m_Request = std::move(_rhs.m_Request);
    m_SourceFD = std::exchange(_rhs.m_SourceFD, -1);
    m_DestinationParentFD = std::exchange(_rhs.m_DestinationParentFD, -1);
    return *this;
}

CrossVolumeStagingTransportBegin::~CrossVolumeStagingTransportBegin() noexcept
{
    if( m_SourceFD >= 0 )
        close(m_SourceFD);
    if( m_DestinationParentFD >= 0 )
        close(m_DestinationParentFD);
}

CrossVolumeStagingClient::CrossVolumeStagingClient(std::shared_ptr<CrossVolumeStagingTransport> _transport) noexcept
    : m_Transport{std::move(_transport)}
{
}

bool CrossVolumeStagingClient::IsAvailable() const noexcept
{
    return m_Transport && m_Transport->IsAvailable();
}

std::expected<std::unique_ptr<CrossVolumeStagingTransaction>, ProviderConditionalCopyTransactionBeginError>
CrossVolumeStagingClient::Begin(CrossVolumeStagingRequest _request,
                                const ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker)
{
    if( IsCancelled(_cancel_checker) )
        return std::unexpected{ProviderConditionalCopyTransactionBeginError::Cancelled};
    if( !IsAvailable() )
        return std::unexpected{ProviderConditionalCopyTransactionBeginError::Unsupported};

    const auto protocol_request = ProtocolBeginRequest(_request);
    if( !protocol_request )
        return std::unexpected{protocol_request.error()};
    const int source_fd = fcntl(_request.SourceFD(), F_DUPFD_CLOEXEC, 0);
    if( source_fd < 0 )
        return std::unexpected{ProviderConditionalCopyTransactionBeginError::ProviderFailure};
    const int destination_parent_fd = fcntl(_request.DestinationParentFD(), F_DUPFD_CLOEXEC, 0);
    if( destination_parent_fd < 0 ) {
        close(source_fd);
        return std::unexpected{ProviderConditionalCopyTransactionBeginError::ProviderFailure};
    }

    std::expected<protocol::BeginResult, TransportError> reply = std::unexpected(TransportError::Failure);
    try {
        reply = m_Transport->Begin(
            CrossVolumeStagingTransportBegin{*protocol_request, source_fd, destination_parent_fd});
    } catch( ... ) {
        return std::unexpected{ProviderConditionalCopyTransactionBeginError::ProviderFailure};
    }
    if( !reply || !Matches(protocol_request->header, reply->header) || !protocol::Validate(*reply) )
        return std::unexpected{ProviderConditionalCopyTransactionBeginError::ProviderFailure};
    if( reply->disposition == protocol::BeginDisposition::Rejected )
        return std::unexpected{MapBeginFailure(reply->failure)};
    if( reply->lease.header != protocol_request->header )
        return std::unexpected{ProviderConditionalCopyTransactionBeginError::ProviderFailure};

    const protocol::CommitRequest commit_request{
        .header = protocol_request->header,
        .lease = reply->lease,
    };
    bool lease_transferred = false;
    bool terminal_request_dispatched = false;
    const auto abort_granted_lease = [&] noexcept {
        terminal_request_dispatched = true;
        return AbortLease(m_Transport,
                          protocol::AbortRequest{
                              .header = commit_request.header,
                              .lease = commit_request.lease,
                          });
    };
    const auto abort_if_untransferred = at_scope_end([&] {
        if( !lease_transferred && !terminal_request_dispatched )
            (void)abort_granted_lease();
    });
    if( IsCancelled(_cancel_checker) ) {
        if( abort_granted_lease() == ProviderConditionalCopyPublicationState::NotPublished )
            return std::unexpected{ProviderConditionalCopyTransactionBeginError::Cancelled};
        return std::unexpected{ProviderConditionalCopyTransactionBeginError::ProviderFailure};
    }

    try {
        auto transaction = std::make_unique<ClientTransaction>(m_Transport, commit_request);
        lease_transferred = true;
        return transaction;
    } catch( ... ) {
        return std::unexpected{ProviderConditionalCopyTransactionBeginError::ProviderFailure};
    }
}

} // namespace nc::vfs::native
