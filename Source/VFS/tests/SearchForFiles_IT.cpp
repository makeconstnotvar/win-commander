// Copyright (C) 2019-2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"
#include "SearchForFiles.h"
#include <Utility/PathManip.h>
#include <Native.h>
#include <set>
#include <map>
#include <vector>
#include <fstream>
#include <sys/stat.h>

using nc::utility::FileMask;
using nc::vfs::SearchForFiles;

#define PREFIX "[nc::vfs::SearchForFiles] "

static void BuildTestData(const std::string &_root_path);
static bool Save(const std::string &_filepath, const std::string &_content);
static bool MkDir(const std::string &_dir_path);

namespace {

class ScriptedSearchHost final : public VFSHost
{
public:
    struct Entry {
        VFSDirEnt::Type type;
        std::string name;
    };

    ScriptedSearchHost() : VFSHost("/", nullptr, "search-tests") {}

    void SetListing(std::string _path, std::vector<Entry> _entries)
    {
        m_Failures.erase(_path);
        m_Listings.insert_or_assign(std::move(_path), std::move(_entries));
    }

    void SetFailure(std::string _path, nc::Error _error)
    {
        m_Listings.erase(_path);
        m_Failures.insert_or_assign(std::move(_path), std::move(_error));
    }

    std::expected<void, nc::Error>
    IterateDirectoryListing(std::string_view _path,
                            const std::function<bool(const VFSDirEnt &_dirent)> &_handler) override
    {
        m_Visited.emplace_back(_path);
        if( const auto failure = m_Failures.find(_path); failure != m_Failures.end() )
            return std::unexpected(failure->second);

        const auto listing = m_Listings.find(_path);
        if( listing == m_Listings.end() )
            return std::unexpected(nc::Error{nc::Error::POSIX, ENOENT});

        for( const Entry &entry : listing->second ) {
            const VFSDirEnt dirent{.type = entry.type, .name = entry.name};
            if( !_handler(dirent) )
                break;
        }
        return {};
    }

    [[nodiscard]] const std::vector<std::string> &Visited() const noexcept { return m_Visited; }

private:
    std::map<std::string, std::vector<Entry>, std::less<>> m_Listings;
    std::map<std::string, nc::Error, std::less<>> m_Failures;
    std::vector<std::string> m_Visited;
};

struct SkippedSnapshot {
    std::string path;
    VFSHostPtr host;
    nc::Error error;
    SearchForFiles::SkippedLocation::Context context;
};

} // namespace

