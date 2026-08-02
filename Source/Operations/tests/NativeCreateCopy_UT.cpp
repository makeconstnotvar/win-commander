// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include "../source/NativeCreateCopy/NativeCreateCopy.h"
#include "../source/Statistics.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <new>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <vector>

#define PREFIX "NativeCreateCopy: "

namespace nc::ops {
namespace {

std::string NativeCreateCopyReadFile(const std::filesystem::path &_path)
{
    std::ifstream stream{_path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void NativeCreateCopyWriteFile(const std::filesystem::path &_path, std::string_view _contents)
{
    std::ofstream stream{_path, std::ios::binary};
    REQUIRE(stream);
    stream.write(_contents.data(), static_cast<std::streamsize>(_contents.size()));
    REQUIRE(stream);
}

NativeCreateCopyCapsule NativeCreateCopyMakeCapsule(const std::filesystem::path &_source,
                                                    const std::filesystem::path &_destination_parent,
                                                    std::string _destination_name)
{
    const int source_fd = open(_source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    REQUIRE(source_fd >= 0);
    const int destination_parent_fd =
        open(_destination_parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    REQUIRE(destination_parent_fd >= 0);

    struct stat source_stat {};
    struct stat parent_stat {};
    REQUIRE(fstat(source_fd, &source_stat) == 0);
    REQUIRE(fstat(destination_parent_fd, &parent_stat) == 0);

    return NativeCreateCopyCapsule{NativeCreateCopyCapsuleInput{
        .source_fd = source_fd,
        .expected_source = NativeCreateCopyIdentity::FromStat(source_stat),
        .destination_parent_fd = destination_parent_fd,
        .expected_destination_parent = NativeCreateCopyIdentity::FromStat(parent_stat),
        .source_display_path = _source.native(),
        .destination_display_path = (_destination_parent / _destination_name).native(),
        .destination_name = std::move(_destination_name),
        .mode = source_stat.st_mode,
        .size = static_cast<uint64_t>(source_stat.st_size),
    }};
}

std::vector<std::filesystem::path> NativeCreateCopyTempFiles(const std::filesystem::path &_directory)
{
    std::vector<std::filesystem::path> result;
    for( const auto &entry : std::filesystem::directory_iterator{_directory} )
        if( entry.path().filename().native().starts_with(".wincommander-copy.") )
            result.emplace_back(entry.path());
    return result;
}

void NativeCreateCopyRemoveRecoveryArtifacts(const std::filesystem::path &_directory)
{
    const auto artifacts = NativeCreateCopyTempFiles(_directory);
    REQUIRE_FALSE(artifacts.empty());
    for( const auto &artifact : artifacts )
        REQUIRE(std::filesystem::remove(artifact));
}

class NativeCreateCopySourceFlagReset final
{
public:
    NativeCreateCopySourceFlagReset(std::filesystem::path _path, bool _active)
        : m_Path{std::move(_path)}, m_Active{_active}
    {
    }

    ~NativeCreateCopySourceFlagReset()
    {
        if( m_Active )
            (void)chflags(m_Path.c_str(), 0);
    }

private:
    std::filesystem::path m_Path;
    bool m_Active;
};

class NativeCreateCopyScriptedIO : public NativeCreateCopyIO
{
public:
    ssize_t PRead(int _fd, void *_buffer, size_t _size, off_t _offset) noexcept override
    {
        if( pread_eintr_count > 0 ) {
            --pread_eintr_count;
            errno = EINTR;
            return -1;
        }
        return NativeCreateCopyIO::PRead(_fd, _buffer, _size, _offset);
    }

    ssize_t Write(int _fd, const void *_buffer, size_t _size) noexcept override
    {
        if( write_eintr_count > 0 ) {
            --write_eintr_count;
            errno = EINTR;
            return -1;
        }
        if( write_error != 0 ) {
            errno = write_error;
            return -1;
        }
        if( short_write_size > 0 )
            _size = std::min(_size, short_write_size);
        return NativeCreateCopyIO::Write(_fd, _buffer, _size);
    }

    void Checkpoint(NativeCreateCopyCheckpoint _checkpoint) override
    {
        if( on_checkpoint )
            on_checkpoint(_checkpoint);
    }

    int pread_eintr_count{0};
    int write_eintr_count{0};
    int write_error{0};
    size_t short_write_size{0};
    std::function<void(NativeCreateCopyCheckpoint)> on_checkpoint;
};

class NativeCreateCopyMetadataObservingIO final : public NativeCreateCopyScriptedIO
{
public:
    int ReadACL(int _fd, std::vector<std::byte> &_acl) noexcept override
    {
        if( source_fd < 0 )
            source_fd = _fd;
        acl_read_source.emplace_back(_fd == source_fd);
        acl_read_after_publish.emplace_back(published);
        events.emplace_back(_fd == source_fd ? "acl-source-snapshot"
                                             : (published ? "acl-verify" : "acl-clear-verify"));
        return NativeCreateCopyIO::ReadACL(_fd, _acl);
    }

    int WriteACL(int _fd, const std::vector<std::byte> &_acl) noexcept override
    {
        acl_write_after_publish.emplace_back(published);
        events.emplace_back(published ? "acl" : "clear-acl");
        return NativeCreateCopyIO::WriteACL(_fd, _acl);
    }

    int CopyMetadata(int _source_fd,
                     int _destination_fd,
                     copyfile_flags_t _flags) noexcept override
    {
        metadata_flags.emplace_back(_flags);
        metadata_after_publish.emplace_back(published);
        events.emplace_back("xattr");
        return NativeCreateCopyIO::CopyMetadata(_source_fd, _destination_fd, _flags);
    }

    int FChmod(int _fd, mode_t _mode) noexcept override
    {
        chmod_after_publish.emplace_back(published);
        events.emplace_back("mode");
        return NativeCreateCopyIO::FChmod(_fd, _mode);
    }

    int FUTimens(int _fd, const struct timespec _times[2]) noexcept override
    {
        times_after_publish.emplace_back(published);
        events.emplace_back("times");
        return NativeCreateCopyIO::FUTimens(_fd, _times);
    }

    int FSetBirthTime(int _fd, const struct timespec &_birth_time) noexcept override
    {
        birth_time_after_publish.emplace_back(published);
        events.emplace_back("birth");
        return NativeCreateCopyIO::FSetBirthTime(_fd, _birth_time);
    }

    int FChflags(int _fd, uint32_t _flags) noexcept override
    {
        flags_after_publish.emplace_back(published);
        events.emplace_back("flags");
        return NativeCreateCopyIO::FChflags(_fd, _flags);
    }

    int RenameExclusive(int _directory_fd, const char *_from, const char *_to) noexcept override
    {
        const int result = NativeCreateCopyIO::RenameExclusive(_directory_fd, _from, _to);
        if( result == 0 )
            published = true;
        if( result == 0 )
            events.emplace_back("rename");
        return result;
    }

    bool published{false};
    int source_fd{-1};
    std::vector<copyfile_flags_t> metadata_flags;
    std::vector<bool> metadata_after_publish;
    std::vector<bool> acl_read_source;
    std::vector<bool> acl_read_after_publish;
    std::vector<bool> acl_write_after_publish;
    std::vector<bool> chmod_after_publish;
    std::vector<bool> times_after_publish;
    std::vector<bool> birth_time_after_publish;
    std::vector<bool> flags_after_publish;
    std::vector<std::string_view> events;
};

class NativeCreateCopyPrivateTempObservingIO final : public NativeCreateCopyScriptedIO
{
public:
    int OpenAt(int _directory_fd, const char *_name, int _flags, mode_t _mode) noexcept override
    {
        const int fd = NativeCreateCopyIO::OpenAt(_directory_fd, _name, _flags, _mode);
        if( fd >= 0 && std::string_view{_name}.starts_with(".wincommander-copy.") ) {
            parent_fd = _directory_fd;
            temp_fd = fd;
            temp_name = _name;
            open_flags = _flags;
            requested_mode = _mode;
        }
        return fd;
    }

    void Checkpoint(NativeCreateCopyCheckpoint _checkpoint) noexcept override
    {
        if( _checkpoint != NativeCreateCopyCheckpoint::BeforePublish || temp_name.empty() )
            return;
        struct stat stat {};
        if( fstatat(parent_fd, temp_name.c_str(), &stat, AT_SYMLINK_NOFOLLOW) == 0 ) {
            observed = true;
            observed_mode = stat.st_mode & 07777;
        }
        std::vector<std::byte> acl;
        if( temp_fd >= 0 && NativeCreateCopyIO::ReadACL(temp_fd, acl) == 0 ) {
            observed_acl = true;
            observed_acl_empty = acl.empty();
        }
    }

    int parent_fd{-1};
    int temp_fd{-1};
    std::string temp_name;
    int open_flags{0};
    mode_t requested_mode{0};
    bool observed{false};
    mode_t observed_mode{0};
    bool observed_acl{false};
    bool observed_acl_empty{false};
};

class NativeCreateCopyExceptionIO final : public NativeCreateCopyIO
{
public:
    enum class Fault : uint8_t {
        BufferBadAlloc,
        BufferUnknown,
        BufferEmpty,
        AfterTempCreatedUnknown,
        AfterPublishUnknown,
        RecoveryCheckpointUnknown
    };

    explicit NativeCreateCopyExceptionIO(Fault _fault) : fault{_fault} {}

    std::vector<std::byte> AllocateCopyBuffer(size_t _size) override
    {
        if( fault == Fault::BufferBadAlloc )
            throw std::bad_alloc{};
        if( fault == Fault::BufferUnknown )
            throw std::runtime_error{"injected buffer allocation failure"};
        if( fault == Fault::BufferEmpty )
            return {};
        return NativeCreateCopyIO::AllocateCopyBuffer(_size);
    }

    ssize_t Write(int _fd, const void *_buffer, size_t _size) noexcept override
    {
        if( fault == Fault::RecoveryCheckpointUnknown ) {
            errno = EIO;
            return -1;
        }
        return NativeCreateCopyIO::Write(_fd, _buffer, _size);
    }

    void Checkpoint(NativeCreateCopyCheckpoint _checkpoint) override
    {
        if( fault == Fault::AfterTempCreatedUnknown &&
            _checkpoint == NativeCreateCopyCheckpoint::AfterTempCreated )
            throw std::runtime_error{"injected checkpoint failure"};
        if( fault == Fault::AfterPublishUnknown &&
            _checkpoint == NativeCreateCopyCheckpoint::AfterPublish )
            throw std::runtime_error{"injected postpublish checkpoint failure"};
        if( fault == Fault::RecoveryCheckpointUnknown &&
            _checkpoint == NativeCreateCopyCheckpoint::BeforeRecoveryAbandon )
            throw std::runtime_error{"injected recovery checkpoint failure"};
    }

    Fault fault;
};

class NativeCreateCopyPostPublishMetadataFailingIO final : public NativeCreateCopyIO
{
public:
    enum class Fault : uint8_t {
        ACL,
        Mode
    };

    explicit NativeCreateCopyPostPublishMetadataFailingIO(Fault _fault) : fault{_fault} {}

    int WriteACL(int _destination_fd, const std::vector<std::byte> &_acl) noexcept override
    {
        ++write_acl_calls;
        if( write_acl_calls > 1 )
            acl_after_publish = published;
        if( fault == Fault::ACL && write_acl_calls > 1 ) {
            errno = EACCES;
            return -1;
        }
        return NativeCreateCopyIO::WriteACL(_destination_fd, _acl);
    }

    int FChmod(int _fd, mode_t _mode) noexcept override
    {
        mode_after_publish = published;
        if( fault == Fault::Mode ) {
            errno = EPERM;
            return -1;
        }
        return NativeCreateCopyIO::FChmod(_fd, _mode);
    }

    int RenameExclusive(int _directory_fd, const char *_from, const char *_to) noexcept override
    {
        const int result = NativeCreateCopyIO::RenameExclusive(_directory_fd, _from, _to);
        if( result == 0 )
            published = true;
        return result;
    }

    Fault fault;
    bool published{false};
    int write_acl_calls{0};
    bool acl_after_publish{false};
    bool mode_after_publish{false};
};

class NativeCreateCopyPostPublishFinalizationIO final : public NativeCreateCopyIO
{
public:
    enum class Fault : uint8_t {
        FlagsUnsupported,
        VerificationMismatch,
        FileSync,
        ParentSync,
        FlagsAndFileSync
    };

    explicit NativeCreateCopyPostPublishFinalizationIO(Fault _fault) : fault{_fault} {}

    int OpenAt(int _directory_fd, const char *_name, int _flags, mode_t _mode) noexcept override
    {
        const int result = NativeCreateCopyIO::OpenAt(_directory_fd, _name, _flags, _mode);
        if( result >= 0 && std::string_view{_name}.starts_with(".wincommander-copy.") ) {
            parent_fd = _directory_fd;
            file_fd = result;
        }
        return result;
    }

    int RenameExclusive(int _directory_fd, const char *_from, const char *_to) noexcept override
    {
        const int result = NativeCreateCopyIO::RenameExclusive(_directory_fd, _from, _to);
        if( result == 0 )
            published = true;
        return result;
    }

    int FChflags(int _fd, uint32_t _flags) noexcept override
    {
        flags_after_publish = published;
        if( fault == Fault::FlagsUnsupported || fault == Fault::FlagsAndFileSync ) {
            errno = ENOTSUP;
            return -1;
        }
        const int result = NativeCreateCopyIO::FChflags(_fd, _flags);
        if( result == 0 )
            flags_applied = true;
        return result;
    }

    int FStat(int _fd, struct stat *_stat) noexcept override
    {
        const int result = NativeCreateCopyIO::FStat(_fd, _stat);
        if( result == 0 && fault == Fault::VerificationMismatch && published && flags_applied &&
            _fd == file_fd )
            ++_stat->st_mtimespec.tv_nsec;
        return result;
    }

    int FSync(int _fd) noexcept override
    {
        if( published && _fd == file_fd ) {
            ++postpublish_file_sync_calls;
            if( fault == Fault::FileSync || fault == Fault::FlagsAndFileSync ) {
                errno = EIO;
                return -1;
            }
        }
        if( published && _fd == parent_fd ) {
            ++postpublish_parent_sync_calls;
            if( fault == Fault::ParentSync ) {
                errno = EIO;
                return -1;
            }
        }
        return NativeCreateCopyIO::FSync(_fd);
    }

    Fault fault;
    int parent_fd{-1};
    int file_fd{-1};
    bool published{false};
    bool flags_after_publish{false};
    bool flags_applied{false};
    int postpublish_file_sync_calls{0};
    int postpublish_parent_sync_calls{0};
};

class NativeCreateCopyBeforePublishRebindingIO final : public NativeCreateCopyIO
{
public:
    explicit NativeCreateCopyBeforePublishRebindingIO(int _parent_fd) : parent_fd{_parent_fd} {}

    int OpenAt(int _directory_fd, const char *_name, int _flags, mode_t _mode) noexcept override
    {
        const int fd = NativeCreateCopyIO::OpenAt(_directory_fd, _name, _flags, _mode);
        if( fd >= 0 && std::string_view{_name}.starts_with(".wincommander-copy.") )
            temp_name = _name;
        return fd;
    }

    void Checkpoint(NativeCreateCopyCheckpoint _checkpoint) override
    {
        if( _checkpoint != NativeCreateCopyCheckpoint::BeforePublish || temp_name.empty() )
            return;

        if( renameat(parent_fd, temp_name.c_str(), parent_fd, detached_name.c_str()) != 0 )
            return;
        const int victim_fd = openat(parent_fd,
                                     temp_name.c_str(),
                                     O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                     0600);
        if( victim_fd < 0 )
            return;
        constexpr std::string_view contents = "before-publish-victim";
        rebound = write(victim_fd, contents.data(), contents.size()) ==
                  static_cast<ssize_t>(contents.size());
        (void)close(victim_fd);
    }

    int RenameExclusive(int _directory_fd, const char *_from, const char *_to) noexcept override
    {
        ++publish_calls;
        return NativeCreateCopyIO::RenameExclusive(_directory_fd, _from, _to);
    }

    int parent_fd;
    std::string temp_name;
    std::string detached_name{"detached-before-publish-temp"};
    bool rebound{false};
    int publish_calls{0};
};

class NativeCreateCopyHardLinkingIO final : public NativeCreateCopyIO
{
public:
    enum class LinkPoint : uint8_t {
        BeforePublish,
        AfterPublish
    };

    explicit NativeCreateCopyHardLinkingIO(LinkPoint _link_point, std::string _alias_name)
        : link_point{_link_point}, alias_name{std::move(_alias_name)}
    {
    }

    int OpenAt(int _directory_fd, const char *_name, int _flags, mode_t _mode) noexcept override
    {
        const int fd = NativeCreateCopyIO::OpenAt(_directory_fd, _name, _flags, _mode);
        if( fd >= 0 && std::string_view{_name}.starts_with(".wincommander-copy.") ) {
            parent_fd = _directory_fd;
            temp_name = _name;
        }
        return fd;
    }

    int RenameExclusive(int _directory_fd, const char *_from, const char *_to) noexcept override
    {
        ++publish_calls;
        destination_name = _to;
        return NativeCreateCopyIO::RenameExclusive(_directory_fd, _from, _to);
    }

    void Checkpoint(NativeCreateCopyCheckpoint _checkpoint) override
    {
        const char *source_name = nullptr;
        if( link_point == LinkPoint::BeforePublish &&
            _checkpoint == NativeCreateCopyCheckpoint::BeforePublish )
            source_name = temp_name.c_str();
        if( link_point == LinkPoint::AfterPublish &&
            _checkpoint == NativeCreateCopyCheckpoint::AfterPublish )
            source_name = destination_name.c_str();
        if( source_name != nullptr && parent_fd >= 0 && !alias_name.empty() )
            linked = linkat(parent_fd, source_name, parent_fd, alias_name.c_str(), 0) == 0;
    }

    LinkPoint link_point;
    std::string alias_name;
    int parent_fd{-1};
    std::string temp_name;
    std::string destination_name;
    bool linked{false};
    int publish_calls{0};
};

class NativeCreateCopyStopBoundaryIO final : public NativeCreateCopyScriptedIO
{
public:
    enum class StopPoint : uint8_t {
        FinalSourceFStat,
        BeforePublish
    };

    explicit NativeCreateCopyStopBoundaryIO(StopPoint _stop_point) : stop_point{_stop_point} {}

    int FStat(int _fd, struct stat *_stat) noexcept override
    {
        const int result = NativeCreateCopyIO::FStat(_fd, _stat);
        if( result < 0 )
            return result;
        if( source_fd < 0 )
            source_fd = _fd;
        if( _fd == source_fd && ++source_fstat_calls == 2 &&
            stop_point == StopPoint::FinalSourceFStat && stop )
            stop();
        return result;
    }

    int RenameExclusive(int _directory_fd, const char *_from, const char *_to) noexcept override
    {
        ++publish_calls;
        return NativeCreateCopyIO::RenameExclusive(_directory_fd, _from, _to);
    }

    void Checkpoint(NativeCreateCopyCheckpoint _checkpoint) override
    {
        if( _checkpoint == NativeCreateCopyCheckpoint::BeforePublish &&
            stop_point == StopPoint::BeforePublish && stop )
            stop();
    }

    StopPoint stop_point;
    int source_fd{-1};
    int source_fstat_calls{0};
    int publish_calls{0};
    std::function<void()> stop;
};

class NativeCreateCopyRebindingIO final : public NativeCreateCopyScriptedIO
{
public:
    explicit NativeCreateCopyRebindingIO(int _parent_fd) : parent_fd{_parent_fd}
    {
    }

    int OpenAt(int _directory_fd, const char *_name, int _flags, mode_t _mode) noexcept override
    {
        const int fd = NativeCreateCopyIO::OpenAt(_directory_fd, _name, _flags, _mode);
        if( fd >= 0 && std::string_view{_name}.starts_with(".wincommander-copy.") )
            temp_name = _name;
        return fd;
    }

    void Checkpoint(NativeCreateCopyCheckpoint _checkpoint) override
    {
        if( _checkpoint == NativeCreateCopyCheckpoint::AfterTempCreated ) {
            if( cancel )
                cancel();
            return;
        }
        if( _checkpoint != NativeCreateCopyCheckpoint::BeforeRecoveryAbandon || temp_name.empty() )
            return;

        (void)renameat(parent_fd, temp_name.c_str(), parent_fd, "detached-original-temp");
        const int victim = openat(parent_fd, temp_name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if( victim >= 0 ) {
            constexpr std::string_view contents = "rebound-victim";
            (void)write(victim, contents.data(), contents.size());
            (void)close(victim);
        }
    }

    int parent_fd;
    std::string temp_name;
    std::function<void()> cancel;
};

class NativeCreateCopyMutationEINTRO final : public NativeCreateCopyIO
{
public:
    enum class Fault : uint8_t {
        Open,
        RenameBeforeCommit,
        RenameAfterCommit
    };

    explicit NativeCreateCopyMutationEINTRO(Fault _fault) : fault{_fault} {}

    int OpenAt(int _directory_fd, const char *_name, int _flags, mode_t _mode) noexcept override
    {
        if( fault == Fault::Open && std::string_view{_name}.starts_with(".wincommander-copy.") ) {
            ++mutation_calls;
            errno = EINTR;
            return -1;
        }
        return NativeCreateCopyIO::OpenAt(_directory_fd, _name, _flags, _mode);
    }

    int RenameExclusive(int _directory_fd, const char *_from, const char *_to) noexcept override
    {
        if( fault == Fault::RenameBeforeCommit ) {
            ++mutation_calls;
            errno = EINTR;
            return -1;
        }
        const int result = NativeCreateCopyIO::RenameExclusive(_directory_fd, _from, _to);
        if( fault == Fault::RenameAfterCommit && result == 0 ) {
            ++mutation_calls;
            errno = EINTR;
            return -1;
        }
        return result;
    }

    Fault fault;
    int mutation_calls{0};
};

} // namespace

TEST_CASE(PREFIX "maps typed outcomes to journal item results",
          "[native-create-copy][native-create-copy-mapper]")
{
    SECTION("success and cancellation")
    {
        const auto success = MapNativeCreateCopyOutcomeToJournalItemResult(
            NativeCreateCopyOutcome{.code = NativeCreateCopyOutcomeCode::Success,
                                    .prior_code = NativeCreateCopyOutcomeCode::Pending,
                                    .error_number = 0,
                                    .bytes_copied = 123,
                                    .destination_publication = NativeCreateCopyPublicationState::Published,
                                    .recovery_artifact_left = false,
                                    .filesystem_sync_confirmed = true},
            7);
        REQUIRE(success);
        CHECK((*success == OperationJournalItemResult{.item_index = 7,
                                                      .status = OperationJournalItemStatus::Succeeded,
                                                      .error = OperationJournalItemError::None,
                                                      .system_error = 0,
                                                      .prior_error = OperationJournalItemError::None,
                                                      .prior_system_error = 0,
                                                      .bytes = 123,
                                                      .destination_publication =
                                                          OperationJournalPublicationState::Published,
                                                      .filesystem_sync_status =
                                                          OperationJournalFilesystemSyncStatus::Confirmed,
                                                      .filesystem_sync_system_error = 0,
                                                      .recovery_action = OperationJournalRecoveryAction::None}));

        const auto cancelled = MapNativeCreateCopyOutcomeToJournalItemResult(
            NativeCreateCopyOutcome{.code = NativeCreateCopyOutcomeCode::Cancelled,
                                    .prior_code = NativeCreateCopyOutcomeCode::Pending,
                                    .error_number = 0,
                                    .bytes_copied = 45,
                                    .destination_publication = NativeCreateCopyPublicationState::NotPublished,
                                    .recovery_artifact_left = false},
            8);
        REQUIRE(cancelled);
        CHECK((*cancelled == OperationJournalItemResult{.item_index = 8,
                                                        .status = OperationJournalItemStatus::Cancelled,
                                                        .error = OperationJournalItemError::Cancelled,
                                                        .system_error = 0,
                                                        .prior_error = OperationJournalItemError::None,
                                                        .prior_system_error = 0,
                                                        .bytes = 45,
                                                        .destination_publication =
                                                            OperationJournalPublicationState::NotPublished,
                                                        .filesystem_sync_status =
                                                            OperationJournalFilesystemSyncStatus::NotAttempted,
                                                        .filesystem_sync_system_error = 0,
                                                        .recovery_action = OperationJournalRecoveryAction::None}));
    }

    SECTION("failures preserve publication and apply recovery precedence")
    {
        struct FailureCase {
            NativeCreateCopyOutcomeCode code;
            OperationJournalItemError unpublished_error;
            OperationJournalItemError published_error;
        };
        const std::array cases{
            FailureCase{NativeCreateCopyOutcomeCode::StaleSource,
                        OperationJournalItemError::SourceChanged,
                        OperationJournalItemError::SourceChanged},
            FailureCase{NativeCreateCopyOutcomeCode::StaleDestination,
                        OperationJournalItemError::DestinationChanged,
                        OperationJournalItemError::DestinationChanged},
            FailureCase{NativeCreateCopyOutcomeCode::ReadFailed,
                        OperationJournalItemError::Read,
                        OperationJournalItemError::Read},
            FailureCase{NativeCreateCopyOutcomeCode::WriteFailed,
                        OperationJournalItemError::Write,
                        OperationJournalItemError::Write},
            FailureCase{NativeCreateCopyOutcomeCode::SyncFailed,
                        OperationJournalItemError::Write,
                        OperationJournalItemError::Commit},
            FailureCase{NativeCreateCopyOutcomeCode::MetadataFailed,
                        OperationJournalItemError::Metadata,
                        OperationJournalItemError::Metadata},
            FailureCase{NativeCreateCopyOutcomeCode::MetadataUnsupported,
                        OperationJournalItemError::Metadata,
                        OperationJournalItemError::Metadata},
            FailureCase{NativeCreateCopyOutcomeCode::MetadataPermissionDenied,
                        OperationJournalItemError::PermissionDenied,
                        OperationJournalItemError::PermissionDenied},
            FailureCase{NativeCreateCopyOutcomeCode::MetadataVerificationFailed,
                        OperationJournalItemError::Metadata,
                        OperationJournalItemError::Metadata},
            FailureCase{NativeCreateCopyOutcomeCode::CommitFailed,
                        OperationJournalItemError::Commit,
                        OperationJournalItemError::Commit},
        };

        size_t item_index = 10;
        for( const auto &test : cases ) {
            for( const bool published : {false, true} ) {
                CAPTURE(test.code, published);
                const uint64_t bytes = 1'000 + item_index;
                const auto result = MapNativeCreateCopyOutcomeToJournalItemResult(
                    NativeCreateCopyOutcome{.code = test.code,
                                            .prior_code = NativeCreateCopyOutcomeCode::Pending,
                                            .error_number = EIO,
                                            .bytes_copied = bytes,
                                            .destination_publication =
                                                published ? NativeCreateCopyPublicationState::Published
                                                          : NativeCreateCopyPublicationState::NotPublished,
                                            .recovery_artifact_left = false,
                                            .filesystem_sync_confirmed = published},
                    item_index);
                REQUIRE(result);
                CHECK(result->item_index == item_index);
                CHECK(result->status == OperationJournalItemStatus::Failed);
                CHECK(result->error == (published ? test.published_error : test.unpublished_error));
                CHECK(result->system_error == EIO);
                CHECK(result->prior_error == OperationJournalItemError::None);
                CHECK(result->prior_system_error == 0);
                CHECK(result->bytes == bytes);
                CHECK(result->destination_publication ==
                      (published ? OperationJournalPublicationState::Published
                                 : OperationJournalPublicationState::NotPublished));
                CHECK(result->filesystem_sync_status ==
                      (published ? OperationJournalFilesystemSyncStatus::Confirmed
                                 : OperationJournalFilesystemSyncStatus::NotAttempted));
                CHECK(result->filesystem_sync_system_error == 0);
                CHECK(result->recovery_action == (published ? OperationJournalRecoveryAction::InspectDestination
                                                            : OperationJournalRecoveryAction::Retry));
                ++item_index;
            }
        }
    }

    SECTION("cleanup failure records no unauthorised temporary-removal action")
    {
        struct CleanupCase {
            NativeCreateCopyOutcomeCode code;
            OperationJournalItemError error;
            int system_error;
        };
        const std::array cases{
            CleanupCase{NativeCreateCopyOutcomeCode::Cancelled, OperationJournalItemError::Cancelled, 0},
            CleanupCase{NativeCreateCopyOutcomeCode::StaleSource, OperationJournalItemError::SourceChanged, EIO},
            CleanupCase{NativeCreateCopyOutcomeCode::StaleDestination,
                        OperationJournalItemError::DestinationChanged,
                        EIO},
            CleanupCase{NativeCreateCopyOutcomeCode::ReadFailed, OperationJournalItemError::Read, EIO},
            CleanupCase{NativeCreateCopyOutcomeCode::WriteFailed, OperationJournalItemError::Write, EIO},
            CleanupCase{NativeCreateCopyOutcomeCode::SyncFailed, OperationJournalItemError::Write, EIO},
            CleanupCase{NativeCreateCopyOutcomeCode::MetadataFailed, OperationJournalItemError::Metadata, EIO},
            CleanupCase{NativeCreateCopyOutcomeCode::MetadataUnsupported,
                        OperationJournalItemError::Metadata,
                        EIO},
            CleanupCase{NativeCreateCopyOutcomeCode::MetadataPermissionDenied,
                        OperationJournalItemError::PermissionDenied,
                        EIO},
            CleanupCase{NativeCreateCopyOutcomeCode::MetadataVerificationFailed,
                        OperationJournalItemError::Metadata,
                        EIO},
            CleanupCase{NativeCreateCopyOutcomeCode::CommitFailed, OperationJournalItemError::Commit, EIO},
        };
        size_t item_index = 40;
        for( const auto &test : cases ) {
            CAPTURE(test.code);
            const uint64_t bytes = 2'000 + item_index;
            const auto result = MapNativeCreateCopyOutcomeToJournalItemResult(
                NativeCreateCopyOutcome{.code = NativeCreateCopyOutcomeCode::CleanupFailed,
                                        .prior_code = test.code,
                                        .error_number = test.system_error,
                                        .bytes_copied = bytes,
                                        .destination_publication =
                                            NativeCreateCopyPublicationState::NotPublished,
                                        .recovery_artifact_left = true},
                item_index);
            REQUIRE(result);
            CHECK((*result == OperationJournalItemResult{
                                  .item_index = item_index,
                                  .status = OperationJournalItemStatus::Failed,
                                  .error = OperationJournalItemError::Cleanup,
                                  .system_error = 0,
                                  .prior_error = test.error,
                                  .prior_system_error = test.system_error,
                                  .bytes = bytes,
                                  .destination_publication =
                                      OperationJournalPublicationState::NotPublished,
                                  .filesystem_sync_status =
                                      OperationJournalFilesystemSyncStatus::NotAttempted,
                                  .filesystem_sync_system_error = 0,
                                  .recovery_action = OperationJournalRecoveryAction::None}));
            ++item_index;
        }
    }

    SECTION("filesystem sync failure preserves published uncertainty")
    {
        const auto result = MapNativeCreateCopyOutcomeToJournalItemResult(
            NativeCreateCopyOutcome{.code = NativeCreateCopyOutcomeCode::FileSystemSyncFailed,
                                    .prior_code = NativeCreateCopyOutcomeCode::Pending,
                                    .error_number = EIO,
                                    .bytes_copied = 91,
                                    .destination_publication = NativeCreateCopyPublicationState::Published,
                                    .recovery_artifact_left = false,
                                    .filesystem_sync_error_number = EIO,
                                    .filesystem_sync_confirmed = false},
            4);
        REQUIRE(result);
        CHECK((*result == OperationJournalItemResult{.item_index = 4,
                                                      .status = OperationJournalItemStatus::Failed,
                                                      .error = OperationJournalItemError::Commit,
                                                      .system_error = EIO,
                                                      .prior_error = OperationJournalItemError::None,
                                                      .prior_system_error = 0,
                                                      .bytes = 91,
                                                      .destination_publication =
                                                          OperationJournalPublicationState::Published,
                                                      .filesystem_sync_status =
                                                          OperationJournalFilesystemSyncStatus::Failed,
                                                      .filesystem_sync_system_error = EIO,
                                                          .recovery_action =
                                                              OperationJournalRecoveryAction::InspectDestination}));
    }

    SECTION("unknown publication remains conservative and requires destination inspection")
    {
        const auto result = MapNativeCreateCopyOutcomeToJournalItemResult(
            NativeCreateCopyOutcome{.code = NativeCreateCopyOutcomeCode::CommitFailed,
                                    .prior_code = NativeCreateCopyOutcomeCode::Pending,
                                    .error_number = EIO,
                                    .bytes_copied = 77,
                                    .destination_publication = NativeCreateCopyPublicationState::Unknown,
                                    .recovery_artifact_left = false,
                                    .filesystem_sync_error_number = 0,
                                    .filesystem_sync_confirmed = false},
            5);
        REQUIRE(result);
        CHECK((*result == OperationJournalItemResult{
                              .item_index = 5,
                              .status = OperationJournalItemStatus::Failed,
                              .error = OperationJournalItemError::Commit,
                              .system_error = EIO,
                              .prior_error = OperationJournalItemError::None,
                              .prior_system_error = 0,
                              .bytes = 77,
                              .destination_publication = OperationJournalPublicationState::Unknown,
                              .filesystem_sync_status = OperationJournalFilesystemSyncStatus::NotAttempted,
                              .filesystem_sync_system_error = 0,
                              .recovery_action = OperationJournalRecoveryAction::InspectDestination}));
    }
}

TEST_CASE(PREFIX "rejects pending and internally inconsistent outcome mappings",
          "[native-create-copy][native-create-copy-mapper]")
{
    const auto pending = MapNativeCreateCopyOutcomeToJournalItemResult(NativeCreateCopyOutcome{}, 0);
    REQUIRE_FALSE(pending);
    CHECK(pending.error() == NativeCreateCopyJournalMappingError::PendingOutcome);

    const auto valid_success = NativeCreateCopyOutcome{.code = NativeCreateCopyOutcomeCode::Success,
                                                       .prior_code = NativeCreateCopyOutcomeCode::Pending,
                                                       .error_number = 0,
                                                       .bytes_copied = 1,
                                                       .destination_publication =
                                                           NativeCreateCopyPublicationState::Published,
                                                       .recovery_artifact_left = false,
                                                       .filesystem_sync_confirmed = true};
    const auto valid_cancelled = NativeCreateCopyOutcome{.code = NativeCreateCopyOutcomeCode::Cancelled,
                                                         .prior_code = NativeCreateCopyOutcomeCode::Pending,
                                                         .error_number = 0,
                                                         .bytes_copied = 1,
                                                         .destination_publication =
                                                             NativeCreateCopyPublicationState::NotPublished,
                                                         .recovery_artifact_left = false};
    const auto valid_failure = NativeCreateCopyOutcome{.code = NativeCreateCopyOutcomeCode::WriteFailed,
                                                       .prior_code = NativeCreateCopyOutcomeCode::Pending,
                                                       .error_number = EIO,
                                                       .bytes_copied = 1,
                                                       .destination_publication =
                                                           NativeCreateCopyPublicationState::NotPublished,
                                                       .recovery_artifact_left = false};
    const auto valid_cleanup = NativeCreateCopyOutcome{.code = NativeCreateCopyOutcomeCode::CleanupFailed,
                                                       .prior_code = NativeCreateCopyOutcomeCode::WriteFailed,
                                                       .error_number = EIO,
                                                       .bytes_copied = 1,
                                                       .destination_publication =
                                                           NativeCreateCopyPublicationState::NotPublished,
                                                       .recovery_artifact_left = true};

    struct InvalidCase {
        std::string_view name;
        NativeCreateCopyOutcome outcome;
    };
    std::vector<InvalidCase> cases;
    const auto add = [&](std::string_view _name, NativeCreateCopyOutcome _outcome) {
        cases.emplace_back(InvalidCase{_name, _outcome});
    };
    {
        auto value = valid_success;
        value.destination_publication = NativeCreateCopyPublicationState::NotPublished;
        add("unpublished success", value);
    }
    {
        auto value = valid_success;
        value.error_number = EIO;
        add("success with error", value);
    }
    {
        auto value = valid_success;
        value.filesystem_sync_confirmed = false;
        add("success without filesystem sync", value);
    }
    {
        auto value = valid_success;
        value.filesystem_sync_error_number = EIO;
        add("success with filesystem sync error", value);
    }
    {
        auto value = valid_success;
        value.recovery_artifact_left = true;
        add("success with artifact", value);
    }
    {
        auto value = valid_success;
        value.prior_code = NativeCreateCopyOutcomeCode::WriteFailed;
        add("success with prior failure", value);
    }
    {
        auto value = valid_cancelled;
        value.destination_publication = NativeCreateCopyPublicationState::Published;
        add("published cancellation", value);
    }
    {
        auto value = valid_cancelled;
        value.error_number = ECANCELED;
        add("cancellation with system error", value);
    }
    {
        auto value = valid_failure;
        value.error_number = 0;
        add("failure without error", value);
    }
    {
        auto value = valid_failure;
        value.filesystem_sync_confirmed = true;
        add("unpublished failure with filesystem sync", value);
    }
    {
        auto value = valid_failure;
        value.filesystem_sync_error_number = EIO;
        add("unpublished failure with filesystem sync error", value);
    }
    {
        auto value = valid_failure;
        value.code = NativeCreateCopyOutcomeCode::FileSystemSyncFailed;
        add("unpublished filesystem sync failure", value);
    }
    {
        auto value = valid_failure;
        value.code = NativeCreateCopyOutcomeCode::FileSystemSyncFailed;
        value.destination_publication = NativeCreateCopyPublicationState::Published;
        value.filesystem_sync_error_number = EPERM;
        add("filesystem sync failure with mismatched evidence", value);
    }
    {
        auto value = valid_failure;
        value.destination_publication = NativeCreateCopyPublicationState::Unknown;
        add("unknown publication for non-commit failure", value);
    }
    {
        auto value = valid_failure;
        value.code = NativeCreateCopyOutcomeCode::CommitFailed;
        value.destination_publication = NativeCreateCopyPublicationState::Unknown;
        value.filesystem_sync_confirmed = true;
        add("unknown publication with confirmed filesystem sync", value);
    }
    {
        auto value = valid_failure;
        value.code = NativeCreateCopyOutcomeCode::CommitFailed;
        value.destination_publication = NativeCreateCopyPublicationState::Unknown;
        value.recovery_artifact_left = true;
        add("unknown publication with cleanup artifact", value);
    }
    {
        auto value = valid_failure;
        value.recovery_artifact_left = true;
        add("ordinary failure with artifact", value);
    }
    {
        auto value = valid_failure;
        value.prior_code = NativeCreateCopyOutcomeCode::ReadFailed;
        add("ordinary failure with prior failure", value);
    }
    {
        auto value = valid_cleanup;
        value.recovery_artifact_left = false;
        add("cleanup without artifact", value);
    }
    {
        auto value = valid_cleanup;
        value.destination_publication = NativeCreateCopyPublicationState::Published;
        add("published cleanup artifact", value);
    }
    {
        auto value = valid_cleanup;
        value.prior_code = NativeCreateCopyOutcomeCode::Pending;
        add("cleanup without prior result", value);
    }
    {
        auto value = valid_cleanup;
        value.prior_code = NativeCreateCopyOutcomeCode::Success;
        add("cleanup after success", value);
    }
    {
        auto value = valid_cleanup;
        value.prior_code = NativeCreateCopyOutcomeCode::CleanupFailed;
        add("recursive cleanup", value);
    }
    {
        auto value = valid_cleanup;
        value.prior_code = NativeCreateCopyOutcomeCode::FileSystemSyncFailed;
        add("cleanup after published filesystem sync failure", value);
    }
    {
        auto value = valid_cleanup;
        value.prior_code = NativeCreateCopyOutcomeCode::Cancelled;
        value.error_number = EIO;
        add("cleanup after cancellation with error", value);
    }
    {
        auto value = valid_cleanup;
        value.error_number = 0;
        add("cleanup after failure without error", value);
    }
    {
        auto value = valid_failure;
        value.code = static_cast<NativeCreateCopyOutcomeCode>(255);
        add("unknown outcome code", value);
    }
    {
        auto value = valid_failure;
        value.destination_publication = static_cast<NativeCreateCopyPublicationState>(255);
        add("unknown publication enum", value);
    }

    for( const auto &test : cases ) {
        DYNAMIC_SECTION(test.name)
        {
            const auto result = MapNativeCreateCopyOutcomeToJournalItemResult(test.outcome, 0);
            REQUIRE_FALSE(result);
            CHECK(result.error() == NativeCreateCopyJournalMappingError::InconsistentOutcome);
        }
    }
}

TEST_CASE(PREFIX "publishes an absent destination with data and native metadata", "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination = temporary.directory / "destination.txt";
    const std::string contents = "native create-only copy\nwith metadata";
    NativeCreateCopyWriteFile(source, contents);
    REQUIRE(chmod(source.c_str(), 0640) == 0);

    constexpr std::string_view xattr_name = "com.wincommander.native-create-copy-test";
    constexpr std::string_view xattr_value = "metadata-value";
    REQUIRE(setxattr(source.c_str(),
                     xattr_name.data(),
                     xattr_value.data(),
                     xattr_value.size(),
                     0,
                     0) == 0);

    const struct timespec source_times[2]{{.tv_sec = 1'700'000'001, .tv_nsec = 123'456'789},
                                          {.tv_sec = 1'700'000'002, .tv_nsec = 987'654'321}};
    REQUIRE(utimensat(AT_FDCWD, source.c_str(), source_times, 0) == 0);
    struct stat source_metadata {};
    REQUIRE(stat(source.c_str(), &source_metadata) == 0);

    NativeCreateCopy operation{
        NativeCreateCopyMakeCapsule(source, temporary.directory, destination.filename().native())};
    operation.Start();
    operation.Wait();

    CHECK(operation.State() == OperationState::Completed);
    CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::Success);
    CHECK(operation.Outcome().destination_publication == NativeCreateCopyPublicationState::Published);
    CHECK_FALSE(operation.Outcome().recovery_artifact_left);
    CHECK(operation.Outcome().filesystem_sync_error_number == 0);
    CHECK(operation.Outcome().filesystem_sync_confirmed);
    CHECK(operation.Outcome().bytes_copied == contents.size());
    CHECK(operation.Statistics().VolumeTotal(Statistics::SourceType::Bytes) == contents.size());
    CHECK(operation.Statistics().VolumeProcessed(Statistics::SourceType::Bytes) == contents.size());
    CHECK(NativeCreateCopyTempFiles(temporary.directory).empty());

    struct stat destination_stat {};
    REQUIRE(stat(destination.c_str(), &destination_stat) == 0);
    CHECK((destination_stat.st_mode & 07777) == 0640);
    CHECK(destination_stat.st_atimespec.tv_sec == source_metadata.st_atimespec.tv_sec);
    CHECK(destination_stat.st_atimespec.tv_nsec == source_metadata.st_atimespec.tv_nsec);
    CHECK(destination_stat.st_mtimespec.tv_sec == source_metadata.st_mtimespec.tv_sec);
    CHECK(destination_stat.st_mtimespec.tv_nsec == source_metadata.st_mtimespec.tv_nsec);
    CHECK(destination_stat.st_birthtimespec.tv_sec == source_metadata.st_birthtimespec.tv_sec);
    CHECK(destination_stat.st_birthtimespec.tv_nsec == source_metadata.st_birthtimespec.tv_nsec);
    CHECK(destination_stat.st_flags == source_metadata.st_flags);
    CHECK(g_NativeCreateCopyDurabilityPolicy == NativeCreateCopyDurabilityPolicy::FileSystemSyncOnly);
    CHECK(NativeCreateCopyReadFile(destination) == contents);

    std::array<char, 64> copied_xattr{};
    const auto copied_xattr_size = getxattr(destination.c_str(),
                                            xattr_name.data(),
                                            copied_xattr.data(),
                                            copied_xattr.size(),
                                            0,
                                            0);
    REQUIRE(copied_xattr_size == static_cast<ssize_t>(xattr_value.size()));
    CHECK(std::string_view{copied_xattr.data(), static_cast<size_t>(copied_xattr_size)} == xattr_value);
}

TEST_CASE(PREFIX "snapshots metadata and applies BSD flags only after publication", "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "immutable-source.txt";
    const auto destination = temporary.directory / "immutable-destination.txt";
    NativeCreateCopyWriteFile(source, "immutable-source-data");

    const bool immutable_supported = chflags(source.c_str(), UF_IMMUTABLE) == 0;
    NativeCreateCopySourceFlagReset source_flag_reset{source, immutable_supported};
    NativeCreateCopySourceFlagReset destination_flag_reset{destination, immutable_supported};

    auto io = std::make_shared<NativeCreateCopyMetadataObservingIO>();
    NativeCreateCopy operation{
        NativeCreateCopyMakeCapsule(source, temporary.directory, destination.filename().native()), io};
    operation.Start();
    operation.Wait();

    REQUIRE(io->metadata_flags.size() == 1);
    CHECK(io->metadata_flags[0] == COPYFILE_XATTR);
    CHECK((io->metadata_flags[0] & COPYFILE_STAT) == 0);
    REQUIRE(io->metadata_after_publish.size() == 1);
    CHECK_FALSE(io->metadata_after_publish[0]);
    REQUIRE(io->acl_read_source.size() == 3);
    CHECK(io->acl_read_source == std::vector{true, false, false});
    CHECK(io->acl_read_after_publish == std::vector{false, false, true});
    REQUIRE(io->acl_write_after_publish.size() == 2);
    CHECK_FALSE(io->acl_write_after_publish[0]);
    CHECK(io->acl_write_after_publish[1]);
    REQUIRE(io->chmod_after_publish.size() == 1);
    CHECK(io->chmod_after_publish[0]);
    REQUIRE(io->times_after_publish.size() == 1);
    CHECK(io->times_after_publish[0]);
    REQUIRE(io->birth_time_after_publish.size() == 1);
    CHECK(io->birth_time_after_publish[0]);
    REQUIRE(io->flags_after_publish.size() == 1);
    CHECK(io->flags_after_publish[0]);
    CHECK(io->events == std::vector<std::string_view>{"acl-source-snapshot",
                                                       "clear-acl",
                                                       "acl-clear-verify",
                                                       "xattr",
                                                       "rename",
                                                       "acl",
                                                       "mode",
                                                       "times",
                                                       "birth",
                                                       "flags",
                                                       "acl-verify"});
    CHECK(operation.State() == OperationState::Completed);
    CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::Success);
    CHECK(NativeCreateCopyReadFile(destination) == "immutable-source-data");

    struct stat destination_stat {};
    REQUIRE(stat(destination.c_str(), &destination_stat) == 0);
    CHECK((destination_stat.st_flags & UF_IMMUTABLE) ==
          (immutable_supported ? UF_IMMUTABLE : 0));
    if( immutable_supported ) {
        struct stat source_stat {};
        REQUIRE(stat(source.c_str(), &source_stat) == 0);
        CHECK((source_stat.st_flags & UF_IMMUTABLE) != 0);
    }
}

TEST_CASE(PREFIX "keeps the temporary file private until exclusive publish", "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "private-source.txt";
    const auto destination = temporary.directory / "private-destination.txt";
    NativeCreateCopyWriteFile(source, "private-temp-data");
    REQUIRE(chmod(source.c_str(), 0640) == 0);

    auto io = std::make_shared<NativeCreateCopyPrivateTempObservingIO>();
    NativeCreateCopy operation{
        NativeCreateCopyMakeCapsule(source, temporary.directory, destination.filename().native()), io};
    operation.Start();
    operation.Wait();

    REQUIRE(io->observed);
    CHECK(io->observed_mode == 0600);
    REQUIRE(io->observed_acl);
    CHECK(io->observed_acl_empty);
    CHECK(io->requested_mode == 0600);
    CHECK((io->open_flags & O_EXCL) != 0);
    CHECK((io->open_flags & O_NOFOLLOW) != 0);
    CHECK((io->open_flags & O_CLOEXEC) != 0);
    constexpr std::string_view temp_prefix = ".wincommander-copy.";
    constexpr std::string_view temp_suffix = ".tmp";
    const std::string_view temp_name{io->temp_name};
    REQUIRE(temp_name.starts_with(temp_prefix));
    REQUIRE(temp_name.ends_with(temp_suffix));
    const auto random_hex = temp_name.substr(temp_prefix.size(),
                                             temp_name.size() - temp_prefix.size() - temp_suffix.size());
    CHECK(random_hex.size() == 32);
    CHECK(random_hex.find_first_not_of("0123456789abcdef") == std::string_view::npos);
    CHECK(operation.State() == OperationState::Completed);
    CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::Success);

    struct stat destination_stat {};
    REQUIRE(stat(destination.c_str(), &destination_stat) == 0);
    CHECK((destination_stat.st_mode & 07777) == 0640);
}

TEST_CASE(PREFIX "converts C++ exceptions into terminal typed outcomes", "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    NativeCreateCopyWriteFile(source, "exception-boundary-data");

    const auto run_before_temp = [&](NativeCreateCopyExceptionIO::Fault _fault,
                                     int _expected_error,
                                     std::string _destination_name) {
        const auto destination = temporary.directory / _destination_name;
        auto io = std::make_shared<NativeCreateCopyExceptionIO>(_fault);
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, std::move(_destination_name)), io};
        operation.Start();
        operation.Wait();

        CHECK(operation.State() == OperationState::Stopped);
        CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::WriteFailed);
        CHECK(operation.Outcome().error_number == _expected_error);
        CHECK(operation.Outcome().destination_publication == NativeCreateCopyPublicationState::NotPublished);
        CHECK_FALSE(operation.Outcome().recovery_artifact_left);
        CHECK_FALSE(std::filesystem::exists(destination));
        CHECK(NativeCreateCopyTempFiles(temporary.directory).empty());
    };

    SECTION("bad_alloc before temp creation")
    {
        run_before_temp(NativeCreateCopyExceptionIO::Fault::BufferBadAlloc, ENOMEM, "bad-alloc.txt");
    }
    SECTION("unknown allocation exception before temp creation")
    {
        run_before_temp(NativeCreateCopyExceptionIO::Fault::BufferUnknown, EIO, "unknown-alloc.txt");
    }
    SECTION("empty injected buffer before temp creation")
    {
        run_before_temp(NativeCreateCopyExceptionIO::Fault::BufferEmpty, EIO, "empty-buffer.txt");
    }
    SECTION("unknown exception after temp creation")
    {
        auto io = std::make_shared<NativeCreateCopyExceptionIO>(
            NativeCreateCopyExceptionIO::Fault::AfterTempCreatedUnknown);
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, "after-temp.txt"), io};
        operation.Start();
        operation.Wait();

        CHECK(operation.State() == OperationState::Stopped);
        CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::CleanupFailed);
        CHECK(operation.Outcome().prior_code == NativeCreateCopyOutcomeCode::WriteFailed);
        CHECK(operation.Outcome().error_number == EIO);
        CHECK(operation.Outcome().destination_publication == NativeCreateCopyPublicationState::NotPublished);
        CHECK(operation.Outcome().recovery_artifact_left);
        CHECK_FALSE(std::filesystem::exists(temporary.directory / "after-temp.txt"));
        NativeCreateCopyRemoveRecoveryArtifacts(temporary.directory);
    }
    SECTION("unknown exception after publish")
    {
        auto io = std::make_shared<NativeCreateCopyExceptionIO>(
            NativeCreateCopyExceptionIO::Fault::AfterPublishUnknown);
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, "after-publish.txt"), io};
        operation.Start();
        operation.Wait();

        CHECK(operation.State() == OperationState::Stopped);
        CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::WriteFailed);
        CHECK(operation.Outcome().error_number == EIO);
        CHECK(operation.Outcome().destination_publication == NativeCreateCopyPublicationState::Published);
        CHECK_FALSE(operation.Outcome().recovery_artifact_left);
        CHECK(NativeCreateCopyReadFile(temporary.directory / "after-publish.txt") ==
              "exception-boundary-data");
    }
    SECTION("recovery checkpoint exception cannot escape noexcept cleanup")
    {
        auto io = std::make_shared<NativeCreateCopyExceptionIO>(
            NativeCreateCopyExceptionIO::Fault::RecoveryCheckpointUnknown);
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, "recovery-checkpoint.txt"), io};
        operation.Start();
        operation.Wait();

        CHECK(operation.State() == OperationState::Stopped);
        CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::CleanupFailed);
        CHECK(operation.Outcome().prior_code == NativeCreateCopyOutcomeCode::WriteFailed);
        CHECK(operation.Outcome().error_number == EIO);
        CHECK(operation.Outcome().recovery_artifact_left);
        NativeCreateCopyRemoveRecoveryArtifacts(temporary.directory);
    }
}

