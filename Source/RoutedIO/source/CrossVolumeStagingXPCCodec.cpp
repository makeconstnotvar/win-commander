// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
// V1 fixed grammar implementation.
#include "CrossVolumeStagingXPCCodec.h"

#include <array>
#include <cstring>
#include <limits>
#include <unistd.h>

namespace nc::routedio::cross_volume_staging::xpc_codec {
namespace {

enum class MessageKind : uint64_t {
    Begin = 1,
    Commit = 2,
    Abort = 3,
    BeginResult = 4,
    CompletionResult = 5
};

constexpr const char *kKind = "kind";
constexpr const char *kVersion = "version";
constexpr const char *kCorrelation = "correlation";
constexpr const char *kSourceSeal = "source-seal";
constexpr const char *kDestinationParentSeal = "destination-parent-seal";
constexpr const char *kDestinationName = "destination-name";
constexpr const char *kSourceFD = "source-fd";
constexpr const char *kDestinationParentFD = "destination-parent-fd";
constexpr const char *kLeaseToken = "lease-token";
constexpr const char *kDisposition = "disposition";
constexpr const char *kFailure = "failure";
constexpr const char *kPublication = "publication";
constexpr const char *kSystemError = "system-error";
constexpr const char *kFilesystemSync = "filesystem-sync";
constexpr const char *kFilesystemSyncSystemError = "filesystem-sync-system-error";
constexpr const char *kDevice = "device";
constexpr const char *kInode = "inode";
constexpr const char *kUID = "uid";
constexpr const char *kGID = "gid";
constexpr const char *kMode = "mode";
constexpr const char *kFlags = "flags";
constexpr const char *kLinkCount = "link-count";
constexpr const char *kByteSize = "byte-size";
constexpr const char *kBirthTime = "birth-time";
constexpr const char *kModificationTime = "modification-time";
constexpr const char *kStatusChangeTime = "status-change-time";
constexpr const char *kSeconds = "seconds";
constexpr const char *kNanoseconds = "nanoseconds";

constexpr std::array<const char *, 8> kBeginFields{
    kKind, kVersion, kCorrelation, kSourceSeal, kDestinationParentSeal,
    kDestinationName, kSourceFD, kDestinationParentFD,
};
constexpr std::array<const char *, 4> kLeaseRequestFields{
    kKind,
    kVersion,
    kCorrelation,
    kLeaseToken,
};
constexpr std::array<const char *, 6> kBeginResultFields{
    kKind,
    kVersion,
    kCorrelation,
    kDisposition,
    kFailure,
    kLeaseToken,
};
constexpr std::array<const char *, 8> kCompletionResultFields{
    kKind,
    kVersion,
    kCorrelation,
    kPublication,
    kFailure,
    kSystemError,
    kFilesystemSync,
    kFilesystemSyncSystemError,
};
constexpr std::array<const char *, 11> kSealFields{
    kDevice,
    kInode,
    kUID,
    kGID,
    kMode,
    kFlags,
    kLinkCount,
    kByteSize,
    kBirthTime,
    kModificationTime,
    kStatusChangeTime,
};
constexpr std::array<const char *, 2> kTimestampFields{
    kSeconds,
    kNanoseconds,
};

class ScopedXPCObject final
{
public:
    explicit ScopedXPCObject(xpc_object_t _object) noexcept : m_Object{_object} {}
    ScopedXPCObject(const ScopedXPCObject &) = delete;
    ScopedXPCObject &operator=(const ScopedXPCObject &) = delete;
    ~ScopedXPCObject()
    {
        if( m_Object != nullptr )
            xpc_release(m_Object);
    }