TEST_CASE(PREFIX "Test basic searching")
{
    using Options = SearchForFiles::Options;
    TestDir test_dir;
    BuildTestData(test_dir.directory);
    auto &host = TestEnv().vfs_native;

    using set = std::set<std::string>;
    set filenames;
    auto callback = [&](std::string_view _filename, [[maybe_unused]] const char *_in_path, VFSHost &, CFRange) {
        filenames.emplace(_filename);
    };

    SearchForFiles search;
    auto do_search = [&](int _flags) {
        search.Go(test_dir.directory, host, _flags, callback, {});
        search.Wait();
    };

    SECTION("search for all entries, recursively")
    {
        do_search(Options::GoIntoSubDirs | Options::SearchForFiles | Options::SearchForDirs);
        CHECK(filenames == set{"Dir", "filename1.txt", "filename2.txt", "filename3.txt"});
    }
    SECTION("search for all entries, non-recursively")
    {
        do_search(Options::SearchForFiles | Options::SearchForDirs);
        CHECK(filenames == set{"Dir", "filename1.txt", "filename2.txt"});
    }
    SECTION("search for all files")
    {
        do_search(Options::GoIntoSubDirs | Options::SearchForFiles);
        CHECK(filenames == set{"filename1.txt", "filename2.txt", "filename3.txt"});
    }
    SECTION("search for all directories")
    {
        do_search(Options::GoIntoSubDirs | Options::SearchForDirs);
        CHECK(filenames == set{"Dir"});
    }
    SECTION("search for all entries with mask='*.txt'")
    {
        search.SetFilterName(FileMask("*.txt"));
        do_search(Options::GoIntoSubDirs | Options::SearchForDirs | Options::SearchForFiles);
        CHECK(filenames == set{"filename1.txt", "filename2.txt", "filename3.txt"});
    }
    SECTION("search for all entries with mask='*.jpg'")
    {
        search.SetFilterName(FileMask("*.jpg"));
        do_search(Options::GoIntoSubDirs | Options::SearchForDirs | Options::SearchForFiles);
        CHECK(filenames.empty());
    }
    SECTION("search for all entries with mask='*filename*'")
    {
        search.SetFilterName(FileMask("*filename*"));
        do_search(Options::GoIntoSubDirs | Options::SearchForDirs | Options::SearchForFiles);
        CHECK(filenames == set{"filename1.txt", "filename2.txt", "filename3.txt"});
    }
    SECTION("search for all entries with regex='(filename1|filename3).*'")
    {
        search.SetFilterName(FileMask("(filename1|filename3).*", FileMask::Type::RegEx));
        do_search(Options::GoIntoSubDirs | Options::SearchForDirs | Options::SearchForFiles);
        CHECK(filenames == set{"filename1.txt", "filename3.txt"});
    }
    SECTION("search for all entries with mask='*dir*'")
    {
        search.SetFilterName(FileMask("*dir*"));
        do_search(Options::GoIntoSubDirs | Options::SearchForDirs | Options::SearchForFiles);
        CHECK(filenames == set{"Dir"});
    }
}

TEST_CASE(PREFIX "Test size filter")
{
    using Options = SearchForFiles::Options;
    TestDir test_dir;
    BuildTestData(test_dir.directory);
    auto &host = TestEnv().vfs_native;

    using set = std::set<std::string>;
    set filenames;
    auto callback = [&](std::string_view _filename, [[maybe_unused]] const char *_in_path, VFSHost &, CFRange) {
        filenames.emplace(_filename);
    };

    SearchForFiles search;
    auto do_search = [&](int _flags) {
        search.Go(test_dir.directory, host, _flags, callback, {});
        search.Wait();
    };

    SECTION("min = 25")
    {
        auto filter = SearchForFiles::FilterSize{};
        filter.min = 25;
        search.SetFilterSize(filter);
        do_search(Options::GoIntoSubDirs | Options::SearchForFiles | Options::SearchForDirs);
        CHECK(filenames == set{"filename3.txt"});
    }
    SECTION("max = 15")
    {
        auto filter = SearchForFiles::FilterSize{};
        filter.max = 15;
        search.SetFilterSize(filter);
        do_search(Options::GoIntoSubDirs | Options::SearchForFiles | Options::SearchForDirs);
        CHECK(filenames == set{"filename1.txt"});
    }
    SECTION("min = 15 && max = 25")
    {
        auto filter = SearchForFiles::FilterSize{};
        filter.min = 15;
        filter.max = 25;
        search.SetFilterSize(filter);
        do_search(Options::GoIntoSubDirs | Options::SearchForFiles | Options::SearchForDirs);
        CHECK(filenames == set{"filename2.txt", "filename3.txt"});
    }
}

