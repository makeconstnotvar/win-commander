// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "CrossVolumeStagingProtectedRootLedger.h"
#include "CrossVolumeStagingHelperDestinationStage.h"
#include "CrossVolumeStagingHelperPublicationBarrier.h"
#include "CrossVolumeStagingHelperPublicationLifecycle.h"
#include "CrossVolumeStagingHelperSourceSnapshot.h"
#include "CrossVolumeStagingHelperStagingRoots.h"
#include "CrossVolumeStagingHelperStagingSessionRunner.h"

#include <Security/SecRandom.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <pthread.h>
#include <span>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace nc::routedio::cross_volume_staging::helper {
namespace {

constexpr std::string_view kRecordPrefix = ".wc-cross-volume-record-";
constexpr std::string_view kRecordSuffix = ".manifest";
constexpr std::string_view kSealManifestPrefix = ".wc-cross-volume-seal-";
constexpr std::string_view kLifecycleManifestPrefix = ".wc-cross-volume-lifecycle-";
constexpr std::string_view kLifecycleSealManifestPrefix = ".wc-cross-volume-lifecycle-seal-";
constexpr std::string_view kArtifactPrefix = ".wc-cross-volume-artifact-";
constexpr std::string_view kArtifactSuffix = ".data";
constexpr std::string_view kSchema = "wincommander-cross-volume-ledger-v1";
constexpr std::string_view kLifecycleSchema = "wincommander-cross-volume-lifecycle-v1";
constexpr size_t kRecordMaximumBytes = 1024;
constexpr size_t kLifecycleManifestMaximumBytes = 4096;
constexpr size_t kRandomAttempts = 4;
constexpr size_t kArtifactHexBytes = 64;
constexpr size_t kMaximumOpenProtectedRoots = 32;

struct RootIdentity final {
    uint64_t device{0};
    uint64_t inode{0};

    bool operator==(const RootIdentity &) const noexcept = default;
};

class ScopedStagingRootFD final
{
public:
    explicit ScopedStagingRootFD(const int _fd) noexcept : m_FD{_fd} {}
    ScopedStagingRootFD(const ScopedStagingRootFD &) = delete;
    ScopedStagingRootFD &operator=(const ScopedStagingRootFD &) = delete;
    ScopedStagingRootFD(ScopedStagingRootFD &&_other) noexcept : m_FD{std::exchange(_other.m_FD, -1)} {}
    ScopedStagingRootFD &operator=(ScopedStagingRootFD &&) = delete;
    ~ScopedStagingRootFD() noexcept
    {
        if( m_FD >= 0 )
            (void)::close(m_FD);
    }

