// Copyright (C) 2017-2025 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"
#include <filesystem>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <set>
#include <fstream>

#include "../source/Compression/Compression.h"
#include "../source/Statistics.h"

#include <VFS/VFS.h>
#include <VFS/ArcLA.h>
#include <VFS/Native.h>

namespace CompressionTests {

using namespace nc;
using namespace nc::ops;
using namespace nc::vfs;
using namespace std::literals;

#define PREFIX "Operations::Compression "

static std::expected<int, Error> VFSCompareEntries(const std::filesystem::path &_file1_full_path,
                                                   const VFSHostPtr &_file1_host,
                                                   const std::filesystem::path &_file2_full_path,
                                                   const VFSHostPtr &_file2_host);

static std::set<std::string> XAttrNames(const std::shared_ptr<VFSFile> &_file)
{
    std::set<std::string> names;
    _file->XAttrIterateNames([&](std::string_view _name) {
        names.emplace(_name);
        return true;
    });
    return names;
}

static std::vector<VFSListingItem>
FetchItems(const std::string &_directory_path, const std::vector<std::string> &_filenames, VFSHost &_host);
static bool touch(const std::filesystem::path &_path);

struct ExclusiveCreateRaceState {
    bool open_called = false;
    unsigned long open_flags = 0;
    int unlink_calls = 0;
    std::string created_path;
};

class ExclusiveCreateRaceFile final : public VFSFile
{
public:
    ExclusiveCreateRaceFile(std::string_view _path,
                            const VFSHostPtr &_host,
                            std::shared_ptr<ExclusiveCreateRaceState> _state)
        : VFSFile(_path, _host), m_State(std::move(_state))
    {
    }

    std::expected<void, Error>
    Open(const unsigned long _flags, const VFSCancelChecker & /*_cancel_checker*/) override
    {
        m_State->open_called = true;
        m_State->open_flags = _flags;
        return std::unexpected(Error{Error::POSIX, EEXIST});
    }

private:
    std::shared_ptr<ExclusiveCreateRaceState> m_State;
};

class ExclusiveCreateRaceHost final : public vfs::Host
{
public:
    explicit ExclusiveCreateRaceHost(std::shared_ptr<ExclusiveCreateRaceState> _state)
        : Host("/", nullptr, "exclusive_create_race"), m_State(std::move(_state))
    {
    }

    std::expected<VFSStat, Error>
    Stat(std::string_view /*_path*/,
         unsigned long /*_flags*/,
         const VFSCancelChecker & /*_cancel_checker*/) override
    {
        return std::unexpected(Error{Error::POSIX, EIO});
    }

    HostErrorKind ClassifyError(const Error &_error) const noexcept override
    {
        return _error == Error{Error::POSIX, EIO} ? HostErrorKind::Missing : Host::ClassifyError(_error);
    }

    std::expected<std::shared_ptr<VFSFile>, Error>
    CreateFile(std::string_view _path, const VFSCancelChecker & /*_cancel_checker*/) override
    {
        m_State->created_path = _path;
        return std::make_shared<ExclusiveCreateRaceFile>(_path, SharedPtr(), m_State);
    }

    std::expected<void, Error>
    Unlink(std::string_view /*_path*/, const VFSCancelChecker & /*_cancel_checker*/) override
    {
        ++m_State->unlink_calls;
        return {};
    }

private:
    std::shared_ptr<ExclusiveCreateRaceState> m_State;
};

TEST_CASE(PREFIX "Empty archive building")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;
    Compression operation{std::vector<VFSListingItem>{}, tmp_dir.directory, native_host};
    operation.Start();
    operation.Wait();

    REQUIRE(operation.State() == OperationState::Completed);
    REQUIRE(native_host->Exists(operation.ArchivePath()));

    std::shared_ptr<vfs::ArchiveHost> arc_host;
    REQUIRE_NOTHROW(arc_host = std::make_shared<vfs::ArchiveHost>(operation.ArchivePath(), native_host));
    CHECK(arc_host->StatTotalFiles() == 0);
}