    [[nodiscard]] xpc_object_t Get() const noexcept { return m_Object; }
    [[nodiscard]] xpc_object_t Release() noexcept { return std::exchange(m_Object, nullptr); }

private:
    xpc_object_t m_Object;
};

template <size_t Count>
bool HasExactFields(xpc_object_t _dictionary, const std::array<const char *, Count> &_fields) noexcept
{
    if( xpc_get_type(_dictionary) != XPC_TYPE_DICTIONARY || xpc_dictionary_get_count(_dictionary) != Count )
        return false;

    __block bool valid = true;
    xpc_dictionary_apply(_dictionary, ^bool(const char *_key, xpc_object_t) {
      bool known = false;
      for( const char *field : _fields ) {
          if( std::strcmp(_key, field) == 0 ) {
              known = true;
              break;
          }
      }
      valid = known;
      return known;
    });
    return valid;
}

xpc_object_t ValueWithType(xpc_object_t _dictionary, const char *_key, xpc_type_t _type) noexcept
{
    xpc_object_t value = xpc_dictionary_get_value(_dictionary, _key);
    return value != nullptr && xpc_get_type(value) == _type ? value : nullptr;
}

std::expected<uint64_t, Error> UInt64Value(xpc_object_t _dictionary, const char *_key) noexcept
{
    if( ValueWithType(_dictionary, _key, XPC_TYPE_UINT64) == nullptr )
        return std::unexpected{Error::WrongType};
    return xpc_dictionary_get_uint64(_dictionary, _key);
}

std::expected<int64_t, Error> Int64Value(xpc_object_t _dictionary, const char *_key) noexcept
{
    if( ValueWithType(_dictionary, _key, XPC_TYPE_INT64) == nullptr )
        return std::unexpected{Error::WrongType};
    return xpc_dictionary_get_int64(_dictionary, _key);
}

template <size_t Count>
std::expected<std::array<uint8_t, Count>, Error> FixedDataValue(xpc_object_t _dictionary,
                                                                 const char *_key) noexcept
{
    xpc_object_t value = ValueWithType(_dictionary, _key, XPC_TYPE_DATA);
    if( value == nullptr )
        return std::unexpected{Error::WrongType};
    if( xpc_data_get_length(value) != Count )
        return std::unexpected{Error::OutOfRange};

    std::array<uint8_t, Count> result;
    std::memcpy(result.data(), xpc_data_get_bytes_ptr(value), Count);
    return result;
}

std::expected<Header, Error> DecodeHeader(xpc_object_t _dictionary, const MessageKind _kind) noexcept
{
    const auto kind = UInt64Value(_dictionary, kKind);
    if( !kind || *kind != static_cast<uint64_t>(_kind) )
        return std::unexpected{Error::InvalidProtocolValue};
    const auto version = UInt64Value(_dictionary, kVersion);
    if( !version || *version > std::numeric_limits<uint32_t>::max() )
        return std::unexpected{Error::OutOfRange};
    const auto correlation = FixedDataValue<kCorrelationBytes>(_dictionary, kCorrelation);
    if( !correlation )
        return std::unexpected{correlation.error()};

    const Header header{
        .version = static_cast<uint32_t>(*version),
        .correlation = *correlation,
    };
    if( !Validate(header) )
        return std::unexpected{Error::InvalidProtocolValue};
    return header;
}

xpc_object_t EncodeTimestamp(const Timestamp &_timestamp) noexcept
{
    xpc_object_t dictionary = xpc_dictionary_create(nullptr, nullptr, 0);
    if( dictionary == nullptr )
        return nullptr;
    xpc_dictionary_set_int64(dictionary, kSeconds, _timestamp.seconds);
    xpc_dictionary_set_uint64(dictionary, kNanoseconds, _timestamp.nanoseconds);
    return dictionary;
}

std::expected<Timestamp, Error> DecodeTimestamp(xpc_object_t _object) noexcept
{
    if( !HasExactFields(_object, kTimestampFields) )
        return std::unexpected{Error::UnknownMember};
    const auto seconds = Int64Value(_object, kSeconds);
    const auto nanoseconds = UInt64Value(_object, kNanoseconds);
    if( !seconds || !nanoseconds )
        return std::unexpected{!seconds ? seconds.error() : nanoseconds.error()};
    if( *nanoseconds > std::numeric_limits<uint32_t>::max() )
        return std::unexpected{Error::OutOfRange};

    return Timestamp{
        .seconds = *seconds,
        .nanoseconds = static_cast<uint32_t>(*nanoseconds),
    };
}

xpc_object_t EncodeSeal(const ObjectSeal &_seal) noexcept
{
    xpc_object_t dictionary = xpc_dictionary_create(nullptr, nullptr, 0);
    if( dictionary == nullptr )
        return nullptr;
    xpc_dictionary_set_uint64(dictionary, kDevice, _seal.device);
    xpc_dictionary_set_uint64(dictionary, kInode, _seal.inode);
    xpc_dictionary_set_uint64(dictionary, kUID, _seal.uid);
    xpc_dictionary_set_uint64(dictionary, kGID, _seal.gid);
    xpc_dictionary_set_uint64(dictionary, kMode, _seal.mode);
    xpc_dictionary_set_uint64(dictionary, kFlags, _seal.flags);
    xpc_dictionary_set_uint64(dictionary, kLinkCount, _seal.link_count);
    xpc_dictionary_set_uint64(dictionary, kByteSize, _seal.byte_size);

    ScopedXPCObject birth_time{EncodeTimestamp(_seal.birth_time)};
    ScopedXPCObject modification_time{EncodeTimestamp(_seal.modification_time)};
    ScopedXPCObject status_change_time{EncodeTimestamp(_seal.status_change_time)};
    if( birth_time.Get() == nullptr || modification_time.Get() == nullptr || status_change_time.Get() == nullptr ) {
        xpc_release(dictionary);
        return nullptr;
    }
    xpc_dictionary_set_value(dictionary, kBirthTime, birth_time.Get());
    xpc_dictionary_set_value(dictionary, kModificationTime, modification_time.Get());
    xpc_dictionary_set_value(dictionary, kStatusChangeTime, status_change_time.Get());
    return dictionary;
}

std::expected<ObjectSeal, Error> DecodeSeal(xpc_object_t _object) noexcept
{
    if( !HasExactFields(_object, kSealFields) )
        return std::unexpected{Error::UnknownMember};

    const auto device = UInt64Value(_object, kDevice);
    const auto inode = UInt64Value(_object, kInode);
    const auto uid = UInt64Value(_object, kUID);
    const auto gid = UInt64Value(_object, kGID);
    const auto mode = UInt64Value(_object, kMode);
    const auto flags = UInt64Value(_object, kFlags);
    const auto link_count = UInt64Value(_object, kLinkCount);
    const auto byte_size = UInt64Value(_object, kByteSize);
    if( !device || !inode || !uid || !gid || !mode || !flags || !link_count || !byte_size )
        return std::unexpected{Error::WrongType};
    if( *uid > std::numeric_limits<uint32_t>::max() || *gid > std::numeric_limits<uint32_t>::max() ||
        *mode > std::numeric_limits<uint32_t>::max() || *flags > std::numeric_limits<uint32_t>::max() )
        return std::unexpected{Error::OutOfRange};

    xpc_object_t birth_time_value = ValueWithType(_object, kBirthTime, XPC_TYPE_DICTIONARY);
    xpc_object_t modification_time_value = ValueWithType(_object, kModificationTime, XPC_TYPE_DICTIONARY);
    xpc_object_t status_change_time_value = ValueWithType(_object, kStatusChangeTime, XPC_TYPE_DICTIONARY);
    if( birth_time_value == nullptr || modification_time_value == nullptr || status_change_time_value == nullptr )
        return std::unexpected{Error::WrongType};
    const auto birth_time = DecodeTimestamp(birth_time_value);
    const auto modification_time = DecodeTimestamp(modification_time_value);
    const auto status_change_time = DecodeTimestamp(status_change_time_value);
    if( !birth_time || !modification_time || !status_change_time )
        return std::unexpected{Error::InvalidProtocolValue};

    return ObjectSeal{
        .device = *device,
        .inode = *inode,
        .uid = static_cast<uint32_t>(*uid),
        .gid = static_cast<uint32_t>(*gid),
        .mode = static_cast<uint32_t>(*mode),
        .flags = static_cast<uint32_t>(*flags),
        .link_count = *link_count,
        .byte_size = *byte_size,
        .birth_time = *birth_time,
        .modification_time = *modification_time,
        .status_change_time = *status_change_time,
    };
}

std::expected<xpc_object_t, Error> EncodeEnvelope(const MessageKind _kind, const Header &_header) noexcept
{
    xpc_object_t dictionary = xpc_dictionary_create(nullptr, nullptr, 0);
    if( dictionary == nullptr )
        return std::unexpected{Error::AllocationFailed};
    xpc_dictionary_set_uint64(dictionary, kKind, static_cast<uint64_t>(_kind));
    xpc_dictionary_set_uint64(dictionary, kVersion, _header.version);
    xpc_dictionary_set_data(dictionary, kCorrelation, _header.correlation.data(), _header.correlation.size());
    return dictionary;
}

std::expected<void, Error> PopulateEnvelope(xpc_object_t _dictionary,
                                            const MessageKind _kind,
                                            const Header &_header) noexcept
{
    if( xpc_get_type(_dictionary) != XPC_TYPE_DICTIONARY || xpc_dictionary_get_count(_dictionary) != 0 )
        return std::unexpected{Error::InvalidArgument};
    xpc_dictionary_set_uint64(_dictionary, kKind, static_cast<uint64_t>(_kind));
    xpc_dictionary_set_uint64(_dictionary, kVersion, _header.version);
    xpc_dictionary_set_data(_dictionary, kCorrelation, _header.correlation.data(), _header.correlation.size());
    return {};
}

template <typename Request>
std::expected<xpc_object_t, Error> EncodeLeaseRequest(const Request &_request, const MessageKind _kind) noexcept
{
    if( !Validate(_request) )
        return std::unexpected{Error::InvalidArgument};
    auto dictionary = EncodeEnvelope(_kind, _request.header);
    if( !dictionary )
        return std::unexpected{dictionary.error()};
    xpc_dictionary_set_data(*dictionary, kLeaseToken, _request.lease.token.bytes.data(), _request.lease.token.bytes.size());
    return *dictionary;
}

template <typename Request>
std::expected<Request, Error> DecodeLeaseRequest(xpc_object_t _dictionary, const MessageKind _kind) noexcept
{
    if( xpc_get_type(_dictionary) != XPC_TYPE_DICTIONARY )
        return std::unexpected{Error::NotDictionary};
    if( !HasExactFields(_dictionary, kLeaseRequestFields) )
        return std::unexpected{Error::UnknownMember};
    const auto header = DecodeHeader(_dictionary, _kind);
    const auto token = FixedDataValue<kLeaseTokenBytes>(_dictionary, kLeaseToken);
    if( !header || !token )
        return std::unexpected{!header ? header.error() : token.error()};
    Request request{
        .header = *header,
        .lease = Lease{.header = *header, .token = LeaseToken{.bytes = *token}},
    };
    if( !Validate(request) )
        return std::unexpected{Error::InvalidProtocolValue};
    return request;
}

template <typename Enum>
std::expected<Enum, Error> DecodeEnum(xpc_object_t _dictionary, const char *_key, const uint64_t _count) noexcept
{
    const auto value = UInt64Value(_dictionary, _key);
    if( !value )
        return std::unexpected{value.error()};
    if( *value >= _count )
        return std::unexpected{Error::OutOfRange};
    return static_cast<Enum>(*value);
}

} // namespace

OwnedBeginDescriptors::OwnedBeginDescriptors(OwnedBeginDescriptors &&_rhs) noexcept
    : m_SourceFD{std::exchange(_rhs.m_SourceFD, -1)},
      m_DestinationParentFD{std::exchange(_rhs.m_DestinationParentFD, -1)}
{
}

OwnedBeginDescriptors &OwnedBeginDescriptors::operator=(OwnedBeginDescriptors &&_rhs) noexcept
{
    if( this == &_rhs )
        return *this;
    if( m_SourceFD >= 0 )
        close(m_SourceFD);
    if( m_DestinationParentFD >= 0 )
        close(m_DestinationParentFD);
    m_SourceFD = std::exchange(_rhs.m_SourceFD, -1);
    m_DestinationParentFD = std::exchange(_rhs.m_DestinationParentFD, -1);
    return *this;
}

OwnedBeginDescriptors::~OwnedBeginDescriptors() noexcept
{
    if( m_SourceFD >= 0 )
        close(m_SourceFD);
    if( m_DestinationParentFD >= 0 )
        close(m_DestinationParentFD);
}

std::expected<xpc_object_t, Error> EncodeBegin(const BeginRequest &_request,
                                                const BorrowedBeginDescriptors _descriptors) noexcept
{
    if( !Validate(_request) || _descriptors.source_fd < 0 || _descriptors.destination_parent_fd < 0 ||
        _descriptors.source_fd == _descriptors.destination_parent_fd )
        return std::unexpected{Error::InvalidArgument};

    auto dictionary = EncodeEnvelope(MessageKind::Begin, _request.header);
    if( !dictionary )
        return std::unexpected{dictionary.error()};
    ScopedXPCObject source_seal{EncodeSeal(_request.source)};
    ScopedXPCObject destination_parent_seal{EncodeSeal(_request.destination_parent)};
    if( source_seal.Get() == nullptr || destination_parent_seal.Get() == nullptr ) {
        xpc_release(*dictionary);
        return std::unexpected{Error::AllocationFailed};
    }

    xpc_dictionary_set_value(*dictionary, kSourceSeal, source_seal.Get());
    xpc_dictionary_set_value(*dictionary, kDestinationParentSeal, destination_parent_seal.Get());
    const auto destination_name = _request.destination_name.Bytes();
    xpc_dictionary_set_data(*dictionary, kDestinationName, destination_name.data(), destination_name.size());
    xpc_dictionary_set_fd(*dictionary, kSourceFD, _descriptors.source_fd);
    xpc_dictionary_set_fd(*dictionary, kDestinationParentFD, _descriptors.destination_parent_fd);
    return *dictionary;
}

std::expected<DecodedBegin, Error> DecodeBegin(xpc_object_t _dictionary) noexcept
{
    if( xpc_get_type(_dictionary) != XPC_TYPE_DICTIONARY )
        return std::unexpected{Error::NotDictionary};
    if( !HasExactFields(_dictionary, kBeginFields) )
        return std::unexpected{Error::UnknownMember};

    const auto header = DecodeHeader(_dictionary, MessageKind::Begin);
    xpc_object_t source_seal_value = ValueWithType(_dictionary, kSourceSeal, XPC_TYPE_DICTIONARY);
    xpc_object_t destination_parent_seal_value = ValueWithType(_dictionary, kDestinationParentSeal, XPC_TYPE_DICTIONARY);
    xpc_object_t destination_name_value = ValueWithType(_dictionary, kDestinationName, XPC_TYPE_DATA);
    if( !header || source_seal_value == nullptr || destination_parent_seal_value == nullptr ||
        destination_name_value == nullptr )
        return std::unexpected{!header ? header.error() : Error::WrongType};
    const auto source_seal = DecodeSeal(source_seal_value);
    const auto destination_parent_seal = DecodeSeal(destination_parent_seal_value);
    if( !source_seal || !destination_parent_seal )
        return std::unexpected{Error::InvalidProtocolValue};
    const auto destination_name = DestinationComponent::Create(std::span<const uint8_t>{
        static_cast<const uint8_t *>(xpc_data_get_bytes_ptr(destination_name_value)),
        xpc_data_get_length(destination_name_value)});
    if( !destination_name )
        return std::unexpected{Error::InvalidProtocolValue};

    if( ValueWithType(_dictionary, kSourceFD, XPC_TYPE_FD) == nullptr ||
        ValueWithType(_dictionary, kDestinationParentFD, XPC_TYPE_FD) == nullptr )
        return std::unexpected{Error::InvalidDescriptor};
    const BeginRequest request{
        .header = *header,
        .source = *source_seal,
        .destination_parent = *destination_parent_seal,
        .destination_name = *destination_name,
    };
    if( !Validate(request) )
        return std::unexpected{Error::InvalidProtocolValue};

    const int source_fd = xpc_dictionary_dup_fd(_dictionary, kSourceFD);
    if( source_fd < 0 )
        return std::unexpected{Error::DescriptorDuplicationFailed};
    const int destination_parent_fd = xpc_dictionary_dup_fd(_dictionary, kDestinationParentFD);
    if( destination_parent_fd < 0 ) {
        close(source_fd);
        return std::unexpected{Error::DescriptorDuplicationFailed};
    }
    if( source_fd == destination_parent_fd ) {
        close(source_fd);
        close(destination_parent_fd);
        return std::unexpected{Error::InvalidDescriptor};
    }
    return DecodedBegin{request, OwnedBeginDescriptors{source_fd, destination_parent_fd}};
}

std::expected<RequestKind, Error> DecodeRequestKind(xpc_object_t _dictionary) noexcept
{
    if( xpc_get_type(_dictionary) != XPC_TYPE_DICTIONARY )
        return std::unexpected{Error::NotDictionary};
    const auto kind = UInt64Value(_dictionary, kKind);
    if( !kind )
        return std::unexpected{kind.error()};
    switch( static_cast<MessageKind>(*kind) ) {
        case MessageKind::Begin:
            return RequestKind::Begin;
        case MessageKind::Commit:
            return RequestKind::Commit;
        case MessageKind::Abort:
            return RequestKind::Abort;
        case MessageKind::BeginResult:
        case MessageKind::CompletionResult:
            return std::unexpected{Error::InvalidProtocolValue};
    }
    return std::unexpected{Error::InvalidProtocolValue};
}

std::expected<xpc_object_t, Error> EncodeCommit(const CommitRequest &_request) noexcept
{
    return EncodeLeaseRequest(_request, MessageKind::Commit);
}

std::expected<CommitRequest, Error> DecodeCommit(xpc_object_t _dictionary) noexcept
{
    return DecodeLeaseRequest<CommitRequest>(_dictionary, MessageKind::Commit);
}

std::expected<xpc_object_t, Error> EncodeAbort(const AbortRequest &_request) noexcept
{
    return EncodeLeaseRequest(_request, MessageKind::Abort);
}

std::expected<AbortRequest, Error> DecodeAbort(xpc_object_t _dictionary) noexcept
{
    return DecodeLeaseRequest<AbortRequest>(_dictionary, MessageKind::Abort);
}

std::expected<xpc_object_t, Error> EncodeBeginResult(const BeginResult &_result) noexcept
{
    if( !Validate(_result) )
        return std::unexpected{Error::InvalidArgument};
    xpc_object_t dictionary = xpc_dictionary_create(nullptr, nullptr, 0);
    if( dictionary == nullptr )
        return std::unexpected{Error::AllocationFailed};
    const auto populated = PopulateBeginResultReply(dictionary, _result);
    if( !populated ) {
        xpc_release(dictionary);
        return std::unexpected{populated.error()};
    }
    return dictionary;
}

std::expected<BeginResult, Error> DecodeBeginResult(xpc_object_t _dictionary) noexcept
{
    if( xpc_get_type(_dictionary) != XPC_TYPE_DICTIONARY )
        return std::unexpected{Error::NotDictionary};
    if( !HasExactFields(_dictionary, kBeginResultFields) )
        return std::unexpected{Error::UnknownMember};
    const auto header = DecodeHeader(_dictionary, MessageKind::BeginResult);
    const auto disposition = DecodeEnum<BeginDisposition>(_dictionary, kDisposition, 2);
    const auto failure = DecodeEnum<BeginFailure>(_dictionary, kFailure, 8);
    const auto token = FixedDataValue<kLeaseTokenBytes>(_dictionary, kLeaseToken);
    if( !header || !disposition || !failure || !token )
        return std::unexpected{Error::InvalidProtocolValue};
    const BeginResult result{
        .header = *header,
        .disposition = *disposition,
        .failure = *failure,
        .lease = Lease{.header = *header, .token = LeaseToken{.bytes = *token}},
    };
    if( !Validate(result) )
        return std::unexpected{Error::InvalidProtocolValue};
    return result;
}

std::expected<void, Error> PopulateBeginResultReply(xpc_object_t _reply, const BeginResult &_result) noexcept
{
    if( !Validate(_result) )
        return std::unexpected{Error::InvalidArgument};
    const auto envelope = PopulateEnvelope(_reply, MessageKind::BeginResult, _result.header);
    if( !envelope )
        return std::unexpected{envelope.error()};
    xpc_dictionary_set_uint64(_reply, kDisposition, static_cast<uint64_t>(_result.disposition));
    xpc_dictionary_set_uint64(_reply, kFailure, static_cast<uint64_t>(_result.failure));
    xpc_dictionary_set_data(_reply, kLeaseToken, _result.lease.token.bytes.data(), _result.lease.token.bytes.size());
    return {};
}

std::expected<xpc_object_t, Error> EncodeCompletionResult(const CompletionResult &_result) noexcept
{
    if( !Validate(_result) )
        return std::unexpected{Error::InvalidArgument};
    xpc_object_t dictionary = xpc_dictionary_create(nullptr, nullptr, 0);
    if( dictionary == nullptr )
        return std::unexpected{Error::AllocationFailed};
    const auto populated = PopulateCompletionResultReply(dictionary, _result);
    if( !populated ) {
        xpc_release(dictionary);
        return std::unexpected{populated.error()};
    }
    return dictionary;
}

std::expected<CompletionResult, Error> DecodeCompletionResult(xpc_object_t _dictionary) noexcept
{
    if( xpc_get_type(_dictionary) != XPC_TYPE_DICTIONARY )
        return std::unexpected{Error::NotDictionary};
    if( !HasExactFields(_dictionary, kCompletionResultFields) )
        return std::unexpected{Error::UnknownMember};
    const auto header = DecodeHeader(_dictionary, MessageKind::CompletionResult);
    const auto publication = DecodeEnum<Publication>(_dictionary, kPublication, 3);
    const auto failure = DecodeEnum<CompletionFailure>(_dictionary, kFailure, 9);
    const auto system_error = Int64Value(_dictionary, kSystemError);
    const auto filesystem_sync = DecodeEnum<FilesystemSync>(_dictionary, kFilesystemSync, 3);
    const auto filesystem_sync_system_error = Int64Value(_dictionary, kFilesystemSyncSystemError);
    if( !header || !publication || !failure || !system_error || !filesystem_sync || !filesystem_sync_system_error )
        return std::unexpected{Error::InvalidProtocolValue};
    if( *system_error < std::numeric_limits<int32_t>::min() ||
        *system_error > std::numeric_limits<int32_t>::max() ||
        *filesystem_sync_system_error < std::numeric_limits<int32_t>::min() ||
        *filesystem_sync_system_error > std::numeric_limits<int32_t>::max() )
        return std::unexpected{Error::OutOfRange};
    const CompletionResult result{
        .header = *header,
        .publication = *publication,
        .failure = *failure,
        .system_error = static_cast<int32_t>(*system_error),
        .filesystem_sync = *filesystem_sync,
        .filesystem_sync_system_error = static_cast<int32_t>(*filesystem_sync_system_error),
    };
    if( !Validate(result) )
        return std::unexpected{Error::InvalidProtocolValue};
    return result;
}

std::expected<void, Error> PopulateCompletionResultReply(xpc_object_t _reply,
                                                          const CompletionResult &_result) noexcept
{
    if( !Validate(_result) )
        return std::unexpected{Error::InvalidArgument};
    const auto envelope = PopulateEnvelope(_reply, MessageKind::CompletionResult, _result.header);
    if( !envelope )
        return std::unexpected{envelope.error()};
    xpc_dictionary_set_uint64(_reply, kPublication, static_cast<uint64_t>(_result.publication));
    xpc_dictionary_set_uint64(_reply, kFailure, static_cast<uint64_t>(_result.failure));
    xpc_dictionary_set_int64(_reply, kSystemError, _result.system_error);
    xpc_dictionary_set_uint64(_reply, kFilesystemSync, static_cast<uint64_t>(_result.filesystem_sync));
    xpc_dictionary_set_int64(_reply, kFilesystemSyncSystemError, _result.filesystem_sync_system_error);
    return {};
}

} // namespace nc::routedio::cross_volume_staging::xpc_codec