TEST_CASE(PREFIX "reports postpublish ACL and mode failures without rolling back data",
          "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    NativeCreateCopyWriteFile(source, "published-before-metadata");
    REQUIRE(chmod(source.c_str(), 0640) == 0);

    const auto run = [&](NativeCreateCopyPostPublishMetadataFailingIO::Fault _fault,
                         int _expected_error,
                         std::string _destination_name) {
        const auto destination = temporary.directory / _destination_name;
        auto io = std::make_shared<NativeCreateCopyPostPublishMetadataFailingIO>(_fault);
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, std::move(_destination_name)), io};
        operation.Start();
        operation.Wait();

        CHECK(io->published);
        CHECK(io->acl_after_publish);
        if( _fault == NativeCreateCopyPostPublishMetadataFailingIO::Fault::Mode )
            CHECK(io->mode_after_publish);
        else
            CHECK_FALSE(io->mode_after_publish);
        CHECK(operation.State() == OperationState::Stopped);
        CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::MetadataPermissionDenied);
        CHECK(operation.Outcome().prior_code == NativeCreateCopyOutcomeCode::Pending);
        CHECK(operation.Outcome().error_number == _expected_error);
        CHECK(operation.Outcome().destination_publication == NativeCreateCopyPublicationState::Published);
        CHECK_FALSE(operation.Outcome().recovery_artifact_left);
        CHECK(operation.Outcome().filesystem_sync_error_number == 0);
        CHECK(operation.Outcome().filesystem_sync_confirmed);
        CHECK(NativeCreateCopyReadFile(destination) == "published-before-metadata");
        CHECK(NativeCreateCopyTempFiles(temporary.directory).empty());
    };

    SECTION("ACL")
    {
        run(NativeCreateCopyPostPublishMetadataFailingIO::Fault::ACL, EACCES, "acl-failed.txt");
    }
    SECTION("mode")
    {
        run(NativeCreateCopyPostPublishMetadataFailingIO::Fault::Mode, EPERM, "mode-failed.txt");
    }
}

