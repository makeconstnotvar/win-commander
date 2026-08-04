// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"

#include "../source/CopyOperationOrchestrator.h"
#include "../source/OperationJournalTesting.h"
#include "../source/VFSOperationPlanningProbes.h"
#include "../../VFS/source/Native/ConditionalCopy.h"

#include <Operations/OperationJournal.h>
#include <Operations/Pool.h>
#include <Utility/NativeFSManager.h>
#include <VFS/Native.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <expected>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace nc::ops {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view g_MarkerName = ".wincommander-operations-it-root";
constexpr std::string_view g_MarkerContents = "wincommander-operations-it-root\n";
constexpr std::string_view g_InternalRootEnvironment = "WINCOMMANDER_OPERATIONS_IT_INTERNAL_ROOT";
constexpr std::string_view g_ExternalRootEnvironment = "WINCOMMANDER_OPERATIONS_IT_EXTERNAL_ROOT";
constexpr std::string_view g_RequireRootsEnvironment = "WINCOMMANDER_OPERATIONS_IT_REQUIRE_VOLUMES";
constexpr std::string_view g_PowerLossPhaseEnvironment = "WINCOMMANDER_OPERATIONS_IT_POWER_LOSS_PHASE";
constexpr std::string_view g_PowerLossBlockEnvironment = "WINCOMMANDER_OPERATIONS_IT_POWER_LOSS_BLOCK";
constexpr std::string_view g_PowerLossRecoveryWorkspaceEnvironment =
    "WINCOMMANDER_OPERATIONS_IT_POWER_LOSS_RECOVERY_WORKSPACE";
constexpr std::string_view g_PowerLossManifestName = "power-loss-checkpoint.manifest";
constexpr size_t g_PowerLossManifestMaxBytes = 64 * 1024;

enum class PhysicalVolumeRole : uint8_t {
    Internal,
    External
};

enum class PowerLossCheckpointPhase : uint8_t {
    BeforePublish,
    AfterPublishBeforeFullFSync
};

struct PowerLossCheckpointConfiguration final {
    PowerLossCheckpointPhase phase;
    bool block_for_operator{false};
};

std::expected<std::optional<PowerLossCheckpointConfiguration>, std::string> PowerLossCheckpointConfigurationFromEnvironment()
{
    const char *const phase = std::getenv(g_PowerLossPhaseEnvironment.data());
    if( phase == nullptr || phase[0] == '\0' )
        return std::optional<PowerLossCheckpointConfiguration>{};

    PowerLossCheckpointPhase parsed_phase;
    if( std::strcmp(phase, "before-publish") == 0 )
        parsed_phase = PowerLossCheckpointPhase::BeforePublish;
    else if( std::strcmp(phase, "after-publish-before-full-fsync") == 0 )
        parsed_phase = PowerLossCheckpointPhase::AfterPublishBeforeFullFSync;
    else
        return std::unexpected(std::string{g_PowerLossPhaseEnvironment} +
                               " must be before-publish or after-publish-before-full-fsync");

    const char *const block = std::getenv(g_PowerLossBlockEnvironment.data());
    if( block == nullptr || block[0] == '\0' )
        return PowerLossCheckpointConfiguration{parsed_phase, false};
    if( std::strcmp(block, "1") != 0 )
        return std::unexpected(std::string{g_PowerLossBlockEnvironment} + " must be 1 when set");
    return PowerLossCheckpointConfiguration{parsed_phase, true};
}

nc::vfs::native::ConditionalCopyCheckpoint NativeCheckpoint(const PowerLossCheckpointPhase _phase) noexcept
{
    switch( _phase ) {
        case PowerLossCheckpointPhase::BeforePublish:
            return nc::vfs::native::ConditionalCopyCheckpoint::BeforePublish;
        case PowerLossCheckpointPhase::AfterPublishBeforeFullFSync:
            return nc::vfs::native::ConditionalCopyCheckpoint::AfterPublishBeforeFullFSync;
    }
    return nc::vfs::native::ConditionalCopyCheckpoint::BeforePublish;
}

std::string_view PowerLossCheckpointPhaseName(const PowerLossCheckpointPhase _phase) noexcept
{
    switch( _phase ) {
        case PowerLossCheckpointPhase::BeforePublish:
            return "before-publish";
        case PowerLossCheckpointPhase::AfterPublishBeforeFullFSync:
            return "after-publish-before-full-fsync";
    }
    return "unknown";
}

bool PhysicalVolumeRootsRequired() noexcept
{
    const char *const value = std::getenv(g_RequireRootsEnvironment.data());
    return value != nullptr && std::strcmp(value, "1") == 0;
}

[[noreturn]] void SkipOrFailPhysicalFixture(const std::string &_reason)
{
    const auto message = "physical Conditional Copy fixture: " + _reason;
    if( PhysicalVolumeRootsRequired() )
        FAIL(message);
    SKIP(message);
}

bool ValidPhysicalWorkspaceChildName(const std::string_view _name) noexcept
{
    return !_name.empty() && _name != "." && _name != ".." && _name.find('/') == std::string_view::npos;
}

struct PowerLossManifestStat final {
    uint64_t device{0};
    uint64_t inode{0};
    uint64_t size{0};
    uint32_t mode{0};
    int64_t mtime_seconds{0};
    int64_t mtime_nanoseconds{0};
};

struct PowerLossCheckpointManifest final {
    std::string workspace_name;
    PowerLossCheckpointPhase phase{PowerLossCheckpointPhase::BeforePublish};
    std::string journal_filename;
    PowerLossManifestStat journal;
    std::string journal_parent_filename;
    PowerLossManifestStat journal_parent;
};

std::expected<std::string_view, std::string>
TakePowerLossManifestField(const std::vector<std::string_view> &_lines, size_t &_index, const std::string_view _name)
{
    if( _index >= _lines.size() )
        return std::unexpected("manifest ends before " + std::string{_name});
    const auto line = _lines[_index++];
    if( line.size() <= _name.size() || !line.starts_with(_name) || line[_name.size()] != '=' )
        return std::unexpected("manifest field is not " + std::string{_name});
    const auto value = line.substr(_name.size() + 1);
    if( value.empty() )
        return std::unexpected("manifest field is empty: " + std::string{_name});
    return value;
}

template <class T>
std::expected<T, std::string> ParsePowerLossManifestInteger(const std::string_view _value, const std::string_view _name)
{
    T parsed{};
    const auto [parsed_until, error] = std::from_chars(_value.data(), _value.data() + _value.size(), parsed);
    if( error != std::errc{} || parsed_until != _value.data() + _value.size() )
        return std::unexpected("manifest field is not an integer: " + std::string{_name});
    return parsed;
}

