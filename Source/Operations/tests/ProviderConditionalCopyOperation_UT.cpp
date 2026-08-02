// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "../source/ProviderConditionalCopyOperation.h"
#include "../source/ProviderConditionalCopyOperationTesting.h"
#include "../../VFS/source/ProviderCapabilitiesTesting.h"

#include <VFS/Host.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>

#define PREFIX "ProviderConditionalCopyOperation: "

namespace nc::ops {
namespace {

using namespace std::chrono_literals;
using CommitFailure = vfs::ProviderConditionalCopyCommitFailure;
using CommitResult = vfs::ProviderConditionalCopyCommitResult;
using ProviderPublication = vfs::ProviderConditionalCopyPublicationState;
using ProviderSync = vfs::ProviderConditionalCopyFilesystemSyncStatus;

static_assert(!std::is_aggregate_v<CopyOperationExecutionProduct>);
static_assert(!std::is_default_constructible_v<CopyOperationExecutionProduct>);
static_assert(!std::is_copy_constructible_v<CopyOperationExecutionProduct>);
static_assert(!std::is_copy_assignable_v<CopyOperationExecutionProduct>);
static_assert(std::is_move_constructible_v<CopyOperationExecutionProduct>);
static_assert(std::is_move_assignable_v<CopyOperationExecutionProduct>);
static_assert(!std::is_constructible_v<
              CopyOperationExecutionProduct,
              std::shared_ptr<Operation>,
              CopyOperationExecutionProduct::TerminalItemResultAccessor>);

constexpr ProviderConditionalCopyJournalContext g_ProviderConditionalCopyOperationUTContext{
    .item_index = 3,
    .exact_source_bytes = 4096};

class ProviderConditionalCopyOperationUTHost final : public vfs::Host
{
public:
    explicit ProviderConditionalCopyOperationUTHost(const char *_tag) : Host{"", nullptr, _tag} {}
};

struct ProviderConditionalCopyOperationUTProbe final {
    std::atomic_int commit_calls{0};
    std::atomic_int abort_calls{0};
    std::atomic_int cancel_checks{0};
};

vfs::ProviderConditionalCopyReviewedClaims ProviderConditionalCopyOperationUTClaims(
    const std::shared_ptr<vfs::Host> &_source,
    const std::shared_ptr<vfs::Host> &_destination)
{
    return vfs::ProviderConditionalCopyReviewedClaims{
        .plan_id = "provider-conditional-copy-operation-test",
        .source_binding = {.provider_id = "source", .host = _source},
        .destination_binding = {.provider_id = "destination", .host = _destination},
        .source = {.absolute_path = "/source.txt",
                   .kind = vfs::ProviderConditionalCopyExpectedKind::RegularFile,
                   .device = 1,
                   .inode = 2,
                   .birth_time = {.seconds = 3, .nanoseconds = 4},
                   .mode = 0100640,
                   .byte_size = g_ProviderConditionalCopyOperationUTContext.exact_source_bytes,
                   .modification_time = {.seconds = 5, .nanoseconds = 6},
                   .status_change_time = {.seconds = 7, .nanoseconds = 8}},
        .destination_parent = {.absolute_path = "/destination",
                               .kind = vfs::ProviderConditionalCopyExpectedKind::Directory,
                               .device = 9,
                               .inode = 10,
                               .birth_time = {.seconds = 11, .nanoseconds = 12},
                               .mode = 0040750,
                               .byte_size = 13,
                               .modification_time = {.seconds = 14, .nanoseconds = 15},
                               .status_change_time = {.seconds = 16, .nanoseconds = 17}},
        .destination = {.absolute_path = "/destination/source.txt"}};
}

std::unique_ptr<vfs::ProviderConditionalCopyTransaction>
ProviderConditionalCopyOperationUTTransaction(
    vfs::ProviderConditionalCopyTransaction::CommitHandler _commit,
    vfs::ProviderConditionalCopyTransaction::AbortHandler _abort)
{
    auto source = std::make_shared<ProviderConditionalCopyOperationUTHost>("provider-copy-operation-source");
    auto destination =
        std::make_shared<ProviderConditionalCopyOperationUTHost>("provider-copy-operation-destination");
    auto transaction = vfs::ProviderConditionalCopyTransactionTestAccess::Mint(
        *destination,
        vfs::ProviderConditionalCopyTransactionTestAccess::MakeAuthority(
            ProviderConditionalCopyOperationUTClaims(source, destination)),
        std::move(_commit),
        std::move(_abort));
    REQUIRE(transaction);
    REQUIRE(*transaction);
    return std::move(*transaction);
}

CommitResult ProviderConditionalCopyOperationUTSuccess() noexcept
{
    return {.publication = ProviderPublication::Published,
            .failure = CommitFailure::None,
            .system_error = 0,
            .filesystem_sync_status = ProviderSync::Confirmed,
            .filesystem_sync_system_error = 0};
}

OperationJournalItemResult ProviderConditionalCopyOperationUTSuccessItem()
{
    return {.item_index = g_ProviderConditionalCopyOperationUTContext.item_index,
            .status = OperationJournalItemStatus::Succeeded,
            .error = OperationJournalItemError::None,
            .system_error = 0,
            .prior_error = OperationJournalItemError::None,
            .prior_system_error = 0,
            .bytes = g_ProviderConditionalCopyOperationUTContext.exact_source_bytes,
            .destination_publication = OperationJournalPublicationState::Published,
            .filesystem_sync_status = OperationJournalFilesystemSyncStatus::Confirmed,
            .filesystem_sync_system_error = 0,
            .recovery_action = OperationJournalRecoveryAction::None};
}

OperationJournalItemResult ProviderConditionalCopyOperationUTCancelledItem()
{
    return {.item_index = g_ProviderConditionalCopyOperationUTContext.item_index,
            .status = OperationJournalItemStatus::Cancelled,
            .error = OperationJournalItemError::Cancelled,
            .system_error = 0,
            .prior_error = OperationJournalItemError::None,
            .prior_system_error = 0,
            .bytes = 0,
            .destination_publication = OperationJournalPublicationState::NotPublished,
            .filesystem_sync_status = OperationJournalFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
            .recovery_action = OperationJournalRecoveryAction::None};
}

OperationJournalItemResult ProviderConditionalCopyOperationUTUnknownItem()
{
    return {.item_index = g_ProviderConditionalCopyOperationUTContext.item_index,
            .status = OperationJournalItemStatus::Failed,
            .error = OperationJournalItemError::Commit,
            .system_error = EIO,
            .prior_error = OperationJournalItemError::None,
            .prior_system_error = 0,
            .bytes = 0,
            .destination_publication = OperationJournalPublicationState::Unknown,
            .filesystem_sync_status = OperationJournalFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
            .recovery_action = OperationJournalRecoveryAction::InspectDestination};
}

void ProviderConditionalCopyOperationUTCheckPending(
    const CopyOperationExecutionProduct::TerminalItemResultAccessor &_accessor)
{
    const auto result = _accessor();
    REQUIRE_FALSE(result);
    CHECK(result.error() == CopyOperationTerminalResultError::Pending);
}

void ProviderConditionalCopyOperationUTCheckTerminal(
    const CopyOperationExecutionProduct::TerminalItemResultAccessor &_accessor,
    const OperationJournalItemResult &_expected)
{
    const auto first = _accessor();
    REQUIRE(first);
    CHECK(*first == _expected);
    const auto second = _accessor();
    REQUIRE(second);
    CHECK(*second == _expected);
}

bool ProviderConditionalCopyOperationUTWaitUntil(const auto &_predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while( !_predicate() && std::chrono::steady_clock::now() < deadline )
        std::this_thread::yield();
    return _predicate();
}

std::shared_ptr<Operation> &
ProviderConditionalCopyOperationUTOperation(CopyOperationExecutionProduct &_product) noexcept
{
    return ProviderConditionalCopyOperationTesting::Operation(_product);
}

CopyOperationExecutionProduct::TerminalItemResultAccessor &
ProviderConditionalCopyOperationUTTerminal(CopyOperationExecutionProduct &_product) noexcept
{
    return ProviderConditionalCopyOperationTesting::TerminalItemResult(_product);
}

} // namespace

TEST_CASE(PREFIX "commits once and publishes exact success before worker completion",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
    auto transaction = ProviderConditionalCopyOperationUTTransaction(
        [probe] {
            ++probe->commit_calls;
            return ProviderConditionalCopyOperationUTSuccess();
        },
        [probe] {
            ++probe->abort_calls;
            return ProviderPublication::NotPublished;
        });
    auto created = ProviderConditionalCopyOperationTesting::Create(
        std::move(transaction), g_ProviderConditionalCopyOperationUTContext);
    REQUIRE(created);
    auto product = std::move(*created);

    ProviderConditionalCopyOperationUTCheckPending(ProviderConditionalCopyOperationUTTerminal(product));
    ProviderConditionalCopyOperationUTOperation(product)->Start();
    REQUIRE(ProviderConditionalCopyOperationUTOperation(product)->Wait(5s));

    CHECK(ProviderConditionalCopyOperationUTOperation(product)->State() == OperationState::Completed);
    ProviderConditionalCopyOperationUTCheckTerminal(
        ProviderConditionalCopyOperationUTTerminal(product), ProviderConditionalCopyOperationUTSuccessItem());
    CHECK(probe->commit_calls == 1);
    CHECK(probe->abort_calls == 0);
}

TEST_CASE(PREFIX "cold stop owns cancellation and leaves an exact cached terminal",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    for( const auto abort_publication : {ProviderPublication::NotPublished,
                                         ProviderPublication::Unknown} ) {
        DYNAMIC_SECTION((abort_publication == ProviderPublication::NotPublished
                             ? "confirmed not-published abort"
                             : "uncertain abort"))
        {
            auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
            auto transaction = ProviderConditionalCopyOperationUTTransaction(
                [probe] {
                    ++probe->commit_calls;
                    return ProviderConditionalCopyOperationUTSuccess();
                },
                [probe, abort_publication] {
                    ++probe->abort_calls;
                    return abort_publication;
                });
            auto created = ProviderConditionalCopyOperationTesting::Create(
                std::move(transaction),
                g_ProviderConditionalCopyOperationUTContext,
                [probe] {
                    ++probe->cancel_checks;
                    return false;
                });
            REQUIRE(created);
            auto product = std::move(*created);

            ProviderConditionalCopyOperationUTOperation(product)->Stop();
            CHECK(ProviderConditionalCopyOperationUTOperation(product)->State() == OperationState::Stopped);
            ProviderConditionalCopyOperationUTCheckTerminal(
                ProviderConditionalCopyOperationUTTerminal(product),
                abort_publication == ProviderPublication::NotPublished
                    ? ProviderConditionalCopyOperationUTCancelledItem()
                    : ProviderConditionalCopyOperationUTUnknownItem());
            ProviderConditionalCopyOperationUTOperation(product)->Start();
            REQUIRE(ProviderConditionalCopyOperationUTOperation(product)->Wait(5s));
            CHECK(ProviderConditionalCopyOperationUTOperation(product)->State() == OperationState::Stopped);
            CHECK(probe->commit_calls == 0);
            CHECK(probe->abort_calls == 1);
            CHECK(probe->cancel_checks == 0);
        }
    }
}

TEST_CASE(PREFIX "worker launch failure terminalizes provider authority before Start rethrows",
          "[provider-conditional-copy][provider-conditional-copy-operation][job-launch]")
{
    for( const auto abort_publication : {ProviderPublication::NotPublished,
                                         ProviderPublication::Unknown} ) {
        DYNAMIC_SECTION((abort_publication == ProviderPublication::NotPublished
                             ? "confirmed not-published abort"
                             : "uncertain abort"))
        {
            auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
            auto transaction = ProviderConditionalCopyOperationUTTransaction(
                [probe] {
                    ++probe->commit_calls;
                    return ProviderConditionalCopyOperationUTSuccess();
                },
                [probe, abort_publication] {
                    ++probe->abort_calls;
                    return abort_publication;
                });
            auto created = ProviderConditionalCopyOperationTesting::Create(
                std::move(transaction),
                g_ProviderConditionalCopyOperationUTContext,
                [probe] {
                    ++probe->cancel_checks;
                    return false;
                },
                ProviderConditionalCopyOperationTestHooks{.before_worker_launch = [] {
                    throw std::runtime_error{"deterministic provider worker launch failure"};
                }});
            REQUIRE(created);
            auto product = std::move(*created);
            ProviderConditionalCopyOperationUTCheckPending(
                ProviderConditionalCopyOperationUTTerminal(product));

            REQUIRE_THROWS_AS(
                ProviderConditionalCopyOperationUTOperation(product)->Start(), std::runtime_error);

            CHECK(ProviderConditionalCopyOperationUTOperation(product)->State() == OperationState::Stopped);
            ProviderConditionalCopyOperationUTCheckTerminal(
                ProviderConditionalCopyOperationUTTerminal(product),
                abort_publication == ProviderPublication::NotPublished
                    ? ProviderConditionalCopyOperationUTCancelledItem()
                    : ProviderConditionalCopyOperationUTUnknownItem());
            CHECK(probe->commit_calls == 0);
            CHECK(probe->abort_calls == 1);
            CHECK(probe->cancel_checks == 0);

            ProviderConditionalCopyOperationUTOperation(product)->Start();
            REQUIRE(ProviderConditionalCopyOperationUTOperation(product)->Wait(5s));
            CHECK(ProviderConditionalCopyOperationUTOperation(product)->State() == OperationState::Stopped);
            CHECK(probe->commit_calls == 0);
            CHECK(probe->abort_calls == 1);
        }
    }
}

TEST_CASE(PREFIX "linearizes stop and worker commit with either gate winner",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    SECTION("stop wins before the worker commit gate")
    {
        auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
        std::mutex mutex;
        std::condition_variable condition;
        bool worker_at_gate = false;
        bool release_worker = false;
        auto transaction = ProviderConditionalCopyOperationUTTransaction(
            [probe] {
                ++probe->commit_calls;
                return ProviderConditionalCopyOperationUTSuccess();
            },
            [probe] {
                ++probe->abort_calls;
                return ProviderPublication::NotPublished;
            });
        auto created = ProviderConditionalCopyOperationTesting::Create(
            std::move(transaction),
            g_ProviderConditionalCopyOperationUTContext,
            {},
            ProviderConditionalCopyOperationTestHooks{.before_commit_gate = [&] {
                auto lock = std::unique_lock{mutex};
                worker_at_gate = true;
                condition.notify_all();
                condition.wait(lock, [&] { return release_worker; });
            }});
        REQUIRE(created);
        auto product = std::move(*created);
        ProviderConditionalCopyOperationUTOperation(product)->Start();
        {
            auto lock = std::unique_lock{mutex};
            REQUIRE(condition.wait_for(lock, 5s, [&] { return worker_at_gate; }));
        }

        ProviderConditionalCopyOperationUTOperation(product)->Stop();
        {
            const auto guard = std::lock_guard{mutex};
            release_worker = true;
        }
        condition.notify_all();
        REQUIRE(ProviderConditionalCopyOperationUTOperation(product)->Wait(5s));

        CHECK(ProviderConditionalCopyOperationUTOperation(product)->State() == OperationState::Stopped);
        ProviderConditionalCopyOperationUTCheckTerminal(
            ProviderConditionalCopyOperationUTTerminal(product),
            ProviderConditionalCopyOperationUTCancelledItem());
        CHECK(probe->commit_calls == 0);
        CHECK(probe->abort_calls == 1);
    }

    SECTION("worker commit wins and a late stop is rejected")
    {
        auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
        std::mutex mutex;
        std::condition_variable condition;
        bool commit_entered = false;
        bool release_commit = false;
        auto transaction = ProviderConditionalCopyOperationUTTransaction(
            [&, probe] {
                ++probe->commit_calls;
                auto lock = std::unique_lock{mutex};
                commit_entered = true;
                condition.notify_all();
                condition.wait(lock, [&] { return release_commit; });
                return ProviderConditionalCopyOperationUTSuccess();
            },
            [probe] {
                ++probe->abort_calls;
                return ProviderPublication::NotPublished;
            });
        auto created = ProviderConditionalCopyOperationTesting::Create(
            std::move(transaction), g_ProviderConditionalCopyOperationUTContext);
        REQUIRE(created);
        auto product = std::move(*created);
        ProviderConditionalCopyOperationUTOperation(product)->Start();
        {
            auto lock = std::unique_lock{mutex};
            REQUIRE(condition.wait_for(lock, 5s, [&] { return commit_entered; }));
        }

        ProviderConditionalCopyOperationUTOperation(product)->Stop();
        CHECK(ProviderConditionalCopyOperationUTOperation(product)->State() == OperationState::Running);
        {
            const auto guard = std::lock_guard{mutex};
            release_commit = true;
        }
        condition.notify_all();
        REQUIRE(ProviderConditionalCopyOperationUTOperation(product)->Wait(5s));

        CHECK(ProviderConditionalCopyOperationUTOperation(product)->State() == OperationState::Completed);
        ProviderConditionalCopyOperationUTCheckTerminal(
            ProviderConditionalCopyOperationUTTerminal(product), ProviderConditionalCopyOperationUTSuccessItem());
        CHECK(probe->commit_calls == 1);
        CHECK(probe->abort_calls == 0);
    }
}

TEST_CASE(PREFIX "retains and sanitizes the normal cancel checker until worker commit",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    struct Case final {
        std::string_view name;
        bool checker_throws;
        ProviderPublication abort_publication;
    };
    constexpr Case cases[] = {
        {"late true checker with confirmed abort", false, ProviderPublication::NotPublished},
        {"throwing checker with confirmed abort", true, ProviderPublication::NotPublished},
        {"late true checker with uncertain abort", false, ProviderPublication::Unknown},
        {"throwing checker with uncertain abort", true, ProviderPublication::Unknown},
    };

    for( const auto &test : cases ) {
        DYNAMIC_SECTION(test.name)
        {
            auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
            auto cancel_requested = std::make_shared<std::atomic_bool>(false);
            auto transaction = ProviderConditionalCopyOperationUTTransaction(
                [probe] {
                    ++probe->commit_calls;
                    return ProviderConditionalCopyOperationUTSuccess();
                },
                [probe, publication = test.abort_publication] {
                    ++probe->abort_calls;
                    return publication;
                });
            auto cancel_checker = [probe, cancel_requested, throws = test.checker_throws] {
                ++probe->cancel_checks;
                if( throws )
                    throw 1;
                return cancel_requested->load();
            };
            auto created = ProviderConditionalCopyOperationTesting::Create(
                std::move(transaction),
                g_ProviderConditionalCopyOperationUTContext,
                std::move(cancel_checker));
            REQUIRE(created);
            auto product = std::move(*created);

            cancel_requested->store(true);
            ProviderConditionalCopyOperationUTOperation(product)->Start();
            REQUIRE(ProviderConditionalCopyOperationUTOperation(product)->Wait(5s));

            if( test.abort_publication == ProviderPublication::NotPublished ) {
                CHECK(ProviderConditionalCopyOperationUTOperation(product)->State() == OperationState::Stopped);
                ProviderConditionalCopyOperationUTCheckTerminal(
                    ProviderConditionalCopyOperationUTTerminal(product),
                    ProviderConditionalCopyOperationUTCancelledItem());
            }
            else {
                CHECK(ProviderConditionalCopyOperationUTOperation(product)->State() == OperationState::Completed);
                ProviderConditionalCopyOperationUTCheckTerminal(
                    ProviderConditionalCopyOperationUTTerminal(product),
                    ProviderConditionalCopyOperationUTUnknownItem());
            }
            CHECK(probe->commit_calls == 0);
            CHECK(probe->abort_calls == 1);
            CHECK(probe->cancel_checks == 1);
        }
    }
}

TEST_CASE(PREFIX "accessor owns terminal state without retaining the Operation",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
    auto transaction = ProviderConditionalCopyOperationUTTransaction(
        [probe] {
            ++probe->commit_calls;
            return ProviderConditionalCopyOperationUTSuccess();
        },
        [probe] {
            ++probe->abort_calls;
            return ProviderPublication::NotPublished;
        });
    auto created = ProviderConditionalCopyOperationTesting::Create(
        std::move(transaction), g_ProviderConditionalCopyOperationUTContext);
    REQUIRE(created);
    auto product = std::move(*created);
    auto accessor = ProviderConditionalCopyOperationUTTerminal(product);
    std::weak_ptr<Operation> weak_operation = ProviderConditionalCopyOperationUTOperation(product);

    ProviderConditionalCopyOperationUTOperation(product).reset();

    CHECK(weak_operation.expired());
    CHECK(probe->commit_calls == 0);
    CHECK(probe->abort_calls == 1);
    const auto terminal = accessor();
    REQUIRE_FALSE(terminal);
    CHECK(terminal.error() == CopyOperationTerminalResultError::Inconsistent);
    const auto repeated = accessor();
    REQUIRE_FALSE(repeated);
    CHECK(repeated.error() == CopyOperationTerminalResultError::Inconsistent);
}

TEST_CASE(PREFIX "dropping a cold product aborts provider authority exactly once",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
    {
        auto transaction = ProviderConditionalCopyOperationUTTransaction(
            [probe] {
                ++probe->commit_calls;
                return ProviderConditionalCopyOperationUTSuccess();
            },
            [probe] {
                ++probe->abort_calls;
                return ProviderPublication::NotPublished;
            });
        auto created = ProviderConditionalCopyOperationTesting::Create(
            std::move(transaction), g_ProviderConditionalCopyOperationUTContext);
        REQUIRE(created);
        auto product = std::move(*created);
        ProviderConditionalCopyOperationUTCheckPending(
            ProviderConditionalCopyOperationUTTerminal(product));
    }

    CHECK(probe->commit_calls == 0);
    CHECK(probe->abort_calls == 1);
}

TEST_CASE(PREFIX "maps a pre-consumed aborted transaction as submitted-path inconsistency",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
    auto transaction = ProviderConditionalCopyOperationUTTransaction(
        [probe] {
            ++probe->commit_calls;
            return ProviderConditionalCopyOperationUTSuccess();
        },
        [probe] {
            ++probe->abort_calls;
            return ProviderPublication::NotPublished;
        });
    REQUIRE(transaction->Abort().failure == CommitFailure::Aborted);
    auto created = ProviderConditionalCopyOperationTesting::Create(
        std::move(transaction), g_ProviderConditionalCopyOperationUTContext);
    REQUIRE(created);
    auto product = std::move(*created);

    ProviderConditionalCopyOperationUTOperation(product)->Start();
    REQUIRE(ProviderConditionalCopyOperationUTOperation(product)->Wait(5s));
    const auto terminal = ProviderConditionalCopyOperationUTTerminal(product)();
    REQUIRE_FALSE(terminal);
    CHECK(terminal.error() == CopyOperationTerminalResultError::Inconsistent);
    CHECK(ProviderConditionalCopyOperationUTOperation(product)->State() == OperationState::Completed);
    CHECK(probe->commit_calls == 0);
    CHECK(probe->abort_calls == 1);
}

TEST_CASE(PREFIX "keeps destruction behind the completion callback lifetime",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
    auto transaction = ProviderConditionalCopyOperationUTTransaction(
        [probe] {
            ++probe->commit_calls;
            return ProviderConditionalCopyOperationUTSuccess();
        },
        [probe] {
            ++probe->abort_calls;
            return ProviderPublication::NotPublished;
        });
    auto created = ProviderConditionalCopyOperationTesting::Create(
        std::move(transaction), g_ProviderConditionalCopyOperationUTContext);
    REQUIRE(created);
    auto product = std::move(*created);
    auto accessor = ProviderConditionalCopyOperationUTTerminal(product);
    auto operation = ProviderConditionalCopyOperationUTOperation(product);
    std::weak_ptr<Operation> weak_operation = operation;
    std::mutex mutex;
    std::condition_variable condition;
    bool callback_entered = false;
    bool release_callback = false;
    auto ticket = operation->Observe(Operation::NotifyAboutCompletion, [&] {
        auto lock = std::unique_lock{mutex};
        callback_entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release_callback; });
    });

    operation->Start();
    {
        auto lock = std::unique_lock{mutex};
        REQUIRE(condition.wait_for(lock, 5s, [&] { return callback_entered; }));
    }
    ProviderConditionalCopyOperationUTOperation(product).reset();
    operation.reset();
    CHECK_FALSE(weak_operation.expired());
    {
        const auto guard = std::lock_guard{mutex};
        release_callback = true;
    }
    condition.notify_all();

    REQUIRE(ProviderConditionalCopyOperationUTWaitUntil([&] { return weak_operation.expired(); }));
    ProviderConditionalCopyOperationUTCheckTerminal(
        accessor, ProviderConditionalCopyOperationUTSuccessItem());
    CHECK(probe->commit_calls == 1);
    CHECK(probe->abort_calls == 0);
    CHECK(ticket);
}

} // namespace nc::ops

#undef PREFIX