TEST_CASE(PREFIX "classifies postpublish metadata and filesystem sync failures",
          "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    NativeCreateCopyWriteFile(source, "postpublish-finalization");

    const auto run = [&](NativeCreateCopyPostPublishFinalizationIO::Fault _fault,
                         NativeCreateCopyOutcomeCode _expected_code,
                         int _expected_error,
                         int _expected_sync_error,
                         std::string _destination_name) {
        const auto destination = temporary.directory / _destination_name;
        auto io = std::make_shared<NativeCreateCopyPostPublishFinalizationIO>(_fault);
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, std::move(_destination_name)), io};
        operation.Start();
        operation.Wait();

        CHECK(io->published);
        CHECK(io->flags_after_publish);
        CHECK(io->postpublish_file_sync_calls == 1);
        CHECK(io->postpublish_parent_sync_calls == 1);
        CHECK(operation.State() == OperationState::Stopped);
        CHECK(operation.Outcome().code == _expected_code);
        CHECK(operation.Outcome().error_number == _expected_error);
        CHECK(operation.Outcome().destination_publication == NativeCreateCopyPublicationState::Published);
        CHECK_FALSE(operation.Outcome().recovery_artifact_left);
        CHECK(operation.Outcome().filesystem_sync_error_number == _expected_sync_error);
        CHECK(operation.Outcome().filesystem_sync_confirmed == (_expected_sync_error == 0));
        CHECK(NativeCreateCopyReadFile(destination) == "postpublish-finalization");
    };

    SECTION("unsupported flags")
    {
        run(NativeCreateCopyPostPublishFinalizationIO::Fault::FlagsUnsupported,
            NativeCreateCopyOutcomeCode::MetadataUnsupported,
            ENOTSUP,
            0,
            "flags-unsupported.txt");
    }
    SECTION("metadata verification mismatch")
    {
        run(NativeCreateCopyPostPublishFinalizationIO::Fault::VerificationMismatch,
            NativeCreateCopyOutcomeCode::MetadataVerificationFailed,
            ESTALE,
            0,
            "verification-mismatch.txt");
    }
    SECTION("final file sync")
    {
        run(NativeCreateCopyPostPublishFinalizationIO::Fault::FileSync,
            NativeCreateCopyOutcomeCode::FileSystemSyncFailed,
            EIO,
            EIO,
            "file-sync-failed.txt");
    }
    SECTION("parent sync")
    {
        run(NativeCreateCopyPostPublishFinalizationIO::Fault::ParentSync,
            NativeCreateCopyOutcomeCode::FileSystemSyncFailed,
            EIO,
            EIO,
            "parent-sync-failed.txt");
    }
    SECTION("metadata and file sync preserve primary and secondary errors")
    {
        run(NativeCreateCopyPostPublishFinalizationIO::Fault::FlagsAndFileSync,
            NativeCreateCopyOutcomeCode::MetadataUnsupported,
            ENOTSUP,
            EIO,
            "metadata-and-sync-failed.txt");
    }
}

