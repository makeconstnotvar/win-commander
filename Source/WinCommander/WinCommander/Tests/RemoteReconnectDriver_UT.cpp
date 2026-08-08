// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Remote/RemoteReconnectDriver.h>

#include <VFS/NetSFTP.h>

#include <cerrno>
#include <map>
#include <string>
#include <vector>

namespace {

using nc::Error;
using nc::core::RemoteConnectionFailure;
using nc::core::RemoteConnectionRegistry;
using nc::core::RemoteConnectionStatus;
using nc::core::RemoteReconnectDriver;
using nc::core::RemoteReconnectPassReport;
using nc::core::RemoteRetryPolicy;
using namespace std::chrono_literals;

RemoteRetryPolicy Policy()
{
    return RemoteRetryPolicy{
        .maximum_attempts = 3, .initial_backoff = 500ms, .maximum_backoff = 30'000ms, .multiplier = 2.0};
}

/** Answers whatever the test queued for a key, and records what it was asked. */
struct ScriptedConnector {
    std::map<std::string, std::vector<std::expected<void, Error>>> answers;
    std::vector<std::string> asked;

    std::expected<void, Error> operator()(const std::string_view _key)
    {
        asked.emplace_back(_key);
        auto found = answers.find(std::string{_key});
        if( found == answers.end() || found->second.empty() )
            return {};
        auto answer = found->second.front();
        if( found->second.size() > 1 )
            found->second.erase(found->second.begin());
        return answer;
    }
};

} // namespace

#define PREFIX "nc::core::RemoteReconnectDriver "

TEST_CASE(PREFIX "asks the three decisions in order and folds the outcome back")
{
    RemoteConnectionRegistry registry{Policy()};
    ScriptedConnector connector;
    RemoteReconnectDriver driver{registry, std::ref(connector), [] { return int64_t{1'700'000'000}; }};

    driver.ReportFailure("a", Error{Error::POSIX, ETIMEDOUT}, 1'000ms);
    REQUIRE(registry.State("a"));
    CHECK(registry.State("a")->last_failure == RemoteConnectionFailure::TimedOut);
    CHECK(registry.RetryDeadline("a") == 1'500ms);

    // Nothing is due yet, so nothing is attempted - and the report still says when to come back.
    const RemoteReconnectPassReport early = driver.RunDueRetries(1'200ms);
    CHECK(early.reconnected.empty());
    CHECK(early.failed.empty());
    CHECK(connector.asked.empty());
    CHECK(early.next_deadline == 1'500ms);

    const RemoteReconnectPassReport due = driver.RunDueRetries(1'500ms);
    CHECK(due.reconnected == std::vector<std::string>{"a"});
    CHECK(connector.asked == std::vector<std::string>{"a"});
    CHECK(due.next_deadline == std::nullopt);
    REQUIRE(registry.State("a"));
    CHECK(registry.State("a")->status == RemoteConnectionStatus::Connected);
    CHECK(registry.State("a")->completed_attempts == 0);
}

TEST_CASE(PREFIX "never attempts a connection twice in one pass")
{
    // A zero-backoff policy makes the fresh deadline land in the past, which is the case that would
    // spin: the pass would claim the same connection again and hammer a server that is plainly not
    // answering.
    RemoteConnectionRegistry registry{RemoteRetryPolicy{
        .maximum_attempts = 100, .initial_backoff = 0ms, .maximum_backoff = 0ms, .multiplier = 1.0}};
    ScriptedConnector connector;
    connector.answers["a"] = {std::unexpected(Error{Error::POSIX, ECONNREFUSED})};
    RemoteReconnectDriver driver{registry, std::ref(connector), [] { return int64_t{1'700'000'000}; }};

    driver.ReportFailure("a", Error{Error::POSIX, ECONNREFUSED}, 1'000ms);
    REQUIRE(registry.RetryDeadline("a") == 1'000ms);

    const RemoteReconnectPassReport report = driver.RunDueRetries(1'000ms);
    CHECK(report.failed == std::vector<std::string>{"a"});
    CHECK(connector.asked.size() == 1);
    // The next attempt is armed and due immediately - but it belongs to the next pass, not this one.
    CHECK(report.next_deadline == 1'000ms);
    CHECK(registry.State("a")->completed_attempts == 2);
}

TEST_CASE(PREFIX "carries a refused host key from the connector to a stopped retry")
{
    // The end of the chain built across RC-6 through RC-9: the handshake refuses, the classifier
    // names it, the policy declines to retry, and the registry arms nothing.
    RemoteConnectionRegistry registry{Policy()};
    ScriptedConnector connector;
    connector.answers["a"] = {
        std::unexpected(Error{nc::vfs::sftp::ErrorDomain, nc::vfs::sftp::Errors::host_verification_failed})};
    RemoteReconnectDriver driver{registry, std::ref(connector), [] { return int64_t{1'700'000'000}; }};

    driver.ReportFailure("a", Error{Error::POSIX, ETIMEDOUT}, 1'000ms);
    const RemoteReconnectPassReport report = driver.RunDueRetries(1'500ms);

    CHECK(report.failed == std::vector<std::string>{"a"});
    CHECK(report.next_deadline == std::nullopt);
    REQUIRE(registry.State("a"));
    CHECK(registry.State("a")->last_failure == RemoteConnectionFailure::HostVerificationFailed);
    CHECK(registry.State("a")->status == RemoteConnectionStatus::Blocked);
    CHECK(registry.RetryDeadline("a") == std::nullopt);

    // And it stays stopped: no later pass picks it up again.
    CHECK(driver.RunDueRetries(1'000'000ms).failed.empty());
    CHECK(connector.asked.size() == 1);
}

TEST_CASE(PREFIX "runs several due connections in one pass and reports each outcome")
{
    RemoteConnectionRegistry registry{Policy()};
    ScriptedConnector connector;
    connector.answers["failing"] = {std::unexpected(Error{Error::POSIX, EHOSTUNREACH})};
    RemoteReconnectDriver driver{registry, std::ref(connector), [] { return int64_t{1'700'000'000}; }};

    driver.ReportFailure("working", Error{Error::POSIX, ETIMEDOUT}, 1'000ms);
    driver.ReportFailure("failing", Error{Error::POSIX, ETIMEDOUT}, 1'000ms);
    driver.ReportFailure("blocked", Error{Error::POSIX, EACCES}, 1'000ms);

    const RemoteReconnectPassReport report = driver.RunDueRetries(1'500ms);
    CHECK(report.reconnected == std::vector<std::string>{"working"});
    CHECK(report.failed == std::vector<std::string>{"failing"});
    // The blocked one was never armed, so it is not attempted at all.
    CHECK(connector.asked == std::vector<std::string>{"failing", "working"});
    // The one that failed again is armed for its second attempt: 1500 + 500 * 2.
    CHECK(report.next_deadline == 2'500ms);
}

TEST_CASE(PREFIX "treats a connector that threw as telling us nothing about the server")
{
    RemoteConnectionRegistry registry{Policy()};
    RemoteReconnectDriver driver{
        registry, [](std::string_view) -> std::expected<void, Error> { throw std::runtime_error{"boom"}; }, [] {
            return int64_t{1'700'000'000};
        }};

    driver.ReportFailure("a", Error{Error::POSIX, ETIMEDOUT}, 1'000ms);
    const RemoteReconnectPassReport report = driver.RunDueRetries(1'500ms);

    CHECK(report.failed == std::vector<std::string>{"a"});
    // Kept out of the retryable set rather than spun on: a broken caller is not evidence that the
    // server will answer next time.
    REQUIRE(registry.State("a"));
    CHECK(registry.State("a")->last_failure == RemoteConnectionFailure::ProtocolError);
    CHECK(registry.RetryDeadline("a") == std::nullopt);
}

TEST_CASE(PREFIX "records a success established outside the retry loop")
{
    RemoteConnectionRegistry registry{Policy()};
    ScriptedConnector connector;
    RemoteReconnectDriver driver{registry, std::ref(connector), [] { return int64_t{1'700'000'000}; }};

    driver.ReportFailure("a", Error{Error::POSIX, ETIMEDOUT}, 1'000ms);
    REQUIRE(registry.RetryDeadline("a"));

    // The user reconnected by hand while a retry was pending. The pending one must not then fire
    // against a link that is already up.
    driver.ReportConnected("a", 40ms, true);
    CHECK(registry.RetryDeadline("a") == std::nullopt);
    REQUIRE(registry.State("a"));
    CHECK(registry.State("a")->status == RemoteConnectionStatus::Connected);
    CHECK(registry.State("a")->latency == 40ms);
    CHECK(registry.State("a")->read_only);
    CHECK(registry.State("a")->last_successful_connection == 1'700'000'000);

    CHECK(driver.RunDueRetries(1'000'000ms).reconnected.empty());
    CHECK(connector.asked.empty());
}

#undef PREFIX