TEST_CASE(PREFIX "Existing destination archive is preserved")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;
    const std::filesystem::path source = tmp_dir.directory / "item";
    const std::filesystem::path existing_archive = tmp_dir.directory / "item.zip";
    std::ofstream(source) << "payload";
    std::ofstream(existing_archive) << "sentinel";

    Compression operation{FetchItems(tmp_dir.directory, {"item"}, *native_host), tmp_dir.directory, native_host};
    operation.Start();
    operation.Wait();

    REQUIRE(operation.State() == OperationState::Completed);
    CHECK(operation.ArchivePath() == (tmp_dir.directory / "item 2.zip").native());
    REQUIRE(native_host->Exists(operation.ArchivePath()));

    std::ifstream preserved(existing_archive);
    const std::string contents{std::istreambuf_iterator<char>{preserved}, std::istreambuf_iterator<char>{}};
    CHECK(contents == "sentinel");
}

TEST_CASE(PREFIX "Destination appearing after the name probe is preserved")
{
    const auto state = std::make_shared<ExclusiveCreateRaceState>();
    const auto host = std::make_shared<ExclusiveCreateRaceHost>(state);
    Compression operation{std::vector<VFSListingItem>{}, "/destination", host};
    operation.Start();
    operation.Wait();

    CHECK(operation.State() == OperationState::Stopped);
    CHECK(state->created_path == "/destination/Archive.zip");
    CHECK(state->open_called);
    CHECK((state->open_flags & VFSFlags::OF_NoExist) != 0);
    CHECK(state->unlink_calls == 0);
}

TEST_CASE(PREFIX "Compressing Mac kernel")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;

    Compression operation{
        FetchItems("/System/Library/Kernels/", {"kernel"}, *native_host), tmp_dir.directory, native_host};

    operation.Start();
    operation.Wait();

    REQUIRE(operation.State() == OperationState::Completed);
    CHECK(operation.Statistics().ElapsedTime() > 1ms);
    CHECK(operation.Statistics().ElapsedTime() < 5s);
    REQUIRE(native_host->Exists(operation.ArchivePath()));

    std::shared_ptr<vfs::ArchiveHost> arc_host;
    REQUIRE_NOTHROW(arc_host = std::make_shared<vfs::ArchiveHost>(operation.ArchivePath(), native_host));
    CHECK(arc_host->StatTotalFiles() == 1);
    CHECK(easy::VFSEasyCompareFiles("/System/Library/Kernels/kernel", native_host, "/kernel", arc_host) == 0);
}

TEST_CASE(PREFIX "Compressing Bin utilities")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;

    const std::vector<std::string> filenames = {
        "[",        "bash",  "cat", "chmod",     "cp",   "csh",  "date", "dd",    "df",     "echo",      "ed", "expr",
        "hostname", "kill",  "ksh", "launchctl", "link", "ln",   "ls",   "mkdir", "mv",     "pax",       "ps", "pwd",
        "rm",       "rmdir", "sh",  "sleep",     "stty", "sync", "tcsh", "test",  "unlink", "wait4path", "zsh"};

    Compression operation{FetchItems("/bin/", filenames, *native_host), tmp_dir.directory, native_host};

    operation.Start();
    operation.Wait();

    REQUIRE(operation.State() == OperationState::Completed);
    REQUIRE(native_host->Exists(operation.ArchivePath()));

    std::shared_ptr<vfs::ArchiveHost> arc_host;
    REQUIRE_NOTHROW(arc_host = std::make_shared<vfs::ArchiveHost>(operation.ArchivePath(), native_host));
    CHECK(arc_host->StatTotalFiles() == filenames.size());

    for( auto &fn : filenames ) {
        CHECK(easy::VFSEasyCompareFiles(("/bin/"s + fn).c_str(), native_host, ("/"s + fn).c_str(), arc_host) == 0);
    }
}

