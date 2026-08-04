// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "CrossVolumeStagingAuthority.h"

#include <RoutedIO/CrossVolumeStagingProtocol.h>

#include <cstdint>
#include <expected>
#include <memory>

namespace nc::vfs::native {

namespace staging_protocol = nc::routedio::cross_volume_staging;

enum class CrossVolumeStagingTransportError : uint8_t {
    Unavailable,
    Failure
};

/** Move-only duplicated descriptor rights owned by the narrow helper transport after Native Begin returns. */
class CrossVolumeStagingTransportBegin final
{
public:
    CrossVolumeStagingTransportBegin(const CrossVolumeStagingTransportBegin &) = delete;
    CrossVolumeStagingTransportBegin &operator=(const CrossVolumeStagingTransportBegin &) = delete;
    CrossVolumeStagingTransportBegin(CrossVolumeStagingTransportBegin &&_rhs) noexcept;
    CrossVolumeStagingTransportBegin &operator=(CrossVolumeStagingTransportBegin &&_rhs) noexcept;
    ~CrossVolumeStagingTransportBegin() noexcept;

    [[nodiscard]] const staging_protocol::BeginRequest &Request() const noexcept { return m_Request; }
    [[nodiscard]] int SourceFD() const noexcept { return m_SourceFD; }
    [[nodiscard]] int DestinationParentFD() const noexcept { return m_DestinationParentFD; }

private:
    CrossVolumeStagingTransportBegin(staging_protocol::BeginRequest _request,
                                     int _source_fd,
                                     int _destination_parent_fd) noexcept;

    staging_protocol::BeginRequest m_Request;
    int m_SourceFD;
    int m_DestinationParentFD;

    friend class CrossVolumeStagingClient;
};

/**
 * Process-local port for the dedicated V1 helper.  A concrete XPC transport will encode only this contract; it
 * never receives a VFS pathname, legacy RoutedIO connection or direct POSIX fallback authority.
 */
class CrossVolumeStagingTransport
{
public:
    virtual ~CrossVolumeStagingTransport() = default;

    [[nodiscard]] virtual bool IsAvailable() const noexcept = 0;
    [[nodiscard]] virtual std::expected<staging_protocol::BeginResult, CrossVolumeStagingTransportError>
    Begin(CrossVolumeStagingTransportBegin _request) = 0;
    [[nodiscard]] virtual std::expected<staging_protocol::CompletionResult, CrossVolumeStagingTransportError>
    Commit(const staging_protocol::CommitRequest &_request) = 0;
    [[nodiscard]] virtual std::expected<staging_protocol::CompletionResult, CrossVolumeStagingTransportError>
    Abort(const staging_protocol::AbortRequest &_request) = 0;
};

/**
 * VFS adapter for the separately signed staging authority.  Production does not install it until the concrete
 * XPC transport, helper target and signing/package trust proof are complete.
 */
class CrossVolumeStagingClient final : public CrossVolumeStagingAuthority
{
public:
    explicit CrossVolumeStagingClient(std::shared_ptr<CrossVolumeStagingTransport> _transport) noexcept;

    [[nodiscard]] bool IsAvailable() const noexcept override;
    [[nodiscard]] std::expected<std::unique_ptr<CrossVolumeStagingTransaction>,
                                ProviderConditionalCopyTransactionBeginError>
    Begin(CrossVolumeStagingRequest _request,
          const ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) override;

private:
    std::shared_ptr<CrossVolumeStagingTransport> m_Transport;
};

} // namespace nc::vfs::native