TEST_CASE(PREFIX "Test content filter")
{
    using Options = SearchForFiles::Options;
    TestDir test_dir;
    BuildTestData(test_dir.directory);
    auto &host = TestEnv().vfs_native;

    using set = std::set<std::string>;
    set filenames;
    auto callback = [&](std::string_view _filename, [[maybe_unused]] const char *_in_path, VFSHost &, CFRange) {
        filenames.emplace(_filename);
    };

    SearchForFiles search;
    auto do_search = [&](int _flags) {
        search.Go(test_dir.directory, host, _flags, callback, {});
        search.Wait();
    };

    SECTION("world")
    {
        auto filter = SearchForFiles::FilterContent{};
        filter.text = "world";
        search.SetFilterContent(filter);
        do_search(Options::GoIntoSubDirs | Options::SearchForFiles | Options::SearchForDirs);
        CHECK(filenames == set{"filename1.txt", "filename3.txt"});
    }
    SECTION("hello")
    {
        auto filter = SearchForFiles::FilterContent{};
        filter.text = "hello";
        search.SetFilterContent(filter);
        do_search(Options::GoIntoSubDirs | Options::SearchForFiles | Options::SearchForDirs);
        CHECK(filenames == set{"filename1.txt"});
    }
    SECTION("hello, case sensitive")
    {
        auto filter = SearchForFiles::FilterContent{};
        filter.text = "hello";
        filter.case_sensitive = true;
        search.SetFilterContent(filter);
        do_search(Options::GoIntoSubDirs | Options::SearchForFiles | Options::SearchForDirs);
        CHECK(filenames.empty());
    }
    SECTION("hello, not containing")
    {
        auto filter = SearchForFiles::FilterContent{};
        filter.text = "hello";
        filter.not_containing = true;
        search.SetFilterContent(filter);
        do_search(Options::GoIntoSubDirs | Options::SearchForFiles | Options::SearchForDirs);
        CHECK(filenames == set{"filename2.txt", "filename3.txt"});
    }
    SECTION("hello, whole phrase")
    {
        auto filter = SearchForFiles::FilterContent{};
        filter.text = "hello";
        filter.whole_phrase = true;
        search.SetFilterContent(filter);
        do_search(Options::GoIntoSubDirs | Options::SearchForFiles | Options::SearchForDirs);
        CHECK(filenames == set{"filename1.txt"});
    }
    SECTION("ello, whole phrase")
    {
        auto filter = SearchForFiles::FilterContent{};
        filter.text = "ello";
        filter.whole_phrase = true;
        search.SetFilterContent(filter);
        do_search(Options::GoIntoSubDirs | Options::SearchForFiles | Options::SearchForDirs);
        CHECK(filenames.empty());
    }
    SECTION("мир, UTF8")
    {
        auto filter = SearchForFiles::FilterContent{};
        filter.text = reinterpret_cast<const char *>(u8"мир");
        search.SetFilterContent(filter);
        do_search(Options::GoIntoSubDirs | Options::SearchForFiles | Options::SearchForDirs);
        CHECK(filenames == set{"filename2.txt"});
    }
    SECTION("мир, MACOS_ROMAN_WESTERN")
    {
        auto filter = SearchForFiles::FilterContent{};
        filter.text = reinterpret_cast<const char *>(u8"мир");
        filter.encoding = nc::utility::Encoding::ENCODING_MACOS_ROMAN_WESTERN;
        search.SetFilterContent(filter);
        do_search(Options::GoIntoSubDirs | Options::SearchForFiles | Options::SearchForDirs);
        CHECK(filenames.empty());
    }
}

TEST_CASE(PREFIX "Reports an exact unreadable content item")
{
    using Options = SearchForFiles::Options;
    TestDir test_dir;
    const std::string path = (test_dir.directory / "unreadable.txt").string();
    REQUIRE(Save(path, "needle"));
    REQUIRE(::chmod(path.c_str(), 0000) == 0);

    std::vector<SkippedSnapshot> skipped;
    SearchForFiles search;
    SearchForFiles::FilterContent filter;
    filter.text = "needle";
    search.SetFilterContent(filter);
    REQUIRE(search.Go(
        test_dir.directory,
        TestEnv().vfs_native,
        Options::SearchForFiles,
        {},
        {},
        nullptr,
        nullptr,
        [&](const SearchForFiles::SkippedLocation &_location) {
            skipped.emplace_back(
                SkippedSnapshot{_location.path, _location.host, _location.error, _location.context});
        }));
    search.Wait();
    REQUIRE(::chmod(path.c_str(), 0600) == 0);

    REQUIRE(skipped.size() == 1);
    CHECK(skipped.front().path == path);
    CHECK(skipped.front().error == nc::Error{nc::Error::POSIX, EACCES});
    CHECK(skipped.front().context == SearchForFiles::SkippedLocation::Context::Item);
}