std::expected<PowerLossManifestStat, std::string>
ReadPowerLossManifestStat(const std::vector<std::string_view> &_lines, size_t &_index, const std::string_view _prefix)
{
    const auto read_integer = [&]<class T>(const std::string_view _field) -> std::expected<T, std::string> {
        const auto value = TakePowerLossManifestField(_lines, _index, std::string{_prefix} + "_" + std::string{_field});
        if( !value )
            return std::unexpected(value.error());
        return ParsePowerLossManifestInteger<T>(*value, std::string{_prefix} + "_" + std::string{_field});
    };
    auto device = read_integer.template operator()<uint64_t>("device");
    auto inode = read_integer.template operator()<uint64_t>("inode");
    auto size = read_integer.template operator()<uint64_t>("size");
    auto mode = read_integer.template operator()<uint32_t>("mode");
    auto mtime_seconds = read_integer.template operator()<int64_t>("mtime_seconds");
    auto mtime_nanoseconds = read_integer.template operator()<int64_t>("mtime_nanoseconds");
    if( !device )
        return std::unexpected(device.error());
    if( !inode )
        return std::unexpected(inode.error());
    if( !size )
        return std::unexpected(size.error());
    if( !mode )
        return std::unexpected(mode.error());
    if( !mtime_seconds )
        return std::unexpected(mtime_seconds.error());
    if( !mtime_nanoseconds )
        return std::unexpected(mtime_nanoseconds.error());
    if( *device == 0 || *inode == 0 || *mtime_nanoseconds < 0 || *mtime_nanoseconds >= 1'000'000'000 )
        return std::unexpected("manifest stat values are invalid: " + std::string{_prefix});
    return PowerLossManifestStat{*device, *inode, *size, *mode, *mtime_seconds, *mtime_nanoseconds};
}

std::expected<PowerLossCheckpointManifest, std::string>
ParsePowerLossCheckpointManifest(const std::string_view _contents)
{
    if( _contents.empty() || _contents.back() != '\n' )
        return std::unexpected("manifest must be non-empty and newline-terminated");
    std::vector<std::string_view> lines;
    for( size_t begin = 0; begin < _contents.size(); ) {
        const size_t end = _contents.find('\n', begin);
        if( end == std::string_view::npos || end == begin )
            return std::unexpected("manifest contains an empty or unterminated field");
        lines.emplace_back(_contents.substr(begin, end - begin));
        begin = end + 1;
    }

    size_t index = 0;
    const auto schema = TakePowerLossManifestField(lines, index, "schema");
    if( !schema || *schema != "wincommander-power-loss-checkpoint-v1" )
        return std::unexpected(schema ? "unsupported power-loss manifest schema" : schema.error());
    const auto run_id = TakePowerLossManifestField(lines, index, "run_id");
    if( !run_id )
        return std::unexpected(run_id.error());
    const auto workspace_name = TakePowerLossManifestField(lines, index, "workspace_name");
    if( !workspace_name || !ValidPhysicalWorkspaceChildName(*workspace_name) )
        return std::unexpected(workspace_name ? "manifest workspace name is invalid" : workspace_name.error());
    const auto phase = TakePowerLossManifestField(lines, index, "phase");
    if( !phase )
        return std::unexpected(phase.error());
    PowerLossCheckpointPhase parsed_phase;
    if( *phase == "before-publish" )
        parsed_phase = PowerLossCheckpointPhase::BeforePublish;
    else if( *phase == "after-publish-before-full-fsync" )
        parsed_phase = PowerLossCheckpointPhase::AfterPublishBeforeFullFSync;
    else
        return std::unexpected("manifest checkpoint phase is invalid");
    const auto captured_seconds = TakePowerLossManifestField(lines, index, "captured_at_seconds");
    const auto captured_nanoseconds = TakePowerLossManifestField(lines, index, "captured_at_nanoseconds");
    if( !captured_seconds || !captured_nanoseconds )
        return std::unexpected(captured_seconds ? captured_nanoseconds.error() : captured_seconds.error());
    const auto parsed_seconds = ParsePowerLossManifestInteger<int64_t>(*captured_seconds, "captured_at_seconds");
    const auto parsed_nanoseconds =
        ParsePowerLossManifestInteger<int64_t>(*captured_nanoseconds, "captured_at_nanoseconds");
    if( !parsed_seconds || !parsed_nanoseconds || *parsed_nanoseconds < 0 || *parsed_nanoseconds >= 1'000'000'000 )
        return std::unexpected("manifest capture timestamp is invalid");
    const auto journal_filename = TakePowerLossManifestField(lines, index, "journal_filename");
    if( !journal_filename || *journal_filename != OperationJournal::Filename )
        return std::unexpected(journal_filename ? "manifest journal filename is invalid" : journal_filename.error());
    const auto journal = ReadPowerLossManifestStat(lines, index, "journal");
    if( !journal )
        return std::unexpected(journal.error());
    const auto journal_parent_filename = TakePowerLossManifestField(lines, index, "journal_parent_filename");
    if( !journal_parent_filename || !ValidPhysicalWorkspaceChildName(*journal_parent_filename) ) {
        return std::unexpected(journal_parent_filename ? "manifest journal parent filename is invalid"
                                                        : journal_parent_filename.error());
    }
    const auto journal_parent = ReadPowerLossManifestStat(lines, index, "journal_parent");
    if( !journal_parent )
        return std::unexpected(journal_parent.error());

    const auto source_filename = TakePowerLossManifestField(lines, index, "source_filename");
    if( !source_filename || !ValidPhysicalWorkspaceChildName(*source_filename) )
        return std::unexpected(source_filename ? "manifest source filename is invalid" : source_filename.error());
    const auto source = ReadPowerLossManifestStat(lines, index, "source");
    if( !source )
        return std::unexpected(source.error());
    const auto destination_parent_filename = TakePowerLossManifestField(lines, index, "destination_parent_filename");
    if( !destination_parent_filename || !ValidPhysicalWorkspaceChildName(*destination_parent_filename) ) {
        return std::unexpected(destination_parent_filename ? "manifest destination parent filename is invalid"
                                                            : destination_parent_filename.error());
    }
    const auto destination_parent = ReadPowerLossManifestStat(lines, index, "destination_parent");
    if( !destination_parent )
        return std::unexpected(destination_parent.error());
    const auto destination_filename = TakePowerLossManifestField(lines, index, "destination_filename");
    if( !destination_filename || !ValidPhysicalWorkspaceChildName(*destination_filename) ) {
        return std::unexpected(destination_filename ? "manifest destination filename is invalid"
                                                    : destination_filename.error());
    }
    const auto destination_exists = TakePowerLossManifestField(lines, index, "destination_exists");
    if( !destination_exists || (*destination_exists != "0" && *destination_exists != "1") ) {
        return std::unexpected(destination_exists ? "manifest destination existence is invalid" : destination_exists.error());
    }
    if( *destination_exists == "1" ) {
        const auto destination = ReadPowerLossManifestStat(lines, index, "destination");
        if( !destination )
            return std::unexpected(destination.error());
    }
    if( index != lines.size() )
        return std::unexpected("manifest contains unexpected trailing fields");

    return PowerLossCheckpointManifest{std::string{*workspace_name},
                                       parsed_phase,
                                       std::string{*journal_filename},
                                       *journal,
                                       std::string{*journal_parent_filename},
                                       *journal_parent};
}

std::expected<PowerLossCheckpointManifest, std::string> ReadPowerLossCheckpointManifest(const int _workspace_fd)
{
    const int manifest_fd =
        ::openat(_workspace_fd, g_PowerLossManifestName.data(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if( manifest_fd < 0 )
        return std::unexpected("cannot open checkpoint manifest without following links: " + std::string{std::strerror(errno)});
    struct stat manifest_status{};
    const int manifest_stat_result = ::fstat(manifest_fd, &manifest_status);
    if( manifest_stat_result != 0 || !S_ISREG(manifest_status.st_mode) || manifest_status.st_nlink != 1 ||
        manifest_status.st_uid != ::geteuid() || (manifest_status.st_mode & 077) != 0 || manifest_status.st_size <= 0 ||
        static_cast<uint64_t>(manifest_status.st_size) > g_PowerLossManifestMaxBytes ) {
        const int saved_error = manifest_stat_result == 0 ? EPERM : errno;
        ::close(manifest_fd);
        return std::unexpected("checkpoint manifest attributes are invalid: " + std::string{std::strerror(saved_error)});
    }

    std::string contents;
    contents.reserve(static_cast<size_t>(manifest_status.st_size));
    std::array<char, 4096> buffer{};
    while( true ) {
        const ssize_t count = ::read(manifest_fd, buffer.data(), buffer.size());
        if( count > 0 ) {
            if( contents.size() + static_cast<size_t>(count) > g_PowerLossManifestMaxBytes ) {
                ::close(manifest_fd);
                return std::unexpected("checkpoint manifest exceeds size limit");
            }
            contents.append(buffer.data(), static_cast<size_t>(count));
            continue;
        }
        if( count == 0 )
            break;
        if( errno == EINTR )
            continue;
        const int saved_error = errno;
        ::close(manifest_fd);
        return std::unexpected("cannot read checkpoint manifest: " + std::string{std::strerror(saved_error)});
    }
    if( ::close(manifest_fd) != 0 )
        return std::unexpected("cannot close checkpoint manifest: " + std::string{std::strerror(errno)});
    return ParsePowerLossCheckpointManifest(contents);
}

bool PowerLossManifestIdentityMatches(const PowerLossManifestStat &_manifest, const struct stat &_current) noexcept
{
    return _manifest.device == static_cast<uint64_t>(_current.st_dev) &&
           _manifest.inode == static_cast<uint64_t>(_current.st_ino);
}

std::expected<int, std::string> OpenAbsoluteDirectoryWithoutSymlinks(const std::string_view _path)
{
    if( _path.size() < 2 || _path.front() != '/' || _path.back() == '/' || _path.find("//") != std::string_view::npos )
        return std::unexpected("path must be a canonical absolute directory without a trailing slash");

    int current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if( current < 0 )
        return std::unexpected("cannot open filesystem root: " + std::string{std::strerror(errno)});

    size_t offset = 1;
    while( offset < _path.size() ) {
        const size_t slash = _path.find('/', offset);
        const auto component =
            _path.substr(offset, slash == std::string_view::npos ? _path.size() - offset : slash - offset);
        if( component.empty() || component == "." || component == ".." ) {
            ::close(current);
            return std::unexpected("path contains a non-canonical component");
        }
        const std::string name{component};
        const int next = ::openat(current, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if( next < 0 ) {
            const auto error = errno;
            ::close(current);
            return std::unexpected("cannot open path component " + name + ": " + std::strerror(error));
        }
        ::close(current);
        current = next;
        if( slash == std::string_view::npos )
            break;
        offset = slash + 1;
    }
    return current;
}

std::expected<void, std::string> VerifyMarkerFile(const int _root_fd)
{
    struct stat marker_stat{};
    if( ::fstatat(_root_fd, g_MarkerName.data(), &marker_stat, AT_SYMLINK_NOFOLLOW) != 0 )
        return std::unexpected("marker is absent or inaccessible: " + std::string{std::strerror(errno)});
    if( !S_ISREG(marker_stat.st_mode) || marker_stat.st_uid != ::geteuid() || marker_stat.st_nlink != 1 )
        return std::unexpected("marker is not the required user-owned regular file");

    const int marker_fd = ::openat(_root_fd, g_MarkerName.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if( marker_fd < 0 )
        return std::unexpected("marker cannot be opened without following links: " + std::string{std::strerror(errno)});

    std::array<char, g_MarkerContents.size() + 1> contents{};
    const ssize_t read = ::read(marker_fd, contents.data(), contents.size());
    const auto close_result = ::close(marker_fd);
    if( read != static_cast<ssize_t>(g_MarkerContents.size()) || close_result != 0 ||
        std::string_view{contents.data(), g_MarkerContents.size()} != g_MarkerContents )
        return std::unexpected("marker contents are not exact");
    return {};
}

class PhysicalVolumeRoot final
{
public:
    PhysicalVolumeRoot() = delete;
    PhysicalVolumeRoot(const PhysicalVolumeRoot &) = delete;
    PhysicalVolumeRoot &operator=(const PhysicalVolumeRoot &) = delete;
    PhysicalVolumeRoot(PhysicalVolumeRoot &&_other) noexcept
        : m_Fd{std::exchange(_other.m_Fd, -1)}, m_Path{std::move(_other.m_Path)}, m_Device{_other.m_Device},
          m_Inode{_other.m_Inode}
    {
    }
    PhysicalVolumeRoot &operator=(PhysicalVolumeRoot &&) = delete;
    ~PhysicalVolumeRoot()
    {
        if( m_Fd >= 0 )
            ::close(m_Fd);
    }

    [[nodiscard]] static std::expected<PhysicalVolumeRoot, std::string> Open(const std::string_view _environment,
                                                                             const PhysicalVolumeRole _role)
    {
        const char *const configured = std::getenv(_environment.data());
        if( configured == nullptr || configured[0] == '\0' )
            return std::unexpected(std::string{_environment} + " is not set");
        const std::string raw{configured};
        if( raw.empty() || raw.front() != '/' )
            return std::unexpected(std::string{_environment} + " must be an absolute path");

        std::error_code canonical_error;
        const auto canonical = std::filesystem::canonical(raw, canonical_error);
        if( canonical_error || canonical.generic_string() != raw )
            return std::unexpected(std::string{_environment} + " must already be canonical and contain no aliases");

        auto opened = OpenAbsoluteDirectoryWithoutSymlinks(raw);
        if( !opened )
            return std::unexpected(std::string{_environment} + ": " + opened.error());
        const int root_fd = *opened;
        const auto close_on_error = [&] { ::close(root_fd); };

        struct stat root_stat{};
        if( ::fstat(root_fd, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode) ) {
            close_on_error();
            return std::unexpected(std::string{_environment} + " is not an accessible directory");
        }
        if( root_stat.st_uid != ::geteuid() || (root_stat.st_mode & 0022) != 0 ) {
            close_on_error();
            return std::unexpected(std::string{_environment} + " must be user-owned and not group/world writable");
        }
        if( const auto marker = VerifyMarkerFile(root_fd); !marker ) {
            close_on_error();
            return std::unexpected(std::string{_environment} + ": " + marker.error());
        }

        const auto volume = TestEnv().native_fs_man->VolumeFromFD(root_fd);
        if( !volume ) {
            close_on_error();
            return std::unexpected(std::string{_environment} + " has no native volume identity");
        }
        const auto mounted_at = std::filesystem::path{volume->mounted_at_path}.lexically_normal();
        if( canonical == mounted_at || canonical == std::filesystem::path{"/"} ) {
            close_on_error();
            return std::unexpected(std::string{_environment} +
                                   " must be a dedicated child of its volume, not a mount root");
        }

        const auto decision = nc::vfs::native::EvaluateConditionalCopyVolume(*volume);
        if( _role == PhysicalVolumeRole::Internal ) {
            if( !decision.IsSupported() ) {
                close_on_error();
                return std::unexpected(std::string{_environment} +
                                       " is not a supported internal APFS Conditional Copy root");
            }
        }
        else if( decision.disposition != nc::vfs::native::ConditionalCopyVolumeDisposition::UnsupportedExternalMedia ) {
            close_on_error();
            return std::unexpected(
                std::string{_environment} +
                " must be an external/removable/ejectable APFS volume rejected as UnsupportedExternalMedia");
        }

        return PhysicalVolumeRoot{root_fd, canonical, root_stat.st_dev, root_stat.st_ino};
    }

    [[nodiscard]] int Fd() const noexcept { return m_Fd; }
    [[nodiscard]] const std::filesystem::path &Path() const noexcept { return m_Path; }
    [[nodiscard]] bool Contains(const std::filesystem::path &_candidate) const noexcept
    {
        const auto root = m_Path.generic_string();
        const auto candidate = _candidate.generic_string();
        return candidate.size() > root.size() && candidate.starts_with(root) && candidate[root.size()] == '/';
    }
    [[nodiscard]] bool MarkerStillValid() const noexcept { return VerifyMarkerFile(m_Fd).has_value(); }

private:
    PhysicalVolumeRoot(const int _fd, std::filesystem::path _path, const dev_t _device, const ino_t _inode)
        : m_Fd{_fd}, m_Path{std::move(_path)}, m_Device{_device}, m_Inode{_inode}
    {
    }

    int m_Fd{-1};
    std::filesystem::path m_Path;
    dev_t m_Device{};
    ino_t m_Inode{};
};

struct RecoveredPowerLossWorkspace final {
    int fd{-1};
    std::string name;
    std::string canonical_path;
    struct stat status{};
};

std::expected<RecoveredPowerLossWorkspace, std::string>
OpenRecoveredPowerLossWorkspace(const PhysicalVolumeRoot &_root, const std::string_view _configured_path)
{
    if( _configured_path.empty() || _configured_path.front() != '/' )
        return std::unexpected(std::string{g_PowerLossRecoveryWorkspaceEnvironment} + " must be an absolute path");
    const std::filesystem::path configured{_configured_path};
    std::error_code canonical_error;
    const auto canonical = std::filesystem::canonical(configured, canonical_error);
    if( canonical_error || canonical.generic_string() != _configured_path ) {
        return std::unexpected(std::string{g_PowerLossRecoveryWorkspaceEnvironment} +
                               " must already be canonical and contain no aliases");
    }
    if( canonical.parent_path() != _root.Path() || !ValidPhysicalWorkspaceChildName(canonical.filename().string()) ) {
        return std::unexpected(std::string{g_PowerLossRecoveryWorkspaceEnvironment} +
                               " must name a direct child of the configured internal fixture root");
    }

    const auto name = canonical.filename().string();
    struct stat anchored{};
    if( ::fstatat(_root.Fd(), name.c_str(), &anchored, AT_SYMLINK_NOFOLLOW) != 0 ) {
        return std::unexpected("cannot stat recovery workspace through fixture root: " + std::string{std::strerror(errno)});
    }
    const int workspace_fd = ::openat(_root.Fd(), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if( workspace_fd < 0 ) {
        return std::unexpected("cannot open recovery workspace through fixture root: " + std::string{std::strerror(errno)});
    }
    struct stat opened{};
    const int opened_stat_result = ::fstat(workspace_fd, &opened);
    if( opened_stat_result != 0 || !S_ISDIR(anchored.st_mode) || anchored.st_dev != opened.st_dev ||
        anchored.st_ino != opened.st_ino || opened.st_uid != ::geteuid() || (opened.st_mode & 0022) != 0 ) {
        const int saved_error = opened_stat_result == 0 ? EPERM : errno;
        ::close(workspace_fd);
        return std::unexpected("recovery workspace identity is invalid: " + std::string{std::strerror(saved_error)});
    }
    return RecoveredPowerLossWorkspace{workspace_fd, name, canonical.string(), opened};
}

std::string PhysicalWorkspaceName()
{
    std::array<unsigned char, 16> bytes{};
    arc4random_buf(bytes.data(), bytes.size());
    constexpr char digits[] = "0123456789abcdef";
    std::string name{"conditional-copy-it-"};
    name.reserve(name.size() + bytes.size() * 2);
    for( const auto byte : bytes ) {
        name.push_back(digits[byte >> 4]);
        name.push_back(digits[byte & 0x0f]);
    }
    return name;
}

bool RemoveChildrenAt(const int _directory_fd) noexcept
{
    DIR *const directory = ::fdopendir(::dup(_directory_fd));
    if( directory == nullptr )
        return false;
    bool success = true;
    while( const dirent *const entry = ::readdir(directory) ) {
        const std::string_view name{entry->d_name};
        if( name == "." || name == ".." )
            continue;
        struct stat status{};
        if( ::fstatat(_directory_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0 ) {
            success = false;
            continue;
        }
        if( S_ISDIR(status.st_mode) ) {
            const int child_fd =
                ::openat(_directory_fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            const bool removed_children = child_fd >= 0 && RemoveChildrenAt(child_fd);
            const bool closed = child_fd >= 0 && ::close(child_fd) == 0;
            if( !removed_children || !closed || ::unlinkat(_directory_fd, entry->d_name, AT_REMOVEDIR) != 0 ) {
                success = false;
            }
        }
        else if( ::unlinkat(_directory_fd, entry->d_name, 0) != 0 ) {
            success = false;
        }
    }
    return ::closedir(directory) == 0 && success;
}

class PhysicalWorkspace final
{
public:
    PhysicalWorkspace() = delete;
    PhysicalWorkspace(const PhysicalWorkspace &) = delete;
    PhysicalWorkspace &operator=(const PhysicalWorkspace &) = delete;
    PhysicalWorkspace(PhysicalWorkspace &&_other) noexcept
        : m_Root{_other.m_Root}, m_RootFd{std::exchange(_other.m_RootFd, -1)},
          m_WorkspaceFd{std::exchange(_other.m_WorkspaceFd, -1)}, m_Name{std::move(_other.m_Name)},
          m_Path{std::move(_other.m_Path)}, m_Device{_other.m_Device}, m_Inode{_other.m_Inode},
          m_Cleaned{std::exchange(_other.m_Cleaned, true)},
          m_PreservedForPostInterruptionInspection{std::exchange(_other.m_PreservedForPostInterruptionInspection, true)}
    {
    }
    PhysicalWorkspace &operator=(PhysicalWorkspace &&) = delete;
    ~PhysicalWorkspace()
    {
        if( !m_Cleaned && !m_PreservedForPostInterruptionInspection )
            static_cast<void>(Cleanup());
        if( m_WorkspaceFd >= 0 )
            ::close(m_WorkspaceFd);
        if( m_RootFd >= 0 )
            ::close(m_RootFd);
    }

    [[nodiscard]] static std::expected<PhysicalWorkspace, std::string> Create(const PhysicalVolumeRoot &_root)
    {
        const int root_fd = ::dup(_root.Fd());
        if( root_fd < 0 )
            return std::unexpected("cannot duplicate fixture root descriptor: " + std::string{std::strerror(errno)});

        for( int attempt = 0; attempt < 8; ++attempt ) {
            const auto name = PhysicalWorkspaceName();
            if( ::mkdirat(root_fd, name.c_str(), 0700) != 0 ) {
                if( errno == EEXIST )
                    continue;
                const auto error = std::string{std::strerror(errno)};
                ::close(root_fd);
                return std::unexpected("cannot create fixture workspace: " + error);
            }
            const int workspace_fd = ::openat(root_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if( workspace_fd < 0 ) {
                const auto error = std::string{std::strerror(errno)};
                ::unlinkat(root_fd, name.c_str(), AT_REMOVEDIR);
                ::close(root_fd);
                return std::unexpected("cannot reopen fixture workspace: " + error);
            }
            struct stat created{};
            struct stat opened{};
            if( ::fstatat(root_fd, name.c_str(), &created, AT_SYMLINK_NOFOLLOW) != 0 ||
                ::fstat(workspace_fd, &opened) != 0 || !S_ISDIR(created.st_mode) || created.st_dev != opened.st_dev ||
                created.st_ino != opened.st_ino ) {
                ::close(workspace_fd);
                ::unlinkat(root_fd, name.c_str(), AT_REMOVEDIR);
                ::close(root_fd);
                return std::unexpected("fixture workspace identity cannot be anchored");
            }
            return PhysicalWorkspace{
                _root, root_fd, workspace_fd, name, _root.Path() / name, opened.st_dev, opened.st_ino};
        }
        ::close(root_fd);
        return std::unexpected("could not allocate a unique fixture workspace");
    }

    [[nodiscard]] int Fd() const noexcept { return m_WorkspaceFd; }
    [[nodiscard]] const std::filesystem::path &Path() const noexcept { return m_Path; }

    /**
     * Suppresses destructor cleanup so a checkpoint-driven interruption can be inspected after process restart.
     * An explicit Cleanup() remains available to the supervising fixture after inspection.
     */
    [[nodiscard]] bool PreserveForPostInterruptionInspection() noexcept
    {
        if( m_Cleaned || m_WorkspaceFd < 0 || !m_Root || !m_Root->MarkerStillValid() )
            return false;
        m_PreservedForPostInterruptionInspection = true;
        return true;
    }

    [[nodiscard]] std::expected<std::filesystem::path, std::string> WriteNewFile(const std::string_view _name,
                                                                                 const std::string_view _contents) const
    {
        if( !ValidDirectChildName(_name) )
            return std::unexpected("invalid fixture file name");
        const std::string name{_name};
        const int file =
            ::openat(m_WorkspaceFd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if( file < 0 )
            return std::unexpected("cannot create fixture file: " + std::string{std::strerror(errno)});
        size_t offset = 0;
        while( offset < _contents.size() ) {
            const ssize_t written = ::write(file, _contents.data() + offset, _contents.size() - offset);
            if( written > 0 ) {
                offset += static_cast<size_t>(written);
                continue;
            }
            if( written < 0 && errno == EINTR )
                continue;
            const auto error = std::string{std::strerror(errno)};
            ::close(file);
            return std::unexpected("cannot write fixture file: " + error);
        }
        if( ::fsync(file) != 0 || ::close(file) != 0 )
            return std::unexpected("cannot synchronize fixture file: " + std::string{std::strerror(errno)});
        return m_Path / name;
    }

    [[nodiscard]] std::expected<std::filesystem::path, std::string> CreateDirectory(const std::string_view _name) const
    {
        if( !ValidDirectChildName(_name) )
            return std::unexpected("invalid fixture directory name");
        const std::string name{_name};
        if( ::mkdirat(m_WorkspaceFd, name.c_str(), 0700) != 0 )
            return std::unexpected("cannot create fixture directory: " + std::string{std::strerror(errno)});
        return m_Path / name;
    }

    [[nodiscard]] bool Cleanup() noexcept
    {
        if( m_Cleaned )
            return true;
        if( !m_Root->MarkerStillValid() )
            return false;
        struct stat current{};
        if( ::fstatat(m_RootFd, m_Name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(current.st_mode) ||
            current.st_dev != m_Device || current.st_ino != m_Inode )
            return false;
        if( !RemoveChildrenAt(m_WorkspaceFd) || ::unlinkat(m_RootFd, m_Name.c_str(), AT_REMOVEDIR) != 0 )
            return false;
        m_Cleaned = true;
        m_PreservedForPostInterruptionInspection = false;
        return true;
    }

private:
    static bool ValidDirectChildName(const std::string_view _name) noexcept
    {
        return !_name.empty() && _name != "." && _name != ".." && _name.find('/') == std::string_view::npos;
    }

    PhysicalWorkspace(const PhysicalVolumeRoot &_root,
                      const int _root_fd,
                      const int _workspace_fd,
                      std::string _name,
                      std::filesystem::path _path,
                      const dev_t _device,
                      const ino_t _inode)
        : m_Root{&_root}, m_RootFd{_root_fd}, m_WorkspaceFd{_workspace_fd}, m_Name{std::move(_name)},
          m_Path{std::move(_path)}, m_Device{_device}, m_Inode{_inode}
    {
    }

    const PhysicalVolumeRoot *m_Root;
    int m_RootFd{-1};
    int m_WorkspaceFd{-1};
    std::string m_Name;
    std::filesystem::path m_Path;
    dev_t m_Device{};
    ino_t m_Inode{};
    bool m_Cleaned{false};
    bool m_PreservedForPostInterruptionInspection{false};
};

class PowerLossCheckpointIO final : public nc::vfs::native::ConditionalCopyIO
{
public:
    PowerLossCheckpointIO(PowerLossCheckpointConfiguration _configuration,
                          std::string _run_id,
                          const int _workspace_fd,
                          std::string _workspace_name,
                          std::string _workspace_path,
                          std::string _source_name,
                          std::string _destination_parent_name,
                          std::string _destination_name,
                          std::function<bool()> _preserve_workspace)
        : m_Configuration{_configuration}, m_RunId{std::move(_run_id)}, m_WorkspaceFd{::dup(_workspace_fd)},
          m_WorkspaceName{std::move(_workspace_name)}, m_WorkspacePath{std::move(_workspace_path)},
          m_SourceName{std::move(_source_name)},
          m_DestinationParentName{std::move(_destination_parent_name)},
          m_DestinationName{std::move(_destination_name)}, m_PreserveWorkspace{std::move(_preserve_workspace)}
    {
        if( m_WorkspaceFd < 0 )
            m_CheckpointError.store(errno == 0 ? EIO : errno);
    }

    ~PowerLossCheckpointIO() override
    {
        if( m_WorkspaceFd >= 0 )
            ::close(m_WorkspaceFd);
    }

    void Checkpoint(const nc::vfs::native::ConditionalCopyCheckpoint _checkpoint) noexcept override
    {
        if( _checkpoint != NativeCheckpoint(m_Configuration.phase) )
            return;

        bool expected = false;
        if( !m_Reached.compare_exchange_strong(expected, true) )
            return;

        const int manifest_error = WriteManifest();
        if( manifest_error != 0 ) {
            m_CheckpointError.store(manifest_error);
            return;
        }
        m_ManifestWritten.store(true);

        if( !m_Configuration.block_for_operator )
            return;

        try {
            if( !m_PreserveWorkspace || !m_PreserveWorkspace() ) {
                m_CheckpointError.store(EPERM);
                return;
            }
        }
        catch( ... ) {
            m_CheckpointError.store(EIO);
            return;
        }

        std::fprintf(stderr,
                     "power-loss checkpoint %s reached for run %s; workspace %s preserves durable %s; await operator interruption\n",
                     PowerLossCheckpointPhaseName(m_Configuration.phase).data(),
                     m_RunId.c_str(),
                     m_WorkspacePath.c_str(),
                     g_PowerLossManifestName.data());
        std::fflush(stderr);
        auto lock = std::unique_lock{m_BlockMutex};
        m_BlockCondition.wait(lock, [] { return false; });
    }

    int Clone(const int _source_fd, const int _destination_parent_fd, const char *_name, const uint32_t _flags) noexcept override
    {
        if( const int checkpoint_error = m_CheckpointError.load(); checkpoint_error != 0 ) {
            errno = checkpoint_error;
            return -1;
        }
        return ConditionalCopyIO::Clone(_source_fd, _destination_parent_fd, _name, _flags);
    }

    int FullFSync(const int _fd) noexcept override
    {
        if( const int checkpoint_error = m_CheckpointError.load(); checkpoint_error != 0 ) {
            errno = checkpoint_error;
            return -1;
        }
        return ConditionalCopyIO::FullFSync(_fd);
    }

    [[nodiscard]] bool Reached() const noexcept { return m_Reached.load(); }
    [[nodiscard]] bool ManifestWritten() const noexcept { return m_ManifestWritten.load(); }
    [[nodiscard]] int CheckpointError() const noexcept { return m_CheckpointError.load(); }

private:
    static void AppendStat(std::string &_contents, const std::string_view _prefix, const struct stat &_status)
    {
        const auto append = [&_contents, _prefix](const std::string_view _field, const auto _value) {
            _contents.append(_prefix);
            _contents.push_back('_');
            _contents.append(_field);
            _contents.push_back('=');
            _contents.append(std::to_string(_value));
            _contents.push_back('\n');
        };
        append("device", static_cast<uint64_t>(_status.st_dev));
        append("inode", static_cast<uint64_t>(_status.st_ino));
        append("size", static_cast<uint64_t>(_status.st_size));
        append("mode", static_cast<uint32_t>(_status.st_mode));
        append("mtime_seconds", static_cast<int64_t>(_status.st_mtimespec.tv_sec));
        append("mtime_nanoseconds", static_cast<int64_t>(_status.st_mtimespec.tv_nsec));
    }

    [[nodiscard]] int WriteManifest() noexcept
    {
        try {
            if( m_WorkspaceFd < 0 )
                return m_CheckpointError.load() == 0 ? EBADF : m_CheckpointError.load();

            struct stat journal_parent_status{};
            if( ::fstat(m_WorkspaceFd, &journal_parent_status) != 0 )
                return errno == 0 ? EIO : errno;
            struct stat journal_status{};
            if( ::fstatat(m_WorkspaceFd,
                          OperationJournal::Filename.data(),
                          &journal_status,
                          AT_SYMLINK_NOFOLLOW) != 0 )
                return errno == 0 ? EIO : errno;

            struct stat source_status{};
            if( ::fstatat(m_WorkspaceFd, m_SourceName.c_str(), &source_status, AT_SYMLINK_NOFOLLOW) != 0 )
                return errno == 0 ? EIO : errno;

            const int destination_parent_fd =
                ::openat(m_WorkspaceFd,
                         m_DestinationParentName.c_str(),
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if( destination_parent_fd < 0 )
                return errno == 0 ? EIO : errno;

            struct stat destination_parent_status{};
            struct stat destination_status{};
            bool destination_exists = false;
            int result = 0;
            if( ::fstat(destination_parent_fd, &destination_parent_status) != 0 ) {
                result = errno == 0 ? EIO : errno;
            }
            else if( ::fstatat(destination_parent_fd,
                               m_DestinationName.c_str(),
                               &destination_status,
                               AT_SYMLINK_NOFOLLOW) == 0 ) {
                destination_exists = true;
            }
            else if( errno != ENOENT ) {
                result = errno == 0 ? EIO : errno;
            }
            if( ::close(destination_parent_fd) != 0 && result == 0 )
                result = errno == 0 ? EIO : errno;
            if( result != 0 )
                return result;

            timespec captured_at{};
            if( ::clock_gettime(CLOCK_REALTIME, &captured_at) != 0 )
                return errno == 0 ? EIO : errno;

            std::string contents;
            contents.reserve(1024);
            contents.append("schema=wincommander-power-loss-checkpoint-v1\n");
            contents.append("run_id=");
            contents.append(m_RunId);
            contents.push_back('\n');
            contents.append("workspace_name=");
            contents.append(m_WorkspaceName);
            contents.push_back('\n');
            contents.append("phase=");
            contents.append(PowerLossCheckpointPhaseName(m_Configuration.phase));
            contents.push_back('\n');
            contents.append("captured_at_seconds=");
            contents.append(std::to_string(static_cast<int64_t>(captured_at.tv_sec)));
            contents.push_back('\n');
            contents.append("captured_at_nanoseconds=");
            contents.append(std::to_string(static_cast<int64_t>(captured_at.tv_nsec)));
            contents.push_back('\n');
            contents.append("journal_filename=");
            contents.append(OperationJournal::Filename);
            contents.push_back('\n');
            AppendStat(contents, "journal", journal_status);
            contents.append("journal_parent_filename=");
            contents.append(m_WorkspaceName);
            contents.push_back('\n');
            AppendStat(contents, "journal_parent", journal_parent_status);
            contents.append("source_filename=");
            contents.append(m_SourceName);
            contents.push_back('\n');
            AppendStat(contents, "source", source_status);
            contents.append("destination_parent_filename=");
            contents.append(m_DestinationParentName);
            contents.push_back('\n');
            AppendStat(contents, "destination_parent", destination_parent_status);
            contents.append("destination_filename=");
            contents.append(m_DestinationName);
            contents.push_back('\n');
            contents.append("destination_exists=");
            contents.append(destination_exists ? "1\n" : "0\n");
            if( destination_exists )
                AppendStat(contents, "destination", destination_status);

            const int manifest_fd = ::openat(m_WorkspaceFd,
                                              g_PowerLossManifestName.data(),
                                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                                              0600);
            if( manifest_fd < 0 )
                return errno == 0 ? EIO : errno;

            size_t offset = 0;
            while( offset < contents.size() ) {
                const ssize_t written = ::write(manifest_fd, contents.data() + offset, contents.size() - offset);
                if( written > 0 ) {
                    offset += static_cast<size_t>(written);
                    continue;
                }
                if( written < 0 && errno == EINTR )
                    continue;
                result = errno == 0 ? EIO : errno;
                break;
            }
            if( result == 0 && ::fsync(manifest_fd) != 0 )
                result = errno == 0 ? EIO : errno;
            if( ::close(manifest_fd) != 0 && result == 0 )
                result = errno == 0 ? EIO : errno;
            if( result != 0 )
                return result;
            if( ::fsync(m_WorkspaceFd) != 0 )
                return errno == 0 ? EIO : errno;
            return 0;
        }
        catch( ... ) {
            return ENOMEM;
        }
    }

    const PowerLossCheckpointConfiguration m_Configuration;
    const std::string m_RunId;
    int m_WorkspaceFd{-1};
    const std::string m_WorkspaceName;
    const std::string m_WorkspacePath;
    const std::string m_SourceName;
    const std::string m_DestinationParentName;
    const std::string m_DestinationName;
    const std::function<bool()> m_PreserveWorkspace;
    std::atomic_bool m_Reached{false};
    std::atomic_bool m_ManifestWritten{false};
    std::atomic_int m_CheckpointError{0};
    std::mutex m_BlockMutex;
    std::condition_variable m_BlockCondition;
};

OperationPlan
PhysicalCopyPlan(const std::string _id, const std::filesystem::path &_source, const std::filesystem::path &_destination)
{
    auto plan = OperationPlan::Create({
        .plan_id = _id,
        .type = OperationPlanType::Copy,
        .sources = {OperationPlanSourceInput{"local", _source.string()}},
        .destination =
            OperationPlanDestinationInput{"local", _destination.string(), OperationPlanDestinationKind::Directory},
        .conflict_policy =
            OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem},
        .created_at = OperationPlan::TimePoint{1'700'000'000s},
    });
    REQUIRE(plan);
    return std::move(*plan);
}

ReviewedVFSOperationPreflight PhysicalCopyReview(const std::string _id,
                                                 const std::filesystem::path &_source,
                                                 const std::filesystem::path &_destination,
                                                 std::shared_ptr<nc::vfs::Host> _native_host)
{
    auto bindings = VFSOperationPlanningBindings::Create({{"local", std::move(_native_host)}});
    REQUIRE(bindings);
    auto probes = VFSOperationPlanningProbes::Create(
        *bindings,
        [](const OperationPlanningPath &,
           OperationPlanningRequiredAccess,
           nc::vfs::Host &) -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    REQUIRE(probes);
    auto reviewed = ReviewedVFSOperationPreflight::Review(
        probes->Preflight(PhysicalCopyPlan(_id, _source, _destination)), VFSOperationPreflightReviewDecision::Approved);
    REQUIRE(reviewed);
    return std::move(*reviewed);
}

std::string ReadExactFile(const std::filesystem::path &_path)
{
    std::ifstream stream{_path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void RequireDistinctRoots(const PhysicalVolumeRoot &_internal, const PhysicalVolumeRoot &_external)
{
    REQUIRE(_internal.Path() != _external.Path());
    REQUIRE_FALSE(_internal.Contains(_external.Path()));
    REQUIRE_FALSE(_external.Contains(_internal.Path()));
}

} // namespace

TEST_CASE("CopyAs physical volumes: internal APFS publication reaches durable completion",
          "[reviewed-copy-as-physical][operations-it]")
{
    auto root_result = PhysicalVolumeRoot::Open(g_InternalRootEnvironment, PhysicalVolumeRole::Internal);
    if( !root_result )
        SkipOrFailPhysicalFixture(root_result.error());
    auto root = std::move(*root_result);

    auto workspace_result = PhysicalWorkspace::Create(root);
    REQUIRE(workspace_result);
    auto workspace = std::move(*workspace_result);
    auto source_result = workspace.WriteNewFile("source.txt", "physical-internal-conditional-copy\n");
    auto destination_result = workspace.CreateDirectory("destination");
    REQUIRE(source_result);
    REQUIRE(destination_result);
    const auto source = *source_result;
    const auto destination_directory = *destination_result;
    const auto destination = destination_directory / source.filename();

    REQUIRE(TestEnv().vfs_native->ConditionalCopyPathSupport(source.string(), destination_directory.string()) ==
            nc::vfs::ProviderConditionalCopyPathSupport::SameVolumeClone);
    const auto journal_result = OperationJournal::Open(workspace.Path().string());
    REQUIRE(journal_result);
    auto journal = std::make_shared<OperationJournal>(std::move(*journal_result));
    auto pool = Pool::Make();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();

    std::mutex terminal_lock;
    std::optional<CopyOperationDurableTerminalOutcome> terminal_outcome;
    std::atomic_bool removal_after_terminal{false};
    auto removal_ticket = pool->Observe(Pool::NotifyAboutRemoval, [&] {
        const auto guard = std::lock_guard{terminal_lock};
        removal_after_terminal = terminal_outcome.has_value();
    });
    CopyOperationSubmissionHooks hooks{
        .durable_terminal_observer = [&](const CopyOperationDurableTerminalOutcome &_outcome) {
            const auto guard = std::lock_guard{terminal_lock};
            terminal_outcome = _outcome;
        }};

    CopyOperationOrchestrator orchestrator{journal, pool, custodian};
    auto submitted = orchestrator.Submit(
        PhysicalCopyReview("physical-internal-copy", source, destination_directory, TestEnv().vfs_native), {}, std::move(hooks));
    REQUIRE(submitted);
    REQUIRE((*submitted)->Wait(10s));
    REQUIRE(pool->Empty());
    pool->StopAndWaitForShutdown();

    CHECK(custodian->PendingCount() == 0);
    CHECK(ReadExactFile(destination) == ReadExactFile(source));
    struct stat source_stat{};
    struct stat destination_stat{};
    REQUIRE(::stat(source.c_str(), &source_stat) == 0);
    REQUIRE(::stat(destination.c_str(), &destination_stat) == 0);
    CHECK(source_stat.st_dev == destination_stat.st_dev);
    CHECK(source_stat.st_ino != destination_stat.st_ino);
    CHECK((source_stat.st_mode & 07777) == (destination_stat.st_mode & 07777));
    {
        const auto guard = std::lock_guard{terminal_lock};
        REQUIRE(terminal_outcome);
        CHECK(terminal_outcome->plan_id == "physical-internal-copy");
        CHECK(terminal_outcome->state == OperationJournalState::Completed);
        REQUIRE(terminal_outcome->item_result);
        CHECK(terminal_outcome->item_result->destination_publication == OperationJournalPublicationState::Published);
        CHECK(terminal_outcome->item_result->filesystem_sync_status == OperationJournalFilesystemSyncStatus::Confirmed);
    }
    CHECK(removal_after_terminal);
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Completed);
    REQUIRE(snapshot[0].item_results.size() == 1);
    CHECK(snapshot[0].item_results[0].status == OperationJournalItemStatus::Succeeded);
    CHECK(snapshot[0].item_results[0].destination_publication == OperationJournalPublicationState::Published);
    CHECK(snapshot[0].item_results[0].filesystem_sync_status == OperationJournalFilesystemSyncStatus::Confirmed);
    REQUIRE(workspace.Cleanup());
}

TEST_CASE("CopyAs physical volumes: external APFS scope fails before Pool admission",
          "[reviewed-copy-as-physical][operations-it]")
{
    auto internal_root_result = PhysicalVolumeRoot::Open(g_InternalRootEnvironment, PhysicalVolumeRole::Internal);
    if( !internal_root_result )
        SkipOrFailPhysicalFixture(internal_root_result.error());
    auto external_root_result = PhysicalVolumeRoot::Open(g_ExternalRootEnvironment, PhysicalVolumeRole::External);
    if( !external_root_result )
        SkipOrFailPhysicalFixture(external_root_result.error());
    auto internal_root = std::move(*internal_root_result);
    auto external_root = std::move(*external_root_result);
    RequireDistinctRoots(internal_root, external_root);

    auto journal_workspace_result = PhysicalWorkspace::Create(internal_root);
    auto external_workspace_result = PhysicalWorkspace::Create(external_root);
    REQUIRE(journal_workspace_result);
    REQUIRE(external_workspace_result);
    auto journal_workspace = std::move(*journal_workspace_result);
    auto external_workspace = std::move(*external_workspace_result);
    auto source_result = external_workspace.WriteNewFile("source.txt", "physical-external-rejection\n");
    auto destination_result = external_workspace.CreateDirectory("destination");
    REQUIRE(source_result);
    REQUIRE(destination_result);
    const auto source = *source_result;
    const auto destination_directory = *destination_result;
    const auto destination = destination_directory / source.filename();

    REQUIRE(TestEnv().vfs_native->ConditionalCopyPathSupport(source.string(), destination_directory.string()) ==
            nc::vfs::ProviderConditionalCopyPathSupport::Unsupported);
    const auto journal_result = OperationJournal::Open(journal_workspace.Path().string());
    REQUIRE(journal_result);
    auto journal = std::make_shared<OperationJournal>(std::move(*journal_result));
    auto pool = Pool::Make();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    std::atomic_int additions{0};
    pool->ObserveUnticketed(Pool::NotifyAboutAddition, [&] { ++additions; });

    CopyOperationOrchestrator orchestrator{journal, pool, custodian};
    auto submitted =
        orchestrator.Submit(
            PhysicalCopyReview("physical-external-rejection", source, destination_directory, TestEnv().vfs_native));
    REQUIRE_FALSE(submitted);
    CHECK(submitted.error().code == CopyOperationOrchestratorErrorCode::ExecutionFactoryFailed);
    REQUIRE(submitted.error().reviewed_factory_error);
    CHECK(submitted.error().reviewed_factory_error->code ==
          ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable);
    CHECK(additions == 0);
    CHECK(pool->Empty());
    CHECK(custodian->PendingCount() == 0);
    CHECK_FALSE(std::filesystem::exists(destination));
    CHECK(ReadExactFile(source) == "physical-external-rejection\n");
    const auto snapshot = journal->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Failed);
    CHECK(snapshot[0].item_results.empty());
    pool->StopAndWaitForShutdown();
    REQUIRE(external_workspace.Cleanup());
    REQUIRE(journal_workspace.Cleanup());
}

TEST_CASE("CopyAs physical power-loss checkpoint harness records a durable manifest before optional operator wait",
          "[.reviewed-copy-as-power-loss-checkpoint][operations-it]")
{
    const auto configuration_result = PowerLossCheckpointConfigurationFromEnvironment();
    REQUIRE(configuration_result);
    if( !*configuration_result )
        SKIP(std::string{g_PowerLossPhaseEnvironment} + " must select an explicit checkpoint phase");
    const auto configuration = **configuration_result;

    auto root_result = PhysicalVolumeRoot::Open(g_InternalRootEnvironment, PhysicalVolumeRole::Internal);
    if( !root_result )
        SkipOrFailPhysicalFixture(root_result.error());
    auto root = std::move(*root_result);

    auto workspace_result = PhysicalWorkspace::Create(root);
    REQUIRE(workspace_result);
    auto workspace = std::move(*workspace_result);
    auto source_result = workspace.WriteNewFile("source.txt", "physical-power-loss-checkpoint\n");
    auto destination_result = workspace.CreateDirectory("destination");
    REQUIRE(source_result);
    REQUIRE(destination_result);
    const auto source = *source_result;
    const auto destination_directory = *destination_result;
    const auto destination = destination_directory / source.filename();

    const auto journal_result = OperationJournal::Open(workspace.Path().string());
    REQUIRE(journal_result);
    auto journal = std::make_shared<OperationJournal>(std::move(*journal_result));
    const auto run_id = PhysicalWorkspaceName();
    auto checkpoint_io = std::make_shared<PowerLossCheckpointIO>(configuration,
                                                                  run_id,
                                                                  workspace.Fd(),
                                                                  workspace.Path().filename().string(),
                                                                  workspace.Path().string(),
                                                                  source.filename().string(),
                                                                  destination_directory.filename().string(),
                                                                  destination.filename().string(),
                                                                  [&workspace] {
                                                                      return workspace.PreserveForPostInterruptionInspection();
                                                                  });
    auto host = std::make_shared<nc::vfs::NativeHost>(
        *TestEnv().native_fs_man, *TestEnv().fsevents_file_update, checkpoint_io);
    REQUIRE(host->ConditionalCopyPathSupport(source.string(), destination_directory.string()) ==
            nc::vfs::ProviderConditionalCopyPathSupport::SameVolumeClone);

    auto pool = Pool::Make();
    auto custodian = std::make_shared<CopyOperationRunReceiptCustodian>();
    CopyOperationOrchestrator orchestrator{journal, pool, custodian};
    auto submitted = orchestrator.Submit(
        PhysicalCopyReview("physical-power-loss-checkpoint", source, destination_directory, host));
    REQUIRE(submitted);

    // With WINCOMMANDER_OPERATIONS_IT_POWER_LOSS_BLOCK=1 the worker remains here after fsyncing the manifest,
    // leaving the operator to interrupt power. The non-blocking mode validates setup without emulating a reboot.
    if( configuration.block_for_operator ) {
        (*submitted)->Wait();
        FAIL("power-loss checkpoint worker returned without an operator interruption");
    }
    REQUIRE((*submitted)->Wait(10s));
    pool->StopAndWaitForShutdown();

    CHECK(checkpoint_io->Reached());
    CHECK(checkpoint_io->ManifestWritten());
    CHECK(checkpoint_io->CheckpointError() == 0);
    CHECK(std::filesystem::exists(workspace.Path() / g_PowerLossManifestName));
    CHECK(std::filesystem::exists(destination));
    CHECK(custodian->PendingCount() == 0);
    REQUIRE(workspace.Cleanup());
}

TEST_CASE("CopyAs physical power-loss recovery profile classifies a retained unfinished checkpoint without execution",
          "[.reviewed-copy-as-power-loss-recovery][operations-it]")
{
    const char *const configured_workspace = std::getenv(g_PowerLossRecoveryWorkspaceEnvironment.data());
    if( configured_workspace == nullptr || configured_workspace[0] == '\0' )
        SKIP(std::string{g_PowerLossRecoveryWorkspaceEnvironment} + " must name an explicit retained workspace");

    auto root_result = PhysicalVolumeRoot::Open(g_InternalRootEnvironment, PhysicalVolumeRole::Internal);
    if( !root_result )
        SkipOrFailPhysicalFixture(root_result.error());
    auto root = std::move(*root_result);
    auto workspace_result = OpenRecoveredPowerLossWorkspace(root, configured_workspace);
    if( !workspace_result )
        FAIL(workspace_result.error());
    auto workspace = std::move(*workspace_result);

    const auto manifest = ReadPowerLossCheckpointManifest(workspace.fd);
    if( !manifest )
        FAIL(manifest.error());
    REQUIRE(manifest->workspace_name == workspace.name);
    REQUIRE(manifest->journal_parent_filename == workspace.name);
    REQUIRE(PowerLossManifestIdentityMatches(manifest->journal_parent, workspace.status));
    REQUIRE(manifest->journal_filename == OperationJournal::Filename);

    struct stat journal_status{};
    REQUIRE(::fstatat(workspace.fd,
                      OperationJournal::Filename.data(),
                      &journal_status,
                      AT_SYMLINK_NOFOLLOW) == 0);
    REQUIRE(S_ISREG(journal_status.st_mode));
    REQUIRE(journal_status.st_nlink == 1);
    REQUIRE(journal_status.st_uid == ::geteuid());
    REQUIRE((journal_status.st_mode & 077) == 0);
    REQUIRE(PowerLossManifestIdentityMatches(manifest->journal, journal_status));

    // This is deliberately the first journal API call: it cannot create a lock, migrate the schema, or alter state.
    const auto before_recovery = OperationJournalTesting::InspectPersistedReadOnly(workspace.fd);
    if( !before_recovery )
        FAIL("read-only checkpoint inspection failed with journal error " + std::to_string(static_cast<int>(before_recovery.error().code)));
    REQUIRE(before_recovery->size() == 1);
    CHECK((*before_recovery)[0].plan.Id().Value() == "physical-power-loss-checkpoint");
    CHECK((*before_recovery)[0].state == OperationJournalState::Running);
    CHECK((*before_recovery)[0].item_results.empty());

    {
        auto journal = OperationJournal::Open(workspace.canonical_path);
        REQUIRE(journal);
        const auto recovered = journal->Snapshot();
        REQUIRE(recovered.size() == 1);
        CHECK(recovered[0].plan.Id().Value() == "physical-power-loss-checkpoint");
        CHECK(recovered[0].state == OperationJournalState::Interrupted);
        CHECK(recovered[0].item_results.empty());
    }
    CHECK(std::filesystem::exists(std::filesystem::path{workspace.canonical_path} / g_PowerLossManifestName));
    CHECK(std::filesystem::exists(std::filesystem::path{workspace.canonical_path} / OperationJournal::Filename));
    CHECK(::close(workspace.fd) == 0);
}

} // namespace nc::ops