TEST_CASE(PREFIX "rejects unsupported source security metadata before creating a temp",
          "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    NativeCreateCopyWriteFile(source, "unsupported-source-metadata");

    SECTION("set-ID mode")
    {
        REQUIRE(chmod(source.c_str(), 04755) == 0);
        const auto destination = temporary.directory / "set-id.txt";
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, destination.filename().native())};
        operation.Start();
        operation.Wait();

        CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::MetadataUnsupported);
        CHECK(operation.Outcome().error_number == ENOTSUP);
        CHECK(operation.Outcome().destination_publication == NativeCreateCopyPublicationState::NotPublished);
        CHECK_FALSE(operation.Outcome().recovery_artifact_left);
        CHECK_FALSE(std::filesystem::exists(destination));
        CHECK(NativeCreateCopyTempFiles(temporary.directory).empty());
    }

    SECTION("regular-file-inapplicable BSD flag")
    {
        const bool opaque_supported = chflags(source.c_str(), UF_OPAQUE) == 0;
        NativeCreateCopySourceFlagReset flag_reset{source, opaque_supported};
        if( !opaque_supported ) {
            SUCCEED("filesystem does not permit setting UF_OPAQUE on a regular file");
            return;
        }

        const auto destination = temporary.directory / "opaque.txt";
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, destination.filename().native())};
        operation.Start();
        operation.Wait();

        CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::MetadataUnsupported);
        CHECK(operation.Outcome().error_number == ENOTSUP);
        CHECK(operation.Outcome().destination_publication == NativeCreateCopyPublicationState::NotPublished);
        CHECK_FALSE(operation.Outcome().recovery_artifact_left);
        CHECK_FALSE(std::filesystem::exists(destination));
        CHECK(NativeCreateCopyTempFiles(temporary.directory).empty());
    }
}

