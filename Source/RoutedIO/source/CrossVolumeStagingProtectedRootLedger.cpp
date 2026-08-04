// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CrossVolumeStagingProtectedRootLedger.h"

#include <Security/SecRandom.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace nc::routedio::cross_volume_staging::helper {
namespace {

constexpr std::string_view kRecordPrefix = ".wc-cross-volume-record-";
constexpr std::string_view kRecordSuffix = ".manifest";
constexpr std::string_view kSealManifestPrefix = ".wc-cross-volume-seal-";
constexpr std::string_view kArtifactPrefix = ".wc-cross-volume-artifact-";
constexpr std::string_view kArtifactSuffix = ".data";
constexpr std::string_view kSchema = "wincommander-cross-volume-ledger-v1";
constexpr size_t kRecordMaximumBytes = 1024;
constexpr size_t kRandomAttempts = 4;
constexpr size_t kArtifactHexBytes = 64;
constexpr size_t kMaximumOpenProtectedRoots = 32;

struct RootIdentity final {
    uint64_t device{0};
    uint64_t inode{0};

    bool operator==(const RootIdentity &) const noexcept = default;
};

pthread_mutex_t g_ProtectedRootRegistryMutex = PTHREAD_MUTEX_INITIALIZER;
std::array<RootIdentity, kMaximumOpenProtectedRoots> g_ProtectedRootRegistry;
std::array<bool, kMaximumOpenProtectedRoots> g_ProtectedRootRegistryOccupied{};

class ScopedRootRegistryLock final
{
public:
    ScopedRootRegistryLock() noexcept : m_Locked{pthread_mutex_lock(&g_ProtectedRootRegistryMutex) == 0} {}
    ~ScopedRootRegistryLock() noexcept
    {
        if( m_Locked )
            pthread_mutex_unlock(&g_ProtectedRootRegistryMutex);
    }