TEST_CASE(PREFIX "Compressing Bin directory")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;

    Compression operation{FetchItems("/", {"bin"}, *native_host), tmp_dir.directory, native_host};

    operation.Start();
    operation.Wait();

    REQUIRE(operation.State() == OperationState::Completed);
    REQUIRE(native_host->Exists(operation.ArchivePath()));

    std::shared_ptr<vfs::ArchiveHost> arc_host;
    REQUIRE_NOTHROW(arc_host = std::make_shared<vfs::ArchiveHost>(operation.ArchivePath(), native_host));

    CHECK(VFSCompareEntries("/bin/", native_host, "/bin/", arc_host).value() == 0);
}

TEST_CASE(PREFIX "Compressing Chess.app")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;

    Compression operation{
        FetchItems("/System/Applications/", {"Chess.app"}, *native_host), tmp_dir.directory, native_host};

    operation.Start();
    operation.Wait();

    REQUIRE(operation.State() == OperationState::Completed);
    REQUIRE(native_host->Exists(operation.ArchivePath()));

    std::shared_ptr<vfs::ArchiveHost> arc_host;
    REQUIRE_NOTHROW(arc_host = std::make_shared<vfs::ArchiveHost>(operation.ArchivePath(), native_host));

    CHECK(VFSCompareEntries("/System/Applications/Chess.app", native_host, "/Chess.app", arc_host).value() == 0);
}

TEST_CASE(PREFIX "Compressing a symlink to regular file")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;
    std::ofstream(tmp_dir.directory / "file.txt") << "Hello!";
    std::filesystem::create_symlink("file.txt", tmp_dir.directory / "symlink.txt");

    Compression operation{
        FetchItems(tmp_dir.directory, {"file.txt", "symlink.txt"}, *native_host), tmp_dir.directory, native_host};

    operation.Start();
    operation.Wait();

    REQUIRE(operation.State() == OperationState::Completed);
    REQUIRE(native_host->Exists(operation.ArchivePath()));

    std::shared_ptr<vfs::ArchiveHost> arc_host;
    REQUIRE_NOTHROW(arc_host = std::make_shared<vfs::ArchiveHost>(operation.ArchivePath(), native_host));

    CHECK(VFSCompareEntries(tmp_dir.directory / "file.txt", native_host, "/file.txt", arc_host).value() == 0);
    CHECK(VFSCompareEntries(tmp_dir.directory / "symlink.txt", native_host, "/symlink.txt", arc_host).value() == 0);
}

TEST_CASE(PREFIX "Compressing kernel into encrypted archive")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;
    const auto passwd = "This is a very secret password";

    Compression operation{
        FetchItems("/System/Library/Kernels/", {"kernel"}, *native_host), tmp_dir.directory, native_host, passwd};

    operation.Start();
    operation.Wait();

    REQUIRE(operation.State() == OperationState::Completed);
    REQUIRE(native_host->Exists(operation.ArchivePath()));

    try {
        std::make_shared<vfs::ArchiveHost>(operation.ArchivePath(), native_host);
        REQUIRE(false);
    } catch( const ErrorException &e ) {
        REQUIRE(e.error() == Error{Error::POSIX, ENEEDAUTH});
    }

    std::shared_ptr<vfs::ArchiveHost> arc_host;
    REQUIRE_NOTHROW(arc_host = std::make_shared<vfs::ArchiveHost>(operation.ArchivePath(), native_host, passwd));
    CHECK(easy::VFSEasyCompareFiles("/System/Library/Kernels/kernel", native_host, "/kernel", arc_host) == 0);
}

TEST_CASE(PREFIX "Compressing /bin into encrypted archive")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;
    const auto passwd = "This is a very secret password";

    Compression operation{FetchItems("/", {"bin"}, *native_host), tmp_dir.directory, native_host, passwd};

    operation.Start();
    operation.Wait();

    REQUIRE(operation.State() == OperationState::Completed);
    REQUIRE(native_host->Exists(operation.ArchivePath()));

    std::shared_ptr<vfs::ArchiveHost> arc_host;
    REQUIRE_NOTHROW(arc_host = std::make_shared<vfs::ArchiveHost>(operation.ArchivePath(), native_host, passwd));
    CHECK(VFSCompareEntries("/bin/", native_host, "/bin/", arc_host).value() == 0);
}