TEST_CASE(PREFIX "retries EINTR and completes short writes", "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.bin";
    const auto destination = temporary.directory / "destination.bin";
    const std::string contents(64 * 1024 + 17, 'x');
    NativeCreateCopyWriteFile(source, contents);

    auto io = std::make_shared<NativeCreateCopyScriptedIO>();
    io->pread_eintr_count = 1;
    io->write_eintr_count = 1;
    io->short_write_size = 7;
    NativeCreateCopy operation{
        NativeCreateCopyMakeCapsule(source, temporary.directory, destination.filename().native()), io};
    operation.Start();
    operation.Wait();

    CHECK(operation.State() == OperationState::Completed);
    CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::Success);
    CHECK(NativeCreateCopyReadFile(destination) == contents);
}

TEST_CASE(PREFIX "keeps a destination that appears before exclusive publish", "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination = temporary.directory / "destination.txt";
    NativeCreateCopyWriteFile(source, "source-data");

    auto io = std::make_shared<NativeCreateCopyScriptedIO>();
    bool victim_created = false;
    io->on_checkpoint = [&](NativeCreateCopyCheckpoint _checkpoint) {
        if( _checkpoint != NativeCreateCopyCheckpoint::BeforePublish )
            return;
        const int fd = open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if( fd < 0 )
            return;
        constexpr std::string_view victim = "victim-data";
        victim_created = write(fd, victim.data(), victim.size()) == static_cast<ssize_t>(victim.size());
        (void)close(fd);
    };
    NativeCreateCopy operation{
        NativeCreateCopyMakeCapsule(source, temporary.directory, destination.filename().native()), io};
    operation.Start();
    operation.Wait();

    CHECK(operation.State() == OperationState::Stopped);
    REQUIRE(victim_created);
    CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::CleanupFailed);
    CHECK(operation.Outcome().prior_code == NativeCreateCopyOutcomeCode::StaleDestination);
    CHECK(operation.Outcome().error_number == EEXIST);
    CHECK(operation.Outcome().destination_publication == NativeCreateCopyPublicationState::NotPublished);
    CHECK(operation.Outcome().recovery_artifact_left);
    CHECK(NativeCreateCopyReadFile(destination) == "victim-data");
    NativeCreateCopyRemoveRecoveryArtifacts(temporary.directory);
}