    [[nodiscard]] bool IsLocked() const noexcept { return m_Locked; }

private:
    bool m_Locked;
};

std::expected<void, ProtectedRootLedger::Error> RegisterProtectedRoot(const RootIdentity _identity) noexcept
{
    ScopedRootRegistryLock lock;
    if( !lock.IsLocked() )
        return std::unexpected{ProtectedRootLedger::Error::RootRegistryFull};
    size_t slot = kMaximumOpenProtectedRoots;
    for( size_t index = 0; index != g_ProtectedRootRegistry.size(); ++index ) {
        if( g_ProtectedRootRegistryOccupied[index] && g_ProtectedRootRegistry[index] == _identity )
            return std::unexpected{ProtectedRootLedger::Error::RootBusy};
        if( !g_ProtectedRootRegistryOccupied[index] && slot == kMaximumOpenProtectedRoots )
            slot = index;
    }
    if( slot == kMaximumOpenProtectedRoots )
        return std::unexpected{ProtectedRootLedger::Error::RootRegistryFull};
    g_ProtectedRootRegistry[slot] = _identity;
    g_ProtectedRootRegistryOccupied[slot] = true;
    return {};
}

void UnregisterProtectedRoot(const RootIdentity _identity) noexcept
{
    ScopedRootRegistryLock lock;
    if( !lock.IsLocked() )
        return;
    for( size_t index = 0; index != g_ProtectedRootRegistry.size(); ++index ) {
        if( g_ProtectedRootRegistryOccupied[index] && g_ProtectedRootRegistry[index] == _identity ) {
            g_ProtectedRootRegistryOccupied[index] = false;
            return;
        }
    }
}

bool IsAllZero(const std::array<uint8_t, 32> &_bytes) noexcept
{
    for( const auto byte : _bytes ) {
        if( byte != 0 )
            return false;
    }
    return true;
}

char HexDigit(const uint8_t _value) noexcept
{
    return _value < 10 ? static_cast<char>('0' + _value) : static_cast<char>('a' + (_value - 10));
}

template <size_t Count>
void EncodeHex(const std::array<uint8_t, Count> &_bytes, char *_out) noexcept
{
    for( size_t index = 0; index != Count; ++index ) {
        _out[index * 2] = HexDigit(static_cast<uint8_t>(_bytes[index] >> 4));
        _out[index * 2 + 1] = HexDigit(static_cast<uint8_t>(_bytes[index] & 0x0F));
    }
}

std::optional<uint8_t> DecodeHexDigit(const char _value) noexcept
{
    if( _value >= '0' && _value <= '9' )
        return static_cast<uint8_t>(_value - '0');
    if( _value >= 'a' && _value <= 'f' )
        return static_cast<uint8_t>(_value - 'a' + 10);
    return std::nullopt;
}

template <size_t Count>
bool DecodeHex(const std::string_view _value, std::array<uint8_t, Count> &_out) noexcept
{
    if( _value.size() != Count * 2 )
        return false;
    for( size_t index = 0; index != Count; ++index ) {
        const auto high = DecodeHexDigit(_value[index * 2]);
        const auto low = DecodeHexDigit(_value[index * 2 + 1]);
        if( !high || !low )
            return false;
        _out[index] = static_cast<uint8_t>((*high << 4) | *low);
    }
    return true;
}

bool IsValidRole(const ArtifactRole _role) noexcept
{
    return _role == ArtifactRole::SourceSnapshot || _role == ArtifactRole::DestinationStage;
}

std::string_view RoleName(const ArtifactRole _role) noexcept
{
    switch( _role ) {
        case ArtifactRole::SourceSnapshot:
            return "source-snapshot";
        case ArtifactRole::DestinationStage:
            return "destination-stage";
    }
    return {};
}

std::optional<ArtifactRole> ParseRole(const std::string_view _value) noexcept
{
    if( _value == "source-snapshot" )
        return ArtifactRole::SourceSnapshot;
    if( _value == "destination-stage" )
        return ArtifactRole::DestinationStage;
    return std::nullopt;
}

template <size_t Capacity>
bool Append(std::array<char, Capacity> &_buffer, size_t &_size, const std::string_view _value) noexcept
{
    if( _value.size() > Capacity - _size )
        return false;
    std::memcpy(_buffer.data() + _size, _value.data(), _value.size());
    _size += _value.size();
    return true;
}

template <size_t Capacity, size_t Count>
bool AppendHex(std::array<char, Capacity> &_buffer, size_t &_size, const std::array<uint8_t, Count> &_bytes) noexcept
{
    if( Count * 2 > Capacity - _size )
        return false;
    EncodeHex(_bytes, _buffer.data() + _size);
    _size += Count * 2;
    return true;
}

template <size_t Capacity>
bool MakeRecordName(const ArtifactID &_id, std::array<char, Capacity> &_name, size_t &_size) noexcept
{
    _size = 0;
    return Append(_name, _size, kRecordPrefix) && AppendHex(_name, _size, _id.bytes) && Append(_name, _size, kRecordSuffix) &&
           _size < Capacity && (_name[_size] = '\0', true);
}

template <size_t Capacity>
bool MakeSealManifestName(const ArtifactID &_id, std::array<char, Capacity> &_name, size_t &_size) noexcept
{
    _size = 0;
    return Append(_name, _size, kSealManifestPrefix) && AppendHex(_name, _size, _id.bytes) &&
           Append(_name, _size, kRecordSuffix) && _size < Capacity && (_name[_size] = '\0', true);
}

template <size_t Capacity>
bool MakeArtifactName(const ArtifactID &_id, std::array<char, Capacity> &_name, size_t &_size) noexcept
{
    _size = 0;
    return Append(_name, _size, kArtifactPrefix) && AppendHex(_name, _size, _id.bytes) &&
           Append(_name, _size, kArtifactSuffix) && _size < Capacity && (_name[_size] = '\0', true);
}

enum class RecordState : uint8_t {
    Reserved,
    Sealed,
};

struct DecodedRecord final {
    Header header;
    ArtifactRole role;
    ArtifactID id;
    RecordState state{RecordState::Reserved};
    ObjectSeal artifact_seal;
};

struct ReadRecord final {
    std::array<char, kRecordMaximumBytes> contents{};
    dev_t device{0};
    ino_t inode{0};
    off_t size{0};
    uint32_t flags{0};
};

std::string_view RecordContents(const ReadRecord &_record) noexcept
{
    if( _record.size <= 0 || static_cast<size_t>(_record.size) >= _record.contents.size() )
        return {};
    return {_record.contents.data(), static_cast<size_t>(_record.size)};
}

std::optional<std::string_view> TakeLine(const std::string_view _contents, size_t &_offset) noexcept
{
    if( _offset >= _contents.size() )
        return std::nullopt;
    const auto end = _contents.find('\n', _offset);
    if( end == std::string_view::npos )
        return std::nullopt;
    const auto line = _contents.substr(_offset, end - _offset);
    _offset = end + 1;
    return line;
}

std::optional<std::string_view> FieldValue(const std::optional<std::string_view> _line,
                                           const std::string_view _field) noexcept
{
    if( !_line || !(*_line).starts_with(_field) || _line->size() <= _field.size() || (*_line)[_field.size()] != '=' )
        return std::nullopt;
    return _line->substr(_field.size() + 1);
}

bool ParseUnsigned(const std::string_view _value, uint64_t &_out) noexcept
{
    if( _value.empty() )
        return false;
    uint64_t parsed = 0;
    for( const char character : _value ) {
        if( character < '0' || character > '9' || parsed > (UINT64_MAX - static_cast<uint64_t>(character - '0')) / 10 )
            return false;
        parsed = parsed * 10 + static_cast<uint64_t>(character - '0');
    }
    _out = parsed;
    return true;
}

bool ParseUint32(const std::optional<std::string_view> _value, uint32_t &_out) noexcept
{
    uint64_t parsed = 0;
    if( !_value || !ParseUnsigned(*_value, parsed) || parsed > UINT32_MAX )
        return false;
    _out = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseUint64(const std::optional<std::string_view> _value, uint64_t &_out) noexcept
{
    return _value && ParseUnsigned(*_value, _out);
}

bool ParseTimestamp(const std::optional<std::string_view> _seconds,
                    const std::optional<std::string_view> _nanoseconds,
                    Timestamp &_timestamp) noexcept
{
    uint64_t encoded_seconds = 0;
    if( !ParseUint64(_seconds, encoded_seconds) || encoded_seconds > static_cast<uint64_t>(INT64_MAX) ||
        !ParseUint32(_nanoseconds, _timestamp.nanoseconds) || _timestamp.nanoseconds >= 1'000'000'000 )
        return false;
    _timestamp.seconds = static_cast<int64_t>(encoded_seconds);
    return true;
}

bool IsValidArtifactSeal(const ObjectSeal &_seal, const uint64_t _root_device) noexcept
{
    return _seal.device == _root_device && _seal.inode != 0 && _seal.uid == static_cast<uint32_t>(::geteuid()) &&
           (_seal.mode & S_IFMT) == S_IFREG && (_seal.mode & 07777) == 0600 && _seal.link_count == 1 &&
           _seal.birth_time.nanoseconds < 1'000'000'000 && _seal.modification_time.nanoseconds < 1'000'000'000 &&
           _seal.status_change_time.nanoseconds < 1'000'000'000;
}

bool ParseArtifactSeal(const std::string_view _contents,
                       size_t &_offset,
                       const uint64_t _root_device,
                       ObjectSeal &_seal) noexcept
{
    const auto device = FieldValue(TakeLine(_contents, _offset), "artifact_device");
    const auto inode = FieldValue(TakeLine(_contents, _offset), "artifact_inode");
    const auto uid = FieldValue(TakeLine(_contents, _offset), "artifact_uid");
    const auto gid = FieldValue(TakeLine(_contents, _offset), "artifact_gid");
    const auto mode = FieldValue(TakeLine(_contents, _offset), "artifact_mode");
    const auto flags = FieldValue(TakeLine(_contents, _offset), "artifact_flags");
    const auto link_count = FieldValue(TakeLine(_contents, _offset), "artifact_nlink");
    const auto byte_size = FieldValue(TakeLine(_contents, _offset), "artifact_size");
    const auto birth_seconds = FieldValue(TakeLine(_contents, _offset), "artifact_birth_seconds");
    const auto birth_nanoseconds = FieldValue(TakeLine(_contents, _offset), "artifact_birth_nanoseconds");
    const auto modification_seconds = FieldValue(TakeLine(_contents, _offset), "artifact_mtime_seconds");
    const auto modification_nanoseconds = FieldValue(TakeLine(_contents, _offset), "artifact_mtime_nanoseconds");
    const auto status_change_seconds = FieldValue(TakeLine(_contents, _offset), "artifact_ctime_seconds");
    const auto status_change_nanoseconds = FieldValue(TakeLine(_contents, _offset), "artifact_ctime_nanoseconds");
    return ParseUint64(device, _seal.device) && ParseUint64(inode, _seal.inode) && ParseUint32(uid, _seal.uid) &&
           ParseUint32(gid, _seal.gid) && ParseUint32(mode, _seal.mode) && ParseUint32(flags, _seal.flags) &&
           ParseUint64(link_count, _seal.link_count) && ParseUint64(byte_size, _seal.byte_size) &&
           ParseTimestamp(birth_seconds, birth_nanoseconds, _seal.birth_time) &&
           ParseTimestamp(modification_seconds, modification_nanoseconds, _seal.modification_time) &&
           ParseTimestamp(status_change_seconds, status_change_nanoseconds, _seal.status_change_time) &&
           IsValidArtifactSeal(_seal, _root_device);
}

bool DecodeRecord(const std::string_view _contents,
                  const uint64_t _root_device,
                  const uint64_t _root_inode,
                  DecodedRecord &_record) noexcept
{
    if( _contents.find('\0') != std::string_view::npos )
        return false;
    size_t offset = 0;
    const auto schema = FieldValue(TakeLine(_contents, offset), "schema");
    const auto root_device = FieldValue(TakeLine(_contents, offset), "root_device");
    const auto root_inode = FieldValue(TakeLine(_contents, offset), "root_inode");
    const auto correlation = FieldValue(TakeLine(_contents, offset), "correlation");
    const auto role = FieldValue(TakeLine(_contents, offset), "role");
    const auto artifact = FieldValue(TakeLine(_contents, offset), "artifact");
    const auto state = FieldValue(TakeLine(_contents, offset), "state");
    if( !schema || !root_device || !root_inode || !correlation || !role || !artifact || !state || *schema != kSchema )
        return false;

    uint64_t parsed_device = 0;
    uint64_t parsed_inode = 0;
    if( !ParseUnsigned(*root_device, parsed_device) || !ParseUnsigned(*root_inode, parsed_inode) ||
        parsed_device != _root_device || parsed_inode != _root_inode || !DecodeHex(*correlation, _record.header.correlation) ||
        !DecodeHex(*artifact, _record.id.bytes) || IsAllZero(_record.id.bytes) )
        return false;
    _record.header.version = kProtocolVersion;
    if( !Validate(_record.header) )
        return false;
    const auto parsed_role = ParseRole(*role);
    if( !parsed_role )
        return false;
    _record.role = *parsed_role;
    if( *state == "reserved" ) {
        _record.state = RecordState::Reserved;
        return offset == _contents.size();
    }
    if( *state != "sealed" )
        return false;
    _record.state = RecordState::Sealed;
    return ParseArtifactSeal(_contents, offset, _root_device, _record.artifact_seal) && offset == _contents.size();
}

bool IsRecordName(const std::string_view _name) noexcept
{
    return _name.size() == kRecordPrefix.size() + kArtifactHexBytes + kRecordSuffix.size() &&
           _name.starts_with(kRecordPrefix) && _name.ends_with(kRecordSuffix);
}

bool IsSealManifestName(const std::string_view _name) noexcept
{
    return _name.size() == kSealManifestPrefix.size() + kArtifactHexBytes + kRecordSuffix.size() &&
           _name.starts_with(kSealManifestPrefix) && _name.ends_with(kRecordSuffix);
}

std::expected<size_t, ProtectedRootLedger::Error>
BuildRecordContents(const uint64_t _root_device,
                    const uint64_t _root_inode,
                    const Header &_header,
                    const ArtifactRole _role,
                    const ArtifactID &_id,
                    const RecordState _state,
                    const ObjectSeal *_artifact_seal,
                    std::array<char, kRecordMaximumBytes> &_contents) noexcept
{
    size_t size = 0;
    char numeric[32]{};
    const auto append_number = [&]<class T>(const std::string_view _field, const T _value) noexcept {
        const int written = std::snprintf(numeric, sizeof(numeric), "%llu", static_cast<unsigned long long>(_value));
        return written > 0 && static_cast<size_t>(written) < sizeof(numeric) && Append(_contents, size, _field) &&
               Append(_contents, size, std::string_view{numeric, static_cast<size_t>(written)}) && Append(_contents, size, "\n");
    };
    if( !Append(_contents, size, "schema=") || !Append(_contents, size, kSchema) || !Append(_contents, size, "\n") ||
        !append_number("root_device=", _root_device) || !append_number("root_inode=", _root_inode) ||
        !Append(_contents, size, "correlation=") || !AppendHex(_contents, size, _header.correlation) ||
        !Append(_contents, size, "\nrole=") || !Append(_contents, size, RoleName(_role)) ||
        !Append(_contents, size, "\nartifact=") || !AppendHex(_contents, size, _id.bytes) ||
        !Append(_contents, size, _state == RecordState::Reserved ? "\nstate=reserved\n" : "\nstate=sealed\n") )
        return std::unexpected{ProtectedRootLedger::Error::RecordWriteFailed};
    if( _state == RecordState::Reserved )
        return size;
    if( _artifact_seal == nullptr ||
        !append_number("artifact_device=", _artifact_seal->device) || !append_number("artifact_inode=", _artifact_seal->inode) ||
        !append_number("artifact_uid=", _artifact_seal->uid) || !append_number("artifact_gid=", _artifact_seal->gid) ||
        !append_number("artifact_mode=", _artifact_seal->mode) || !append_number("artifact_flags=", _artifact_seal->flags) ||
        !append_number("artifact_nlink=", _artifact_seal->link_count) || !append_number("artifact_size=", _artifact_seal->byte_size) ||
        !append_number("artifact_birth_seconds=", _artifact_seal->birth_time.seconds) ||
        !append_number("artifact_birth_nanoseconds=", _artifact_seal->birth_time.nanoseconds) ||
        !append_number("artifact_mtime_seconds=", _artifact_seal->modification_time.seconds) ||
        !append_number("artifact_mtime_nanoseconds=", _artifact_seal->modification_time.nanoseconds) ||
        !append_number("artifact_ctime_seconds=", _artifact_seal->status_change_time.seconds) ||
        !append_number("artifact_ctime_nanoseconds=", _artifact_seal->status_change_time.nanoseconds) )
        return std::unexpected{ProtectedRootLedger::Error::RecordWriteFailed};
    return size;
}

std::expected<size_t, ProtectedRootLedger::Error>
WriteRecord(const int _root_fd, const uint64_t _root_device, const uint64_t _root_inode, const Header &_header, const ArtifactRole _role,
            const ArtifactID &_id, std::array<char, 128> &_name) noexcept
{
    size_t name_size = 0;
    if( !MakeRecordName(_id, _name, name_size) )
        return std::unexpected{ProtectedRootLedger::Error::RecordCreateFailed};
    std::array<char, kRecordMaximumBytes> contents{};
    const auto size = BuildRecordContents(
        _root_device, _root_inode, _header, _role, _id, RecordState::Reserved, nullptr, contents);
    if( !size )
        return std::unexpected{size.error()};

    const int fd = ::openat(_root_fd, _name.data(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if( fd < 0 )
        return std::unexpected{errno == EEXIST ? ProtectedRootLedger::Error::RandomFailed
                                                : ProtectedRootLedger::Error::RecordCreateFailed};
    if( ::fchmod(fd, 0600) != 0 ) {
        ::close(fd);
        return std::unexpected{ProtectedRootLedger::Error::RecordCreateFailed};
    }
    size_t offset = 0;
    while( offset < *size ) {
        const ssize_t written = ::write(fd, contents.data() + offset, *size - offset);
        if( written <= 0 ) {
            const int saved_error = errno;
            ::close(fd);
            errno = saved_error;
            return std::unexpected{ProtectedRootLedger::Error::RecordWriteFailed};
        }
        offset += static_cast<size_t>(written);
    }
    if( ::fsync(fd) != 0 ) {
        const int saved_error = errno;
        ::close(fd);
        errno = saved_error;
        return std::unexpected{ProtectedRootLedger::Error::RecordSyncFailed};
    }
    if( ::close(fd) != 0 )
        return std::unexpected{ProtectedRootLedger::Error::RecordCloseFailed};
    if( ::fsync(_root_fd) != 0 )
        return std::unexpected{ProtectedRootLedger::Error::RootSyncFailed};
    return name_size;
}

std::expected<void, ProtectedRootLedger::Error>
WriteSealManifest(const int _root_fd,
                  const uint64_t _root_device,
                  const uint64_t _root_inode,
                  const Header &_header,
                  const ArtifactRole _role,
                  const ArtifactID &_id,
                  const ObjectSeal &_artifact_seal,
                  std::array<char, 128> &_name) noexcept
{
    size_t name_size = 0;
    if( !MakeSealManifestName(_id, _name, name_size) || name_size == 0 )
        return std::unexpected{ProtectedRootLedger::Error::SealManifestCreateFailed};
    std::array<char, kRecordMaximumBytes> contents{};
    const auto size = BuildRecordContents(
        _root_device, _root_inode, _header, _role, _id, RecordState::Sealed, &_artifact_seal, contents);
    if( !size )
        return std::unexpected{ProtectedRootLedger::Error::SealManifestWriteFailed};

    const int fd = ::openat(_root_fd, _name.data(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if( fd < 0 || ::fchmod(fd, 0600) != 0 ) {
        if( fd >= 0 )
            ::close(fd);
        return std::unexpected{ProtectedRootLedger::Error::SealManifestCreateFailed};
    }
    size_t offset = 0;
    while( offset < *size ) {
        const ssize_t written = ::write(fd, contents.data() + offset, *size - offset);
        if( written <= 0 ) {
            const int saved_error = errno;
            ::close(fd);
            errno = saved_error;
            return std::unexpected{ProtectedRootLedger::Error::SealManifestWriteFailed};
        }
        offset += static_cast<size_t>(written);
    }
    if( ::fsync(fd) != 0 ) {
        const int saved_error = errno;
        ::close(fd);
        errno = saved_error;
        return std::unexpected{ProtectedRootLedger::Error::SealManifestSyncFailed};
    }
    if( ::close(fd) != 0 )
        return std::unexpected{ProtectedRootLedger::Error::SealManifestCloseFailed};
    if( ::fsync(_root_fd) != 0 )
        return std::unexpected{ProtectedRootLedger::Error::SealManifestSyncFailed};
    return {};
}

std::optional<ReadRecord> ReadRecordContents(const int _root_fd, const char *_name, const uint64_t _root_device) noexcept
{
    struct stat named{};
    if( ::fstatat(_root_fd, _name, &named, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(named.st_mode) || named.st_uid != ::geteuid() ||
        named.st_nlink != 1 || (named.st_mode & 07777) != 0600 || named.st_size <= 0 ||
        static_cast<size_t>(named.st_size) >= kRecordMaximumBytes || static_cast<uint64_t>(named.st_dev) != _root_device )
        return std::nullopt;
    const int fd = ::openat(_root_fd, _name, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if( fd < 0 )
        return std::nullopt;
    struct stat opened{};
    if( ::fstat(fd, &opened) != 0 || opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
        opened.st_size != named.st_size || opened.st_flags != named.st_flags || !S_ISREG(opened.st_mode) ||
        opened.st_nlink != 1 || opened.st_uid != ::geteuid() || (opened.st_mode & 07777) != 0600 ) {
        ::close(fd);
        return std::nullopt;
    }
    ReadRecord record{
        .device = opened.st_dev,
        .inode = opened.st_ino,
        .size = opened.st_size,
        .flags = static_cast<uint32_t>(opened.st_flags),
    };
    size_t offset = 0;
    while( offset < static_cast<size_t>(opened.st_size) ) {
        const ssize_t read = ::read(fd, record.contents.data() + offset, static_cast<size_t>(opened.st_size) - offset);
        if( read <= 0 ) {
            ::close(fd);
            return std::nullopt;
        }
        offset += static_cast<size_t>(read);
    }
    if( ::close(fd) != 0 )
        return std::nullopt;
    record.contents[offset] = '\0';
    return record;
}

bool SealFromStat(const struct stat &_status, ObjectSeal &_seal) noexcept
{
    if( _status.st_size < 0 || _status.st_nlink <= 0 || _status.st_birthtimespec.tv_nsec < 0 ||
        _status.st_birthtimespec.tv_nsec >= 1'000'000'000 || _status.st_mtimespec.tv_nsec < 0 ||
        _status.st_mtimespec.tv_nsec >= 1'000'000'000 || _status.st_ctimespec.tv_nsec < 0 ||
        _status.st_ctimespec.tv_nsec >= 1'000'000'000 )
        return false;
    _seal = ObjectSeal{
        .device = static_cast<uint64_t>(_status.st_dev),
        .inode = static_cast<uint64_t>(_status.st_ino),
        .uid = static_cast<uint32_t>(_status.st_uid),
        .gid = static_cast<uint32_t>(_status.st_gid),
        .mode = static_cast<uint32_t>(_status.st_mode),
        .flags = static_cast<uint32_t>(_status.st_flags),
        .link_count = static_cast<uint64_t>(_status.st_nlink),
        .byte_size = static_cast<uint64_t>(_status.st_size),
        .birth_time = {.seconds = _status.st_birthtimespec.tv_sec,
                       .nanoseconds = static_cast<uint32_t>(_status.st_birthtimespec.tv_nsec)},
        .modification_time = {.seconds = _status.st_mtimespec.tv_sec,
                              .nanoseconds = static_cast<uint32_t>(_status.st_mtimespec.tv_nsec)},
        .status_change_time = {.seconds = _status.st_ctimespec.tv_sec,
                               .nanoseconds = static_cast<uint32_t>(_status.st_ctimespec.tv_nsec)},
    };
    return true;
}

std::optional<ObjectSeal> ReadArtifactSeal(const int _root_fd, const char *_name, const uint64_t _root_device) noexcept
{
    struct stat named{};
    if( ::fstatat(_root_fd, _name, &named, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(named.st_mode) ||
        named.st_uid != ::geteuid() || named.st_nlink != 1 || (named.st_mode & 07777) != 0600 ||
        static_cast<uint64_t>(named.st_dev) != _root_device )
        return std::nullopt;
    const int fd = ::openat(_root_fd, _name, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if( fd < 0 )
        return std::nullopt;
    struct stat opened{};
    if( ::fstat(fd, &opened) != 0 || opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
        opened.st_size != named.st_size || opened.st_uid != named.st_uid || opened.st_nlink != named.st_nlink ||
        opened.st_mode != named.st_mode || opened.st_flags != named.st_flags ) {
        ::close(fd);
        return std::nullopt;
    }
    ObjectSeal seal;
    const bool sealed = SealFromStat(opened, seal) && IsValidArtifactSeal(seal, _root_device);
    if( ::close(fd) != 0 || !sealed )
        return std::nullopt;
    return seal;
}

bool IsArtifactAbsent(const int _root_fd, const char *_name) noexcept
{
    struct stat status{};
    return ::fstatat(_root_fd, _name, &status, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
}

bool IsEntryAbsent(const int _root_fd, const char *_name) noexcept
{
    struct stat status{};
    return ::fstatat(_root_fd, _name, &status, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
}

struct PersistedReservations final {
    size_t count{0};
    bool has_duplicate_correlation{false};
};

std::expected<PersistedReservations, ProtectedRootLedger::Error>
InspectPersistedReservations(const int _root_fd,
                             const uint64_t _root_device,
                             const uint64_t _root_inode,
                             const Header &_header,
                             const ArtifactRole _role) noexcept
{
    const int scan_fd = ::openat(_root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if( scan_fd < 0 )
        return std::unexpected{ProtectedRootLedger::Error::InvalidRoot};
    DIR *directory = ::fdopendir(scan_fd);
    if( directory == nullptr ) {
        ::close(scan_fd);
        return std::unexpected{ProtectedRootLedger::Error::InvalidRoot};
    }
    PersistedReservations reservations;
    int read_error = 0;
    while( true ) {
        errno = 0;
        const dirent *entry = ::readdir(directory);
        if( entry == nullptr ) {
            read_error = errno;
            break;
        }
        if( !IsRecordName(entry->d_name) )
            continue;
        ++reservations.count;
        const auto record = ReadRecordContents(_root_fd, entry->d_name, _root_device);
        DecodedRecord decoded{};
        if( record && DecodeRecord(RecordContents(*record), _root_device, _root_inode, decoded) &&
            decoded.header == _header && decoded.role == _role )
            reservations.has_duplicate_correlation = true;
    }
    if( ::closedir(directory) != 0 || read_error != 0 )
        return std::unexpected{ProtectedRootLedger::Error::InvalidRoot};
    return reservations;
}

bool HasExactRecordIdentity(const int _root_fd, const char *_name, const ReadRecord &_record) noexcept
{
    struct stat status{};
    return ::fstatat(_root_fd, _name, &status, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(status.st_mode) &&
           status.st_dev == _record.device && status.st_ino == _record.inode && status.st_size == _record.size &&
           status.st_flags == _record.flags && status.st_nlink == 1 && status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) == 0600;
}

bool HasExactReservation(const int _root_fd,
                         const uint64_t _root_device,
                         const uint64_t _root_inode,
                         const Header &_header,
                         const ArtifactRole _role,
                         const ArtifactID &_id,
                         std::array<char, 128> &_name) noexcept
{
    size_t name_size = 0;
    if( !MakeRecordName(_id, _name, name_size) || name_size == 0 )
        return false;
    const auto record = ReadRecordContents(_root_fd, _name.data(), _root_device);
    DecodedRecord decoded{};
    return record && DecodeRecord(RecordContents(*record), _root_device, _root_inode, decoded) &&
           decoded.state == RecordState::Reserved && decoded.header == _header && decoded.role == _role && decoded.id == _id &&
           HasExactRecordIdentity(_root_fd, _name.data(), *record);
}

} // namespace

ProtectedRootLedger::ProtectedRootLedger(const int _root_fd,
                                         const uint64_t _device,
                                         const uint64_t _inode,
                                         const bool _registered_root) noexcept
    : m_RootFD{_root_fd}, m_RootDevice{_device}, m_RootInode{_inode}, m_RegisteredRoot{_registered_root}
{
}

std::expected<ProtectedRootLedger, ProtectedRootLedger::Error> ProtectedRootLedger::Open(const int _borrowed_root_fd) noexcept
{
    if( _borrowed_root_fd < 0 )
        return std::unexpected{Error::InvalidRoot};
    const int root_fd = ::fcntl(_borrowed_root_fd, F_DUPFD_CLOEXEC, 0);
    if( root_fd < 0 )
        return std::unexpected{Error::InvalidRoot};
    struct stat status{};
    if( ::fstat(root_fd, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & 07777) != 0700 ) {
        ::close(root_fd);
        return std::unexpected{Error::InvalidRoot};
    }
    const RootIdentity identity{
        .device = static_cast<uint64_t>(status.st_dev),
        .inode = static_cast<uint64_t>(status.st_ino),
    };
    const auto registered = RegisterProtectedRoot(identity);
    if( !registered ) {
        ::close(root_fd);
        return std::unexpected{registered.error()};
    }
    return ProtectedRootLedger{root_fd, identity.device, identity.inode, true};
}

ProtectedRootLedger::ProtectedRootLedger(ProtectedRootLedger &&_rhs) noexcept
    : m_RootFD{-1}
{
    std::lock_guard lock{_rhs.m_Mutex};
    m_RootFD = std::exchange(_rhs.m_RootFD, -1);
    m_RootDevice = _rhs.m_RootDevice;
    m_RootInode = _rhs.m_RootInode;
    m_RegisteredRoot = std::exchange(_rhs.m_RegisteredRoot, false);
    m_ActiveReservations = std::move(_rhs.m_ActiveReservations);
}

ProtectedRootLedger &ProtectedRootLedger::operator=(ProtectedRootLedger &&_rhs) noexcept
{
    if( this == &_rhs )
        return *this;
    int retired_root_fd = -1;
    RootIdentity retired_identity;
    bool unregister_retired_root = false;
    {
        std::scoped_lock lock{m_Mutex, _rhs.m_Mutex};
        retired_root_fd = std::exchange(m_RootFD, -1);
        retired_identity = {.device = m_RootDevice, .inode = m_RootInode};
        unregister_retired_root = std::exchange(m_RegisteredRoot, false);
        m_RootFD = std::exchange(_rhs.m_RootFD, -1);
        m_RootDevice = _rhs.m_RootDevice;
        m_RootInode = _rhs.m_RootInode;
        m_RegisteredRoot = std::exchange(_rhs.m_RegisteredRoot, false);
        m_ActiveReservations = std::move(_rhs.m_ActiveReservations);
    }
    if( retired_root_fd >= 0 )
        ::close(retired_root_fd);
    if( unregister_retired_root )
        UnregisterProtectedRoot(retired_identity);
    return *this;
}

ProtectedRootLedger::~ProtectedRootLedger() noexcept
{
    int root_fd = -1;
    RootIdentity identity;
    bool unregister_root = false;
    {
        std::lock_guard lock{m_Mutex};
        root_fd = std::exchange(m_RootFD, -1);
        identity = {.device = m_RootDevice, .inode = m_RootInode};
        unregister_root = std::exchange(m_RegisteredRoot, false);
    }
    if( root_fd >= 0 )
        ::close(root_fd);
    if( unregister_root )
        UnregisterProtectedRoot(identity);
}

std::expected<ProtectedRootLedger::Reservation, ProtectedRootLedger::Error>
ProtectedRootLedger::Reserve(const Header &_header, const ArtifactRole _role) noexcept
{
    std::lock_guard lock{m_Mutex};
    if( !HasValidRootUnlocked() )
        return std::unexpected{Error::InvalidRoot};
    if( !Validate(_header) )
        return std::unexpected{Error::InvalidHeader};
    if( !IsValidRole(_role) )
        return std::unexpected{Error::InvalidRole};
    std::optional<ActiveReservation> *slot = nullptr;
    for( auto &active : m_ActiveReservations ) {
        if( active && active->header == _header && active->role == _role )
            return std::unexpected{Error::DuplicateCorrelation};
        if( !active && slot == nullptr )
            slot = &active;
    }
    if( slot == nullptr )
        return std::unexpected{Error::CapacityExceeded};
    const auto persisted_reservations =
        InspectPersistedReservations(m_RootFD, m_RootDevice, m_RootInode, _header, _role);
    if( !persisted_reservations )
        return std::unexpected{persisted_reservations.error()};
    if( persisted_reservations->has_duplicate_correlation )
        return std::unexpected{Error::DuplicateCorrelation};
    if( persisted_reservations->count >= kMaximumReservations )
        return std::unexpected{Error::CapacityExceeded};

    for( size_t attempt = 0; attempt != kRandomAttempts; ++attempt ) {
        ArtifactID id;
        if( SecRandomCopyBytes(kSecRandomDefault, id.bytes.size(), id.bytes.data()) != errSecSuccess || IsAllZero(id.bytes) )
            return std::unexpected{Error::RandomFailed};
        std::array<char, 128> name{};
        const auto written = WriteRecord(m_RootFD, m_RootDevice, m_RootInode, _header, _role, id, name);
        if( written ) {
            *slot = ActiveReservation{.header = _header, .role = _role, .id = id};
            return Reservation{_header, _role, id};
        }
        if( written.error() != Error::RandomFailed )
            return std::unexpected{written.error()};
    }
    return std::unexpected{Error::RandomFailed};
}

std::expected<void, ProtectedRootLedger::Error>
ProtectedRootLedger::MaterializeEmptyAndSeal(Reservation &&_reservation) noexcept
{
    std::lock_guard lock{m_Mutex};
    if( !HasValidRootUnlocked() )
        return std::unexpected{Error::InvalidRoot};

    bool active = false;
    for( const auto &entry : m_ActiveReservations ) {
        if( entry && entry->header == _reservation.m_Header && entry->role == _reservation.m_Role &&
            entry->id == _reservation.m_ID ) {
            active = true;
            break;
        }
    }
    if( !active )
        return std::unexpected{Error::UnknownReservation};

    std::array<char, 128> reservation_name{};
    if( !HasExactReservation(m_RootFD,
                             m_RootDevice,
                             m_RootInode,
                             _reservation.m_Header,
                             _reservation.m_Role,
                             _reservation.m_ID,
                             reservation_name) )
        return std::unexpected{Error::ArtifactValidationFailed};

    std::array<char, 128> artifact_name{};
    size_t artifact_name_size = 0;
    std::array<char, 128> seal_manifest_name{};
    size_t seal_manifest_name_size = 0;
    if( !MakeArtifactName(_reservation.m_ID, artifact_name, artifact_name_size) || artifact_name_size == 0 ||
        !MakeSealManifestName(_reservation.m_ID, seal_manifest_name, seal_manifest_name_size) ||
        seal_manifest_name_size == 0 || !IsArtifactAbsent(m_RootFD, artifact_name.data()) ||
        !IsEntryAbsent(m_RootFD, seal_manifest_name.data()) )
        return std::unexpected{Error::ArtifactCreateFailed};

    const int artifact_fd =
        ::openat(m_RootFD, artifact_name.data(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if( artifact_fd < 0 )
        return std::unexpected{Error::ArtifactCreateFailed};
    if( ::fchmod(artifact_fd, 0600) != 0 ) {
        ::close(artifact_fd);
        return std::unexpected{Error::ArtifactCreateFailed};
    }

    struct stat artifact_status{};
    ObjectSeal artifact_seal;
    if( ::fstat(artifact_fd, &artifact_status) != 0 || !SealFromStat(artifact_status, artifact_seal) ||
        !IsValidArtifactSeal(artifact_seal, m_RootDevice) || artifact_seal.byte_size != 0 ) {
        ::close(artifact_fd);
        return std::unexpected{Error::ArtifactValidationFailed};
    }
    if( ::fsync(artifact_fd) != 0 || ::fsync(m_RootFD) != 0 || ::fcntl(artifact_fd, F_FULLFSYNC) != 0 ) {
        ::close(artifact_fd);
        return std::unexpected{Error::ArtifactSyncFailed};
    }
    if( ::fstat(artifact_fd, &artifact_status) != 0 || !SealFromStat(artifact_status, artifact_seal) ||
        !IsValidArtifactSeal(artifact_seal, m_RootDevice) || artifact_seal.byte_size != 0 ) {
        ::close(artifact_fd);
        return std::unexpected{Error::ArtifactValidationFailed};
    }
    if( ::close(artifact_fd) != 0 )
        return std::unexpected{Error::ArtifactCloseFailed};

    if( !HasExactReservation(m_RootFD,
                             m_RootDevice,
                             m_RootInode,
                             _reservation.m_Header,
                             _reservation.m_Role,
                             _reservation.m_ID,
                             reservation_name) )
        return std::unexpected{Error::SealManifestCreateFailed};
    const auto written = WriteSealManifest(m_RootFD,
                                            m_RootDevice,
                                            m_RootInode,
                                            _reservation.m_Header,
                                            _reservation.m_Role,
                                            _reservation.m_ID,
                                            artifact_seal,
                                            seal_manifest_name);
    if( !written )
        return std::unexpected{written.error()};

    const auto sealed_record = ReadRecordContents(m_RootFD, seal_manifest_name.data(), m_RootDevice);
    DecodedRecord sealed{};
    const auto sealed_artifact = ReadArtifactSeal(m_RootFD, artifact_name.data(), m_RootDevice);
    if( !sealed_record || !DecodeRecord(RecordContents(*sealed_record), m_RootDevice, m_RootInode, sealed) ||
        sealed.state != RecordState::Sealed || sealed.header != _reservation.m_Header || sealed.role != _reservation.m_Role ||
        sealed.id != _reservation.m_ID || sealed.artifact_seal != artifact_seal ||
        !HasExactRecordIdentity(m_RootFD, seal_manifest_name.data(), *sealed_record) || !sealed_artifact ||
        *sealed_artifact != artifact_seal )
        return std::unexpected{Error::PostSealValidationFailed};
    return {};
}

std::expected<void, ProtectedRootLedger::Error> ProtectedRootLedger::Release(Reservation &&_reservation) noexcept
{
    std::lock_guard lock{m_Mutex};
    if( !HasValidRootUnlocked() )
        return std::unexpected{Error::InvalidRoot};
    auto active = m_ActiveReservations.end();
    for( auto entry = m_ActiveReservations.begin(); entry != m_ActiveReservations.end(); ++entry ) {
        if( *entry && (*entry)->header == _reservation.m_Header && (*entry)->role == _reservation.m_Role &&
            (*entry)->id == _reservation.m_ID ) {
            active = entry;
            break;
        }
    }
    if( active == m_ActiveReservations.end() )
        return std::unexpected{Error::UnknownReservation};

    std::array<char, 128> name{};
    size_t name_size = 0;
    if( !MakeRecordName(_reservation.m_ID, name, name_size) || name_size == 0 )
        return std::unexpected{Error::RecordRemoveFailed};
    std::array<char, 128> artifact_name{};
    size_t artifact_name_size = 0;
    std::array<char, 128> seal_manifest_name{};
    size_t seal_manifest_name_size = 0;
    if( !MakeArtifactName(_reservation.m_ID, artifact_name, artifact_name_size) || artifact_name_size == 0 ||
        !MakeSealManifestName(_reservation.m_ID, seal_manifest_name, seal_manifest_name_size) ||
        seal_manifest_name_size == 0 || !IsArtifactAbsent(m_RootFD, artifact_name.data()) ||
        !IsEntryAbsent(m_RootFD, seal_manifest_name.data()) )
        return std::unexpected{Error::RecordRemoveFailed};
    const auto record = ReadRecordContents(m_RootFD, name.data(), m_RootDevice);
    DecodedRecord decoded{};
    if( !record || !DecodeRecord(RecordContents(*record), m_RootDevice, m_RootInode, decoded) ||
        decoded.state != RecordState::Reserved || decoded.header != _reservation.m_Header ||
        decoded.role != _reservation.m_Role || decoded.id != _reservation.m_ID ||
        !HasExactRecordIdentity(m_RootFD, name.data(), *record) || ::unlinkat(m_RootFD, name.data(), 0) != 0 )
        return std::unexpected{Error::RecordRemoveFailed};
    if( ::fsync(m_RootFD) != 0 )
        return std::unexpected{Error::RootSyncFailed};
    active->reset();
    return {};
}

std::expected<ProtectedRootLedger::ReconcileResult, ProtectedRootLedger::Error> ProtectedRootLedger::Reconcile() noexcept
{
    std::lock_guard lock{m_Mutex};
    if( !HasValidRootUnlocked() )
        return std::unexpected{Error::InvalidRoot};
    if( ActiveReservationCountUnlocked() != 0 )
        return std::unexpected{Error::Busy};
    const int scan_fd = ::openat(m_RootFD, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if( scan_fd < 0 )
        return std::unexpected{Error::InvalidRoot};
    DIR *directory = ::fdopendir(scan_fd);
    if( directory == nullptr ) {
        ::close(scan_fd);
        return std::unexpected{Error::InvalidRoot};
    }

    ReconcileResult result;
    bool changed = false;
    int read_error = 0;
    while( true ) {
        errno = 0;
        const dirent *entry = ::readdir(directory);
        if( entry == nullptr ) {
            read_error = errno;
            break;
        }
        const std::string_view name{entry->d_name};
        if( name == "." || name == ".." )
            continue;
        if( IsSealManifestName(name) )
            continue;
        if( !IsRecordName(name) ) {
            ++result.ignored_entries;
            continue;
        }
        const auto contents = ReadRecordContents(m_RootFD, entry->d_name, m_RootDevice);
        DecodedRecord record{};
        if( !contents || !DecodeRecord(RecordContents(*contents), m_RootDevice, m_RootInode, record) ) {
            ++result.retained_records;
            continue;
        }
        std::array<char, 128> expected_name{};
        size_t expected_size = 0;
        if( !MakeRecordName(record.id, expected_name, expected_size) || name != std::string_view{expected_name.data(), expected_size} ) {
            ++result.retained_records;
            continue;
        }
        std::array<char, 128> artifact_name{};
        size_t artifact_name_size = 0;
        if( !MakeArtifactName(record.id, artifact_name, artifact_name_size) || artifact_name_size == 0 ) {
            ++result.retained_records;
            continue;
        }
        std::array<char, 128> seal_manifest_name{};
        size_t seal_manifest_name_size = 0;
        if( !MakeSealManifestName(record.id, seal_manifest_name, seal_manifest_name_size) || seal_manifest_name_size == 0 ) {
            ++result.retained_records;
            continue;
        }
        if( record.state != RecordState::Reserved ) {
            ++result.retained_incomplete_artifacts;
            ++result.retained_records;
            continue;
        }
        const auto sealed_contents = ReadRecordContents(m_RootFD, seal_manifest_name.data(), m_RootDevice);
        DecodedRecord sealed{};
        const bool has_exact_sealed_manifest =
            sealed_contents && DecodeRecord(RecordContents(*sealed_contents), m_RootDevice, m_RootInode, sealed) &&
            sealed.state == RecordState::Sealed && sealed.header == record.header && sealed.role == record.role &&
            sealed.id == record.id && HasExactRecordIdentity(m_RootFD, seal_manifest_name.data(), *sealed_contents);
        if( has_exact_sealed_manifest ) {
            ++result.inspected_sealed_artifacts;
            const auto artifact_seal = ReadArtifactSeal(m_RootFD, artifact_name.data(), m_RootDevice);
            if( artifact_seal && *artifact_seal == sealed.artifact_seal )
                ++result.exact_sealed_artifacts;
            else
                ++result.retained_incomplete_artifacts;
            ++result.retained_records;
            continue;
        }
        if( !IsArtifactAbsent(m_RootFD, artifact_name.data()) || !IsEntryAbsent(m_RootFD, seal_manifest_name.data()) ) {
            ++result.retained_incomplete_artifacts;
            ++result.retained_records;
            continue;
        }
        if( !HasExactRecordIdentity(m_RootFD, entry->d_name, *contents) || ::unlinkat(m_RootFD, entry->d_name, 0) != 0 ) {
            ++result.retained_records;
            continue;
        }
        ++result.removed_reservations;
        changed = true;
    }
    if( ::closedir(directory) != 0 )
        return std::unexpected{Error::InvalidRoot};
    if( changed && ::fsync(m_RootFD) != 0 )
        return std::unexpected{Error::RootSyncFailed};
    if( read_error != 0 )
        return std::unexpected{Error::InvalidRoot};
    return result;
}

size_t ProtectedRootLedger::ActiveReservationCount() const noexcept
{
    std::lock_guard lock{m_Mutex};
    return ActiveReservationCountUnlocked();
}

bool ProtectedRootLedger::HasValidRootUnlocked() const noexcept
{
    if( m_RootFD < 0 || !m_RegisteredRoot )
        return false;
    struct stat status{};
    return ::fstat(m_RootFD, &status) == 0 && S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) == 0700 && static_cast<uint64_t>(status.st_dev) == m_RootDevice &&
           static_cast<uint64_t>(status.st_ino) == m_RootInode;
}

size_t ProtectedRootLedger::ActiveReservationCountUnlocked() const noexcept
{
    size_t active = 0;
    for( const auto &reservation : m_ActiveReservations ) {
        if( reservation )
            ++active;
    }
    return active;
}

} // namespace nc::routedio::cross_volume_staging::helper