    [[nodiscard]] int Get() const noexcept { return m_FD; }

private:
    int m_FD{-1};
};

bool IsCanonicalRootOrder(const RootIdentity _left, const RootIdentity _right) noexcept
{
    return _left.device < _right.device || (_left.device == _right.device && _left.inode < _right.inode);
}

std::expected<ScopedStagingRootFD, StagingRootAuthority::Error>
AnchorStagingRoot(const int _borrowed_root_fd, const StagingRootAuthority::Error _invalid_error) noexcept
{
    if( _borrowed_root_fd < 0 )
        return std::unexpected{_invalid_error};
    const int anchored_fd = ::openat(_borrowed_root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if( anchored_fd < 0 )
        return std::unexpected{_invalid_error};
    return ScopedStagingRootFD{anchored_fd};
}

std::expected<RootIdentity, StagingRootAuthority::Error>
PrevalidateStagingRoot(const int _anchored_root_fd,
                       const uint64_t _expected_device,
                       const StagingRootAuthority::Error _invalid_error,
                       const StagingRootAuthority::Error _device_mismatch_error) noexcept
{
    if( _anchored_root_fd < 0 )
        return std::unexpected{_invalid_error};

    struct stat status{};
    if( ::fstat(_anchored_root_fd, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & 07777) != 0700 || status.st_dev == 0 || status.st_ino == 0 )
        return std::unexpected{_invalid_error};

    const auto identity = RootIdentity{
        .device = static_cast<uint64_t>(status.st_dev),
        .inode = static_cast<uint64_t>(status.st_ino),
    };
    if( identity.device != _expected_device )
        return std::unexpected{_device_mismatch_error};
    return identity;
}

StagingRootAuthority::Error MapProtectedRootOpenError(const ProtectedRootLedger::Error _error,
                                                      const bool _source_root) noexcept
{
    switch( _error ) {
        case ProtectedRootLedger::Error::InvalidRoot:
            return _source_root ? StagingRootAuthority::Error::SourceRootInvalid
                                : StagingRootAuthority::Error::DestinationRootInvalid;
        case ProtectedRootLedger::Error::RootBusy:
            return _source_root ? StagingRootAuthority::Error::SourceRootBusy
                                : StagingRootAuthority::Error::DestinationRootBusy;
        case ProtectedRootLedger::Error::ForkedProcess:
            return StagingRootAuthority::Error::ForkedProcess;
        default:
            return _source_root ? StagingRootAuthority::Error::SourceRootLockFailed
                                : StagingRootAuthority::Error::DestinationRootLockFailed;
    }
}

StagingPublicationBarrier::Error MapPublicationRootOpenError(const ProtectedRootLedger::Error _error) noexcept
{
    switch( _error ) {
        case ProtectedRootLedger::Error::InvalidRoot:
            return StagingPublicationBarrier::Error::DestinationRootInvalid;
        case ProtectedRootLedger::Error::RootBusy:
            return StagingPublicationBarrier::Error::DestinationRootBusy;
        case ProtectedRootLedger::Error::ForkedProcess:
            return StagingPublicationBarrier::Error::ForkedProcess;
        default:
            return StagingPublicationBarrier::Error::DestinationRootLockFailed;
    }
}

struct ProtectedRootRegistryEntry final {
    RootIdentity identity;
    int locked_root_fd{-1};
    bool occupied{false};
};

pthread_mutex_t g_ProtectedRootRegistryMutex = PTHREAD_MUTEX_INITIALIZER;
std::array<ProtectedRootRegistryEntry, kMaximumOpenProtectedRoots> g_ProtectedRootRegistry;
pid_t g_ProtectedRootRegistryOwnerPID{0};
pthread_once_t g_ProtectedRootRegistryAtForkOnce = PTHREAD_ONCE_INIT;
int g_ProtectedRootRegistryAtForkResult{0};

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

void PrepareProtectedRootRegistryFork() noexcept
{
    (void)::pthread_mutex_lock(&g_ProtectedRootRegistryMutex);
}

void ParentProtectedRootRegistryFork() noexcept
{
    (void)::pthread_mutex_unlock(&g_ProtectedRootRegistryMutex);
}

void ChildProtectedRootRegistryFork() noexcept
{
    for( auto &entry : g_ProtectedRootRegistry ) {
        if( entry.occupied && entry.locked_root_fd >= 0 ) {
            (void)::close(entry.locked_root_fd);
            // A later fork in this child must not close an unrelated descriptor that reused this number.
            entry.locked_root_fd = -1;
        }
    }
    (void)::pthread_mutex_unlock(&g_ProtectedRootRegistryMutex);
}

void InstallProtectedRootRegistryAtFork() noexcept
{
    g_ProtectedRootRegistryAtForkResult = ::pthread_atfork(
        PrepareProtectedRootRegistryFork, ParentProtectedRootRegistryFork, ChildProtectedRootRegistryFork);
}

bool HasProtectedRootRegistryAtForkHandler() noexcept
{
    return ::pthread_once(&g_ProtectedRootRegistryAtForkOnce, InstallProtectedRootRegistryAtFork) == 0 &&
           g_ProtectedRootRegistryAtForkResult == 0;
}

std::expected<void, ProtectedRootLedger::Error> RegisterProtectedRoot(const RootIdentity _identity,
                                                                      const int _locked_root_fd) noexcept
{
    ScopedRootRegistryLock lock;
    if( !lock.IsLocked() )
        return std::unexpected{ProtectedRootLedger::Error::RootRegistryFull};
    const pid_t current_pid = ::getpid();
    if( g_ProtectedRootRegistryOwnerPID != 0 && g_ProtectedRootRegistryOwnerPID != current_pid )
        return std::unexpected{ProtectedRootLedger::Error::ForkedProcess};
    size_t slot = kMaximumOpenProtectedRoots;
    for( size_t index = 0; index != g_ProtectedRootRegistry.size(); ++index ) {
        if( g_ProtectedRootRegistry[index].occupied && g_ProtectedRootRegistry[index].identity == _identity )
            return std::unexpected{ProtectedRootLedger::Error::RootBusy};
        if( !g_ProtectedRootRegistry[index].occupied && slot == kMaximumOpenProtectedRoots )
            slot = index;
    }
    if( slot == kMaximumOpenProtectedRoots )
        return std::unexpected{ProtectedRootLedger::Error::RootRegistryFull};
    g_ProtectedRootRegistry[slot] = {.identity = _identity, .locked_root_fd = _locked_root_fd, .occupied = true};
    g_ProtectedRootRegistryOwnerPID = current_pid;
    return {};
}

/**
 * Releases the exact FD registered for a root while holding the same mutex used by the atfork handlers.  The close
 * and registry erase are deliberately indivisible with respect to fork: a child can otherwise inherit either a
 * lock whose registry entry has already disappeared, or a stale descriptor number after it has been reused.
 */
void CloseAndUnregisterProtectedRoot(const RootIdentity _identity, const int _locked_root_fd) noexcept
{
    if( _locked_root_fd < 0 )
        return;
    ScopedRootRegistryLock lock;
    if( !lock.IsLocked() )
        // Retaining the FD and its registry entry is a fail-closed leak on a broken mutex, safer than a split state.
        return;
    if( g_ProtectedRootRegistryOwnerPID != ::getpid() )
        return;
    for( auto &entry : g_ProtectedRootRegistry ) {
        if( entry.occupied && entry.identity == _identity && entry.locked_root_fd == _locked_root_fd ) {
            (void)::close(_locked_root_fd);
            entry = {};
            break;
        }
    }
    for( const auto &entry : g_ProtectedRootRegistry ) {
        if( entry.occupied )
            return;
    }
    g_ProtectedRootRegistryOwnerPID = 0;
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
    return Append(_name, _size, kRecordPrefix) && AppendHex(_name, _size, _id.bytes) &&
           Append(_name, _size, kRecordSuffix) && _size < Capacity && (_name[_size] = '\0', true);
}

template <size_t Capacity>
bool MakeSealManifestName(const ArtifactID &_id, std::array<char, Capacity> &_name, size_t &_size) noexcept
{
    _size = 0;
    return Append(_name, _size, kSealManifestPrefix) && AppendHex(_name, _size, _id.bytes) &&
           Append(_name, _size, kRecordSuffix) && _size < Capacity && (_name[_size] = '\0', true);
}

template <size_t Capacity>
bool MakeLifecycleManifestName(const ArtifactID &_id, std::array<char, Capacity> &_name, size_t &_size) noexcept
{
    _size = 0;
    return Append(_name, _size, kLifecycleManifestPrefix) && AppendHex(_name, _size, _id.bytes) &&
           Append(_name, _size, kRecordSuffix) && _size < Capacity && (_name[_size] = '\0', true);
}

template <size_t Capacity>
bool MakeLifecycleSealManifestName(const ArtifactID &_id, std::array<char, Capacity> &_name, size_t &_size) noexcept
{
    _size = 0;
    return Append(_name, _size, kLifecycleSealManifestPrefix) && AppendHex(_name, _size, _id.bytes) &&
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
        parsed_device != _root_device || parsed_inode != _root_inode ||
        !DecodeHex(*correlation, _record.header.correlation) || !DecodeHex(*artifact, _record.id.bytes) ||
        IsAllZero(_record.id.bytes) )
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

bool IsLifecycleManifestName(const std::string_view _name) noexcept
{
    return _name.size() == kLifecycleManifestPrefix.size() + kArtifactHexBytes + kRecordSuffix.size() &&
           _name.starts_with(kLifecycleManifestPrefix) && _name.ends_with(kRecordSuffix);
}

bool IsLifecycleSealManifestName(const std::string_view _name) noexcept
{
    return _name.size() == kLifecycleSealManifestPrefix.size() + kArtifactHexBytes + kRecordSuffix.size() &&
           _name.starts_with(kLifecycleSealManifestPrefix) && _name.ends_with(kRecordSuffix);
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
               Append(_contents, size, std::string_view{numeric, static_cast<size_t>(written)}) &&
               Append(_contents, size, "\n");
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
    if( _artifact_seal == nullptr || !append_number("artifact_device=", _artifact_seal->device) ||
        !append_number("artifact_inode=", _artifact_seal->inode) ||
        !append_number("artifact_uid=", _artifact_seal->uid) || !append_number("artifact_gid=", _artifact_seal->gid) ||
        !append_number("artifact_mode=", _artifact_seal->mode) ||
        !append_number("artifact_flags=", _artifact_seal->flags) ||
        !append_number("artifact_nlink=", _artifact_seal->link_count) ||
        !append_number("artifact_size=", _artifact_seal->byte_size) ||
        !append_number("artifact_birth_seconds=", _artifact_seal->birth_time.seconds) ||
        !append_number("artifact_birth_nanoseconds=", _artifact_seal->birth_time.nanoseconds) ||
        !append_number("artifact_mtime_seconds=", _artifact_seal->modification_time.seconds) ||
        !append_number("artifact_mtime_nanoseconds=", _artifact_seal->modification_time.nanoseconds) ||
        !append_number("artifact_ctime_seconds=", _artifact_seal->status_change_time.seconds) ||
        !append_number("artifact_ctime_nanoseconds=", _artifact_seal->status_change_time.nanoseconds) )
        return std::unexpected{ProtectedRootLedger::Error::RecordWriteFailed};
    return size;
}

std::expected<size_t, ProtectedRootLedger::Error> WriteRecord(const int _root_fd,
                                                              const uint64_t _root_device,
                                                              const uint64_t _root_inode,
                                                              const Header &_header,
                                                              const ArtifactRole _role,
                                                              const ArtifactID &_id,
                                                              std::array<char, 128> &_name) noexcept
{
    size_t name_size = 0;
    if( !MakeRecordName(_id, _name, name_size) )
        return std::unexpected{ProtectedRootLedger::Error::RecordCreateFailed};
    std::array<char, kRecordMaximumBytes> contents{};
    const auto size =
        BuildRecordContents(_root_device, _root_inode, _header, _role, _id, RecordState::Reserved, nullptr, contents);
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

std::expected<void, ProtectedRootLedger::Error> WriteSealManifest(const int _root_fd,
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

std::optional<ReadRecord>
ReadRecordContents(const int _root_fd, const char *_name, const uint64_t _root_device) noexcept
{
    struct stat named{};
    if( ::fstatat(_root_fd, _name, &named, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(named.st_mode) ||
        named.st_uid != ::geteuid() || named.st_nlink != 1 || (named.st_mode & 07777) != 0600 || named.st_size <= 0 ||
        static_cast<size_t>(named.st_size) >= kRecordMaximumBytes ||
        static_cast<uint64_t>(named.st_dev) != _root_device )
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

struct ReadLifecycleManifest final {
    std::array<char, kLifecycleManifestMaximumBytes> contents{};
    dev_t device{0};
    ino_t inode{0};
    off_t size{0};
    uint32_t flags{0};
};

std::string_view LifecycleManifestContents(const ReadLifecycleManifest &_record) noexcept
{
    if( _record.size <= 0 || static_cast<size_t>(_record.size) >= _record.contents.size() )
        return {};
    return {_record.contents.data(), static_cast<size_t>(_record.size)};
}

std::optional<ReadLifecycleManifest>
ReadLifecycleManifestContents(const int _root_fd, const char *_name, const uint64_t _root_device) noexcept
{
    struct stat named{};
    if( ::fstatat(_root_fd, _name, &named, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(named.st_mode) ||
        named.st_uid != ::geteuid() || named.st_nlink != 1 || (named.st_mode & 07777) != 0600 || named.st_size <= 0 ||
        static_cast<size_t>(named.st_size) >= kLifecycleManifestMaximumBytes ||
        static_cast<uint64_t>(named.st_dev) != _root_device )
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
    ReadLifecycleManifest record{
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

bool HasExactLifecycleManifestIdentity(const int _root_fd,
                                       const char *_name,
                                       const ReadLifecycleManifest &_record) noexcept
{
    struct stat status{};
    return ::fstatat(_root_fd, _name, &status, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(status.st_mode) &&
           status.st_dev == _record.device && status.st_ino == _record.inode && status.st_size == _record.size &&
           status.st_flags == _record.flags && status.st_nlink == 1 && status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) == 0600;
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
           decoded.state == RecordState::Reserved && decoded.header == _header && decoded.role == _role &&
           decoded.id == _id && HasExactRecordIdentity(_root_fd, _name.data(), *record);
}

enum class LifecycleManifestState : uint8_t {
    Reserved,
    Sealed,
};

struct LifecycleManifest final {
    Header header;
    RootIdentity source_root;
    RootIdentity destination_root;
    ArtifactID source_snapshot_id;
    ArtifactID destination_stage_id;
    ObjectSeal source_snapshot_seal;
    ObjectSeal destination_stage_seal;
    ObjectSeal source_seal;
    ObjectSeal destination_parent_seal;
    std::array<uint8_t, kMaximumDestinationComponentBytes> destination_name{};
    uint16_t destination_name_size{0};

    bool operator==(const LifecycleManifest &) const noexcept = default;
};

template <size_t Capacity>
bool AppendLifecycleNumber(std::array<char, Capacity> &_contents,
                           size_t &_size,
                           const std::string_view _prefix,
                           const std::string_view _suffix,
                           const uint64_t _value) noexcept
{
    char numeric[32]{};
    const int written = std::snprintf(numeric, sizeof(numeric), "%llu", static_cast<unsigned long long>(_value));
    return written > 0 && static_cast<size_t>(written) < sizeof(numeric) && Append(_contents, _size, _prefix) &&
           Append(_contents, _size, _suffix) && Append(_contents, _size, "=") &&
           Append(_contents, _size, std::string_view{numeric, static_cast<size_t>(written)}) &&
           Append(_contents, _size, "\n");
}

template <size_t Capacity>
bool AppendLifecycleSeal(std::array<char, Capacity> &_contents,
                         size_t &_size,
                         const std::string_view _prefix,
                         const ObjectSeal &_seal) noexcept
{
    return AppendLifecycleNumber(_contents, _size, _prefix, "device", _seal.device) &&
           AppendLifecycleNumber(_contents, _size, _prefix, "inode", _seal.inode) &&
           AppendLifecycleNumber(_contents, _size, _prefix, "uid", _seal.uid) &&
           AppendLifecycleNumber(_contents, _size, _prefix, "gid", _seal.gid) &&
           AppendLifecycleNumber(_contents, _size, _prefix, "mode", _seal.mode) &&
           AppendLifecycleNumber(_contents, _size, _prefix, "flags", _seal.flags) &&
           AppendLifecycleNumber(_contents, _size, _prefix, "nlink", _seal.link_count) &&
           AppendLifecycleNumber(_contents, _size, _prefix, "size", _seal.byte_size) &&
           AppendLifecycleNumber(
               _contents, _size, _prefix, "birth_seconds", static_cast<uint64_t>(_seal.birth_time.seconds)) &&
           AppendLifecycleNumber(_contents, _size, _prefix, "birth_nanoseconds", _seal.birth_time.nanoseconds) &&
           AppendLifecycleNumber(
               _contents, _size, _prefix, "mtime_seconds", static_cast<uint64_t>(_seal.modification_time.seconds)) &&
           AppendLifecycleNumber(_contents, _size, _prefix, "mtime_nanoseconds", _seal.modification_time.nanoseconds) &&
           AppendLifecycleNumber(
               _contents, _size, _prefix, "ctime_seconds", static_cast<uint64_t>(_seal.status_change_time.seconds)) &&
           AppendLifecycleNumber(_contents, _size, _prefix, "ctime_nanoseconds", _seal.status_change_time.nanoseconds);
}

template <size_t Capacity>
bool AppendLifecycleHex(std::array<char, Capacity> &_contents,
                        size_t &_size,
                        const std::span<const uint8_t> _bytes) noexcept
{
    if( _bytes.size() > (Capacity - _size) / 2 )
        return false;
    for( const uint8_t byte : _bytes ) {
        _contents[_size++] = HexDigit(static_cast<uint8_t>(byte >> 4));
        _contents[_size++] = HexDigit(static_cast<uint8_t>(byte & 0x0F));
    }
    return true;
}

bool BuildLifecycleManifestContents(const LifecycleManifest &_manifest,
                                    const LifecycleManifestState _state,
                                    std::array<char, kLifecycleManifestMaximumBytes> &_contents,
                                    size_t &_size) noexcept
{
    _size = 0;
    if( !Append(_contents, _size, "schema=") || !Append(_contents, _size, kLifecycleSchema) ||
        !Append(_contents, _size, "\ncorrelation=") || !AppendHex(_contents, _size, _manifest.header.correlation) ||
        !Append(_contents, _size, "\n") ||
        !AppendLifecycleNumber(_contents, _size, "source_root_", "device", _manifest.source_root.device) ||
        !AppendLifecycleNumber(_contents, _size, "source_root_", "inode", _manifest.source_root.inode) ||
        !AppendLifecycleNumber(_contents, _size, "destination_root_", "device", _manifest.destination_root.device) ||
        !AppendLifecycleNumber(_contents, _size, "destination_root_", "inode", _manifest.destination_root.inode) ||
        !Append(_contents, _size, "source_snapshot_artifact=") ||
        !AppendHex(_contents, _size, _manifest.source_snapshot_id.bytes) ||
        !Append(_contents, _size, "\ndestination_stage_artifact=") ||
        !AppendHex(_contents, _size, _manifest.destination_stage_id.bytes) || !Append(_contents, _size, "\n") ||
        !AppendLifecycleSeal(_contents, _size, "source_snapshot_", _manifest.source_snapshot_seal) ||
        !AppendLifecycleSeal(_contents, _size, "destination_stage_", _manifest.destination_stage_seal) ||
        !AppendLifecycleSeal(_contents, _size, "source_", _manifest.source_seal) ||
        !AppendLifecycleSeal(_contents, _size, "destination_parent_", _manifest.destination_parent_seal) ||
        !Append(_contents, _size, "destination_name=") ||
        !AppendLifecycleHex(
            _contents,
            _size,
            std::span<const uint8_t>{_manifest.destination_name.data(), _manifest.destination_name_size}) ||
        !Append(
            _contents, _size, _state == LifecycleManifestState::Reserved ? "\nstate=reserved\n" : "\nstate=sealed\n") )
        return false;
    return _size < _contents.size();
}

std::optional<std::string_view> TakeLifecycleField(const std::string_view _contents,
                                                   size_t &_offset,
                                                   const std::string_view _prefix,
                                                   const std::string_view _suffix) noexcept
{
    const auto line = TakeLine(_contents, _offset);
    if( !line || !line->starts_with(_prefix) )
        return std::nullopt;
    return FieldValue(line->substr(_prefix.size()), _suffix);
}

bool ParseLifecycleSeal(const std::string_view _contents,
                        size_t &_offset,
                        const std::string_view _prefix,
                        ObjectSeal &_seal) noexcept
{
    const auto device = TakeLifecycleField(_contents, _offset, _prefix, "device");
    const auto inode = TakeLifecycleField(_contents, _offset, _prefix, "inode");
    const auto uid = TakeLifecycleField(_contents, _offset, _prefix, "uid");
    const auto gid = TakeLifecycleField(_contents, _offset, _prefix, "gid");
    const auto mode = TakeLifecycleField(_contents, _offset, _prefix, "mode");
    const auto flags = TakeLifecycleField(_contents, _offset, _prefix, "flags");
    const auto link_count = TakeLifecycleField(_contents, _offset, _prefix, "nlink");
    const auto byte_size = TakeLifecycleField(_contents, _offset, _prefix, "size");
    const auto birth_seconds = TakeLifecycleField(_contents, _offset, _prefix, "birth_seconds");
    const auto birth_nanoseconds = TakeLifecycleField(_contents, _offset, _prefix, "birth_nanoseconds");
    const auto modification_seconds = TakeLifecycleField(_contents, _offset, _prefix, "mtime_seconds");
    const auto modification_nanoseconds = TakeLifecycleField(_contents, _offset, _prefix, "mtime_nanoseconds");
    const auto status_change_seconds = TakeLifecycleField(_contents, _offset, _prefix, "ctime_seconds");
    const auto status_change_nanoseconds = TakeLifecycleField(_contents, _offset, _prefix, "ctime_nanoseconds");
    return ParseUint64(device, _seal.device) && ParseUint64(inode, _seal.inode) && ParseUint32(uid, _seal.uid) &&
           ParseUint32(gid, _seal.gid) && ParseUint32(mode, _seal.mode) && ParseUint32(flags, _seal.flags) &&
           ParseUint64(link_count, _seal.link_count) && ParseUint64(byte_size, _seal.byte_size) &&
           ParseTimestamp(birth_seconds, birth_nanoseconds, _seal.birth_time) &&
           ParseTimestamp(modification_seconds, modification_nanoseconds, _seal.modification_time) &&
           ParseTimestamp(status_change_seconds, status_change_nanoseconds, _seal.status_change_time);
}

bool DecodeLifecycleDestinationName(const std::string_view _encoded,
                                    std::array<uint8_t, kMaximumDestinationComponentBytes> &_destination_name,
                                    uint16_t &_destination_name_size) noexcept
{
    if( _encoded.empty() || (_encoded.size() & 1) != 0 || _encoded.size() / 2 > _destination_name.size() )
        return false;
    _destination_name_size = static_cast<uint16_t>(_encoded.size() / 2);
    for( size_t index = 0; index < _destination_name_size; ++index ) {
        const auto high = DecodeHexDigit(_encoded[index * 2]);
        const auto low = DecodeHexDigit(_encoded[index * 2 + 1]);
        if( !high || !low )
            return false;
        _destination_name[index] = static_cast<uint8_t>((*high << 4) | *low);
    }
    return true;
}

bool DecodeLifecycleManifest(const std::string_view _contents,
                             LifecycleManifest &_manifest,
                             LifecycleManifestState &_state) noexcept
{
    if( _contents.find('\0') != std::string_view::npos )
        return false;
    size_t offset = 0;
    const auto schema = FieldValue(TakeLine(_contents, offset), "schema");
    const auto correlation = FieldValue(TakeLine(_contents, offset), "correlation");
    const auto source_root_device = TakeLifecycleField(_contents, offset, "source_root_", "device");
    const auto source_root_inode = TakeLifecycleField(_contents, offset, "source_root_", "inode");
    const auto destination_root_device = TakeLifecycleField(_contents, offset, "destination_root_", "device");
    const auto destination_root_inode = TakeLifecycleField(_contents, offset, "destination_root_", "inode");
    const auto source_snapshot_artifact = FieldValue(TakeLine(_contents, offset), "source_snapshot_artifact");
    const auto destination_stage_artifact = FieldValue(TakeLine(_contents, offset), "destination_stage_artifact");
    if( !schema || *schema != kLifecycleSchema || !correlation || !source_root_device || !source_root_inode ||
        !destination_root_device || !destination_root_inode || !source_snapshot_artifact ||
        !destination_stage_artifact || !DecodeHex(*correlation, _manifest.header.correlation) ||
        !ParseUnsigned(*source_root_device, _manifest.source_root.device) ||
        !ParseUnsigned(*source_root_inode, _manifest.source_root.inode) ||
        !ParseUnsigned(*destination_root_device, _manifest.destination_root.device) ||
        !ParseUnsigned(*destination_root_inode, _manifest.destination_root.inode) ||
        !DecodeHex(*source_snapshot_artifact, _manifest.source_snapshot_id.bytes) ||
        !DecodeHex(*destination_stage_artifact, _manifest.destination_stage_id.bytes) ||
        IsAllZero(_manifest.source_snapshot_id.bytes) || IsAllZero(_manifest.destination_stage_id.bytes) )
        return false;
    _manifest.header.version = kProtocolVersion;
    if( !Validate(_manifest.header) || _manifest.source_root.device == 0 || _manifest.source_root.inode == 0 ||
        _manifest.destination_root.device == 0 || _manifest.destination_root.inode == 0 ||
        !ParseLifecycleSeal(_contents, offset, "source_snapshot_", _manifest.source_snapshot_seal) ||
        !ParseLifecycleSeal(_contents, offset, "destination_stage_", _manifest.destination_stage_seal) ||
        !ParseLifecycleSeal(_contents, offset, "source_", _manifest.source_seal) ||
        !ParseLifecycleSeal(_contents, offset, "destination_parent_", _manifest.destination_parent_seal) )
        return false;
    const auto destination_name = FieldValue(TakeLine(_contents, offset), "destination_name");
    const auto state = FieldValue(TakeLine(_contents, offset), "state");
    if( !destination_name || !state || offset != _contents.size() ||
        !DecodeLifecycleDestinationName(
            *destination_name, _manifest.destination_name, _manifest.destination_name_size) ||
        !IsValidArtifactSeal(_manifest.source_snapshot_seal, _manifest.source_root.device) ||
        !IsValidArtifactSeal(_manifest.destination_stage_seal, _manifest.destination_root.device) ||
        _manifest.source_snapshot_seal.byte_size != _manifest.source_seal.byte_size ||
        _manifest.destination_stage_seal.byte_size != _manifest.source_seal.byte_size ||
        _manifest.source_seal.device != _manifest.source_root.device ||
        _manifest.destination_parent_seal.device != _manifest.destination_root.device )
        return false;
    if( *state == "reserved" )
        _state = LifecycleManifestState::Reserved;
    else if( *state == "sealed" )
        _state = LifecycleManifestState::Sealed;
    else
        return false;
    const auto component = DestinationComponent::Create(
        std::span<const uint8_t>{_manifest.destination_name.data(), _manifest.destination_name_size});
    if( !component )
        return false;
    const BeginRequest request{
        .header = _manifest.header,
        .source = _manifest.source_seal,
        .destination_parent = _manifest.destination_parent_seal,
        .destination_name = *component,
    };
    return Validate(request).has_value();
}

bool WriteAll(const int _fd, const char *_buffer, size_t _size) noexcept
{
    while( _size != 0 ) {
        ssize_t written = -1;
        do {
            written = ::write(_fd, _buffer, _size);
        } while( written < 0 && errno == EINTR );
        if( written <= 0 )
            return false;
        _buffer += written;
        _size -= static_cast<size_t>(written);
    }
    return true;
}

bool WriteLifecycleRecord(const int _root_fd,
                          const std::array<char, 128> &_name,
                          const size_t _name_size,
                          const LifecycleManifest &_manifest,
                          const LifecycleManifestState _state) noexcept
{
    std::array<char, kLifecycleManifestMaximumBytes> contents{};
    size_t contents_size = 0;
    if( _name_size == 0 || !BuildLifecycleManifestContents(_manifest, _state, contents, contents_size) )
        return false;
    const int fd = ::openat(_root_fd, _name.data(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if( fd < 0 )
        return false;
    const bool written = ::fchmod(fd, 0600) == 0 && WriteAll(fd, contents.data(), contents_size) && ::fsync(fd) == 0 &&
                         ::fsync(_root_fd) == 0 && ::fcntl(fd, F_FULLFSYNC) == 0;
    const bool closed = ::close(fd) == 0;
    if( !written || !closed )
        return false;
    return true;
}

bool WriteLifecycleManifest(const int _root_fd,
                            const ArtifactID &_local_artifact,
                            const LifecycleManifest &_manifest) noexcept
{
    std::array<char, 128> primary_name{};
    size_t primary_name_size = 0;
    std::array<char, 128> sealed_name{};
    size_t sealed_name_size = 0;
    if( !MakeLifecycleManifestName(_local_artifact, primary_name, primary_name_size) || primary_name_size == 0 ||
        !MakeLifecycleSealManifestName(_local_artifact, sealed_name, sealed_name_size) || sealed_name_size == 0 )
        return false;
    return WriteLifecycleRecord(
               _root_fd, primary_name, primary_name_size, _manifest, LifecycleManifestState::Reserved) &&
           WriteLifecycleRecord(_root_fd, sealed_name, sealed_name_size, _manifest, LifecycleManifestState::Sealed);
}

bool HasExactSealedArtifact(const int _root_fd,
                            const uint64_t _root_device,
                            const uint64_t _root_inode,
                            const Header &_header,
                            const ArtifactRole _role,
                            const ArtifactID &_id,
                            const ObjectSeal &_seal) noexcept
{
    std::array<char, 128> reservation_name{};
    if( !HasExactReservation(_root_fd, _root_device, _root_inode, _header, _role, _id, reservation_name) )
        return false;
    std::array<char, 128> artifact_name{};
    size_t artifact_name_size = 0;
    std::array<char, 128> seal_manifest_name{};
    size_t seal_manifest_name_size = 0;
    if( !MakeArtifactName(_id, artifact_name, artifact_name_size) || artifact_name_size == 0 ||
        !MakeSealManifestName(_id, seal_manifest_name, seal_manifest_name_size) || seal_manifest_name_size == 0 )
        return false;
    const auto sealed_record = ReadRecordContents(_root_fd, seal_manifest_name.data(), _root_device);
    DecodedRecord sealed{};
    const auto artifact = ReadArtifactSeal(_root_fd, artifact_name.data(), _root_device);
    return sealed_record && DecodeRecord(RecordContents(*sealed_record), _root_device, _root_inode, sealed) &&
           sealed.state == RecordState::Sealed && sealed.header == _header && sealed.role == _role &&
           sealed.id == _id && sealed.artifact_seal == _seal &&
           HasExactRecordIdentity(_root_fd, seal_manifest_name.data(), *sealed_record) && artifact &&
           *artifact == _seal;
}

bool HasExactLifecycleManifest(const int _root_fd,
                               const uint64_t _root_device,
                               const ArtifactID &_local_artifact,
                               const LifecycleManifest &_expected) noexcept
{
    std::array<char, 128> primary_name{};
    size_t primary_name_size = 0;
    std::array<char, 128> sealed_name{};
    size_t sealed_name_size = 0;
    if( !MakeLifecycleManifestName(_local_artifact, primary_name, primary_name_size) || primary_name_size == 0 ||
        !MakeLifecycleSealManifestName(_local_artifact, sealed_name, sealed_name_size) || sealed_name_size == 0 )
        return false;
    const auto primary = ReadLifecycleManifestContents(_root_fd, primary_name.data(), _root_device);
    const auto sealed = ReadLifecycleManifestContents(_root_fd, sealed_name.data(), _root_device);
    LifecycleManifest decoded_primary{};
    LifecycleManifest decoded_sealed{};
    LifecycleManifestState primary_state{};
    LifecycleManifestState sealed_state{};
    return primary && sealed &&
           DecodeLifecycleManifest(LifecycleManifestContents(*primary), decoded_primary, primary_state) &&
           DecodeLifecycleManifest(LifecycleManifestContents(*sealed), decoded_sealed, sealed_state) &&
           primary_state == LifecycleManifestState::Reserved && sealed_state == LifecycleManifestState::Sealed &&
           decoded_primary == _expected && decoded_sealed == _expected &&
           HasExactLifecycleManifestIdentity(_root_fd, primary_name.data(), *primary) &&
           HasExactLifecycleManifestIdentity(_root_fd, sealed_name.data(), *sealed);
}

bool HasLifecycleManifestEntry(const int _root_fd, const ArtifactID &_local_artifact) noexcept
{
    std::array<char, 128> primary_name{};
    size_t primary_name_size = 0;
    std::array<char, 128> sealed_name{};
    size_t sealed_name_size = 0;
    if( !MakeLifecycleManifestName(_local_artifact, primary_name, primary_name_size) || primary_name_size == 0 ||
        !MakeLifecycleSealManifestName(_local_artifact, sealed_name, sealed_name_size) || sealed_name_size == 0 )
        return true;
    struct stat status{};
    if( ::fstatat(_root_fd, primary_name.data(), &status, AT_SYMLINK_NOFOLLOW) == 0 )
        return true;
    const int primary_error = errno;
    if( ::fstatat(_root_fd, sealed_name.data(), &status, AT_SYMLINK_NOFOLLOW) == 0 )
        return true;
    const int sealed_error = errno;
    // A failed probe is not absence.  Reconciliation may remove a bare R only when both deterministic lifecycle
    // names are conclusively absent; otherwise an interrupted lifecycle record stays retained.
    return primary_error != ENOENT || sealed_error != ENOENT;
}

struct LifecycleScan final {
    bool malformed{false};
    bool ambiguous{false};
    std::optional<LifecycleManifest> primary;
    std::optional<LifecycleManifest> manifest;
};

std::optional<LifecycleScan> ScanLifecycleManifests(const int _root_fd,
                                                    const RootIdentity _root,
                                                    const Header &_header,
                                                    const bool _source_side) noexcept
{
    const int scan_fd = ::openat(_root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if( scan_fd < 0 )
        return std::nullopt;
    DIR *directory = ::fdopendir(scan_fd);
    if( directory == nullptr ) {
        ::close(scan_fd);
        return std::nullopt;
    }
    LifecycleScan result;
    int read_error = 0;
    while( true ) {
        errno = 0;
        const dirent *entry = ::readdir(directory);
        if( entry == nullptr ) {
            read_error = errno;
            break;
        }
        const std::string_view name{entry->d_name};
        const bool primary = IsLifecycleManifestName(name);
        const bool sealed = IsLifecycleSealManifestName(name);
        if( !primary && !sealed )
            continue;
        const auto record = ReadLifecycleManifestContents(_root_fd, entry->d_name, _root.device);
        LifecycleManifest manifest{};
        LifecycleManifestState state{};
        if( !record || !DecodeLifecycleManifest(LifecycleManifestContents(*record), manifest, state) ||
            !HasExactLifecycleManifestIdentity(_root_fd, entry->d_name, *record) ) {
            result.malformed = true;
            continue;
        }
        const ArtifactID &local_id = _source_side ? manifest.source_snapshot_id : manifest.destination_stage_id;
        std::array<char, 128> expected_name{};
        size_t expected_name_size = 0;
        const bool expected_name_built =
            primary ? MakeLifecycleManifestName(local_id, expected_name, expected_name_size)
                    : MakeLifecycleSealManifestName(local_id, expected_name, expected_name_size);
        const auto expected_state = primary ? LifecycleManifestState::Reserved : LifecycleManifestState::Sealed;
        if( !expected_name_built || expected_name_size == 0 || state != expected_state ||
            name != std::string_view{expected_name.data(), expected_name_size} ||
            (_source_side ? manifest.source_root : manifest.destination_root) != _root ) {
            result.malformed = true;
            continue;
        }
        if( manifest.header != _header )
            continue;
        if( primary ) {
            if( result.primary )
                result.ambiguous = true;
            else
                result.primary = std::move(manifest);
            continue;
        }
        if( result.manifest )
            result.ambiguous = true;
        else
            result.manifest = std::move(manifest);
    }
    if( ::closedir(directory) != 0 || read_error != 0 )
        return std::nullopt;
    return result;
}

std::optional<RootIdentity> ReadProtectedRootIdentity(const int _borrowed_root_fd) noexcept
{
    struct stat status{};
    if( _borrowed_root_fd < 0 || ::fstat(_borrowed_root_fd, &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != ::geteuid() || (status.st_mode & 07777) != 0700 || status.st_dev == 0 || status.st_ino == 0 )
        return std::nullopt;
    return RootIdentity{
        .device = static_cast<uint64_t>(status.st_dev),
        .inode = static_cast<uint64_t>(status.st_ino),
    };
}

StagingPublicationLifecycle::Error MapLifecycleRootOpenError(const ProtectedRootLedger::Error _error,
                                                             const bool _source_root) noexcept
{
    switch( _error ) {
        case ProtectedRootLedger::Error::InvalidRoot:
            return _source_root ? StagingPublicationLifecycle::Error::SourceRootInvalid
                                : StagingPublicationLifecycle::Error::DestinationRootInvalid;
        case ProtectedRootLedger::Error::RootBusy:
            return _source_root ? StagingPublicationLifecycle::Error::SourceRootBusy
                                : StagingPublicationLifecycle::Error::DestinationRootBusy;
        case ProtectedRootLedger::Error::ForkedProcess:
            return StagingPublicationLifecycle::Error::ForkedProcess;
        default:
            return StagingPublicationLifecycle::Error::RootLockFailed;
    }
}

class ScopedFD final
{
public:
    explicit ScopedFD(const int _fd = -1) noexcept : m_FD{_fd} {}
    ScopedFD(const ScopedFD &) = delete;
    ScopedFD &operator=(const ScopedFD &) = delete;
    ~ScopedFD() noexcept
    {
        if( m_FD >= 0 )
            ::close(m_FD);
    }

    [[nodiscard]] int Get() const noexcept { return m_FD; }
    [[nodiscard]] int Release() noexcept { return std::exchange(m_FD, -1); }

private:
    int m_FD;
};

bool IsReadOnlyCloexecDescriptor(const int _fd) noexcept
{
    const int descriptor_flags = ::fcntl(_fd, F_GETFD);
    if( descriptor_flags < 0 || (descriptor_flags & FD_CLOEXEC) == 0 )
        return false;
    const int status_flags = ::fcntl(_fd, F_GETFL);
    return status_flags >= 0 && (status_flags & O_ACCMODE) == O_RDONLY;
}

bool IsExactProtectedRoot(const int _root_fd, const uint64_t _root_device, const uint64_t _root_inode) noexcept
{
    struct stat status{};
    return ::fstat(_root_fd, &status) == 0 && S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & 07777) == 0700 && static_cast<uint64_t>(status.st_dev) == _root_device &&
           static_cast<uint64_t>(status.st_ino) == _root_inode;
}

bool HasExactReadOnlySeal(const int _fd, const ObjectSeal &_expected) noexcept
{
    struct stat status{};
    ObjectSeal actual;
    return IsReadOnlyCloexecDescriptor(_fd) && ::fstat(_fd, &status) == 0 && SealFromStat(status, actual) &&
           actual == _expected;
}

bool HasExactSourceSeal(const int _fd, const ObjectSeal &_expected) noexcept
{
    return HasExactReadOnlySeal(_fd, _expected);
}

int OpenExactArtifactForRead(const int _root_fd,
                             const char *_name,
                             const uint64_t _root_device,
                             const ObjectSeal &_expected) noexcept
{
    struct stat named{};
    if( ::fstatat(_root_fd, _name, &named, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(named.st_mode) ||
        named.st_uid != ::geteuid() || named.st_nlink != 1 || (named.st_mode & 07777) != 0600 ||
        static_cast<uint64_t>(named.st_dev) != _root_device )
        return -1;

    const int fd = ::openat(_root_fd, _name, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if( fd < 0 )
        return -1;
    struct stat opened{};
    ObjectSeal actual;
    if( !IsReadOnlyCloexecDescriptor(fd) || ::fstat(fd, &opened) != 0 || opened.st_dev != named.st_dev ||
        opened.st_ino != named.st_ino || opened.st_size != named.st_size || opened.st_uid != named.st_uid ||
        opened.st_nlink != named.st_nlink || opened.st_mode != named.st_mode || opened.st_flags != named.st_flags ||
        !SealFromStat(opened, actual) || !IsValidArtifactSeal(actual, _root_device) || actual != _expected ) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int OpenExactRootForRead(const int _root_fd, const uint64_t _root_device, const uint64_t _root_inode) noexcept
{
    const int fd = ::openat(_root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if( fd < 0 )
        return -1;
    struct stat status{};
    if( !IsReadOnlyCloexecDescriptor(fd) || ::fstat(fd, &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != ::geteuid() || (status.st_mode & 07777) != 0700 ||
        static_cast<uint64_t>(status.st_dev) != _root_device || static_cast<uint64_t>(status.st_ino) != _root_inode ) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

ProtectedRootLedger::ProtectedRootLedger(const int _root_fd,
                                         const uint64_t _device,
                                         const uint64_t _inode,
                                         const bool _registered_root,
                                         const int _owner_pid) noexcept
    : m_RootFD{_root_fd}, m_RootDevice{_device}, m_RootInode{_inode}, m_RegisteredRoot{_registered_root},
      m_OwnerPID{_owner_pid}
{
}

std::expected<ProtectedRootLedger, ProtectedRootLedger::Error>
ProtectedRootLedger::Open(const int _borrowed_root_fd) noexcept
{
    if( _borrowed_root_fd < 0 )
        return std::unexpected{Error::InvalidRoot};
    if( !HasProtectedRootRegistryAtForkHandler() )
        return std::unexpected{Error::RootLockFailed};
    const int root_fd = ::openat(_borrowed_root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
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
    const auto registered = RegisterProtectedRoot(identity, root_fd);
    if( !registered ) {
        ::close(root_fd);
        return std::unexpected{registered.error()};
    }
    if( ::flock(root_fd, LOCK_EX | LOCK_NB) != 0 ) {
        const int lock_error = errno;
        CloseAndUnregisterProtectedRoot(identity, root_fd);
        return std::unexpected{lock_error == EWOULDBLOCK || lock_error == EAGAIN ? Error::RootBusy
                                                                                 : Error::RootLockFailed};
    }
    if( !IsExactProtectedRoot(root_fd, identity.device, identity.inode) ) {
        // Do not explicitly unlock: only the final close of this open-file description may release its flock.
        CloseAndUnregisterProtectedRoot(identity, root_fd);
        return std::unexpected{Error::RootLockFailed};
    }
    return ProtectedRootLedger{root_fd, identity.device, identity.inode, true, static_cast<int>(::getpid())};
}

ProtectedRootLedger::ProtectedRootLedger(ProtectedRootLedger &&_rhs) noexcept : m_RootFD{-1}
{
    // A child after fork must never acquire an inherited C++ mutex.  The atfork handler has already closed the
    // corresponding root FD; leave this destination inert rather than attempting to move stale authority.
    if( _rhs.m_OwnerPID != static_cast<int>(::getpid()) )
        return;
    std::lock_guard lock{_rhs.m_Mutex};
    m_RootFD = std::exchange(_rhs.m_RootFD, -1);
    m_RootDevice = _rhs.m_RootDevice;
    m_RootInode = _rhs.m_RootInode;
    m_RegisteredRoot = std::exchange(_rhs.m_RegisteredRoot, false);
    m_OwnerPID = _rhs.m_OwnerPID;
    m_ActiveReservations = std::move(_rhs.m_ActiveReservations);
}

ProtectedRootLedger &ProtectedRootLedger::operator=(ProtectedRootLedger &&_rhs) noexcept
{
    if( this == &_rhs )
        return *this;
    const int current_pid = static_cast<int>(::getpid());
    // See the move constructor: both mutexes may have been inherited while locked by another parent thread.
    // The atfork handler makes an inherited ledger inert, but does not make its C++ mutex safe to acquire.  Do not
    // special-case that inert representation here: any owner-PID mismatch is foreign authority and must fail closed.
    if( m_OwnerPID != current_pid || _rhs.m_OwnerPID != current_pid )
        return *this;
    int retired_root_fd = -1;
    RootIdentity retired_identity;
    bool unregister_retired_root = false;
    bool close_retired_root = false;
    {
        std::scoped_lock lock{m_Mutex, _rhs.m_Mutex};
        retired_root_fd = std::exchange(m_RootFD, -1);
        retired_identity = {.device = m_RootDevice, .inode = m_RootInode};
        unregister_retired_root = std::exchange(m_RegisteredRoot, false);
        close_retired_root = m_OwnerPID == current_pid;
        m_RootFD = std::exchange(_rhs.m_RootFD, -1);
        m_RootDevice = _rhs.m_RootDevice;
        m_RootInode = _rhs.m_RootInode;
        m_RegisteredRoot = std::exchange(_rhs.m_RegisteredRoot, false);
        m_OwnerPID = _rhs.m_OwnerPID;
        m_ActiveReservations = std::move(_rhs.m_ActiveReservations);
    }
    if( close_retired_root && unregister_retired_root )
        CloseAndUnregisterProtectedRoot(retired_identity, retired_root_fd);
    return *this;
}

ProtectedRootLedger::~ProtectedRootLedger() noexcept
{
    // Never lock an inherited mutex or close a descriptor number that the child may have reused after the atfork
    // handler invalidated this authority.
    if( m_OwnerPID != static_cast<int>(::getpid()) )
        return;
    int root_fd = -1;
    RootIdentity identity;
    bool unregister_root = false;
    bool close_root = false;
    {
        std::lock_guard lock{m_Mutex};
        root_fd = std::exchange(m_RootFD, -1);
        identity = {.device = m_RootDevice, .inode = m_RootInode};
        unregister_root = std::exchange(m_RegisteredRoot, false);
        close_root = true;
    }
    if( close_root && unregister_root )
        CloseAndUnregisterProtectedRoot(identity, root_fd);
}

std::expected<ProtectedRootLedger::Reservation, ProtectedRootLedger::Error>
ProtectedRootLedger::Reserve(const Header &_header, const ArtifactRole _role) noexcept
{
    if( m_OwnerPID != static_cast<int>(::getpid()) )
        return std::unexpected{Error::InvalidRoot};
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
        if( SecRandomCopyBytes(kSecRandomDefault, id.bytes.size(), id.bytes.data()) != errSecSuccess ||
            IsAllZero(id.bytes) )
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
    if( m_OwnerPID != static_cast<int>(::getpid()) )
        return std::unexpected{Error::InvalidRoot};
    std::lock_guard lock{m_Mutex};
    if( !_reservation.m_Valid )
        return std::unexpected{Error::UnknownReservation};
    _reservation.m_Valid = false;
    if( !HasValidRootUnlocked() )
        return std::unexpected{Error::InvalidRoot};
    if( _reservation.m_Role != ArtifactRole::DestinationStage )
        return std::unexpected{Error::InvalidRole};

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
        sealed.state != RecordState::Sealed || sealed.header != _reservation.m_Header ||
        sealed.role != _reservation.m_Role || sealed.id != _reservation.m_ID || sealed.artifact_seal != artifact_seal ||
        !HasExactRecordIdentity(m_RootFD, seal_manifest_name.data(), *sealed_record) || !sealed_artifact ||
        *sealed_artifact != artifact_seal )
        return std::unexpected{Error::PostSealValidationFailed};
    return {};
}

std::expected<void, ProtectedRootLedger::Error> ProtectedRootLedger::Release(Reservation &&_reservation) noexcept
{
    if( m_OwnerPID != static_cast<int>(::getpid()) )
        return std::unexpected{Error::InvalidRoot};
    std::lock_guard lock{m_Mutex};
    if( !_reservation.m_Valid )
        return std::unexpected{Error::UnknownReservation};
    _reservation.m_Valid = false;
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

std::expected<ProtectedRootLedger::ReconcileResult, ProtectedRootLedger::Error>
ProtectedRootLedger::Reconcile() noexcept
{
    if( m_OwnerPID != static_cast<int>(::getpid()) )
        return std::unexpected{Error::InvalidRoot};
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
        if( IsSealManifestName(name) || IsLifecycleManifestName(name) || IsLifecycleSealManifestName(name) )
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
        if( !MakeRecordName(record.id, expected_name, expected_size) ||
            name != std::string_view{expected_name.data(), expected_size} ) {
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
        if( !MakeSealManifestName(record.id, seal_manifest_name, seal_manifest_name_size) ||
            seal_manifest_name_size == 0 ) {
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
        if( !IsArtifactAbsent(m_RootFD, artifact_name.data()) || !IsEntryAbsent(m_RootFD, seal_manifest_name.data()) ||
            HasLifecycleManifestEntry(m_RootFD, record.id) ) {
            ++result.retained_incomplete_artifacts;
            ++result.retained_records;
            continue;
        }
        if( !HasExactRecordIdentity(m_RootFD, entry->d_name, *contents) ||
            ::unlinkat(m_RootFD, entry->d_name, 0) != 0 ) {
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
    if( m_OwnerPID != static_cast<int>(::getpid()) )
        return 0;
    std::lock_guard lock{m_Mutex};
    return ActiveReservationCountUnlocked();
}

bool ProtectedRootLedger::HasValidRootUnlocked() const noexcept
{
    if( m_RootFD < 0 || !m_RegisteredRoot || m_OwnerPID != static_cast<int>(::getpid()) )
        return false;
    return IsExactProtectedRoot(m_RootFD, m_RootDevice, m_RootInode);
}

bool ProtectedRootLedger::MatchesRootIdentity(const uint64_t _device, const uint64_t _inode) const noexcept
{
    // A fork child must reject before it touches a potentially inherited mutex.  The identity comparison is a
    // read-only authority check for StagingRootAuthority immediately after it anchors and locks a borrowed root.
    if( m_OwnerPID != static_cast<int>(::getpid()) )
        return false;
    std::lock_guard lock{m_Mutex};
    return HasValidRootUnlocked() && m_RootDevice == _device && m_RootInode == _inode;
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

StagingRootAuthority::LockedSession::LockedSession(LeaseStore::TerminalLease _terminal_lease,
                                                   ProtectedRootLedger _source_root,
                                                   ProtectedRootLedger _destination_root) noexcept
    : m_CreatorPID{static_cast<int>(::getpid())}, m_TerminalLease{std::move(_terminal_lease)},
      m_SourceRoot{std::move(_source_root)}, m_DestinationRoot{std::move(_destination_root)}
{
}

StagingRootAuthority::LockedSession::LockedSession(LockedSession &&_other) noexcept : m_CreatorPID{_other.m_CreatorPID}
{
    // Neither the session nor either ledger may touch an inherited C++ mutex or root descriptor after fork.
    if( !_other.IsCreatedByCurrentProcess() )
        return;

    m_TerminalLease.emplace(std::move(*_other.m_TerminalLease));
    m_SourceRoot.emplace(std::move(*_other.m_SourceRoot));
    m_DestinationRoot.emplace(std::move(*_other.m_DestinationRoot));
    _other.m_TerminalLease.reset();
    _other.m_SourceRoot.reset();
    _other.m_DestinationRoot.reset();
}

bool StagingRootAuthority::LockedSession::IsCreatedByCurrentProcess() const noexcept
{
    return m_CreatorPID == static_cast<int>(::getpid()) && m_TerminalLease && m_SourceRoot && m_DestinationRoot &&
           m_TerminalLease->IsCreatedByCurrentProcess();
}

std::expected<StagingRootAuthority::LockedSession, StagingRootAuthority::Error>
StagingRootAuthority::Acquire(LeaseStore::TerminalLease _terminal_lease,
                              const int _borrowed_source_root_fd,
                              const int _borrowed_destination_root_fd) noexcept
{
    // Distinguish a consumed same-process lease from an inherited lease before borrowing either root descriptor.
    // The former is an invalid one-use capability; the latter must fail closed without touching inherited state.
    if( !_terminal_lease.HasCurrentProcessCreator() )
        return std::unexpected{Error::ForkedProcess};
    if( !_terminal_lease.IsCreatedByCurrentProcess() || !_terminal_lease.IsCommit() )
        return std::unexpected{Error::InvalidTerminalLease};
    if( !Validate(_terminal_lease.Request()) )
        return std::unexpected{Error::InvalidTerminalLease};

    const auto source_root = AnchorStagingRoot(_borrowed_source_root_fd, Error::SourceRootInvalid);
    if( !source_root )
        return std::unexpected{source_root.error()};
    const auto destination_root = AnchorStagingRoot(_borrowed_destination_root_fd, Error::DestinationRootInvalid);
    if( !destination_root )
        return std::unexpected{destination_root.error()};

    const auto source_identity = PrevalidateStagingRoot(source_root->Get(),
                                                        _terminal_lease.Request().source.device,
                                                        Error::SourceRootInvalid,
                                                        Error::SourceRootDeviceMismatch);
    if( !source_identity )
        return std::unexpected{source_identity.error()};
    const auto destination_identity = PrevalidateStagingRoot(destination_root->Get(),
                                                             _terminal_lease.Request().destination_parent.device,
                                                             Error::DestinationRootInvalid,
                                                             Error::DestinationRootDeviceMismatch);
    if( !destination_identity )
        return std::unexpected{destination_identity.error()};
    if( source_identity->device == destination_identity->device )
        return std::unexpected{Error::RootsOnSameDevice};

    const bool source_first = IsCanonicalRootOrder(*source_identity, *destination_identity);
    const int first_root_fd = source_first ? source_root->Get() : destination_root->Get();
    const int second_root_fd = source_first ? destination_root->Get() : source_root->Get();
    const RootIdentity first_identity = source_first ? *source_identity : *destination_identity;
    const RootIdentity second_identity = source_first ? *destination_identity : *source_identity;

    auto first_ledger = ProtectedRootLedger::Open(first_root_fd);
    if( !first_ledger )
        return std::unexpected{MapProtectedRootOpenError(first_ledger.error(), source_first)};
    if( !first_ledger->MatchesRootIdentity(first_identity.device, first_identity.inode) )
        return std::unexpected{source_first ? Error::SourceRootLockFailed : Error::DestinationRootLockFailed};

    // `first_ledger` is a local RAII guard.  If the second exact root cannot be acquired, its destructor releases
    // the first lock before the error reaches a caller.
    auto second_ledger = ProtectedRootLedger::Open(second_root_fd);
    if( !second_ledger )
        return std::unexpected{MapProtectedRootOpenError(second_ledger.error(), !source_first)};
    if( !second_ledger->MatchesRootIdentity(second_identity.device, second_identity.inode) )
        return std::unexpected{source_first ? Error::DestinationRootLockFailed : Error::SourceRootLockFailed};

    if( source_first )
        return LockedSession{std::move(_terminal_lease), std::move(*first_ledger), std::move(*second_ledger)};
    return LockedSession{std::move(_terminal_lease), std::move(*second_ledger), std::move(*first_ledger)};
}

std::expected<void, StagingPublicationLifecycle::Error>
StagingPublicationLifecycle::RecordStaged(ProtectedRootLedger &_source_root,
                                          ProtectedRootLedger &_destination_root,
                                          const DestinationStageWriter::SealedDestinationStage &_stage) noexcept
{
    const int current_pid = static_cast<int>(::getpid());
    const auto &snapshot = _stage.m_SourceSnapshot;
    const auto &terminal_lease = snapshot.m_TerminalLease;
    if( (_stage.m_CreatorPID >= 0 && _stage.m_CreatorPID != current_pid) ||
        (snapshot.m_CreatorPID >= 0 && snapshot.m_CreatorPID != current_pid) ||
        !terminal_lease.HasCurrentProcessCreator() )
        return std::unexpected{Error::ForkedProcess};
    if( !_stage.IsCreatedByCurrentProcess() || !snapshot.IsCreatedByCurrentProcess() ||
        !terminal_lease.IsCreatedByCurrentProcess() || !terminal_lease.IsCommit() )
        return std::unexpected{Error::InvalidStage};
    if( _source_root.m_OwnerPID != current_pid )
        return std::unexpected{Error::SourceRootInvalid};
    if( _destination_root.m_OwnerPID != current_pid )
        return std::unexpected{Error::DestinationRootInvalid};
    if( _source_root.m_RootDevice == _destination_root.m_RootDevice )
        return std::unexpected{Error::RootsOnSameDevice};

    std::unique_lock source_lock{_source_root.m_Mutex, std::defer_lock};
    std::unique_lock destination_lock{_destination_root.m_Mutex, std::defer_lock};
    if( IsCanonicalRootOrder({.device = _source_root.m_RootDevice, .inode = _source_root.m_RootInode},
                             {.device = _destination_root.m_RootDevice, .inode = _destination_root.m_RootInode}) ) {
        source_lock.lock();
        destination_lock.lock();
    }
    else {
        destination_lock.lock();
        source_lock.lock();
    }

    if( !_source_root.HasValidRootUnlocked() )
        return std::unexpected{Error::SourceRootInvalid};
    if( !_destination_root.HasValidRootUnlocked() )
        return std::unexpected{Error::DestinationRootInvalid};
    const BeginRequest &request = terminal_lease.Request();
    const auto descriptors = terminal_lease.Descriptors();
    if( !Validate(request) || snapshot.m_Header != request.header || snapshot.m_SourceSeal != request.source ||
        _stage.m_ID.bytes == ArtifactID{}.bytes || snapshot.m_ID.bytes == ArtifactID{}.bytes ||
        _source_root.m_RootDevice != request.source.device ||
        _destination_root.m_RootDevice != request.destination_parent.device ||
        _stage.m_RootDevice != _destination_root.m_RootDevice || _stage.m_RootInode != _destination_root.m_RootInode ||
        !IsReadOnlyCloexecDescriptor(_stage.m_RootFD) ||
        !IsExactProtectedRoot(_stage.m_RootFD, _stage.m_RootDevice, _stage.m_RootInode) ||
        !HasExactReadOnlySeal(descriptors.source_fd, request.source) ||
        !HasExactReadOnlySeal(descriptors.destination_parent_fd, request.destination_parent) ||
        !HasExactReadOnlySeal(snapshot.m_ArtifactFD, snapshot.m_ArtifactSeal) ||
        !HasExactReadOnlySeal(_stage.m_StageFD, _stage.m_StageSeal) ||
        !IsValidArtifactSeal(snapshot.m_ArtifactSeal, _source_root.m_RootDevice) ||
        !IsValidArtifactSeal(_stage.m_StageSeal, _destination_root.m_RootDevice) ||
        snapshot.m_ArtifactSeal.byte_size != request.source.byte_size ||
        _stage.m_StageSeal.byte_size != request.source.byte_size )
        return std::unexpected{Error::InvalidStage};
    if( !HasExactSealedArtifact(_source_root.m_RootFD,
                                _source_root.m_RootDevice,
                                _source_root.m_RootInode,
                                request.header,
                                ArtifactRole::SourceSnapshot,
                                snapshot.m_ID,
                                snapshot.m_ArtifactSeal) )
        return std::unexpected{Error::SourceManifestFailed};
    if( !HasExactSealedArtifact(_destination_root.m_RootFD,
                                _destination_root.m_RootDevice,
                                _destination_root.m_RootInode,
                                request.header,
                                ArtifactRole::DestinationStage,
                                _stage.m_ID,
                                _stage.m_StageSeal) )
        return std::unexpected{Error::DestinationManifestFailed};

    LifecycleManifest manifest{
        .header = request.header,
        .source_root = {.device = _source_root.m_RootDevice, .inode = _source_root.m_RootInode},
        .destination_root = {.device = _destination_root.m_RootDevice, .inode = _destination_root.m_RootInode},
        .source_snapshot_id = snapshot.m_ID,
        .destination_stage_id = _stage.m_ID,
        .source_snapshot_seal = snapshot.m_ArtifactSeal,
        .destination_stage_seal = _stage.m_StageSeal,
        .source_seal = request.source,
        .destination_parent_seal = request.destination_parent,
    };
    const auto destination_name = request.destination_name.Bytes();
    if( destination_name.empty() || destination_name.size() > manifest.destination_name.size() )
        return std::unexpected{Error::InvalidStage};
    std::copy(destination_name.begin(), destination_name.end(), manifest.destination_name.begin());
    manifest.destination_name_size = static_cast<uint16_t>(destination_name.size());

    const bool source_first = IsCanonicalRootOrder(manifest.source_root, manifest.destination_root);
    const auto write_source = [&] noexcept {
        return WriteLifecycleManifest(_source_root.m_RootFD, manifest.source_snapshot_id, manifest);
    };
    const auto write_destination = [&] noexcept {
        return WriteLifecycleManifest(_destination_root.m_RootFD, manifest.destination_stage_id, manifest);
    };
    if( source_first ) {
        if( !write_source() )
            return std::unexpected{Error::SourceManifestFailed};
        if( !write_destination() )
            return std::unexpected{Error::DestinationManifestFailed};
    }
    else {
        if( !write_destination() )
            return std::unexpected{Error::DestinationManifestFailed};
        if( !write_source() )
            return std::unexpected{Error::SourceManifestFailed};
    }
    if( !HasExactLifecycleManifest(
            _source_root.m_RootFD, _source_root.m_RootDevice, manifest.source_snapshot_id, manifest) )
        return std::unexpected{Error::SourceManifestFailed};
    if( !HasExactLifecycleManifest(
            _destination_root.m_RootFD, _destination_root.m_RootDevice, manifest.destination_stage_id, manifest) )
        return std::unexpected{Error::DestinationManifestFailed};
    return {};
}

std::expected<StagingPublicationLifecycle::Inspection, StagingPublicationLifecycle::Error>
StagingPublicationLifecycle::Inspect(const int _borrowed_source_root_fd,
                                     const int _borrowed_destination_root_fd,
                                     const Header &_header) noexcept
{
    if( !Validate(_header) )
        return std::unexpected{Error::InvalidHeader};
    const auto source_identity = ReadProtectedRootIdentity(_borrowed_source_root_fd);
    if( !source_identity )
        return std::unexpected{Error::SourceRootInvalid};
    const auto destination_identity = ReadProtectedRootIdentity(_borrowed_destination_root_fd);
    if( !destination_identity )
        return std::unexpected{Error::DestinationRootInvalid};
    if( *source_identity == *destination_identity )
        return std::unexpected{Error::SourceRootBusy};

    const bool source_first = IsCanonicalRootOrder(*source_identity, *destination_identity);
    const int first_root_fd = source_first ? _borrowed_source_root_fd : _borrowed_destination_root_fd;
    const int second_root_fd = source_first ? _borrowed_destination_root_fd : _borrowed_source_root_fd;
    auto first_root = ProtectedRootLedger::Open(first_root_fd);
    if( !first_root )
        return std::unexpected{MapLifecycleRootOpenError(first_root.error(), source_first)};
    auto second_root = ProtectedRootLedger::Open(second_root_fd);
    if( !second_root )
        return std::unexpected{MapLifecycleRootOpenError(second_root.error(), !source_first)};
    auto source_root = source_first ? std::move(*first_root) : std::move(*second_root);
    auto destination_root = source_first ? std::move(*second_root) : std::move(*first_root);
    if( !source_root.MatchesRootIdentity(source_identity->device, source_identity->inode) )
        return std::unexpected{Error::SourceRootInvalid};
    if( !destination_root.MatchesRootIdentity(destination_identity->device, destination_identity->inode) )
        return std::unexpected{Error::DestinationRootInvalid};

    std::unique_lock source_lock{source_root.m_Mutex, std::defer_lock};
    std::unique_lock destination_lock{destination_root.m_Mutex, std::defer_lock};
    if( source_first ) {
        source_lock.lock();
        destination_lock.lock();
    }
    else {
        destination_lock.lock();
        source_lock.lock();
    }
    if( !source_root.HasValidRootUnlocked() )
        return std::unexpected{Error::SourceRootInvalid};
    if( !destination_root.HasValidRootUnlocked() )
        return std::unexpected{Error::DestinationRootInvalid};
    const RootIdentity exact_source{.device = source_root.m_RootDevice, .inode = source_root.m_RootInode};
    const RootIdentity exact_destination{.device = destination_root.m_RootDevice,
                                         .inode = destination_root.m_RootInode};
    const auto source_scan = ScanLifecycleManifests(source_root.m_RootFD, exact_source, _header, true);
    if( !source_scan )
        return std::unexpected{Error::SourceRootInvalid};
    const auto destination_scan = ScanLifecycleManifests(destination_root.m_RootFD, exact_destination, _header, false);
    if( !destination_scan )
        return std::unexpected{Error::DestinationRootInvalid};
    if( source_scan->malformed || destination_scan->malformed )
        return Inspection{.state = State::Malformed};
    if( !source_scan->manifest && !destination_scan->manifest ) {
        return Inspection{.state =
                              source_scan->primary || destination_scan->primary ? State::Incomplete : State::Absent};
    }
    if( !source_scan->manifest || !destination_scan->manifest || !source_scan->primary || !destination_scan->primary )
        return Inspection{.state = State::Incomplete};
    if( source_scan->ambiguous || destination_scan->ambiguous || *source_scan->primary != *source_scan->manifest ||
        *destination_scan->primary != *destination_scan->manifest ||
        *source_scan->manifest != *destination_scan->manifest )
        return Inspection{.state = State::Mismatched};
    const auto &manifest = *source_scan->manifest;
    if( manifest.source_root != exact_source || manifest.destination_root != exact_destination )
        return Inspection{.state = State::Mismatched};
    // A same-device record cannot originate from the CrossVolumeStaged writer.  Keep no-record same-device scans
    // usable for deterministic read-only tests, but never promote a persisted same-device candidate to ExactPending.
    if( manifest.source_root.device == manifest.destination_root.device )
        return Inspection{.state = State::Mismatched};
    if( !HasExactLifecycleManifest(
            source_root.m_RootFD, source_root.m_RootDevice, manifest.source_snapshot_id, manifest) ||
        !HasExactLifecycleManifest(
            destination_root.m_RootFD, destination_root.m_RootDevice, manifest.destination_stage_id, manifest) ||
        !HasExactSealedArtifact(source_root.m_RootFD,
                                source_root.m_RootDevice,
                                source_root.m_RootInode,
                                manifest.header,
                                ArtifactRole::SourceSnapshot,
                                manifest.source_snapshot_id,
                                manifest.source_snapshot_seal) ||
        !HasExactSealedArtifact(destination_root.m_RootFD,
                                destination_root.m_RootDevice,
                                destination_root.m_RootInode,
                                manifest.header,
                                ArtifactRole::DestinationStage,
                                manifest.destination_stage_id,
                                manifest.destination_stage_seal) )
        return Inspection{.state = State::Incomplete};
    return Inspection{.state = State::ExactPending};
}

std::expected<DestinationStageWriter::SealedDestinationStage, StagingSessionRunner::Error>
StagingSessionRunner::Run(StagingRootAuthority::LockedSession _session,
                          const SourceSnapshotWriter::Cancellation _source_cancellation,
                          const DestinationStageWriter::Cancellation _destination_cancellation) noexcept
{
    // Test the PID before any continuation or ledger state: a fork child must not touch inherited C++ mutexes or
    // descriptor authority.  A same-process moved-from session retains its creator PID but has no valid capability.
    if( _session.m_CreatorPID != static_cast<int>(::getpid()) )
        return std::unexpected{Error{.phase = Phase::Session, .cause = SessionError::ForkedProcess}};
    if( !_session.IsCreatedByCurrentProcess() )
        return std::unexpected{Error{.phase = Phase::Session, .cause = SessionError::InvalidSession}};

    auto terminal_lease = std::move(*_session.m_TerminalLease);
    _session.m_TerminalLease.reset();
    auto snapshot =
        SourceSnapshotWriter::Create(*_session.m_SourceRoot, std::move(terminal_lease), _source_cancellation);
    if( !snapshot )
        return std::unexpected{Error{.phase = Phase::SourceSnapshot, .cause = snapshot.error()}};

    auto stage =
        DestinationStageWriter::Create(*_session.m_DestinationRoot, std::move(*snapshot), _destination_cancellation);
    if( !stage )
        return std::unexpected{Error{.phase = Phase::DestinationStage, .cause = stage.error()}};
    const auto lifecycle =
        StagingPublicationLifecycle::RecordStaged(*_session.m_SourceRoot, *_session.m_DestinationRoot, *stage);
    if( !lifecycle )
        return std::unexpected{Error{.phase = Phase::Lifecycle, .cause = lifecycle.error()}};
    return std::move(*stage);
}

StagingPublicationBarrier::PublicationPermit::PublicationPermit(DestinationStageWriter::SealedDestinationStage _stage,
                                                                ProtectedRootLedger _destination_root) noexcept
    : m_CreatorPID{static_cast<int>(::getpid())}, m_Stage{std::move(_stage)},
      m_DestinationRoot{std::move(_destination_root)}
{
}

StagingPublicationBarrier::PublicationPermit::PublicationPermit(PublicationPermit &&_other) noexcept
    : m_CreatorPID{std::exchange(_other.m_CreatorPID, -1)}, m_Stage{std::move(_other.m_Stage)},
      m_DestinationRoot{std::move(_other.m_DestinationRoot)}
{
}

bool StagingPublicationBarrier::PublicationPermit::IsCreatedByCurrentProcess() const noexcept
{
    return m_CreatorPID == static_cast<int>(::getpid());
}

std::expected<StagingPublicationBarrier::PublicationPermit, StagingPublicationBarrier::Error>
StagingPublicationBarrier::Prepare(DestinationStageWriter::SealedDestinationStage _stage,
                                   const Cancellation _cancellation) noexcept
{
    const int current_pid = static_cast<int>(::getpid());
    const auto &source_snapshot = _stage.m_SourceSnapshot;
    const auto &terminal_lease = source_snapshot.m_TerminalLease;

    // Reject inherited authority before the cancellation callback, ledger registry or any descriptor revalidation.
    // A moved-from same-process stage has an inert creator PID and is merely invalid, not a forked capability.
    if( (_stage.m_CreatorPID >= 0 && _stage.m_CreatorPID != current_pid) ||
        (source_snapshot.m_CreatorPID >= 0 && source_snapshot.m_CreatorPID != current_pid) ||
        !terminal_lease.HasCurrentProcessCreator() )
        return std::unexpected{Error::ForkedProcess};
    if( !_stage.IsCreatedByCurrentProcess() || !source_snapshot.IsCreatedByCurrentProcess() ||
        !terminal_lease.IsCreatedByCurrentProcess() || !terminal_lease.IsCommit() )
        return std::unexpected{Error::InvalidStage};

    const BeginRequest &request = terminal_lease.Request();
    const auto descriptors = terminal_lease.Descriptors();
    if( !Validate(request) || _stage.m_ID.bytes == ArtifactID{}.bytes ||
        source_snapshot.m_ID.bytes == ArtifactID{}.bytes || source_snapshot.m_Header != request.header ||
        source_snapshot.m_SourceSeal != request.source || descriptors.source_fd < 0 ||
        descriptors.destination_parent_fd < 0 || _stage.m_StageFD < 0 || _stage.m_RootFD < 0 ||
        source_snapshot.m_ArtifactFD < 0 ||
        request.source.byte_size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) )
        return std::unexpected{Error::InvalidStage};

    std::array<char, kMaximumDestinationComponentBytes + 1> destination_name{};
    const auto destination_component = request.destination_name.Bytes();
    std::memcpy(destination_name.data(), destination_component.data(), destination_component.size());

    auto destination_root = ProtectedRootLedger::Open(_stage.m_RootFD);
    if( !destination_root )
        return std::unexpected{MapPublicationRootOpenError(destination_root.error())};
    if( !destination_root->MatchesRootIdentity(_stage.m_RootDevice, _stage.m_RootInode) )
        return std::unexpected{Error::DestinationRootBindingFailed};

    std::unique_lock lock{destination_root->m_Mutex};
    const auto revalidate = [&]() noexcept -> std::optional<Error> {
        if( !destination_root->HasValidRootUnlocked() || destination_root->m_RootDevice != _stage.m_RootDevice ||
            destination_root->m_RootInode != _stage.m_RootInode ||
            destination_root->m_RootDevice != request.destination_parent.device ||
            request.source.device == request.destination_parent.device ||
            !IsReadOnlyCloexecDescriptor(_stage.m_RootFD) ||
            !IsExactProtectedRoot(_stage.m_RootFD, _stage.m_RootDevice, _stage.m_RootInode) )
            return Error::DestinationRootBindingFailed;

        if( !HasExactReadOnlySeal(descriptors.source_fd, request.source) )
            return Error::SourceStale;
        if( !HasExactReadOnlySeal(descriptors.destination_parent_fd, request.destination_parent) )
            return Error::DestinationParentStale;
        if( !HasExactReadOnlySeal(source_snapshot.m_ArtifactFD, source_snapshot.m_ArtifactSeal) ||
            !IsValidArtifactSeal(source_snapshot.m_ArtifactSeal, request.source.device) ||
            source_snapshot.m_ArtifactSeal.byte_size != request.source.byte_size )
            return Error::StageStale;
        if( !HasExactReadOnlySeal(_stage.m_StageFD, _stage.m_StageSeal) ||
            !IsValidArtifactSeal(_stage.m_StageSeal, request.destination_parent.device) ||
            _stage.m_StageSeal.byte_size != request.source.byte_size )
            return Error::StageStale;

        std::array<char, 128> reservation_name{};
        if( !HasExactReservation(destination_root->m_RootFD,
                                 destination_root->m_RootDevice,
                                 destination_root->m_RootInode,
                                 request.header,
                                 ArtifactRole::DestinationStage,
                                 _stage.m_ID,
                                 reservation_name) )
            return Error::StageManifestFailed;

        std::array<char, 128> artifact_name{};
        size_t artifact_name_size = 0;
        std::array<char, 128> seal_manifest_name{};
        size_t seal_manifest_name_size = 0;
        if( !MakeArtifactName(_stage.m_ID, artifact_name, artifact_name_size) || artifact_name_size == 0 ||
            !MakeSealManifestName(_stage.m_ID, seal_manifest_name, seal_manifest_name_size) ||
            seal_manifest_name_size == 0 )
            return Error::StageManifestFailed;

        const auto sealed_record =
            ReadRecordContents(destination_root->m_RootFD, seal_manifest_name.data(), destination_root->m_RootDevice);
        DecodedRecord sealed{};
        const auto named_stage =
            ReadArtifactSeal(destination_root->m_RootFD, artifact_name.data(), destination_root->m_RootDevice);
        if( !sealed_record ||
            !DecodeRecord(RecordContents(*sealed_record),
                          destination_root->m_RootDevice,
                          destination_root->m_RootInode,
                          sealed) ||
            sealed.state != RecordState::Sealed || sealed.header != request.header ||
            sealed.role != ArtifactRole::DestinationStage || sealed.id != _stage.m_ID ||
            sealed.artifact_seal != _stage.m_StageSeal ||
            !HasExactRecordIdentity(destination_root->m_RootFD, seal_manifest_name.data(), *sealed_record) ||
            !named_stage || *named_stage != _stage.m_StageSeal )
            return Error::StageManifestFailed;

        struct stat destination_status{};
        if( ::fstatat(
                descriptors.destination_parent_fd, destination_name.data(), &destination_status, AT_SYMLINK_NOFOLLOW) ==
            0 )
            return Error::DestinationExists;
        if( errno != ENOENT )
            return Error::DestinationLookupFailed;
        return std::nullopt;
    };

    if( const auto failure = revalidate() )
        return std::unexpected{*failure};

    // Keep the root flock while the cancellation callback runs.  Only the in-process ledger mutex is released so
    // the callback cannot deadlock on a test observer; all filesystem authority is exact-revalidated afterward.
    lock.unlock();
    const bool cancelled = _cancellation.IsCancelled(CancellationPoint::BeforePublication);
    lock.lock();
    if( const auto failure = revalidate() )
        return std::unexpected{*failure};
    if( cancelled )
        return std::unexpected{Error::Cancelled};

    // The ledger move constructor takes its own mutex to transfer active-reservation state.  Release only this
    // in-process mutex before that move; the root FD remains owned by the ledger and continues to retain its flock.
    lock.unlock();
    return PublicationPermit{std::move(_stage), std::move(*destination_root)};
}

SourceSnapshotWriter::SealedSourceSnapshot::SealedSourceSnapshot(Header _header,
                                                                 ArtifactID _id,
                                                                 ObjectSeal _source_seal,
                                                                 ObjectSeal _artifact_seal,
                                                                 LeaseStore::TerminalLease _terminal_lease,
                                                                 const int _artifact_fd) noexcept
    : m_CreatorPID{static_cast<int>(::getpid())}, m_Header{_header}, m_ID{_id}, m_SourceSeal{_source_seal},
      m_ArtifactSeal{_artifact_seal}, m_TerminalLease{std::move(_terminal_lease)}, m_ArtifactFD{_artifact_fd}
{
}

SourceSnapshotWriter::SealedSourceSnapshot::SealedSourceSnapshot(SealedSourceSnapshot &&_other) noexcept
    : m_CreatorPID{std::exchange(_other.m_CreatorPID, -1)}, m_Header{_other.m_Header}, m_ID{_other.m_ID},
      m_SourceSeal{_other.m_SourceSeal}, m_ArtifactSeal{_other.m_ArtifactSeal},
      m_TerminalLease{std::move(_other.m_TerminalLease)}, m_ArtifactFD{std::exchange(_other.m_ArtifactFD, -1)}
{
}

bool SourceSnapshotWriter::SealedSourceSnapshot::IsCreatedByCurrentProcess() const noexcept
{
    return m_CreatorPID == static_cast<int>(::getpid());
}

SourceSnapshotWriter::SealedSourceSnapshot::~SealedSourceSnapshot() noexcept
{
    if( m_ArtifactFD >= 0 )
        ::close(m_ArtifactFD);
}

DestinationStageWriter::SealedDestinationStage::SealedDestinationStage(
    SourceSnapshotWriter::SealedSourceSnapshot _source_snapshot,
    const ArtifactID _id,
    const ObjectSeal _stage_seal,
    const int _stage_fd,
    const int _root_fd,
    const uint64_t _root_device,
    const uint64_t _root_inode) noexcept
    : m_CreatorPID{static_cast<int>(::getpid())}, m_SourceSnapshot{std::move(_source_snapshot)}, m_ID{_id},
      m_StageSeal{_stage_seal}, m_StageFD{_stage_fd}, m_RootFD{_root_fd}, m_RootDevice{_root_device},
      m_RootInode{_root_inode}
{
}

DestinationStageWriter::SealedDestinationStage::SealedDestinationStage(SealedDestinationStage &&_other) noexcept
    : m_CreatorPID{std::exchange(_other.m_CreatorPID, -1)}, m_SourceSnapshot{std::move(_other.m_SourceSnapshot)},
      m_ID{_other.m_ID}, m_StageSeal{_other.m_StageSeal}, m_StageFD{std::exchange(_other.m_StageFD, -1)},
      m_RootFD{std::exchange(_other.m_RootFD, -1)}, m_RootDevice{_other.m_RootDevice}, m_RootInode{_other.m_RootInode}
{
}

bool DestinationStageWriter::SealedDestinationStage::IsCreatedByCurrentProcess() const noexcept
{
    return m_CreatorPID == static_cast<int>(::getpid());
}

DestinationStageWriter::SealedDestinationStage::~SealedDestinationStage() noexcept
{
    if( m_StageFD >= 0 )
        ::close(m_StageFD);
    if( m_RootFD >= 0 )
        ::close(m_RootFD);
}

std::expected<DestinationStageWriter::SealedDestinationStage, DestinationStageWriter::Error>
DestinationStageWriter::Create(ProtectedRootLedger &_destination_ledger,
                               SourceSnapshotWriter::SealedSourceSnapshot _source_snapshot,
                               const Cancellation _cancellation) noexcept
{
    if( !_source_snapshot.IsCreatedByCurrentProcess() || !_source_snapshot.m_TerminalLease.IsCreatedByCurrentProcess() )
        return std::unexpected{Error::InvalidSourceSnapshot};
    const auto &terminal_lease = _source_snapshot.m_TerminalLease;
    if( !terminal_lease.IsCommit() )
        return std::unexpected{Error::InvalidSourceSnapshot};
    // Do not acquire an inherited ledger mutex in a fork child.  The continuation checks above deliberately do not
    // substitute for this guard: a child can receive a fresh protocol claim after fork while the ledger remains
    // parent-owned.
    if( _destination_ledger.m_OwnerPID != static_cast<int>(::getpid()) )
        return std::unexpected{Error::CrossVolumeBindingFailed};
    const BeginRequest &request = terminal_lease.Request();
    const auto descriptors = terminal_lease.Descriptors();
    if( !Validate(request) || _source_snapshot.m_Header != request.header ||
        _source_snapshot.m_SourceSeal != request.source || _source_snapshot.m_ArtifactFD < 0 ||
        descriptors.source_fd < 0 || descriptors.destination_parent_fd < 0 ||
        request.source.byte_size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) )
        return std::unexpected{Error::InvalidSourceSnapshot};
    if( !HasExactReadOnlySeal(_source_snapshot.m_ArtifactFD, _source_snapshot.m_ArtifactSeal) ||
        !IsValidArtifactSeal(_source_snapshot.m_ArtifactSeal, request.source.device) ||
        _source_snapshot.m_ArtifactSeal.byte_size != request.source.byte_size )
        return std::unexpected{Error::InvalidSourceSnapshot};
    if( !HasExactReadOnlySeal(descriptors.source_fd, request.source) )
        return std::unexpected{Error::SourceStale};
    if( !HasExactReadOnlySeal(descriptors.destination_parent_fd, request.destination_parent) )
        return std::unexpected{Error::DestinationParentStale};
    {
        std::lock_guard lock{_destination_ledger.m_Mutex};
        if( !_destination_ledger.HasValidRootUnlocked() ||
            _destination_ledger.m_RootDevice != request.destination_parent.device ||
            request.source.device == request.destination_parent.device )
            return std::unexpected{Error::CrossVolumeBindingFailed};
    }
    if( _cancellation.IsCancelled(CancellationPoint::BeforeReservation) )
        return std::unexpected{Error::Cancelled};

    auto reservation = _destination_ledger.Reserve(request.header, ArtifactRole::DestinationStage);
    if( !reservation )
        return std::unexpected{Error::ReservationFailed};

    std::unique_lock lock{_destination_ledger.m_Mutex};
    if( !reservation->m_Valid )
        return std::unexpected{Error::ReservationFailed};
    reservation->m_Valid = false;
    if( !_destination_ledger.HasValidRootUnlocked() ||
        _destination_ledger.m_RootDevice != request.destination_parent.device ||
        reservation->m_Header != request.header || reservation->m_Role != ArtifactRole::DestinationStage )
        return std::unexpected{Error::ReservationFailed};

    const auto has_active_reservation = [&] noexcept {
        for( const auto &entry : _destination_ledger.m_ActiveReservations ) {
            if( entry && entry->header == reservation->m_Header && entry->role == reservation->m_Role &&
                entry->id == reservation->m_ID )
                return true;
        }
        return false;
    };
    std::array<char, 128> reservation_name{};
    const auto has_exact_active_reservation = [&] noexcept {
        return has_active_reservation() && HasExactReservation(_destination_ledger.m_RootFD,
                                                               _destination_ledger.m_RootDevice,
                                                               _destination_ledger.m_RootInode,
                                                               reservation->m_Header,
                                                               reservation->m_Role,
                                                               reservation->m_ID,
                                                               reservation_name);
    };
    const auto revalidate = [&] noexcept -> std::optional<Error> {
        if( !_destination_ledger.HasValidRootUnlocked() ||
            _destination_ledger.m_RootDevice != request.destination_parent.device )
            return Error::CrossVolumeBindingFailed;
        if( !HasExactReadOnlySeal(_source_snapshot.m_ArtifactFD, _source_snapshot.m_ArtifactSeal) ||
            !IsValidArtifactSeal(_source_snapshot.m_ArtifactSeal, request.source.device) ||
            _source_snapshot.m_ArtifactSeal.byte_size != request.source.byte_size )
            return Error::InvalidSourceSnapshot;
        if( !HasExactReadOnlySeal(descriptors.source_fd, request.source) )
            return Error::SourceStale;
        if( !HasExactReadOnlySeal(descriptors.destination_parent_fd, request.destination_parent) )
            return Error::DestinationParentStale;
        return std::nullopt;
    };
    const auto check_cancellation = [&](const CancellationPoint _point) noexcept -> std::expected<bool, Error> {
        lock.unlock();
        const bool cancelled = _cancellation.IsCancelled(_point);
        lock.lock();
        if( const auto failure = revalidate() )
            return std::unexpected{*failure};
        if( !has_exact_active_reservation() )
            return std::unexpected{Error::ReservationFailed};
        return cancelled;
    };

    if( const auto failure = revalidate() )
        return std::unexpected{*failure};
    if( !has_exact_active_reservation() )
        return std::unexpected{Error::StageValidationFailed};

    std::array<char, 128> artifact_name{};
    size_t artifact_name_size = 0;
    std::array<char, 128> seal_manifest_name{};
    size_t seal_manifest_name_size = 0;
    if( !MakeArtifactName(reservation->m_ID, artifact_name, artifact_name_size) || artifact_name_size == 0 ||
        !MakeSealManifestName(reservation->m_ID, seal_manifest_name, seal_manifest_name_size) ||
        seal_manifest_name_size == 0 || !IsArtifactAbsent(_destination_ledger.m_RootFD, artifact_name.data()) ||
        !IsEntryAbsent(_destination_ledger.m_RootFD, seal_manifest_name.data()) )
        return std::unexpected{Error::StageCreateFailed};

    const auto cancelled_before_artifact_create = check_cancellation(CancellationPoint::BeforeArtifactCreate);
    if( !cancelled_before_artifact_create )
        return std::unexpected{cancelled_before_artifact_create.error()};
    if( *cancelled_before_artifact_create )
        return std::unexpected{Error::Cancelled};

    ScopedFD stage{::openat(_destination_ledger.m_RootFD,
                            artifact_name.data(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                            0600)};
    if( stage.Get() < 0 || ::fchmod(stage.Get(), 0600) != 0 )
        return std::unexpected{Error::StageCreateFailed};

    struct stat stage_status{};
    ObjectSeal stage_seal;
    if( ::fstat(stage.Get(), &stage_status) != 0 || !SealFromStat(stage_status, stage_seal) ||
        !IsValidArtifactSeal(stage_seal, _destination_ledger.m_RootDevice) || stage_seal.byte_size != 0 )
        return std::unexpected{Error::StageValidationFailed};

    constexpr size_t kCopyBufferBytes = 16 * 1024;
    std::array<char, kCopyBufferBytes> buffer{};
    uint64_t offset = 0;
    while( offset < request.source.byte_size ) {
        const auto cancelled_before_copy_chunk = check_cancellation(CancellationPoint::BeforeCopyChunk);
        if( !cancelled_before_copy_chunk )
            return std::unexpected{cancelled_before_copy_chunk.error()};
        if( *cancelled_before_copy_chunk )
            return std::unexpected{Error::Cancelled};
        const auto bytes = static_cast<size_t>(
            std::min<uint64_t>(request.source.byte_size - offset, static_cast<uint64_t>(buffer.size())));
        ssize_t read = -1;
        do {
            read = ::pread(_source_snapshot.m_ArtifactFD, buffer.data(), bytes, static_cast<off_t>(offset));
        } while( read < 0 && errno == EINTR );
        if( read != static_cast<ssize_t>(bytes) )
            return std::unexpected{Error::SnapshotReadFailed};

        size_t written = 0;
        while( written < bytes ) {
            ssize_t write = -1;
            do {
                write = ::pwrite(
                    stage.Get(), buffer.data() + written, bytes - written, static_cast<off_t>(offset + written));
            } while( write < 0 && errno == EINTR );
            if( write <= 0 )
                return std::unexpected{Error::StageWriteFailed};
            written += static_cast<size_t>(write);
        }
        offset += bytes;
    }

    const auto cancelled_after_copy = check_cancellation(CancellationPoint::AfterCopy);
    if( !cancelled_after_copy )
        return std::unexpected{cancelled_after_copy.error()};
    if( *cancelled_after_copy )
        return std::unexpected{Error::Cancelled};
    if( ::fstat(stage.Get(), &stage_status) != 0 || !SealFromStat(stage_status, stage_seal) ||
        !IsValidArtifactSeal(stage_seal, _destination_ledger.m_RootDevice) ||
        stage_seal.byte_size != request.source.byte_size )
        return std::unexpected{Error::StageValidationFailed};
    if( ::fsync(stage.Get()) != 0 || ::fsync(_destination_ledger.m_RootFD) != 0 ||
        ::fcntl(stage.Get(), F_FULLFSYNC) != 0 )
        return std::unexpected{Error::StageSyncFailed};
    if( ::fstat(stage.Get(), &stage_status) != 0 || !SealFromStat(stage_status, stage_seal) ||
        !IsValidArtifactSeal(stage_seal, _destination_ledger.m_RootDevice) ||
        stage_seal.byte_size != request.source.byte_size )
        return std::unexpected{Error::StageValidationFailed};
    if( ::close(stage.Release()) != 0 )
        return std::unexpected{Error::StageCloseFailed};

    const auto cancelled_before_seal_manifest = check_cancellation(CancellationPoint::BeforeSealManifest);
    if( !cancelled_before_seal_manifest )
        return std::unexpected{cancelled_before_seal_manifest.error()};
    if( *cancelled_before_seal_manifest )
        return std::unexpected{Error::Cancelled};
    if( !HasExactReservation(_destination_ledger.m_RootFD,
                             _destination_ledger.m_RootDevice,
                             _destination_ledger.m_RootInode,
                             reservation->m_Header,
                             reservation->m_Role,
                             reservation->m_ID,
                             reservation_name) )
        return std::unexpected{Error::SealManifestFailed};
    const auto sealed = WriteSealManifest(_destination_ledger.m_RootFD,
                                          _destination_ledger.m_RootDevice,
                                          _destination_ledger.m_RootInode,
                                          reservation->m_Header,
                                          reservation->m_Role,
                                          reservation->m_ID,
                                          stage_seal,
                                          seal_manifest_name);
    if( !sealed )
        return std::unexpected{Error::SealManifestFailed};

    const auto sealed_record =
        ReadRecordContents(_destination_ledger.m_RootFD, seal_manifest_name.data(), _destination_ledger.m_RootDevice);
    DecodedRecord decoded{};
    const auto sealed_artifact =
        ReadArtifactSeal(_destination_ledger.m_RootFD, artifact_name.data(), _destination_ledger.m_RootDevice);
    if( const auto failure = revalidate();
        failure || !sealed_record ||
        !DecodeRecord(RecordContents(*sealed_record),
                      _destination_ledger.m_RootDevice,
                      _destination_ledger.m_RootInode,
                      decoded) ||
        decoded.state != RecordState::Sealed || decoded.header != reservation->m_Header ||
        decoded.role != reservation->m_Role || decoded.id != reservation->m_ID || decoded.artifact_seal != stage_seal ||
        !HasExactRecordIdentity(_destination_ledger.m_RootFD, seal_manifest_name.data(), *sealed_record) ||
        !sealed_artifact || *sealed_artifact != stage_seal )
        return std::unexpected{Error::PostSealValidationFailed};

    ScopedFD sealed_stage{OpenExactArtifactForRead(
        _destination_ledger.m_RootFD, artifact_name.data(), _destination_ledger.m_RootDevice, stage_seal)};
    if( sealed_stage.Get() < 0 )
        return std::unexpected{Error::PostSealValidationFailed};
    ScopedFD sealed_root{OpenExactRootForRead(
        _destination_ledger.m_RootFD, _destination_ledger.m_RootDevice, _destination_ledger.m_RootInode)};
    if( sealed_root.Get() < 0 )
        return std::unexpected{Error::PostSealValidationFailed};
    return SealedDestinationStage{std::move(_source_snapshot),
                                  reservation->m_ID,
                                  stage_seal,
                                  sealed_stage.Release(),
                                  sealed_root.Release(),
                                  _destination_ledger.m_RootDevice,
                                  _destination_ledger.m_RootInode};
}

std::expected<SourceSnapshotWriter::SealedSourceSnapshot, SourceSnapshotWriter::Error>
SourceSnapshotWriter::Create(ProtectedRootLedger &_ledger,
                             LeaseStore::TerminalLease _terminal_lease,
                             const Cancellation _cancellation) noexcept
{
    if( !_terminal_lease.IsCreatedByCurrentProcess() )
        return std::unexpected{Error::InvalidTerminalLease};
    if( !_terminal_lease.IsCommit() )
        return std::unexpected{Error::InvalidTerminalLease};
    // This must precede every cancellation callback and direct ledger mutex acquisition.  A fork child has no
    // authority to reuse a parent ledger, even if it obtains a fresh terminal lease afterwards.
    if( _ledger.m_OwnerPID != static_cast<int>(::getpid()) )
        return std::unexpected{Error::SourceRootBindingFailed};
    if( _cancellation.IsCancelled(CancellationPoint::BeforeReservation) )
        return std::unexpected{Error::Cancelled};
    const BeginRequest &request = _terminal_lease.Request();
    const auto descriptors = _terminal_lease.Descriptors();
    if( !Validate(request) || descriptors.source_fd < 0 || !IsReadOnlyCloexecDescriptor(descriptors.source_fd) )
        return std::unexpected{Error::InvalidTerminalLease};
    if( request.source.byte_size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) )
        return std::unexpected{Error::SourceTooLarge};
    if( !HasExactSourceSeal(descriptors.source_fd, request.source) )
        return std::unexpected{Error::SourceStale};
    {
        std::lock_guard lock{_ledger.m_Mutex};
        if( !_ledger.HasValidRootUnlocked() || _ledger.m_RootDevice != request.source.device )
            return std::unexpected{Error::SourceRootBindingFailed};
    }

    auto reservation = _ledger.Reserve(request.header, ArtifactRole::SourceSnapshot);
    if( !reservation )
        return std::unexpected{Error::ReservationFailed};

    std::unique_lock lock{_ledger.m_Mutex};
    if( !reservation->m_Valid )
        return std::unexpected{Error::ReservationFailed};
    reservation->m_Valid = false;
    if( !_ledger.HasValidRootUnlocked() || _ledger.m_RootDevice != request.source.device ||
        reservation->m_Header != request.header || reservation->m_Role != ArtifactRole::SourceSnapshot )
        return std::unexpected{Error::SourceRootBindingFailed};

    const auto has_active_reservation = [&] noexcept {
        for( const auto &entry : _ledger.m_ActiveReservations ) {
            if( entry && entry->header == reservation->m_Header && entry->role == reservation->m_Role &&
                entry->id == reservation->m_ID )
                return true;
        }
        return false;
    };
    if( !has_active_reservation() )
        return std::unexpected{Error::ReservationFailed};

    std::array<char, 128> reservation_name{};
    const auto has_exact_active_reservation = [&] noexcept {
        return has_active_reservation() && HasExactReservation(_ledger.m_RootFD,
                                                               _ledger.m_RootDevice,
                                                               _ledger.m_RootInode,
                                                               reservation->m_Header,
                                                               reservation->m_Role,
                                                               reservation->m_ID,
                                                               reservation_name);
    };

    const auto check_cancellation = [&](const CancellationPoint _point) noexcept -> std::expected<bool, Error> {
        lock.unlock();
        const bool cancelled = _cancellation.IsCancelled(_point);
        lock.lock();
        if( !_ledger.HasValidRootUnlocked() || _ledger.m_RootDevice != request.source.device )
            return std::unexpected{Error::SourceRootBindingFailed};
        if( !has_exact_active_reservation() )
            return std::unexpected{Error::ReservationFailed};
        return cancelled;
    };

    if( !has_exact_active_reservation() )
        return std::unexpected{Error::ArtifactValidationFailed};

    std::array<char, 128> artifact_name{};
    size_t artifact_name_size = 0;
    std::array<char, 128> seal_manifest_name{};
    size_t seal_manifest_name_size = 0;
    if( !MakeArtifactName(reservation->m_ID, artifact_name, artifact_name_size) || artifact_name_size == 0 ||
        !MakeSealManifestName(reservation->m_ID, seal_manifest_name, seal_manifest_name_size) ||
        seal_manifest_name_size == 0 || !IsArtifactAbsent(_ledger.m_RootFD, artifact_name.data()) ||
        !IsEntryAbsent(_ledger.m_RootFD, seal_manifest_name.data()) )
        return std::unexpected{Error::ArtifactCreateFailed};

    const auto cancelled_before_artifact_create = check_cancellation(CancellationPoint::BeforeArtifactCreate);
    if( !cancelled_before_artifact_create )
        return std::unexpected{cancelled_before_artifact_create.error()};
    if( *cancelled_before_artifact_create )
        return std::unexpected{Error::Cancelled};
    if( !HasExactSourceSeal(descriptors.source_fd, request.source) )
        return std::unexpected{Error::SourceStale};

    ScopedFD artifact{
        ::openat(_ledger.m_RootFD, artifact_name.data(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
    if( artifact.Get() < 0 || ::fchmod(artifact.Get(), 0600) != 0 )
        return std::unexpected{Error::ArtifactCreateFailed};

    struct stat artifact_status{};
    ObjectSeal artifact_seal;
    if( ::fstat(artifact.Get(), &artifact_status) != 0 || !SealFromStat(artifact_status, artifact_seal) ||
        !IsValidArtifactSeal(artifact_seal, _ledger.m_RootDevice) || artifact_seal.byte_size != 0 )
        return std::unexpected{Error::ArtifactValidationFailed};

    constexpr size_t kCopyBufferBytes = 16 * 1024;
    std::array<char, kCopyBufferBytes> buffer{};
    uint64_t offset = 0;
    while( offset < request.source.byte_size ) {
        const auto cancelled_before_copy_chunk = check_cancellation(CancellationPoint::BeforeCopyChunk);
        if( !cancelled_before_copy_chunk )
            return std::unexpected{cancelled_before_copy_chunk.error()};
        if( *cancelled_before_copy_chunk )
            return std::unexpected{Error::Cancelled};
        const auto bytes = static_cast<size_t>(
            std::min<uint64_t>(request.source.byte_size - offset, static_cast<uint64_t>(buffer.size())));
        ssize_t read = -1;
        do {
            read = ::pread(descriptors.source_fd, buffer.data(), bytes, static_cast<off_t>(offset));
        } while( read < 0 && errno == EINTR );
        if( read != static_cast<ssize_t>(bytes) )
            return std::unexpected{Error::SourceReadFailed};

        size_t written = 0;
        while( written < bytes ) {
            ssize_t write = -1;
            do {
                write = ::pwrite(
                    artifact.Get(), buffer.data() + written, bytes - written, static_cast<off_t>(offset + written));
            } while( write < 0 && errno == EINTR );
            if( write <= 0 )
                return std::unexpected{Error::ArtifactWriteFailed};
            written += static_cast<size_t>(write);
        }
        offset += bytes;
    }

    const auto cancelled_after_copy = check_cancellation(CancellationPoint::AfterCopy);
    if( !cancelled_after_copy )
        return std::unexpected{cancelled_after_copy.error()};
    if( *cancelled_after_copy )
        return std::unexpected{Error::Cancelled};
    if( !HasExactSourceSeal(descriptors.source_fd, request.source) )
        return std::unexpected{Error::SourceStale};
    if( ::fstat(artifact.Get(), &artifact_status) != 0 || !SealFromStat(artifact_status, artifact_seal) ||
        !IsValidArtifactSeal(artifact_seal, _ledger.m_RootDevice) ||
        artifact_seal.byte_size != request.source.byte_size )
        return std::unexpected{Error::ArtifactValidationFailed};
    if( ::fsync(artifact.Get()) != 0 || ::fsync(_ledger.m_RootFD) != 0 || ::fcntl(artifact.Get(), F_FULLFSYNC) != 0 )
        return std::unexpected{Error::ArtifactSyncFailed};
    if( ::fstat(artifact.Get(), &artifact_status) != 0 || !SealFromStat(artifact_status, artifact_seal) ||
        !IsValidArtifactSeal(artifact_seal, _ledger.m_RootDevice) ||
        artifact_seal.byte_size != request.source.byte_size )
        return std::unexpected{Error::ArtifactValidationFailed};
    if( ::close(artifact.Release()) != 0 )
        return std::unexpected{Error::ArtifactCloseFailed};

    const auto cancelled_before_seal_manifest = check_cancellation(CancellationPoint::BeforeSealManifest);
    if( !cancelled_before_seal_manifest )
        return std::unexpected{cancelled_before_seal_manifest.error()};
    if( *cancelled_before_seal_manifest )
        return std::unexpected{Error::Cancelled};
    if( !HasExactSourceSeal(descriptors.source_fd, request.source) )
        return std::unexpected{Error::SourceStale};
    if( !HasExactReservation(_ledger.m_RootFD,
                             _ledger.m_RootDevice,
                             _ledger.m_RootInode,
                             reservation->m_Header,
                             reservation->m_Role,
                             reservation->m_ID,
                             reservation_name) )
        return std::unexpected{Error::SealManifestFailed};
    const auto written = WriteSealManifest(_ledger.m_RootFD,
                                           _ledger.m_RootDevice,
                                           _ledger.m_RootInode,
                                           reservation->m_Header,
                                           reservation->m_Role,
                                           reservation->m_ID,
                                           artifact_seal,
                                           seal_manifest_name);
    if( !written )
        return std::unexpected{Error::SealManifestFailed};

    const auto sealed_record = ReadRecordContents(_ledger.m_RootFD, seal_manifest_name.data(), _ledger.m_RootDevice);
    DecodedRecord sealed{};
    const auto sealed_artifact = ReadArtifactSeal(_ledger.m_RootFD, artifact_name.data(), _ledger.m_RootDevice);
    if( !sealed_record ||
        !DecodeRecord(RecordContents(*sealed_record), _ledger.m_RootDevice, _ledger.m_RootInode, sealed) ||
        sealed.state != RecordState::Sealed || sealed.header != reservation->m_Header ||
        sealed.role != reservation->m_Role || sealed.id != reservation->m_ID || sealed.artifact_seal != artifact_seal ||
        !HasExactRecordIdentity(_ledger.m_RootFD, seal_manifest_name.data(), *sealed_record) || !sealed_artifact ||
        *sealed_artifact != artifact_seal )
        return std::unexpected{Error::PostSealValidationFailed};

    const int sealed_artifact_fd =
        OpenExactArtifactForRead(_ledger.m_RootFD, artifact_name.data(), _ledger.m_RootDevice, artifact_seal);
    if( sealed_artifact_fd < 0 )
        return std::unexpected{Error::PostSealValidationFailed};
    return SealedSourceSnapshot{reservation->m_Header,
                                reservation->m_ID,
                                request.source,
                                artifact_seal,
                                std::move(_terminal_lease),
                                sealed_artifact_fd};
}

} // namespace nc::routedio::cross_volume_staging::helper
