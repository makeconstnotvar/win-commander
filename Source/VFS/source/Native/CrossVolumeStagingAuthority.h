// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/ProviderCapabilities.h>

#include <expected>
#include <memory>
#include <string>
#include <utility>

namespace nc::vfs {
class NativeHost;
}

namespace nc::vfs::native {

/**
 * Scalar reviewed identity that can cross the Native-to-helper boundary.  It intentionally excludes
 * source, destination and staging pathnames: helper operations are descriptor-bound.
 */
struct CrossVolumeStagingObjectSeal final {
    uint64_t device{0};
    uint64_t inode{0};
    ProviderConditionalCopyTimestamp birth_time;
    uint32_t uid{0};
    uint32_t gid{0};
    uint32_t mode{0};
    uint32_t flags{0};
    uint64_t link_count{0};
    uint64_t byte_size{0};
    ProviderConditionalCopyTimestamp modification_time;
    ProviderConditionalCopyTimestamp status_change_time;

    bool operator==(const CrossVolumeStagingObjectSeal &) const noexcept = default;
};

/**
 * Borrowed descriptors anchored by NativeHost immediately before the helper request.  The helper/client must
 * duplicate them before it returns success; NativeHost retains and closes its own descriptors on every path.
 */
class CrossVolumeStagingRequest final
{
public:
    CrossVolumeStagingRequest(const CrossVolumeStagingRequest &) = delete;
    CrossVolumeStagingRequest &operator=(const CrossVolumeStagingRequest &) = delete;
    CrossVolumeStagingRequest(CrossVolumeStagingRequest &&) noexcept = default;
    CrossVolumeStagingRequest &operator=(CrossVolumeStagingRequest &&) = delete;
    ~CrossVolumeStagingRequest() = default;

    [[nodiscard]] int SourceFD() const noexcept { return m_SourceFD; }
    [[nodiscard]] int DestinationParentFD() const noexcept { return m_DestinationParentFD; }
    [[nodiscard]] const std::string &DestinationName() const noexcept { return m_DestinationName; }
    [[nodiscard]] const CrossVolumeStagingObjectSeal &SourceSeal() const noexcept { return m_SourceSeal; }
    [[nodiscard]] const CrossVolumeStagingObjectSeal &DestinationParentSeal() const noexcept
    {
        return m_DestinationParentSeal;
    }

private:
    CrossVolumeStagingRequest(int _source_fd,
                              int _destination_parent_fd,
                              std::string _destination_name,
                              CrossVolumeStagingObjectSeal _source_seal,
                              CrossVolumeStagingObjectSeal _destination_parent_seal) noexcept
        : m_SourceFD{_source_fd}, m_DestinationParentFD{_destination_parent_fd},
          m_DestinationName{std::move(_destination_name)}, m_SourceSeal{std::move(_source_seal)},
          m_DestinationParentSeal{std::move(_destination_parent_seal)}
    {
    }

    int m_SourceFD;
    int m_DestinationParentFD;
    std::string m_DestinationName;
    CrossVolumeStagingObjectSeal m_SourceSeal;
    CrossVolumeStagingObjectSeal m_DestinationParentSeal;

    friend class ::nc::vfs::NativeHost;
};

/**
 * Opaque helper-owned transaction.  Its implementation owns every protected-root lease and cleanup decision;
 * the Native provider receives only the existing conservative publication result.
 */
class CrossVolumeStagingTransaction
{
public:
    virtual ~CrossVolumeStagingTransaction() = default;

    [[nodiscard]] virtual ProviderConditionalCopyCommitResult
    Commit(const ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) noexcept = 0;
    [[nodiscard]] virtual ProviderConditionalCopyPublicationState Abort() noexcept = 0;
};

/**
 * Private seam for the future separately signed staging helper.  Availability must be side-effect free and
 * Begin must never fall back to a client-controlled named staging path.
 */
class CrossVolumeStagingAuthority
{
public:
    virtual ~CrossVolumeStagingAuthority() = default;

    [[nodiscard]] virtual bool IsAvailable() const noexcept = 0;
    [[nodiscard]] virtual std::expected<std::unique_ptr<CrossVolumeStagingTransaction>,
                                        ProviderConditionalCopyTransactionBeginError>
    Begin(CrossVolumeStagingRequest _request, const ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) = 0;
};

} // namespace nc::vfs::native