TEST_CASE(PREFIX "rejects a rebound temp entry before exclusive publish", "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination = temporary.directory / "destination.txt";
    NativeCreateCopyWriteFile(source, "sealed-source-data");
    const int checkpoint_parent_fd = open(temporary.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    REQUIRE(checkpoint_parent_fd >= 0);

    auto io = std::make_shared<NativeCreateCopyBeforePublishRebindingIO>(checkpoint_parent_fd);
    auto operation = std::make_unique<NativeCreateCopy>(
        NativeCreateCopyMakeCapsule(source, temporary.directory, destination.filename().native()), io);
    operation->Start();
    operation->Wait();

    REQUIRE(io->rebound);
    CHECK(io->publish_calls == 0);
    CHECK(operation->State() == OperationState::Stopped);
    CHECK(operation->Outcome().code == NativeCreateCopyOutcomeCode::CleanupFailed);
    CHECK(operation->Outcome().prior_code == NativeCreateCopyOutcomeCode::StaleDestination);
    CHECK(operation->Outcome().error_number == ESTALE);
    CHECK(operation->Outcome().destination_publication == NativeCreateCopyPublicationState::NotPublished);
    CHECK(operation->Outcome().recovery_artifact_left);
    CHECK_FALSE(std::filesystem::exists(destination));
    REQUIRE_FALSE(io->temp_name.empty());
    CHECK(NativeCreateCopyReadFile(temporary.directory / io->temp_name) == "before-publish-victim");
    CHECK(NativeCreateCopyReadFile(temporary.directory / io->detached_name) == "sealed-source-data");

    operation.reset();
    CHECK(close(checkpoint_parent_fd) == 0);
    REQUIRE(std::filesystem::remove(temporary.directory / io->temp_name));
    REQUIRE(std::filesystem::remove(temporary.directory / io->detached_name));
}

TEST_CASE(PREFIX "rejects hostile hard-link aliases before and after publish", "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    NativeCreateCopyWriteFile(source, "hard-link-source-data");

    SECTION("before publish")
    {
        const auto destination = temporary.directory / "before-publish.txt";
        const auto alias = temporary.directory / "before-publish-alias.txt";
        auto io = std::make_shared<NativeCreateCopyHardLinkingIO>(
            NativeCreateCopyHardLinkingIO::LinkPoint::BeforePublish, alias.filename().native());
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, destination.filename().native()), io};
        operation.Start();
        operation.Wait();

        REQUIRE(io->linked);
        CHECK(io->publish_calls == 0);
        CHECK(operation.State() == OperationState::Stopped);
        CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::CleanupFailed);
        CHECK(operation.Outcome().prior_code == NativeCreateCopyOutcomeCode::StaleDestination);
        CHECK(operation.Outcome().error_number == ESTALE);
        CHECK(operation.Outcome().destination_publication == NativeCreateCopyPublicationState::NotPublished);
        CHECK(operation.Outcome().recovery_artifact_left);
        CHECK_FALSE(std::filesystem::exists(destination));
        CHECK(NativeCreateCopyReadFile(alias) == "hard-link-source-data");

        REQUIRE(std::filesystem::remove(alias));
        NativeCreateCopyRemoveRecoveryArtifacts(temporary.directory);
    }

    SECTION("after publish")
    {
        const auto destination = temporary.directory / "after-publish.txt";
        const auto alias = temporary.directory / "after-publish-alias.txt";
        auto io = std::make_shared<NativeCreateCopyHardLinkingIO>(
            NativeCreateCopyHardLinkingIO::LinkPoint::AfterPublish, alias.filename().native());
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, destination.filename().native()), io};
        operation.Start();
        operation.Wait();

        REQUIRE(io->linked);
        CHECK(io->publish_calls == 1);
        CHECK(operation.State() == OperationState::Stopped);
        CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::CommitFailed);
        CHECK(operation.Outcome().error_number == ESTALE);
        CHECK(operation.Outcome().destination_publication == NativeCreateCopyPublicationState::Published);
        CHECK_FALSE(operation.Outcome().recovery_artifact_left);
        CHECK(operation.Outcome().filesystem_sync_confirmed);
        CHECK(NativeCreateCopyReadFile(destination) == "hard-link-source-data");
        CHECK(NativeCreateCopyReadFile(alias) == "hard-link-source-data");

        REQUIRE(std::filesystem::remove(alias));
        REQUIRE(std::filesystem::remove(destination));
    }
}