TEST_CASE(PREFIX "Compressing an item with xattrs")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;
    auto str_to_bytes = [](std::string_view _str) -> std::vector<std::byte> {
        return {reinterpret_cast<const std::byte *>(_str.data()),
                reinterpret_cast<const std::byte *>(_str.data()) + _str.length()};
    };

    struct EA {
        std::string name;
        std::vector<std::byte> bytes;
    };
    struct TC {
        std::vector<EA> eas;
    } const tcs[] = {
        {{EA{.name = "hello", .bytes = str_to_bytes("hello")}}},
        {{EA{.name = "hello", .bytes = str_to_bytes("hello")}, EA{.name = "hi", .bytes = str_to_bytes("privet")}}},
        {{EA{.name = "hello", .bytes = str_to_bytes("hello")},
          EA{.name = "hi", .bytes = str_to_bytes("privet")},
          EA{.name = "another", .bytes = str_to_bytes("hola")}}},
        {{EA{.name = "empty", .bytes = str_to_bytes("")}}},
        {{EA{.name = std::string(XATTR_MAXNAMELEN, 'X'), .bytes = str_to_bytes("an xattr with a very long name")}}},
        {{EA{.name = "an xattr with a 128KB content",
             .bytes = std::vector<std::byte>(128ull * 1024ull, std::byte{0xFE})}}},
    };

    const std::filesystem::path filepath = tmp_dir.directory / "a";
    for( const TC &tc : tcs ) {
        // create the file to compress
        REQUIRE(touch(filepath));

        // write the extended attributes into the file
        for( const EA &ea : tc.eas ) {
            REQUIRE(setxattr(filepath.c_str(), ea.name.c_str(), ea.bytes.data(), ea.bytes.size(), 0, 0) == 0);
        }

        const std::shared_ptr<VFSFile> source_file = native_host->CreateFile(filepath.native()).value();
        REQUIRE(source_file->Open(VFSFlags::OF_Read));
        const auto source_xattrs = XAttrNames(source_file);

        // compress
        Compression operation{
            FetchItems(tmp_dir.directory, {filepath.filename()}, *native_host), tmp_dir.directory, native_host};
        operation.Start();
        operation.Wait();
        REQUIRE(operation.State() == OperationState::Completed);
        REQUIRE(native_host->Exists(operation.ArchivePath()));

        // open the archive
        std::shared_ptr<vfs::ArchiveHost> arc_host;
        REQUIRE_NOTHROW(arc_host = std::make_shared<vfs::ArchiveHost>(operation.ArchivePath(), native_host));

        // open the compressed file in the archive
        const std::shared_ptr<VFSFile> file = arc_host->CreateFile("/" + filepath.filename().native()).value();
        REQUIRE(file->Open(VFSFlags::OF_Read));

        // System-managed attributes can be attached to freshly created files. The archive must preserve the
        // exact source set in addition to retaining every explicitly arranged value below.
        REQUIRE(XAttrNames(file) == source_xattrs);

        // check that each extracted extended attribute is equal to the original
        for( const EA &ea : tc.eas ) {
            REQUIRE(file->XAttrGet(ea.name, nullptr, 0) == ea.bytes.size());
            std::vector<std::byte> bytes(ea.bytes.size());
            REQUIRE(file->XAttrGet(ea.name, bytes.data(), bytes.size()) == ea.bytes.size());
            REQUIRE(bytes == ea.bytes);
        }

        // cleanup the file that was compressed
        REQUIRE(std::filesystem::remove(filepath));
    }
}