TEST_CASE(PREFIX "Reports a failed search root")
{
    auto host = std::make_shared<ScriptedSearchHost>();
    host->SetFailure("/root", nc::Error{nc::Error::POSIX, EACCES});

    std::vector<SkippedSnapshot> skipped;
    bool finished = false;
    SearchForFiles search;
    REQUIRE(search.Go(
        "/root",
        host,
        SearchForFiles::Options::SearchForFiles,
        {},
        [&] { finished = true; },
        nullptr,
        nullptr,
        [&](const SearchForFiles::SkippedLocation &_location) {
            skipped.emplace_back(
                SkippedSnapshot{_location.path, _location.host, _location.error, _location.context});
        }));
    search.Wait();

    REQUIRE(finished);
    REQUIRE(skipped.size() == 1);
    CHECK(skipped.front().path == "/root");
    CHECK(skipped.front().host == host);
    CHECK(skipped.front().error == nc::Error{nc::Error::POSIX, EACCES});
    CHECK(skipped.front().context == SearchForFiles::SkippedLocation::Context::Root);
}

TEST_CASE(PREFIX "Reports a failed child and continues with partial results")
{
    using Entry = ScriptedSearchHost::Entry;
    auto host = std::make_shared<ScriptedSearchHost>();
    host->SetListing("/root", {{VFSDirEnt::Dir, "bad"}, {VFSDirEnt::Dir, "good"}, {VFSDirEnt::Reg, "root.txt"}});
    host->SetFailure("/root/bad", nc::Error{nc::Error::POSIX, EACCES});
    host->SetListing("/root/good", {Entry{VFSDirEnt::Reg, "nested.txt"}});

    std::set<std::string> found;
    std::vector<SkippedSnapshot> skipped;
    SearchForFiles search;
    REQUIRE(search.Go(
        "/root",
        host,
        SearchForFiles::Options::GoIntoSubDirs | SearchForFiles::Options::SearchForFiles,
        [&](std::string_view _filename, const char *, VFSHost &, CFRange) { found.emplace(_filename); },
        {},
        nullptr,
        nullptr,
        [&](const SearchForFiles::SkippedLocation &_location) {
            skipped.emplace_back(
                SkippedSnapshot{_location.path, _location.host, _location.error, _location.context});
        }));
    search.Wait();

    CHECK(found == std::set<std::string>{"nested.txt", "root.txt"});
    REQUIRE(skipped.size() == 1);
    CHECK(skipped.front().path == "/root/bad");
    CHECK(skipped.front().host == host);
    CHECK(skipped.front().error == nc::Error{nc::Error::POSIX, EACCES});
    CHECK(skipped.front().context == SearchForFiles::SkippedLocation::Context::Descendant);
    CHECK(host->Visited() == std::vector<std::string>{"/root", "/root/bad", "/root/good"});
}

