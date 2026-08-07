// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <Utility/FSEventsFileUpdate.h>
#include <Utility/NativeFSManager.h>
#include <VFS/Native.h>
#include <WinCommander/Core/Search/SearchPlanning.h>
#include <WinCommander/States/Explorer/ExplorerDirectSearchBackend.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <optional>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace nc::core;
using namespace nc::explorer;

class DirectSearchNativeFSManager final : public nc::utility::NativeFSManager
{
public:
    std::vector<Info> Volumes() const override { return {}; }
    Info VolumeFromFD(int) const noexcept override { return nullptr; }
    Info VolumeFromPath(std::string_view) const noexcept override { return nullptr; }
    Info VolumeFromPathFast(std::string_view) const noexcept override { return nullptr; }
    Info VolumeFromMountPoint(std::string_view) const noexcept override { return nullptr; }
    void UpdateSpaceInformation(const Info &) override {}
    void EjectVolumeContainingPath(const std::string &) override {}
    bool IsVolumeContainingPathEjectable(const std::string &) override { return false; }
};

class DirectSearchFSEvents final : public nc::utility::FSEventsFileUpdate
{
public:
    uint64_t AddWatchPath(const std::filesystem::path &, std::function<void()>) override { return 0; }
    void RemoveWatchPathWithToken(uint64_t) override {}
};

class RootFailureHost final : public nc::vfs::Host
{
public:
    RootFailureHost() : Host("/", nullptr, "direct-search-root-failure") {}

    std::expected<void, nc::Error>
    IterateDirectoryListing(std::string_view, const std::function<bool(const VFSDirEnt &)> &) override
    {
        return std::unexpected(nc::Error{nc::Error::POSIX, EIO});
    }
};

class BlockingSearchHost final : public nc::vfs::Host
{
public:
    BlockingSearchHost() : Host("/", nullptr, "direct-search-blocking") {}

    std::expected<void, nc::Error>
    IterateDirectoryListing(std::string_view, const std::function<bool(const VFSDirEnt &)> &) override
    {
        std::unique_lock lock{mutex};
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return released; });
        return {};
    }

    bool WaitUntilEntered()
    {
        std::unique_lock lock{mutex};
        return condition.wait_for(lock, std::chrono::seconds{1}, [&] { return entered; });
    }

    void Release()
    {
        const std::lock_guard lock{mutex};
        released = true;
        condition.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
};

class CurrentDiskPermissionHost final : public nc::vfs::Host
{
public:
    CurrentDiskPermissionHost() : Host("/", nullptr, "direct-search-current-disk-permission") {}

    std::expected<VFSStat, nc::Error>
    Stat(std::string_view _path, unsigned long, const VFSCancelChecker &) override
    {
        if( _path == "/root" ) {
            VFSStat stat;
            stat.dev = 17;
            stat.mode = S_IFDIR | 0755;
            stat.meaning.dev = 1;
            stat.meaning.mode = 1;
            return stat;
        }
        return std::unexpected(nc::Error{nc::Error::POSIX, EACCES});
    }

    std::expected<void, nc::Error>
    IterateDirectoryListing(std::string_view _path,
                            const std::function<bool(const VFSDirEnt &)> &_handler) override
    {
        if( _path != "/root" )
            return std::unexpected(nc::Error{nc::Error::POSIX, EACCES});
        const VFSDirEnt denied{.type = VFSDirEnt::Dir, .name = "denied"};
        static_cast<void>(_handler(denied));
        return {};
    }
};

void Save(const std::filesystem::path &_path, const std::string &_content)
{
    std::ofstream stream{_path, std::ios::binary};
    REQUIRE(stream);
    stream << _content;
    REQUIRE(stream.good());
}

class UniqueTempDirectory final
{
public:
    UniqueTempDirectory()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "wc-direct-search-XXXXXX").string();
        std::vector<char> storage(pattern.begin(), pattern.end());
        storage.push_back('\0');
        const char *const created = ::mkdtemp(storage.data());
        if( !created )
            throw std::runtime_error{"mkdtemp failed"};
        path = created;
    }

    ~UniqueTempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    UniqueTempDirectory(const UniqueTempDirectory &) = delete;
    UniqueTempDirectory &operator=(const UniqueTempDirectory &) = delete;

    std::filesystem::path path;
};

struct NativeFixture {
    NativeFixture()
    {
        host = std::make_shared<nc::vfs::NativeHost>(native_fs, events);
        root = temporary.path;
        Save(root / "visible.txt", "needle visible payload");
        Save(root / "exact.pdf", "123456789");
        Save(root / ".hidden.txt", "needle hidden payload");
        std::filesystem::create_directory(root / "sub");
        Save(root / "sub" / "nested.txt", "needle nested payload");
        std::filesystem::create_directory(root / "folder.app");
        REQUIRE(::symlink("visible.txt", (root / "visible-link").c_str()) == 0);
    }