TEST_CASE(PREFIX "Compressing multiple items with xattrs")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;

    // arrange the file structure to compress
    const std::filesystem::path file0 = "file0.txt";
    const std::filesystem::path dir1 = "dir1";
    const std::filesystem::path file1 = "dir1/file1.txt";
    const std::filesystem::path dir2 = "dir2";
    const std::filesystem::path file2 = "dir2/file2.txt";
    REQUIRE(std::filesystem::create_directory(tmp_dir.directory / dir1));
    REQUIRE(std::filesystem::create_directory(tmp_dir.directory / dir2));
    REQUIRE(touch(tmp_dir.directory / file0));
    REQUIRE(touch(tmp_dir.directory / file1));
    REQUIRE(touch(tmp_dir.directory / file2));
    for( const auto &p : {file0, file1, file2, dir1, dir2} ) {
        // write a single xattr to each file - the filename as a string
        const std::string val = p.filename().native();
        REQUIRE(setxattr((tmp_dir.directory / p).c_str(), "attr", val.c_str(), val.length(), 0, 0) == 0);
    }

    // compress
    Compression operation{
        FetchItems(tmp_dir.directory, {file0.filename(), dir1.filename(), dir2.filename()}, *native_host),
        tmp_dir.directory,
        native_host};
    operation.Start();
    operation.Wait();
    REQUIRE(operation.State() == OperationState::Completed);
    REQUIRE(native_host->Exists(operation.ArchivePath()));

    // open the archive
    std::shared_ptr<vfs::ArchiveHost> arc_host;
    REQUIRE_NOTHROW(arc_host = std::make_shared<vfs::ArchiveHost>(operation.ArchivePath(), native_host));

    for( const auto &p : {file0, file1, file2, dir1, dir2} ) {
        // open the compressed file in the archive
        const std::filesystem::path path = std::filesystem::path("/") / p;
        const std::shared_ptr<VFSFile> file = arc_host->CreateFile(path.native()).value();
        REQUIRE(file->Open(p.native().ends_with(".txt") ? VFSFlags::OF_Read
                                                        : (VFSFlags::OF_Read | VFSFlags::OF_Directory)));
        // Compare the complete source set because macOS can attach system-managed attributes to new items.
        const std::shared_ptr<VFSFile> source_file = native_host->CreateFile((tmp_dir.directory / p).native()).value();
        REQUIRE(source_file->Open(p.native().ends_with(".txt") ? VFSFlags::OF_Read
                                                               : (VFSFlags::OF_Read | VFSFlags::OF_Directory)));
        REQUIRE(XAttrNames(file) == XAttrNames(source_file));

        // Read the arranged xattr and check its value.
        REQUIRE(file->XAttrGet("attr", nullptr, 0).value() > 0);
        std::string val(file->XAttrGet("attr", nullptr, 0).value(), '\0');
        REQUIRE(file->XAttrGet("attr", val.data(), val.size()).value() > 0);
        REQUIRE(val == p.filename().native());
    }
}

TEST_CASE(PREFIX "Long compression stats (compressing Music.app)")
{
    const TempTestDir tmp_dir;
    const auto native_host = TestEnv().vfs_native;
    Compression operation{
        FetchItems("/System/Applications/", {"Music.app"}, *native_host), tmp_dir.directory, native_host};

    operation.Start();
    operation.Wait(1000ms);
    const auto eta = operation.Statistics().ETA(Statistics::SourceType::Bytes);
    REQUIRE(eta);
    CHECK(*eta > std::chrono::milliseconds(1000));

    operation.Pause();
    REQUIRE(operation.State() == OperationState::Paused);
    operation.Wait(5000ms);
    REQUIRE(operation.State() == OperationState::Paused);
    operation.Resume();
    operation.Wait();
    REQUIRE(operation.State() == OperationState::Completed);
    REQUIRE(native_host->Exists(operation.ArchivePath()));

    std::shared_ptr<vfs::ArchiveHost> arc_host;
    REQUIRE_NOTHROW(arc_host = std::make_shared<vfs::ArchiveHost>(operation.ArchivePath(), native_host));
    CHECK(VFSCompareEntries("/System/Applications/Music.app", native_host, "/Music.app", arc_host).value() == 0);
}