TEST_CASE(PREFIX "reports success when cancellation loses the publish race", "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination = temporary.directory / "destination.txt";
    NativeCreateCopyWriteFile(source, "committed-data");

    auto io = std::make_shared<NativeCreateCopyScriptedIO>();
    std::unique_ptr<NativeCreateCopy> operation;
    io->on_checkpoint = [&](NativeCreateCopyCheckpoint _checkpoint) {
        if( _checkpoint == NativeCreateCopyCheckpoint::AfterPublish )
            operation->Stop();
    };
    operation = std::make_unique<NativeCreateCopy>(
        NativeCreateCopyMakeCapsule(source, temporary.directory, destination.filename().native()), io);
    operation->Start();
    operation->Wait();

    CHECK(operation->State() == OperationState::Completed);
    CHECK(operation->Outcome().code == NativeCreateCopyOutcomeCode::Success);
    CHECK(operation->Outcome().destination_publication == NativeCreateCopyPublicationState::Published);
    CHECK(NativeCreateCopyReadFile(destination) == "committed-data");
}

TEST_CASE(PREFIX "linearizes cancellation before the exclusive publish gate", "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    NativeCreateCopyWriteFile(source, "source-data");

    const auto run = [&](NativeCreateCopyStopBoundaryIO::StopPoint _stop_point,
                         std::string _destination_name) {
        const auto destination = temporary.directory / _destination_name;
        auto io = std::make_shared<NativeCreateCopyStopBoundaryIO>(_stop_point);
        std::unique_ptr<NativeCreateCopy> operation;
        io->stop = [&] { operation->Stop(); };
        operation = std::make_unique<NativeCreateCopy>(
            NativeCreateCopyMakeCapsule(source, temporary.directory, std::move(_destination_name)), io);
        operation->Start();
        operation->Wait();

        CHECK(io->publish_calls == 0);
        CHECK(operation->State() == OperationState::Stopped);
        CHECK(operation->Outcome().code == NativeCreateCopyOutcomeCode::CleanupFailed);
        CHECK(operation->Outcome().prior_code == NativeCreateCopyOutcomeCode::Cancelled);
        CHECK(operation->Outcome().recovery_artifact_left);
        CHECK_FALSE(std::filesystem::exists(destination));
        NativeCreateCopyRemoveRecoveryArtifacts(temporary.directory);
    };

    SECTION("stop during final source fstat")
    {
        run(NativeCreateCopyStopBoundaryIO::StopPoint::FinalSourceFStat, "fstat-stop.txt");
    }
    SECTION("stop at before-publish checkpoint")
    {
        run(NativeCreateCopyStopBoundaryIO::StopPoint::BeforePublish, "checkpoint-stop.txt");
    }
}