    DirectSearchNativeFSManager native_fs;
    DirectSearchFSEvents events;
    UniqueTempDirectory temporary;
    std::filesystem::path root;
    std::shared_ptr<nc::vfs::NativeHost> host;
};

SearchPlanningFacts Facts(const std::string &_root)
{
    return {
        .current_folder = _root,
        .current_disk_root = "/",
        .provider_available = true,
        .provider_is_native = true,
        .provider_supports_recursive = true,
        .provider_supports_current_disk = true,
        .provider_supports_metadata = true,
        .provider_supports_content = true,
        .provider_supports_hidden_items = true,
        .provider_reports_determinate_progress = false,
    };
}

ExplorerSearchBackendCompletion Execute(const VFSHostPtr &_host,
                                        const std::string &_root,
                                        SearchRequest _request,
                                        ExplorerSearchBackendLimits _limits = {})
{
    const auto plan = SearchPlanning::Plan(std::move(_request), Facts(_root));
    REQUIRE(plan);
    REQUIRE(plan->backend.support == SearchBackendSupport::Supported);
    std::optional<ExplorerSearchBackendCompletion> completion;
    ExplorerDirectSearchBackend backend;
    auto run = backend.Start(
        {.plan = *plan, .origin_host = _host, .limits = _limits},
        [](ExplorerSearchBackendProgress) {},
        [&](ExplorerSearchBackendCompletion _completion) { completion = std::move(_completion); });
    REQUIRE(run);
    run->Wait();
    REQUIRE(completion);
    return std::move(*completion);
}

std::set<std::string> Paths(const ExplorerSearchBackendCompletion &_completion)
{
    std::set<std::string> paths;
    if( !_completion.listing )
        return paths;
    for( unsigned index = 0; index != _completion.listing->Count(); ++index )
        paths.emplace(_completion.listing->Path(index));
    return paths;
}

SearchRequest Request(std::string _query, const SearchScope _scope = SearchScope::CurrentFolder)
{
    return {.query = std::move(_query), .scope = _scope};
}

} // namespace

#define PREFIX "ExplorerDirectSearchBackend "

TEST_CASE(PREFIX "distinguishes current-folder and recursive scopes")
{
    NativeFixture fixture;
    const std::string root = fixture.root;

    const auto folder = Execute(fixture.host, root, Request(".txt"));
    REQUIRE(folder.kind == ExplorerSearchBackendCompletionKind::Completed);
    CHECK(Paths(folder) == std::set<std::string>{root + "/visible.txt"});

    const auto recursive = Execute(fixture.host, root, Request(".txt", SearchScope::Recursive));
    REQUIRE(recursive.kind == ExplorerSearchBackendCompletionKind::Completed);
    CHECK(Paths(recursive) ==
          std::set<std::string>{root + "/sub/nested.txt", root + "/visible.txt"});
}

TEST_CASE(PREFIX "applies exact extension type size modified content and hidden filters")
{
    NativeFixture fixture;
    const std::string root = fixture.root;

    SECTION("exact name")
    {
        auto request = Request("exact.pdf");
        request.filters.name_match = SearchNameMatch::Exact;
        CHECK(Paths(Execute(fixture.host, root, request)) == std::set<std::string>{root + "/exact.pdf"});
    }

    SECTION("extension")
    {
        auto request = Request("exact");
        request.filters.extension = "pdf";
        CHECK(Paths(Execute(fixture.host, root, request)) == std::set<std::string>{root + "/exact.pdf"});
        request.filters.extension = "*";
        CHECK(Paths(Execute(fixture.host, root, request)).empty());
    }

    SECTION("regular size")
    {
        auto request = Request("exact.pdf");
        request.filters.file_type = SearchFileType::RegularFile;
        request.filters.size = {.minimum_bytes = 9, .maximum_bytes = 9};
        CHECK(Paths(Execute(fixture.host, root, request)) == std::set<std::string>{root + "/exact.pdf"});
    }

    SECTION("directory and package")
    {
        auto directory = Request("sub");
        directory.filters.file_type = SearchFileType::Directory;
        CHECK(Paths(Execute(fixture.host, root, directory)) == std::set<std::string>{root + "/sub"});

        auto package = Request("folder.app");
        package.filters.name_match = SearchNameMatch::Exact;
        package.filters.file_type = SearchFileType::Package;
        CHECK(Paths(Execute(fixture.host, root, package)) == std::set<std::string>{root + "/folder.app"});
    }

    SECTION("symbolic link")
    {
        auto request = Request("visible-link");
        request.filters.file_type = SearchFileType::SymbolicLink;
        CHECK(Paths(Execute(fixture.host, root, request)) == std::set<std::string>{root + "/visible-link"});
    }

    SECTION("modified")
    {
        struct ::stat stat_buffer = {};
        REQUIRE(::stat((fixture.root / "visible.txt").c_str(), &stat_buffer) == 0);
        auto request = Request("visible.txt");
        request.filters.modified = {.earliest_seconds = stat_buffer.st_mtimespec.tv_sec,
                                    .latest_seconds = stat_buffer.st_mtimespec.tv_sec};
        CHECK(Paths(Execute(fixture.host, root, request)) == std::set<std::string>{root + "/visible.txt"});
    }

    SECTION("content")
    {
        auto request = Request("visible.txt");
        request.filters.content = "needle visible";
        CHECK(Paths(Execute(fixture.host, root, request)) == std::set<std::string>{root + "/visible.txt"});
        request.filters.content = "absent content";
        CHECK(Paths(Execute(fixture.host, root, request)).empty());
    }

    SECTION("content permission")
    {
        Save(fixture.root / "private.txt", "needle private payload");
        REQUIRE(::chmod((fixture.root / "private.txt").c_str(), 0000) == 0);
        auto request = Request("private.txt");
        request.filters.name_match = SearchNameMatch::Exact;
        request.filters.content = "needle";
        const auto result = Execute(fixture.host, root, request);
        CHECK(result.kind == ExplorerSearchBackendCompletionKind::PermissionLimited);
        CHECK(result.limitations ==
              std::vector<SearchBackendLimitation>{SearchBackendLimitation::PermissionDeniedLocations});
        REQUIRE(result.listing);
        CHECK(result.listing->Empty());
    }

    SECTION("hidden")
    {
        auto request = Request("hidden.txt");
        CHECK(Paths(Execute(fixture.host, root, request)).empty());
        request.filters.include_hidden = true;
        CHECK(Paths(Execute(fixture.host, root, request)) == std::set<std::string>{root + "/.hidden.txt"});
    }
}