TEST_CASE(PREFIX "Item reporting")
{
    const TempTestDir tmp_dir;
    REQUIRE(mkdir((tmp_dir.directory / "dir").c_str(), 0755) == 0);
    REQUIRE(close(creat((tmp_dir.directory / "dir/f1").c_str(), 0755)) == 0);
    REQUIRE(symlink("./f1", (tmp_dir.directory / "dir/f2").c_str()) == 0);
    const auto native_host = TestEnv().vfs_native;
    Compression operation{FetchItems(tmp_dir.directory, {"dir"}, *native_host), tmp_dir.directory, native_host};
    std::set<std::string> processed;
    operation.SetItemStatusCallback([&](nc::ops::ItemStateReport _report) {
        REQUIRE(&_report.host == native_host.get());
        REQUIRE(_report.status == nc::ops::ItemStatus::Processed);
        processed.emplace(_report.path);
    });

    operation.Start();
    operation.Wait();
    REQUIRE(operation.State() == OperationState::Completed);

    const std::set<std::string> expected{
        tmp_dir.directory / "dir", tmp_dir.directory / "dir/f1", tmp_dir.directory / "dir/f2"};
    CHECK(processed == expected);
}

static std::expected<int, Error> VFSCompareEntries(const std::filesystem::path &_file1_full_path,
                                                   const VFSHostPtr &_file1_host,
                                                   const std::filesystem::path &_file2_full_path,
                                                   const VFSHostPtr &_file2_host)
{
    // not comparing flags, perm, times, xattrs, acls etc now

    const std::expected<VFSStat, Error> st1 = _file1_host->Stat(_file1_full_path.native(), VFSFlags::F_NoFollow);
    if( !st1 )
        return std::unexpected(st1.error());

    const std::expected<VFSStat, Error> st2 = _file2_host->Stat(_file2_full_path.native(), VFSFlags::F_NoFollow);
    if( !st2 )
        return std::unexpected(st2.error());

    if( (st1->mode & S_IFMT) != (st2->mode & S_IFMT) ) {
        return -1;
    }

    if( S_ISREG(st1->mode) ) {
        if( int64_t(st1->size) - int64_t(st2->size) != 0 )
            return int(int64_t(st1->size) - int64_t(st2->size));
    }
    else if( S_ISLNK(st1->mode) ) {
        const std::expected<std::string, Error> link1 = _file1_host->ReadSymlink(_file1_full_path.native());
        if( !link1 )
            return std::unexpected(link1.error());

        const std::expected<std::string, Error> link2 = _file2_host->ReadSymlink(_file2_full_path.native());
        if( !link2 )
            return std::unexpected(link2.error());

        if( const int cmp = link1.value().compare(link2.value()); cmp != 0 )
            return cmp;
    }
    else if( S_ISDIR(st1->mode) ) {
        std::expected<int, Error> result = 0;
        const std::expected<void, Error> rc =
            _file1_host->IterateDirectoryListing(_file1_full_path.native(), [&](const VFSDirEnt &_dirent) {
                result = VFSCompareEntries(
                    _file1_full_path / _dirent.name, _file1_host, _file2_full_path / _dirent.name, _file2_host);
                return result.has_value() && result.value() == 0;
            });
        if( !rc ) {
            return std::unexpected(rc.error());
        }
        return result;
    }
    return 0;
}

static std::vector<VFSListingItem>
FetchItems(const std::string &_directory_path, const std::vector<std::string> &_filenames, VFSHost &_host)
{
    return _host.FetchFlexibleListingItems(_directory_path, _filenames, 0).value_or(std::vector<VFSListingItem>{});
}

static bool touch(const std::filesystem::path &_path)
{
    return close(open(_path.c_str(), O_CREAT | O_RDWR, S_IRWXU)) == 0;
}

} // namespace CompressionTests

#undef PREFIX
