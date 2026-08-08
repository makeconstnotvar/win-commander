// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Git/GitStatusReader.h>

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <filesystem>
#include <fstream>
#include <string>

extern char **environ;

namespace {

using nc::core::GitFileState;
using nc::core::GitStatusReadError;
using nc::core::GitStatusReadLimits;
using nc::core::ReadGitStatus;
using namespace std::chrono_literals;

/**
 * Runs a git command in a directory, returning true on success.
 *
 * Spawned with an explicit argument vector rather than through a shell - the same care the code
 * under test takes, and for the same reason: one of these tests deliberately uses a directory name
 * full of shell syntax, and a helper that pasted it into a command line would fail on the fixture
 * rather than on the behaviour.
 */
bool Git(const std::filesystem::path &_directory, const std::vector<std::string> &_arguments)
{
    std::vector<std::string> storage{"git", "-C", _directory.native()};
    storage.insert(storage.end(), _arguments.begin(), _arguments.end());
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for( std::string &argument : storage )
        argv.push_back(argument.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if( ::posix_spawn_file_actions_init(&actions) != 0 )
        return false;
    ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t child = -1;
    const int spawn_rc = ::posix_spawn(&child, "/usr/bin/git", &actions, nullptr, argv.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    if( spawn_rc != 0 )
        return false;

    int status = 0;
    while( ::waitpid(child, &status, 0) < 0 && errno == EINTR )
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/** A repository with an identity configured, so committing works on a bare test machine. */
std::filesystem::path MakeRepository(const std::filesystem::path &_base, const std::string &_name)
{
    const std::filesystem::path root = _base / _name;
    std::filesystem::create_directories(root);
    REQUIRE(Git(root, {"init", "-q", "."}));
    REQUIRE(Git(root, {"config", "user.email", "test@example.org"}));
    REQUIRE(Git(root, {"config", "user.name", "Test"}));
    return root;
}

[[nodiscard]] std::optional<GitFileState> StateOf(const std::vector<nc::core::GitStatusEntry> &_entries,
                                                  const std::string &_path)
{
    for( const auto &entry : _entries )
        if( entry.path == _path )
            return entry.state;
    return std::nullopt;
}

} // namespace

#define PREFIX "nc::core::ReadGitStatus "

TEST_CASE(PREFIX "reads a real repository and reports the root it read")
{
    const TempTestDir tmp_dir;
    const std::filesystem::path root = MakeRepository(tmp_dir.directory, "repo");
    std::ofstream(root / "committed.txt") << "one";
    REQUIRE(Git(root, {"add", "committed.txt"}));
    REQUIRE(Git(root, {"commit", "-q", "-m", "first"}));

    std::ofstream(root / "committed.txt") << "two";
    std::ofstream(root / "fresh.txt") << "new";
    std::filesystem::create_directory(root / "sub");
    std::ofstream(root / "sub" / "deep.txt") << "deep";

    // Asked from a subdirectory: discovery finds the root, and the answer is about the whole
    // repository rather than the folder that happened to be open.
    const auto snapshot = ReadGitStatus(root / "sub");
    REQUIRE(snapshot);
    CHECK(snapshot->root == std::filesystem::canonical(root));
    CHECK(StateOf(snapshot->entries, "committed.txt") == GitFileState::Modified);
    CHECK(StateOf(snapshot->entries, "fresh.txt") == GitFileState::Untracked);
}

TEST_CASE(PREFIX "survives a path that a line-based reader would split in two")
{
    // The reason GT-1 insists on the NUL-separated form. A newline is legal in a filename, and a
    // line-based pipeline would turn one path into two entries and mis-badge whatever it collided
    // with. This is the end-to-end proof, through a real git.
    const TempTestDir tmp_dir;
    const std::filesystem::path root = MakeRepository(tmp_dir.directory, "repo");
    const std::string awkward = "line\none more.txt";
    std::ofstream(root / awkward) << "x";

    const auto snapshot = ReadGitStatus(root);
    REQUIRE(snapshot);
    CHECK(StateOf(snapshot->entries, awkward) == GitFileState::Untracked);
}

TEST_CASE(PREFIX "ignores a GIT_DIR pointing somewhere else entirely")
{
    // Inherited from whoever launched the application, this would silently make every badge describe
    // a different working tree.
    const TempTestDir tmp_dir;
    const std::filesystem::path real = MakeRepository(tmp_dir.directory, "real");
    const std::filesystem::path decoy = MakeRepository(tmp_dir.directory, "decoy");
    std::ofstream(real / "in-real.txt") << "x";
    std::ofstream(decoy / "in-decoy.txt") << "x";

    ::setenv("GIT_DIR", (decoy / ".git").c_str(), 1);
    ::setenv("GIT_WORK_TREE", decoy.c_str(), 1);
    const auto snapshot = ReadGitStatus(real);
    ::unsetenv("GIT_DIR");
    ::unsetenv("GIT_WORK_TREE");

    REQUIRE(snapshot);
    CHECK(StateOf(snapshot->entries, "in-real.txt") == GitFileState::Untracked);
    CHECK(StateOf(snapshot->entries, "in-decoy.txt") == std::nullopt);
}

TEST_CASE(PREFIX "reads a repository whose path is full of shell syntax")
{
    // An explicit argument vector rather than a shell command line is what makes this a filename
    // instead of a sequence of operators.
    const TempTestDir tmp_dir;
    const std::filesystem::path root = MakeRepository(tmp_dir.directory, "a dir; rm -rf $x 'quoted' & odd");
    std::ofstream(root / "file.txt") << "x";

    const auto snapshot = ReadGitStatus(root);
    REQUIRE(snapshot);
    CHECK(StateOf(snapshot->entries, "file.txt") == GitFileState::Untracked);
}

TEST_CASE(PREFIX "refuses a directory that is not in a repository")
{
    const TempTestDir tmp_dir;
    const std::filesystem::path plain = std::filesystem::path{tmp_dir.directory} / "plain";
    REQUIRE(std::filesystem::create_directory(plain));

    const auto snapshot = ReadGitStatus(plain);
    REQUIRE_FALSE(snapshot);
    // Never launched git at all - there was nothing to ask it about.
    CHECK(snapshot.error() == GitStatusReadError::NotARepository);
}

TEST_CASE(PREFIX "reports an over-budget repository rather than truncating its status")
{
    // A truncated status is indistinguishable from a clean tree for the paths it omits, which is the
    // silent wrong answer GT-1 refuses to give. The bound must therefore be reported, not applied.
    const TempTestDir tmp_dir;
    const std::filesystem::path root = MakeRepository(tmp_dir.directory, "repo");
    for( int i = 0; i < 200; ++i )
        std::ofstream(root / ("file-with-a-fairly-long-name-" + std::to_string(i) + ".txt")) << "x";

    const auto snapshot = ReadGitStatus(root, GitStatusReadLimits{.timeout = 5'000ms, .maximum_output_bytes = 64});
    REQUIRE_FALSE(snapshot);
    CHECK(snapshot.error() == GitStatusReadError::OutputTooLarge);

    // The same repository is read fine with a budget that fits it.
    CHECK(ReadGitStatus(root).has_value());
}

TEST_CASE(PREFIX "gives up on a repository it cannot read in time")
{
    const TempTestDir tmp_dir;
    const std::filesystem::path root = MakeRepository(tmp_dir.directory, "repo");
    std::ofstream(root / "file.txt") << "x";

    // A budget nothing can meet. A refresh that ran without limit would turn one unlucky folder into
    // a permanently stuck listing.
    const auto snapshot = ReadGitStatus(root, GitStatusReadLimits{.timeout = 0ms, .maximum_output_bytes = 1 << 20});
    REQUIRE_FALSE(snapshot);
    CHECK(snapshot.error() == GitStatusReadError::TimedOut);
}

#undef PREFIX
