// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Remote/RemoteConnectionAttempt.h>

#include <VFS/NetSFTP.h>
#include <VFS/VFSGenericMemReadOnlyFile.h>

#include <cerrno>
#include <stdexcept>

namespace {

using nc::Error;
using nc::ErrorException;
using nc::core::RecordConnectionAttempt;
using nc::core::RemoteConnectionFailure;
using nc::core::RemoteConnectionRegistry;
using nc::core::RemoteConnectionStatus;
using nc::core::RemoteRetryPolicy;
using namespace std::chrono_literals;

RemoteRetryPolicy Policy()
{
    return RemoteRetryPolicy{
        .maximum_attempts = 3, .initial_backoff = 500ms, .maximum_backoff = 30'000ms, .multiplier = 2.0};
}

/** The smallest host that can stand in for a connected one. */
class StubHost final : public nc::vfs::Host
{
public:
    explicit StubHost(const bool _writable) : Host("stub", nullptr, "stub"), m_Writable{_writable} {}
    bool IsWritable() const override { return m_Writable; }

private:
    bool m_Writable;
};

} // namespace

#define PREFIX "nc::core::RecordConnectionAttempt "

TEST_CASE(PREFIX "records a host that arrived, and hands it straight back")
{
    RemoteConnectionRegistry registry{Policy()};
    const auto host = std::make_shared<StubHost>(true);

    const VFSHostPtr returned = RecordConnectionAttempt(registry, "a", 1'000ms, [&] { return host; });
    CHECK(returned == host);

    REQUIRE(registry.State("a"));
    CHECK(registry.State("a")->status == RemoteConnectionStatus::Connected);
    CHECK_FALSE(registry.State("a")->read_only);
    CHECK(registry.State("a")->last_successful_connection.has_value());
    CHECK(registry.RetryDeadline("a") == std::nullopt);
}

TEST_CASE(PREFIX "carries read-only through from the host itself")
{
    RemoteConnectionRegistry registry{Policy()};
    std::ignore =
        RecordConnectionAttempt(registry, "a", 1'000ms, [] { return std::make_shared<StubHost>(false); });

    REQUIRE(registry.State("a"));
    CHECK(registry.State("a")->read_only);
}

TEST_CASE(PREFIX "does not treat a cancelled password prompt as a failure")
{
    // A null host is what cancelling the prompt returns, and nothing was ever asked of the server.
    // Recording it as a failure would spend a retry budget - and eventually block a connection -
    // because somebody pressed Cancel.
    RemoteConnectionRegistry registry{Policy()};

    const VFSHostPtr returned = RecordConnectionAttempt(registry, "a", 1'000ms, [] { return VFSHostPtr{}; });
    CHECK(returned == nullptr);
    CHECK(registry.State("a") == std::nullopt);
    CHECK(registry.RetryDeadline("a") == std::nullopt);
}

TEST_CASE(PREFIX "records a raised error, then raises it again unchanged")
{
    RemoteConnectionRegistry registry{Policy()};
    const Error raised{Error::POSIX, ETIMEDOUT};

    // Every existing caller handles the raise the way it always did; the recording is invisible.
    bool rethrown = false;
    try {
        std::ignore = RecordConnectionAttempt(registry, "a", 1'000ms, [&]() -> VFSHostPtr {
            throw ErrorException{raised};
        });
    } catch( const ErrorException &exception ) {
        rethrown = true;
        CHECK(exception.error() == raised);
    }
    CHECK(rethrown);

    REQUIRE(registry.State("a"));
    CHECK(registry.State("a")->last_failure == RemoteConnectionFailure::TimedOut);
    // Retryable, so a deadline is armed.
    CHECK(registry.RetryDeadline("a") == 1'500ms);
}

TEST_CASE(PREFIX "arms nothing when the server's identity is what failed")
{
    // The chain from the handshake: refused host key, classified, and never retried.
    RemoteConnectionRegistry registry{Policy()};
    const Error refused{nc::vfs::sftp::ErrorDomain, nc::vfs::sftp::Errors::host_verification_failed};

    try {
        std::ignore = RecordConnectionAttempt(registry, "a", 1'000ms, [&]() -> VFSHostPtr {
            throw ErrorException{refused};
        });
    } catch( const ErrorException & ) {
    }

    REQUIRE(registry.State("a"));
    CHECK(registry.State("a")->last_failure == RemoteConnectionFailure::HostVerificationFailed);
    CHECK(registry.State("a")->status == RemoteConnectionStatus::Blocked);
    CHECK(registry.RetryDeadline("a") == std::nullopt);
}

TEST_CASE(PREFIX "keeps an unnameable failure out of the retryable set")
{
    RemoteConnectionRegistry registry{Policy()};

    bool rethrown = false;
    try {
        std::ignore = RecordConnectionAttempt(registry, "a", 1'000ms, [&]() -> VFSHostPtr {
            throw std::runtime_error{"something else entirely"};
        });
    } catch( const std::runtime_error & ) {
        rethrown = true;
    }
    CHECK(rethrown);

    REQUIRE(registry.State("a"));
    // Something we cannot describe is not evidence that trying again will help.
    CHECK(registry.State("a")->last_failure == RemoteConnectionFailure::ProtocolError);
    CHECK(registry.RetryDeadline("a") == std::nullopt);
}

TEST_CASE(PREFIX "a success clears what an earlier failure left behind")
{
    RemoteConnectionRegistry registry{Policy()};
    try {
        std::ignore = RecordConnectionAttempt(registry, "a", 1'000ms, [] -> VFSHostPtr {
            throw ErrorException{Error{Error::POSIX, ETIMEDOUT}};
        });
    } catch( const ErrorException & ) {
    }
    REQUIRE(registry.RetryDeadline("a"));

    std::ignore = RecordConnectionAttempt(registry, "a", 2'000ms, [] { return std::make_shared<StubHost>(true); });
    CHECK(registry.RetryDeadline("a") == std::nullopt);
    REQUIRE(registry.State("a"));
    CHECK(registry.State("a")->completed_attempts == 0);
    CHECK(registry.State("a")->status == RemoteConnectionStatus::Connected);
}

TEST_CASE(PREFIX "records nothing at all when there is nothing to run")
{
    RemoteConnectionRegistry registry{Policy()};
    CHECK(RecordConnectionAttempt(registry, "a", 1'000ms, {}) == nullptr);
    CHECK(registry.State("a") == std::nullopt);
}

#undef PREFIX
