// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Git/GitRepositoryDiscovery.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace {

using nc::core::FindGitRepositoryRoot;
using nc::core::GitDiscoveryProbe;
using nc::core::NativeGitDiscoveryProbe;

/** A filesystem described by which directories hold a `.git`, and which device each one is on. */
struct FakeFilesystem {
    std::set<std::string> repositories;
    std::map<std::string, uint64_t> devices;
    uint64_t default_device = 1;
    /** Directories whose device cannot be read at all. */
    std::set<std::string> unreadable;

    [[nodiscard]] GitDiscoveryProbe Probe() const
    {
        return GitDiscoveryProbe{
            .has_git_entry = [this](const std::filesystem::path &_dir) { return repositories.contains(_dir.native()); },
            .device_id =
                [this](const std::filesystem::path &_dir) -> std::optional<uint64_t> {
                if( unreadable.contains(_dir.native()) )
                    return std::nullopt;
                const auto found = devices.find(_dir.native());
                return found == devices.end() ? default_device : found->second;
            },
        };
    }
};

} // namespace

#define PREFIX "nc::core::FindGitRepositoryRoot "

TEST_CASE(PREFIX "walks up to the nearest enclosing repository")
{
    FakeFilesystem fs;
    fs.repositories = {"/Users/me/project"};

    CHECK(FindGitRepositoryRoot("/Users/me/project/src/deep", fs.Probe()) == "/Users/me/project");
    CHECK(FindGitRepositoryRoot("/Users/me/project", fs.Probe()) == "/Users/me/project");
    CHECK(FindGitRepositoryRoot("/Users/me", fs.Probe()) == std::nullopt);
    CHECK(FindGitRepositoryRoot("/", fs.Probe()) == std::nullopt);
}

TEST_CASE(PREFIX "stops at the nearest one, so a submodule is its own repository")
{
    FakeFilesystem fs;
    fs.repositories = {"/Users/me/project", "/Users/me/project/vendor/lib"};

    // Nearest wins. Reporting the outer repository would badge the submodule's files against a
    // status that knows nothing about them.
    CHECK(FindGitRepositoryRoot("/Users/me/project/vendor/lib/src", fs.Probe()) == "/Users/me/project/vendor/lib");
    CHECK(FindGitRepositoryRoot("/Users/me/project/vendor", fs.Probe()) == "/Users/me/project");
}

TEST_CASE(PREFIX "does not walk out of the filesystem it started in")
{
    // A volume mounted inside a checkout is not part of that checkout. Letting it inherit the
    // repository would badge files git knows nothing about - which is why git itself does not cross
    // filesystems when discovering a repository either.
    FakeFilesystem fs;
    fs.repositories = {"/Users/me/project"};
    fs.devices["/Users/me/project/mnt"] = 2;
    fs.devices["/Users/me/project/mnt/data"] = 2;

    CHECK(FindGitRepositoryRoot("/Users/me/project/mnt/data", fs.Probe()) == std::nullopt);
    // The repository itself is still found from inside its own filesystem.
    CHECK(FindGitRepositoryRoot("/Users/me/project/src", fs.Probe()) == "/Users/me/project");
}

TEST_CASE(PREFIX "a mount point that is itself a repository is still found")
{
    // The boundary rule must not hide a repository that begins exactly at the mount point - the walk
    // starts there, so there is no boundary to cross.
    FakeFilesystem fs;
    fs.repositories = {"/Volumes/work"};
    fs.devices["/Volumes/work"] = 2;
    fs.devices["/Volumes/work/src"] = 2;

    CHECK(FindGitRepositoryRoot("/Volumes/work/src", fs.Probe()) == "/Volumes/work");
    CHECK(FindGitRepositoryRoot("/Volumes/work", fs.Probe()) == "/Volumes/work");
}

TEST_CASE(PREFIX "an unreadable directory ends the walk instead of being stepped over")
{
    // Continuing past a directory we could not check would mean crossing a boundary we failed to
    // look for, which is the thing the boundary rule exists to prevent.
    FakeFilesystem fs;
    fs.repositories = {"/Users/me/project"};
    fs.unreadable = {"/Users/me/project/private"};

    CHECK(FindGitRepositoryRoot("/Users/me/project/private/inner", fs.Probe()) == std::nullopt);
    // And an unreadable starting point answers nothing rather than guessing.
    CHECK(FindGitRepositoryRoot("/Users/me/project/private", fs.Probe()) == std::nullopt);
}

TEST_CASE(PREFIX "refuses a relative path rather than resolving it against somewhere else")
{
    FakeFilesystem fs;
    fs.repositories = {"/Users/me/project"};

    // The caller's "here" is a panel's directory. Resolving a relative path against the process
    // working directory would answer confidently about a completely different place.
    CHECK(FindGitRepositoryRoot("project/src", fs.Probe()) == std::nullopt);
    CHECK(FindGitRepositoryRoot("", fs.Probe()) == std::nullopt);
    CHECK(FindGitRepositoryRoot(".", fs.Probe()) == std::nullopt);

    // A probe that cannot answer is not a reason to guess either.
    CHECK(FindGitRepositoryRoot("/Users/me/project", GitDiscoveryProbe{}) == std::nullopt);
}

TEST_CASE(PREFIX "normalizes a path before walking it")
{
    FakeFilesystem fs;
    fs.repositories = {"/Users/me/project"};

    CHECK(FindGitRepositoryRoot("/Users/me/project/src/../src", fs.Probe()) == "/Users/me/project");
    CHECK(FindGitRepositoryRoot("/Users/me/project/./src", fs.Probe()) == "/Users/me/project");
    CHECK(FindGitRepositoryRoot("/Users/me/project/src/", fs.Probe()) == "/Users/me/project");
}

TEST_CASE(PREFIX "accepts a .git that is a file, not only a directory")
{
    // A linked worktree and a submodule both have a `.git` *file* pointing elsewhere. Accepting only
    // a directory would report those as not repositories at all.
    const TempTestDir tmp_dir;
    const std::filesystem::path worktree = std::filesystem::path{tmp_dir.directory} / "worktree";
    const std::filesystem::path inner = worktree / "src";
    REQUIRE(std::filesystem::create_directories(inner));
    std::ofstream(worktree / ".git") << "gitdir: /elsewhere/.git/worktrees/wt\n";

    CHECK(FindGitRepositoryRoot(inner, NativeGitDiscoveryProbe()) == worktree);

    const std::filesystem::path plain = std::filesystem::path{tmp_dir.directory} / "plain";
    REQUIRE(std::filesystem::create_directory(plain));
    CHECK(FindGitRepositoryRoot(plain, NativeGitDiscoveryProbe()) == std::nullopt);
}

TEST_CASE(PREFIX "does not accept a dangling symlink as a repository marker")
{
    // A `.git` pointing at nothing is not a usable marker, and treating it as one would send every
    // later git call at a repository that is not there.
    const TempTestDir tmp_dir;
    const std::filesystem::path directory = std::filesystem::path{tmp_dir.directory} / "broken";
    REQUIRE(std::filesystem::create_directory(directory));
    std::filesystem::create_symlink("/nonexistent/target", directory / ".git");

    CHECK(FindGitRepositoryRoot(directory, NativeGitDiscoveryProbe()) == std::nullopt);
}

#undef PREFIX
