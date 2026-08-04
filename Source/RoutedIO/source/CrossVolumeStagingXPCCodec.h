// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <RoutedIO/CrossVolumeStagingProtocol.h>

#include <expected>
#include <utility>
#include <xpc/xpc.h>

namespace nc::routedio::cross_volume_staging::xpc_codec {

/** Wire grammar failures are transport-local and never authorize a local fallback. */
enum class Error : uint8_t {
    InvalidArgument,
    AllocationFailed,
    NotDictionary,
    UnknownMember,
    MissingMember,
    WrongType,
    OutOfRange,
    InvalidProtocolValue,
    InvalidDescriptor,
    DescriptorDuplicationFailed
};

/** The listener may dispatch only these three request envelopes.  Result envelopes are never client requests. */
enum class RequestKind : uint8_t {
    Begin,
    Commit,
    Abort
};

/** Borrowed Native descriptors; EncodeBegin duplicates them into the outgoing XPC dictionary. */
struct BorrowedBeginDescriptors final {
    int source_fd{-1};
    int destination_parent_fd{-1};
};

/** Move-only descriptor rights duplicated from a decoded Begin dictionary. */
class OwnedBeginDescriptors final
{
public:
    OwnedBeginDescriptors() = delete;
    OwnedBeginDescriptors(const OwnedBeginDescriptors &) = delete;
    OwnedBeginDescriptors &operator=(const OwnedBeginDescriptors &) = delete;
    OwnedBeginDescriptors(OwnedBeginDescriptors &&_rhs) noexcept;
    OwnedBeginDescriptors &operator=(OwnedBeginDescriptors &&_rhs) noexcept;
    ~OwnedBeginDescriptors() noexcept;

    [[nodiscard]] int SourceFD() const noexcept { return m_SourceFD; }
    [[nodiscard]] int DestinationParentFD() const noexcept { return m_DestinationParentFD; }
    [[nodiscard]] BorrowedBeginDescriptors Borrow() const noexcept
    {
        return BorrowedBeginDescriptors{.source_fd = m_SourceFD, .destination_parent_fd = m_DestinationParentFD};
    }

private:
    OwnedBeginDescriptors(int _source_fd, int _destination_parent_fd) noexcept
        : m_SourceFD{_source_fd}, m_DestinationParentFD{_destination_parent_fd}
    {
    }

    int m_SourceFD;
    int m_DestinationParentFD;

    friend std::expected<struct DecodedBegin, Error> DecodeBegin(xpc_object_t _dictionary) noexcept;
};

struct DecodedBegin final {
    BeginRequest request;
    OwnedBeginDescriptors descriptors;

    DecodedBegin(BeginRequest _request, OwnedBeginDescriptors _descriptors) noexcept
        : request{std::move(_request)}, descriptors{std::move(_descriptors)}
    {
    }

    DecodedBegin(const DecodedBegin &) = delete;
    DecodedBegin &operator=(const DecodedBegin &) = delete;
    DecodedBegin(DecodedBegin &&) noexcept = default;
    DecodedBegin &operator=(DecodedBegin &&) noexcept = default;
};

/** Each Encode function returns a retained dictionary that the caller releases with xpc_release(). */
[[nodiscard]] std::expected<xpc_object_t, Error> EncodeBegin(const BeginRequest &_request,
                                                              BorrowedBeginDescriptors _descriptors) noexcept;
[[nodiscard]] std::expected<DecodedBegin, Error> DecodeBegin(xpc_object_t _dictionary) noexcept;

/** Reads only the fixed numeric discriminator.  Call the matching full decoder before taking any action. */
[[nodiscard]] std::expected<RequestKind, Error> DecodeRequestKind(xpc_object_t _dictionary) noexcept;

[[nodiscard]] std::expected<xpc_object_t, Error> EncodeCommit(const CommitRequest &_request) noexcept;
[[nodiscard]] std::expected<CommitRequest, Error> DecodeCommit(xpc_object_t _dictionary) noexcept;

[[nodiscard]] std::expected<xpc_object_t, Error> EncodeAbort(const AbortRequest &_request) noexcept;
[[nodiscard]] std::expected<AbortRequest, Error> DecodeAbort(xpc_object_t _dictionary) noexcept;

[[nodiscard]] std::expected<xpc_object_t, Error> EncodeBeginResult(const BeginResult &_result) noexcept;
[[nodiscard]] std::expected<BeginResult, Error> DecodeBeginResult(xpc_object_t _dictionary) noexcept;
/** Populates an empty reply dictionary created by xpc_dictionary_create_reply(). */
[[nodiscard]] std::expected<void, Error> PopulateBeginResultReply(xpc_object_t _reply,
                                                                    const BeginResult &_result) noexcept;

[[nodiscard]] std::expected<xpc_object_t, Error> EncodeCompletionResult(const CompletionResult &_result) noexcept;
[[nodiscard]] std::expected<CompletionResult, Error> DecodeCompletionResult(xpc_object_t _dictionary) noexcept;
/** Populates an empty reply dictionary created by xpc_dictionary_create_reply(). */
[[nodiscard]] std::expected<void, Error> PopulateCompletionResultReply(xpc_object_t _reply,
                                                                         const CompletionResult &_result) noexcept;

} // namespace nc::routedio::cross_volume_staging::xpc_codec