TEST_CASE(PREFIX "revalidates the source version after data and metadata reads", "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination = temporary.directory / "destination.txt";
    NativeCreateCopyWriteFile(source, "reviewed-data");

    auto io = std::make_shared<NativeCreateCopyScriptedIO>();
    bool mutation_succeeded = false;
    io->on_checkpoint = [&](NativeCreateCopyCheckpoint _checkpoint) {
        if( _checkpoint != NativeCreateCopyCheckpoint::BeforeSourceRevalidation )
            return;
        const int fd = open(source.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if( fd < 0 )
            return;
        mutation_succeeded = write(fd, "!", 1) == 1;
        (void)close(fd);
    };
    NativeCreateCopy operation{
        NativeCreateCopyMakeCapsule(source, temporary.directory, destination.filename().native()), io};
    operation.Start();
    operation.Wait();

    CHECK(operation.State() == OperationState::Stopped);
    REQUIRE(mutation_succeeded);
    CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::CleanupFailed);
    CHECK(operation.Outcome().prior_code == NativeCreateCopyOutcomeCode::StaleSource);
    CHECK(operation.Outcome().error_number == ESTALE);
    CHECK(operation.Outcome().recovery_artifact_left);
    CHECK_FALSE(std::filesystem::exists(destination));
    NativeCreateCopyRemoveRecoveryArtifacts(temporary.directory);
}

TEST_CASE(PREFIX "abandons its temp for explicit recovery on cancellation and write failure",
          "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    NativeCreateCopyWriteFile(source, "source-data");

    SECTION("cancellation")
    {
        auto io = std::make_shared<NativeCreateCopyScriptedIO>();
        std::unique_ptr<NativeCreateCopy> operation;
        io->on_checkpoint = [&](NativeCreateCopyCheckpoint _checkpoint) {
            if( _checkpoint == NativeCreateCopyCheckpoint::AfterTempCreated )
                operation->Stop();
        };
        operation = std::make_unique<NativeCreateCopy>(
            NativeCreateCopyMakeCapsule(source, temporary.directory, "cancelled.txt"), io);
        operation->Start();
        operation->Wait();

        CHECK(operation->State() == OperationState::Stopped);
        CHECK(operation->Outcome().code == NativeCreateCopyOutcomeCode::CleanupFailed);
        CHECK(operation->Outcome().prior_code == NativeCreateCopyOutcomeCode::Cancelled);
        CHECK(operation->Outcome().error_number == 0);
        CHECK(operation->Outcome().recovery_artifact_left);
        CHECK_FALSE(std::filesystem::exists(temporary.directory / "cancelled.txt"));
        NativeCreateCopyRemoveRecoveryArtifacts(temporary.directory);
    }

    SECTION("write failure")
    {
        auto io = std::make_shared<NativeCreateCopyScriptedIO>();
        io->write_error = EIO;
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, "failed.txt"), io};
        operation.Start();
        operation.Wait();

        CHECK(operation.State() == OperationState::Stopped);
        CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::CleanupFailed);
        CHECK(operation.Outcome().prior_code == NativeCreateCopyOutcomeCode::WriteFailed);
        CHECK(operation.Outcome().error_number == EIO);
        CHECK(operation.Outcome().recovery_artifact_left);
        CHECK_FALSE(std::filesystem::exists(temporary.directory / "failed.txt"));
        NativeCreateCopyRemoveRecoveryArtifacts(temporary.directory);
    }
}

TEST_CASE(PREFIX "abandons recovery artifacts without unlinking a rebound victim", "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    NativeCreateCopyWriteFile(source, "source-data");
    const int checkpoint_parent_fd = open(temporary.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    REQUIRE(checkpoint_parent_fd >= 0);

    auto io = std::make_shared<NativeCreateCopyRebindingIO>(checkpoint_parent_fd);
    std::unique_ptr<NativeCreateCopy> operation;
    io->cancel = [&] { operation->Stop(); };
    operation = std::make_unique<NativeCreateCopy>(
        NativeCreateCopyMakeCapsule(source, temporary.directory, "destination.txt"), io);
    operation->Start();
    operation->Wait();

    REQUIRE(operation->Outcome().code == NativeCreateCopyOutcomeCode::CleanupFailed);
    CHECK(operation->Outcome().prior_code == NativeCreateCopyOutcomeCode::Cancelled);
    CHECK(operation->Outcome().error_number == 0);
    CHECK(operation->Outcome().recovery_artifact_left);
    REQUIRE_FALSE(io->temp_name.empty());
    CHECK(NativeCreateCopyReadFile(temporary.directory / io->temp_name) == "rebound-victim");
    CHECK_FALSE(std::filesystem::exists(temporary.directory / "destination.txt"));

    operation.reset();
    CHECK(close(checkpoint_parent_fd) == 0);
    REQUIRE(std::filesystem::remove(temporary.directory / io->temp_name));
    REQUIRE(std::filesystem::remove(temporary.directory / "detached-original-temp"));
}

TEST_CASE(PREFIX "reconciles ambiguous mutating syscall results without retrying", "[native-create-copy]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    NativeCreateCopyWriteFile(source, "source-data");

    SECTION("exclusive temp creation EINTR is not retried")
    {
        auto io = std::make_shared<NativeCreateCopyMutationEINTRO>(NativeCreateCopyMutationEINTRO::Fault::Open);
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, "open-eintr.txt"), io};
        operation.Start();
        operation.Wait();

        CHECK(io->mutation_calls == 1);
        CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::WriteFailed);
        CHECK(operation.Outcome().error_number == EINTR);
        CHECK_FALSE(operation.Outcome().recovery_artifact_left);
        CHECK_FALSE(std::filesystem::exists(temporary.directory / "open-eintr.txt"));
        CHECK(NativeCreateCopyTempFiles(temporary.directory).empty());
    }

    SECTION("exclusive rename EINTR before commit is not retried")
    {
        auto io = std::make_shared<NativeCreateCopyMutationEINTRO>(
            NativeCreateCopyMutationEINTRO::Fault::RenameBeforeCommit);
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, "rename-eintr.txt"), io};
        operation.Start();
        operation.Wait();

        CHECK(io->mutation_calls == 1);
        CHECK(operation.State() == OperationState::Stopped);
        CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::CleanupFailed);
        CHECK(operation.Outcome().prior_code == NativeCreateCopyOutcomeCode::CommitFailed);
        CHECK(operation.Outcome().error_number == EINTR);
        CHECK(operation.Outcome().recovery_artifact_left);
        CHECK_FALSE(std::filesystem::exists(temporary.directory / "rename-eintr.txt"));
        NativeCreateCopyRemoveRecoveryArtifacts(temporary.directory);
    }

    SECTION("exclusive rename EINTR after commit is reconciled")
    {
        auto io = std::make_shared<NativeCreateCopyMutationEINTRO>(
            NativeCreateCopyMutationEINTRO::Fault::RenameAfterCommit);
        NativeCreateCopy operation{
            NativeCreateCopyMakeCapsule(source, temporary.directory, "rename-committed.txt"), io};
        operation.Start();
        operation.Wait();

        CHECK(io->mutation_calls == 1);
        CHECK(operation.State() == OperationState::Completed);
        CHECK(operation.Outcome().code == NativeCreateCopyOutcomeCode::Success);
        CHECK(NativeCreateCopyReadFile(temporary.directory / "rename-committed.txt") == "source-data");
    }

}

} // namespace nc::ops

#undef PREFIX