TEST_CASE(PREFIX "bounds results and maps cancellation and root provider failure")
{
    NativeFixture fixture;
    const std::string root = fixture.root;

    SECTION("result bound")
    {
        const auto result = Execute(fixture.host,
                                    root,
                                    Request(".txt", SearchScope::Recursive),
                                    {.maximum_results = 1, .maximum_path_bytes = 1024});
        CHECK(result.kind == ExplorerSearchBackendCompletionKind::TooManyResults);
        REQUIRE(result.listing);
        CHECK(result.listing->Count() == 1);
    }

    SECTION("cancellation")
    {
        auto host = std::make_shared<BlockingSearchHost>();
        auto request = Request("anything");
        const auto plan = SearchPlanning::Plan(request, Facts("/blocked-root"));
        REQUIRE(plan);
        std::optional<ExplorerSearchBackendCompletion> completion;
        ExplorerDirectSearchBackend backend;
        auto run = backend.Start({.plan = *plan, .origin_host = host},
                                 [](ExplorerSearchBackendProgress) {},
                                 [&](ExplorerSearchBackendCompletion _completion) {
                                   completion = std::move(_completion);
                                 });
        REQUIRE(run);
        const bool entered = host->WaitUntilEntered();
        if( !entered )
            host->Release();
        REQUIRE(entered);
        run->Stop();
        host->Release();
        run->Wait();
        REQUIRE(completion);
        CHECK(completion->kind == ExplorerSearchBackendCompletionKind::Cancelled);
        CHECK_FALSE(completion->listing);
    }

    SECTION("root failure")
    {
        auto host = std::make_shared<RootFailureHost>();
        const auto result = Execute(host, "/failed-root", Request("anything"));
        CHECK(result.kind == ExplorerSearchBackendCompletionKind::Failed);
        REQUIRE(result.failure);
        CHECK(result.failure->code == SearchFailureCode::ExecutionFailed);
        CHECK_FALSE(result.listing);
    }

    SECTION("current-disk permission fence")
    {
        auto host = std::make_shared<CurrentDiskPermissionHost>();
        SearchRequest request = Request("anything", SearchScope::CurrentDisk);
        auto facts = Facts("/origin");
        facts.current_disk_root = "/root";
        const auto plan = SearchPlanning::Plan(request, facts);
        REQUIRE(plan);
        std::optional<ExplorerSearchBackendCompletion> completion;
        ExplorerDirectSearchBackend backend;
        auto run = backend.Start({.plan = *plan, .origin_host = host},
                                 [](ExplorerSearchBackendProgress) {},
                                 [&](ExplorerSearchBackendCompletion _completion) {
                                   completion = std::move(_completion);
                                 });
        REQUIRE(run);
        run->Wait();
        REQUIRE(completion);
        CHECK(completion->kind == ExplorerSearchBackendCompletionKind::PermissionLimited);
        CHECK(completion->limitations ==
              std::vector<SearchBackendLimitation>{SearchBackendLimitation::PermissionDeniedLocations});
        REQUIRE(completion->listing);
        CHECK(completion->listing->Empty());
    }
}

#undef PREFIX