TEST_CASE(PREFIX "Descend predicate blocks a subtree without reporting an error")
{
    using Entry = ScriptedSearchHost::Entry;
    auto host = std::make_shared<ScriptedSearchHost>();
    host->SetListing("/root", {{VFSDirEnt::Dir, "blocked"}, {VFSDirEnt::Dir, "allowed"}});
    host->SetListing("/root/blocked", {Entry{VFSDirEnt::Reg, "hidden.txt"}});
    host->SetListing("/root/allowed", {Entry{VFSDirEnt::Reg, "visible.txt"}});

    struct PredicateCall {
        std::string path;
        std::string name;
        VFSDirEnt::Type type;
        VFSHost *host;
    };
    std::vector<PredicateCall> predicate_calls;
    std::set<std::string> found;
    size_t skipped_count = 0;
    SearchForFiles search;
    REQUIRE(search.Go(
        "/root",
        host,
        SearchForFiles::Options::GoIntoSubDirs | SearchForFiles::Options::SearchForFiles,
        [&](std::string_view _filename, const char *, VFSHost &, CFRange) { found.emplace(_filename); },
        {},
        nullptr,
        nullptr,
        [&](const SearchForFiles::SkippedLocation &) { ++skipped_count; },
        [&](std::string_view _full_path, const VFSDirEnt &_dirent, VFSHost &_in_host) {
            predicate_calls.emplace_back(
                PredicateCall{std::string{_full_path}, std::string{_dirent.name}, _dirent.type, &_in_host});
            return _full_path != "/root/blocked";
        }));
    search.Wait();

    CHECK(found == std::set<std::string>{"visible.txt"});
    CHECK(skipped_count == 0);
    CHECK(host->Visited() == std::vector<std::string>{"/root", "/root/allowed"});
    REQUIRE(predicate_calls.size() == 2);
    CHECK(predicate_calls[0].path == "/root/blocked");
    CHECK(predicate_calls[0].name == "blocked");
    CHECK(predicate_calls[0].type == VFSDirEnt::Dir);
    CHECK(predicate_calls[0].host == host.get());
    CHECK(predicate_calls[1].path == "/root/allowed");
    CHECK(predicate_calls[1].name == "allowed");
    CHECK(predicate_calls[1].type == VFSDirEnt::Dir);
    CHECK(predicate_calls[1].host == host.get());
}

TEST_CASE(PREFIX "Legacy call ignores failures and a completed search can be reused")
{
    using Entry = ScriptedSearchHost::Entry;
    auto host = std::make_shared<ScriptedSearchHost>();
    host->SetFailure("/root", nc::Error{nc::Error::POSIX, EIO});

    size_t finish_count = 0;
    SearchForFiles search;
    REQUIRE(search.Go("/root", host, SearchForFiles::Options::SearchForFiles, {}, [&] { ++finish_count; }));
    search.Wait();
    CHECK(finish_count == 1);
    CHECK_FALSE(search.IsRunning());

    host->SetListing("/root", {Entry{VFSDirEnt::Reg, "after.txt"}});
    std::set<std::string> found;
    REQUIRE(search.Go(
        "/root",
        host,
        SearchForFiles::Options::SearchForFiles,
        [&](std::string_view _filename, const char *, VFSHost &, CFRange) { found.emplace(_filename); },
        [&] { ++finish_count; }));
    search.Wait();

    CHECK(found == std::set<std::string>{"after.txt"});
    CHECK(finish_count == 2);
    CHECK_FALSE(search.IsStopped());
}

TEST_CASE(PREFIX "Cancellation suppresses skipped-location diagnostics and still finishes")
{
    auto host = std::make_shared<ScriptedSearchHost>();
    host->SetFailure("/root", nc::Error{nc::Error::POSIX, EIO});

    size_t skipped_count = 0;
    size_t finish_count = 0;
    SearchForFiles search;
    REQUIRE(search.Go(
        "/root",
        host,
        SearchForFiles::Options::SearchForFiles,
        {},
        [&] { ++finish_count; },
        [&](const char *, VFSHost &) { search.Stop(); },
        nullptr,
        [&](const SearchForFiles::SkippedLocation &) { ++skipped_count; }));
    search.Wait();

    CHECK(skipped_count == 0);
    CHECK(finish_count == 1);
    CHECK_FALSE(search.IsRunning());
    CHECK_FALSE(search.IsStopped());
}

static void BuildTestData(const std::string &_root_path)
{
    Save(_root_path + "filename1.txt", "Hello, world!");
    Save(_root_path + "filename2.txt", reinterpret_cast<const char *>(u8"Привет, мир!"));
    MkDir(_root_path + "Dir");
    Save(_root_path + "Dir/filename3.txt", "Almost edge of the world!");
}

static bool Save(const std::string &_filepath, const std::string &_content)
{
    std::ofstream out(_filepath, std::ios::out | std::ios::binary);
    if( !out )
        return false;
    out << _content;
    out.close();
    return true;
}

static bool MkDir(const std::string &_dir_path)
{
    return mkdir(_dir_path.c_str(), S_IRWXU) != 0;
}
