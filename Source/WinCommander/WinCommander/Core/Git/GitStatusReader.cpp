// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "GitStatusReader.h"

#include "GitRepositoryDiscovery.h"

#include <fcntl.h>
#include <spawn.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#include <array>
#include <cerrno>
#include <vector>
#include <cstring>
#include <string>

extern char **environ;

namespace nc::core {

namespace {

constexpr const char *g_GitExecutable = "/usr/bin/git";

/** The environment minus every `GIT_*` variable, as a null-terminated argv-style block. */
class SanitizedEnvironment
{
public:
    SanitizedEnvironment()
    {
        for( char **entry = environ; entry != nullptr && *entry != nullptr; ++entry ) {
            // A GIT_DIR or GIT_WORK_TREE inherited from whoever launched the application would point
            // git at a different repository, and the badges would describe someone else's tree.
            if( std::strncmp(*entry, "GIT_", 4) == 0 )
                continue;
            m_Storage.emplace_back(*entry);
        }
        m_Pointers.reserve(m_Storage.size() + 1);
        for( std::string &entry : m_Storage )
            m_Pointers.push_back(entry.data());
        m_Pointers.push_back(nullptr);
    }

    [[nodiscard]] char **Get() noexcept { return m_Pointers.data(); }

private:
    std::vector<std::string> m_Storage;
    std::vector<char *> m_Pointers;
};

/** Closes a descriptor once, and never a negative one. */
class OwnedFD
{
public:
    explicit OwnedFD(const int _fd = -1) noexcept : m_FD{_fd} {}
    OwnedFD(const OwnedFD &) = delete;
    OwnedFD &operator=(const OwnedFD &) = delete;
    ~OwnedFD() { Reset(); }

    [[nodiscard]] int Get() const noexcept { return m_FD; }
    void Reset() noexcept
    {
        if( m_FD >= 0 )
            ::close(m_FD);
        m_FD = -1;
    }

private:
    int m_FD;
};

/** Waits for the child, stopping it first when it is still running. */
void ReapChild(const pid_t _pid, const bool _kill_first) noexcept
{
    if( _kill_first )
        ::kill(_pid, SIGKILL);
    int status = 0;
    while( ::waitpid(_pid, &status, 0) < 0 && errno == EINTR )
        ;
}

} // namespace

std::expected<GitStatusSnapshot, GitStatusReadError> ReadGitStatus(const std::filesystem::path &_directory,
                                                                   const GitStatusReadLimits &_limits)
{
    const std::optional<std::filesystem::path> root = FindGitRepositoryRoot(_directory);
    if( !root )
        return std::unexpected(GitStatusReadError::NotARepository);

    std::array<int, 2> pipe_fds{-1, -1};
    if( ::pipe(pipe_fds.data()) != 0 )
        return std::unexpected(GitStatusReadError::LaunchFailed);
    OwnedFD read_end{pipe_fds[0]};
    OwnedFD write_end{pipe_fds[1]};

    posix_spawn_file_actions_t actions;
    if( ::posix_spawn_file_actions_init(&actions) != 0 )
        return std::unexpected(GitStatusReadError::LaunchFailed);
    // stdout to the pipe; stdin and stderr to /dev/null. git must never be able to ask a question -
    // a credential or editor prompt would block a badge refresh forever.
    bool actions_ok = ::posix_spawn_file_actions_adddup2(&actions, write_end.Get(), STDOUT_FILENO) == 0 &&
                      ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
                      ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
                      ::posix_spawn_file_actions_addclose(&actions, read_end.Get()) == 0;

    const std::string root_argument = root->native();
    // An explicit argument vector, never a shell: a repository path containing spaces, quotes or
    // newlines cannot then be re-read as syntax.
    const std::array<const char *, 8> argv{
        "git",
        "-C",
        root_argument.c_str(),
        // A badge refresh must not take the index lock. Refreshing the index behind the user's back
        // is how a background redraw ends up fighting the git command they typed themselves.
        "--no-optional-locks",
        "status",
        "--porcelain=v1",
        "-z",
        nullptr,
    };

    SanitizedEnvironment environment;
    pid_t child = -1;
    const int spawn_rc =
        actions_ok ? ::posix_spawn(&child,
                                   g_GitExecutable,
                                   &actions,
                                   nullptr,
                                   const_cast<char *const *>(argv.data()), // NOLINT posix_spawn's signature
                                   environment.Get())
                   : -1;
    ::posix_spawn_file_actions_destroy(&actions);
    if( !actions_ok || spawn_rc != 0 )
        return std::unexpected(GitStatusReadError::LaunchFailed);

    // Closed in the parent so the read end sees EOF when git exits. Leaving it open is how this
    // would block until the timeout on every single call.
    write_end.Reset();

    const auto deadline = std::chrono::steady_clock::now() + _limits.timeout;
    std::string output;
    // On the heap: a read buffer this size on the stack overruns the project's frame limit, and the
    // limit is right to complain - this runs on whatever thread a listing refresh happens to use.
    std::vector<char> buffer(64 * 1024);
    while( true ) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if( remaining.count() <= 0 ) {
            ReapChild(child, true);
            return std::unexpected(GitStatusReadError::TimedOut);
        }

        pollfd poll_fd{.fd = read_end.Get(), .events = POLLIN, .revents = 0};
        const int poll_rc = ::poll(&poll_fd, 1, static_cast<int>(remaining.count()));
        if( poll_rc == 0 ) {
            ReapChild(child, true);
            return std::unexpected(GitStatusReadError::TimedOut);
        }
        if( poll_rc < 0 ) {
            if( errno == EINTR )
                continue;
            ReapChild(child, true);
            return std::unexpected(GitStatusReadError::LaunchFailed);
        }

        const ssize_t read_rc = ::read(read_end.Get(), buffer.data(), buffer.size());
        if( read_rc < 0 ) {
            if( errno == EINTR || errno == EAGAIN )
                continue;
            ReapChild(child, true);
            return std::unexpected(GitStatusReadError::LaunchFailed);
        }
        if( read_rc == 0 )
            break;

        // Reported rather than truncated. A truncated status is indistinguishable from a clean tree
        // for the paths it omits, which is the silent wrong answer GT-1 refuses to give.
        if( output.size() + static_cast<size_t>(read_rc) > _limits.maximum_output_bytes ) {
            ReapChild(child, true);
            return std::unexpected(GitStatusReadError::OutputTooLarge);
        }
        output.append(buffer.data(), static_cast<size_t>(read_rc));
    }

    int status = 0;
    while( ::waitpid(child, &status, 0) < 0 && errno == EINTR )
        ;
    if( !WIFEXITED(status) || WEXITSTATUS(status) != 0 )
        return std::unexpected(GitStatusReadError::GitFailed);

    std::optional<std::vector<GitStatusEntry>> entries = ParseGitPorcelainV1(output);
    if( !entries )
        return std::unexpected(GitStatusReadError::Unparsable);

    return GitStatusSnapshot{.root = *root, .entries = std::move(*entries)};
}

} // namespace nc::core
