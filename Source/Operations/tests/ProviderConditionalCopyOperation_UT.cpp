// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "../source/ProviderConditionalCopyOperation.h"
#include "../source/ProviderConditionalCopyOperationTesting.h"
#include "../source/CopyOperationOrchestratorTesting.h"
#include "../source/Statistics.h"
#include "../../VFS/source/ProviderCapabilitiesTesting.h"

#include <VFS/Host.h>

#include <catch2/catch_all.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

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
static_assert(!std::is_constructible_v<CopyOperationExecutionProduct,
                                       std::shared_ptr<Operation>,
                                       CopyOperationExecutionProduct::TerminalItemResultAccessor>);
static_assert(!std::is_constructible_v<CopyOperationExecutionProduct,
                                       std::shared_ptr<Operation>,
                                       CopyOperationExecutionProduct::TerminalEvidenceAccessor>);

constexpr ProviderConditionalCopyJournalContext g_ProviderConditionalCopyOperationUTContext{.item_index = 3,
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

struct ProviderConditionalCopyOperationUTReportProbe final {
    void Capture(ItemStateReport _report)
    {
        ++calls;
        host = &_report.host;
        path = _report.path;
        status = _report.status;
    }

    int calls{0};
    vfs::Host *host{nullptr};
    std::string path;
    ItemStatus status{ItemStatus::Skipped};
};

std::shared_ptr<vfs::Host> ProviderConditionalCopyOperationUTSourceHost()
{
    return std::make_shared<ProviderConditionalCopyOperationUTHost>("provider-copy-operation-source");
}

std::shared_ptr<vfs::Host> ProviderConditionalCopyOperationUTDestinationHost()
{
    return std::make_shared<ProviderConditionalCopyOperationUTHost>("provider-copy-operation-destination");
}

ProviderConditionalCopyOperationPresentation ProviderConditionalCopyOperationUTPresentation()
{
    return {
        .source_host = ProviderConditionalCopyOperationUTSourceHost(),
        .source_path = "/source.txt",
        .destination_path = "/destination/source.txt",
    };
}

vfs::ProviderConditionalCopyReviewedClaims
ProviderConditionalCopyOperationUTClaims(const std::shared_ptr<vfs::Host> &_source,
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

std::unique_ptr<vfs::ProviderConditionalCopyTransaction> ProviderConditionalCopyOperationUTTransactionForPresentation(
    const ProviderConditionalCopyOperationPresentation &_presentation,
    vfs::ProviderConditionalCopyTransaction::CommitHandler _commit,
    vfs::ProviderConditionalCopyTransaction::AbortHandler _abort)
{
    auto source = _presentation.source_host;
    auto destination = ProviderConditionalCopyOperationUTDestinationHost();
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

std::unique_ptr<vfs::ProviderConditionalCopyTransaction>
ProviderConditionalCopyOperationUTTransaction(vfs::ProviderConditionalCopyTransaction::CommitHandler _commit,
                                              vfs::ProviderConditionalCopyTransaction::AbortHandler _abort)
{
    const auto presentation = ProviderConditionalCopyOperationUTPresentation();
    return ProviderConditionalCopyOperationUTTransactionForPresentation(
        presentation, std::move(_commit), std::move(_abort));
}

CommitResult ProviderConditionalCopyOperationUTSuccess() noexcept
{
    return {.publication = ProviderPublication::Published,
            .failure = CommitFailure::None,
            .system_error = 0,
            .filesystem_sync_status = ProviderSync::Confirmed,
            .filesystem_sync_system_error = 0};
}

CommitResult ProviderConditionalCopyOperationUTFailure() noexcept
{
    return {.publication = ProviderPublication::NotPublished,
            .failure = CommitFailure::ProviderFailure,
            .system_error = EIO,
            .filesystem_sync_status = ProviderSync::NotAttempted,
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

OperationJournalItemResult ProviderConditionalCopyOperationUTFailureItem()
{
    return {.item_index = g_ProviderConditionalCopyOperationUTContext.item_index,
            .status = OperationJournalItemStatus::Failed,
            .error = OperationJournalItemError::Unknown,
            .system_error = EIO,
            .prior_error = OperationJournalItemError::None,
            .prior_system_error = 0,
            .bytes = 0,
            .destination_publication = OperationJournalPublicationState::NotPublished,
            .filesystem_sync_status = OperationJournalFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
            .recovery_action = OperationJournalRecoveryAction::Retry};
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

void ProviderConditionalCopyOperationUTCheckPending(
    const CopyOperationExecutionProduct::TerminalEvidenceAccessor &_accessor)
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

void ProviderConditionalCopyOperationUTCheckTerminalEvidence(
    const CopyOperationExecutionProduct::TerminalEvidenceAccessor &_accessor,
    OperationJournalState _expected_state,
    const OperationJournalItemResult &_expected_item)
{
    const auto first = _accessor();
    REQUIRE(first);
    CHECK(first->state == _expected_state);
    REQUIRE(first->item_results.size() == 1);
    CHECK(first->item_results.front() == _expected_item);
    const auto second = _accessor();
    REQUIRE(second);
    CHECK(*second == *first);
}

ProviderConditionalCopyOperationPresentation ProviderConditionalCopyOperationUTPresentationAt(size_t _index)
{
    const auto name = "source-" + std::to_string(_index) + ".txt";
    return {
        .source_host = ProviderConditionalCopyOperationUTSourceHost(),
        .source_path = "/" + name,
        .destination_path = "/destination/" + name,
    };
}

/**
 * `_index` is the item's place in the batch, `_journal_index` its place in the journal's numbering.
 * They are separate on purpose: the operation must publish by the former and record by the latter,
 * and a fixture that made them equal everywhere would hide the difference.
 */
ProviderConditionalCopyOperationItem
ProviderConditionalCopyOperationUTItem(size_t _index,
                                       size_t _journal_index,
                                       uint64_t _bytes,
                                       vfs::ProviderConditionalCopyTransaction::CommitHandler _commit,
                                       vfs::ProviderConditionalCopyTransaction::AbortHandler _abort)
{
    auto presentation = ProviderConditionalCopyOperationUTPresentationAt(_index);
    auto transaction = ProviderConditionalCopyOperationUTTransactionForPresentation(
        presentation, std::move(_commit), std::move(_abort));
    return ProviderConditionalCopyOperationItem{
        .transaction = std::move(transaction),
        .journal_context = {.item_index = _journal_index, .exact_source_bytes = _bytes},
        .presentation = std::move(presentation),
    };
}

ProviderConditionalCopyOperationItem
ProviderConditionalCopyOperationUTItem(size_t _index,
                                       uint64_t _bytes,
                                       vfs::ProviderConditionalCopyTransaction::CommitHandler _commit,
                                       vfs::ProviderConditionalCopyTransaction::AbortHandler _abort)
{
    return ProviderConditionalCopyOperationUTItem(_index, _index, _bytes, std::move(_commit), std::move(_abort));
}

/** Counts what each item's own transaction was asked to do, so a tail can be told from a run. */
struct ProviderConditionalCopyOperationUTItemProbe final {
    std::atomic_int commit_calls{0};
    std::atomic_int abort_calls{0};
};

OperationJournalItemResult ProviderConditionalCopyOperationUTSuccessItemAt(size_t _index, uint64_t _bytes)
{
    auto result = ProviderConditionalCopyOperationUTSuccessItem();
    result.item_index = _index;
    result.bytes = _bytes;
    return result;
}

OperationJournalItemResult ProviderConditionalCopyOperationUTCancelledItemAt(size_t _index)
{
    auto result = ProviderConditionalCopyOperationUTCancelledItem();
    result.item_index = _index;
    return result;
}

OperationJournalItemResult ProviderConditionalCopyOperationUTFailureItemAt(size_t _index)
{
    auto result = ProviderConditionalCopyOperationUTFailureItem();
    result.item_index = _index;
    return result;
}

OperationJournalItemResult ProviderConditionalCopyOperationUTUnknownItemAt(size_t _index)
{
    auto result = ProviderConditionalCopyOperationUTUnknownItem();
    result.item_index = _index;
    return result;
}

/** Reads the evidence twice: reconciliation confirms a durable write by exact equality. */
CopyOperationTerminalEvidence
ProviderConditionalCopyOperationUTEvidence(const CopyOperationExecutionProduct::TerminalEvidenceAccessor &_accessor)
{
    const auto first = _accessor();
    REQUIRE(first);
    const auto second = _accessor();
    REQUIRE(second);
    CHECK(*second == *first);
    return *first;
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

CopyOperationExecutionProduct::TerminalEvidenceAccessor &
ProviderConditionalCopyOperationUTTerminalEvidence(CopyOperationExecutionProduct &_product) noexcept
{
    return ProviderConditionalCopyOperationTesting::TerminalEvidence(_product);
}

} // namespace

TEST_CASE(PREFIX "commits once and publishes exact success before worker completion",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
    auto presentation = ProviderConditionalCopyOperationUTPresentation();
    auto transaction = ProviderConditionalCopyOperationUTTransactionForPresentation(
        presentation,
        [probe](const auto &) {
            ++probe->commit_calls;
            return ProviderConditionalCopyOperationUTSuccess();
        },
        [probe] {
            ++probe->abort_calls;
            return ProviderPublication::NotPublished;
        });
    auto created = ProviderConditionalCopyOperationTesting::Create(
        std::move(transaction), g_ProviderConditionalCopyOperationUTContext, presentation);
    REQUIRE(created);
    auto product = std::move(*created);
    auto report = ProviderConditionalCopyOperationUTReportProbe{};
    auto &operation = ProviderConditionalCopyOperationUTOperation(product);
    operation->SetItemStatusCallback([&report](ItemStateReport _report) { report.Capture(_report); });

    ProviderConditionalCopyOperationUTCheckPending(ProviderConditionalCopyOperationUTTerminal(product));
    ProviderConditionalCopyOperationUTCheckPending(ProviderConditionalCopyOperationUTTerminalEvidence(product));
    CHECK(operation->Title() == "Copying /source.txt \u2192 /destination/source.txt");
    CHECK(operation->Statistics().PreferredSource() == Statistics::SourceType::Items);
    CHECK(operation->Statistics().VolumeTotal(Statistics::SourceType::Items) == 1);
    CHECK(operation->Statistics().VolumeProcessed(Statistics::SourceType::Items) == 0);
    operation->Start();
    REQUIRE(operation->Wait(5s));

    CHECK(operation->State() == OperationState::Completed);
    ProviderConditionalCopyOperationUTCheckTerminal(ProviderConditionalCopyOperationUTTerminal(product),
                                                    ProviderConditionalCopyOperationUTSuccessItem());
    ProviderConditionalCopyOperationUTCheckTerminalEvidence(ProviderConditionalCopyOperationUTTerminalEvidence(product),
                                                            OperationJournalState::Completed,
                                                            ProviderConditionalCopyOperationUTSuccessItem());
    CHECK(operation->Statistics().VolumeTotal(Statistics::SourceType::Items) == 1);
    CHECK(operation->Statistics().VolumeProcessed(Statistics::SourceType::Items) == 1);
    CHECK(report.calls == 1);
    CHECK(report.host == presentation.source_host.get());
    CHECK(report.path == "/source.txt");
    CHECK(report.status == ItemStatus::Processed);
    CHECK(probe->commit_calls == 1);
    CHECK(probe->abort_calls == 0);
}

TEST_CASE(PREFIX "adapts singleton terminal accessors to exact terminal evidence",
          "[provider-conditional-copy][copy-operation-terminal-evidence]")
{
    const auto check = [](OperationJournalItemResult _item_result, OperationJournalState _expected_state) {
        const auto expected_item = _item_result;
        auto product = CopyOperationOrchestratorTesting::MakeExecutionProduct(
            std::shared_ptr<Operation>{},
            [_item_result = std::move(_item_result)] {
                return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{_item_result};
            });
        const auto evidence = CopyOperationOrchestratorTesting::TerminalEvidence(product)();
        REQUIRE(evidence);
        CHECK(evidence->state == _expected_state);
        REQUIRE(evidence->item_results.size() == 1);
        CHECK(evidence->item_results.front() == expected_item);
    };

    check(ProviderConditionalCopyOperationUTSuccessItem(), OperationJournalState::Completed);
    auto skipped = ProviderConditionalCopyOperationUTSuccessItem();
    skipped.status = OperationJournalItemStatus::Skipped;
    check(std::move(skipped), OperationJournalState::Completed);
    check(ProviderConditionalCopyOperationUTFailureItem(), OperationJournalState::Failed);
    check(ProviderConditionalCopyOperationUTCancelledItem(), OperationJournalState::Cancelled);

    auto invalid_item = ProviderConditionalCopyOperationUTSuccessItem();
    invalid_item.status = static_cast<OperationJournalItemStatus>(255);
    auto invalid_product = CopyOperationOrchestratorTesting::MakeExecutionProduct(
        std::shared_ptr<Operation>{},
        [_item_result = std::move(invalid_item)] {
            return std::expected<OperationJournalItemResult, CopyOperationTerminalResultError>{_item_result};
        });
    const auto invalid_evidence = CopyOperationOrchestratorTesting::TerminalEvidence(invalid_product)();
    REQUIRE_FALSE(invalid_evidence);
    CHECK(invalid_evidence.error() == CopyOperationTerminalResultError::Inconsistent);
}

TEST_CASE(PREFIX "publishes one skipped source report and closes item statistics on failure",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
    auto presentation = ProviderConditionalCopyOperationUTPresentation();
    auto transaction = ProviderConditionalCopyOperationUTTransactionForPresentation(
        presentation,
        [probe](const auto &) {
            ++probe->commit_calls;
            return ProviderConditionalCopyOperationUTFailure();
        },
        [probe] {
            ++probe->abort_calls;
            return ProviderPublication::NotPublished;
        });
    auto created = ProviderConditionalCopyOperationTesting::Create(
        std::move(transaction), g_ProviderConditionalCopyOperationUTContext, presentation);
    REQUIRE(created);
    auto product = std::move(*created);
    auto report = ProviderConditionalCopyOperationUTReportProbe{};
    auto &operation = ProviderConditionalCopyOperationUTOperation(product);
    operation->SetItemStatusCallback([&report](ItemStateReport _report) { report.Capture(_report); });

    operation->Start();
    REQUIRE(operation->Wait(5s));

    CHECK(operation->State() == OperationState::Completed);
    ProviderConditionalCopyOperationUTCheckTerminal(ProviderConditionalCopyOperationUTTerminal(product),
                                                    ProviderConditionalCopyOperationUTFailureItem());
    ProviderConditionalCopyOperationUTCheckTerminalEvidence(ProviderConditionalCopyOperationUTTerminalEvidence(product),
                                                            OperationJournalState::Failed,
                                                            ProviderConditionalCopyOperationUTFailureItem());
    CHECK(operation->Statistics().VolumeTotal(Statistics::SourceType::Items) == 0);
    CHECK(operation->Statistics().VolumeProcessed(Statistics::SourceType::Items) == 0);
    CHECK(report.calls == 1);
    CHECK(report.host == presentation.source_host.get());
    CHECK(report.path == "/source.txt");
    CHECK(report.status == ItemStatus::Skipped);
    CHECK(probe->commit_calls == 1);
    CHECK(probe->abort_calls == 1);
}

TEST_CASE(PREFIX "cold stop owns cancellation and leaves an exact cached terminal",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    for( const auto abort_publication : {ProviderPublication::NotPublished, ProviderPublication::Unknown} ) {
        DYNAMIC_SECTION((abort_publication == ProviderPublication::NotPublished ? "confirmed not-published abort"
                                                                                : "uncertain abort"))
        {
            auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
            auto transaction = ProviderConditionalCopyOperationUTTransaction(
                [probe](const auto &) {
                    ++probe->commit_calls;
                    return ProviderConditionalCopyOperationUTSuccess();
                },
                [probe, abort_publication] {
                    ++probe->abort_calls;
                    return abort_publication;
                });
            auto created =
                ProviderConditionalCopyOperationTesting::Create(std::move(transaction),
                                                                g_ProviderConditionalCopyOperationUTContext,
                                                                ProviderConditionalCopyOperationUTPresentation(),
                                                                [probe] {
                                                                    ++probe->cancel_checks;
                                                                    return false;
                                                                });
            REQUIRE(created);
            auto product = std::move(*created);

            ProviderConditionalCopyOperationUTOperation(product)->Stop();
            CHECK(ProviderConditionalCopyOperationUTOperation(product)->State() == OperationState::Stopped);
            ProviderConditionalCopyOperationUTCheckTerminal(ProviderConditionalCopyOperationUTTerminal(product),
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
    for( const auto abort_publication : {ProviderPublication::NotPublished, ProviderPublication::Unknown} ) {
        DYNAMIC_SECTION((abort_publication == ProviderPublication::NotPublished ? "confirmed not-published abort"
                                                                                : "uncertain abort"))
        {
            auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
            auto transaction = ProviderConditionalCopyOperationUTTransaction(
                [probe](const auto &) {
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
                ProviderConditionalCopyOperationUTPresentation(),
                [probe] {
                    ++probe->cancel_checks;
                    return false;
                },
                ProviderConditionalCopyOperationTestHooks{.before_worker_launch = [] {
                    throw std::runtime_error{"deterministic provider worker launch failure"};
                }});
            REQUIRE(created);
            auto product = std::move(*created);
            ProviderConditionalCopyOperationUTCheckPending(ProviderConditionalCopyOperationUTTerminal(product));

            REQUIRE_THROWS_AS(ProviderConditionalCopyOperationUTOperation(product)->Start(), std::runtime_error);

            CHECK(ProviderConditionalCopyOperationUTOperation(product)->State() == OperationState::Stopped);
            ProviderConditionalCopyOperationUTCheckTerminal(ProviderConditionalCopyOperationUTTerminal(product),
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
            [probe](const auto &) {
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
            ProviderConditionalCopyOperationUTPresentation(),
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
        ProviderConditionalCopyOperationUTCheckTerminal(ProviderConditionalCopyOperationUTTerminal(product),
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
            [&, probe](const auto &) {
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
        auto created =
            ProviderConditionalCopyOperationTesting::Create(std::move(transaction),
                                                            g_ProviderConditionalCopyOperationUTContext,
                                                            ProviderConditionalCopyOperationUTPresentation());
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
        ProviderConditionalCopyOperationUTCheckTerminal(ProviderConditionalCopyOperationUTTerminal(product),
                                                        ProviderConditionalCopyOperationUTSuccessItem());
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
            auto presentation = ProviderConditionalCopyOperationUTPresentation();
            auto transaction = ProviderConditionalCopyOperationUTTransactionForPresentation(
                presentation,
                [probe](const auto &) {
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
            auto created = ProviderConditionalCopyOperationTesting::Create(std::move(transaction),
                                                                           g_ProviderConditionalCopyOperationUTContext,
                                                                           presentation,
                                                                           std::move(cancel_checker));
            REQUIRE(created);
            auto product = std::move(*created);
            auto report = ProviderConditionalCopyOperationUTReportProbe{};
            auto &operation = ProviderConditionalCopyOperationUTOperation(product);
            operation->SetItemStatusCallback([&report](ItemStateReport _report) { report.Capture(_report); });

            cancel_requested->store(true);
            operation->Start();
            REQUIRE(operation->Wait(5s));

            if( test.abort_publication == ProviderPublication::NotPublished ) {
                CHECK(operation->State() == OperationState::Stopped);
                ProviderConditionalCopyOperationUTCheckTerminal(ProviderConditionalCopyOperationUTTerminal(product),
                                                                ProviderConditionalCopyOperationUTCancelledItem());
            }
            else {
                CHECK(operation->State() == OperationState::Completed);
                ProviderConditionalCopyOperationUTCheckTerminal(ProviderConditionalCopyOperationUTTerminal(product),
                                                                ProviderConditionalCopyOperationUTUnknownItem());
            }
            CHECK(operation->Statistics().VolumeTotal(Statistics::SourceType::Items) == 0);
            CHECK(operation->Statistics().VolumeProcessed(Statistics::SourceType::Items) == 0);
            CHECK(report.calls == 1);
            CHECK(report.host == presentation.source_host.get());
            CHECK(report.path == "/source.txt");
            CHECK(report.status == ItemStatus::Skipped);
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
        [probe](const auto &) {
            ++probe->commit_calls;
            return ProviderConditionalCopyOperationUTSuccess();
        },
        [probe] {
            ++probe->abort_calls;
            return ProviderPublication::NotPublished;
        });
    auto created = ProviderConditionalCopyOperationTesting::Create(std::move(transaction),
                                                                   g_ProviderConditionalCopyOperationUTContext,
                                                                   ProviderConditionalCopyOperationUTPresentation());
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
            [probe](const auto &) {
                ++probe->commit_calls;
                return ProviderConditionalCopyOperationUTSuccess();
            },
            [probe] {
                ++probe->abort_calls;
                return ProviderPublication::NotPublished;
            });
        auto created =
            ProviderConditionalCopyOperationTesting::Create(std::move(transaction),
                                                            g_ProviderConditionalCopyOperationUTContext,
                                                            ProviderConditionalCopyOperationUTPresentation());
        REQUIRE(created);
        auto product = std::move(*created);
        ProviderConditionalCopyOperationUTCheckPending(ProviderConditionalCopyOperationUTTerminal(product));
    }

    CHECK(probe->commit_calls == 0);
    CHECK(probe->abort_calls == 1);
}

TEST_CASE(PREFIX "maps a pre-consumed aborted transaction as submitted-path inconsistency",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
    auto transaction = ProviderConditionalCopyOperationUTTransaction(
        [probe](const auto &) {
            ++probe->commit_calls;
            return ProviderConditionalCopyOperationUTSuccess();
        },
        [probe] {
            ++probe->abort_calls;
            return ProviderPublication::NotPublished;
        });
    REQUIRE(transaction->Abort().failure == CommitFailure::Aborted);
    auto created = ProviderConditionalCopyOperationTesting::Create(std::move(transaction),
                                                                   g_ProviderConditionalCopyOperationUTContext,
                                                                   ProviderConditionalCopyOperationUTPresentation());
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

TEST_CASE(PREFIX "runs a batch in order and reports one result per item",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    constexpr uint64_t bytes[] = {10, 200, 3000};
    // Sparse, and deliberately not the batch positions: an operation that published results by
    // journal number instead of by item would still pass if the two ever agreed everywhere.
    constexpr size_t journal_indices[] = {0, 2, 5};
    std::array<std::shared_ptr<ProviderConditionalCopyOperationUTItemProbe>, 3> probes;
    std::vector<ProviderConditionalCopyOperationItem> items;
    // Written only by the worker thread and read only after Wait, so no assertion runs off the main
    // thread - Catch2 does not survive that.
    std::vector<std::string> commit_order;
    auto read_evidence_mid_run = std::make_shared<std::function<void()>>();
    for( size_t index = 0; index != probes.size(); ++index ) {
        probes[index] = std::make_shared<ProviderConditionalCopyOperationUTItemProbe>();
        items.push_back(ProviderConditionalCopyOperationUTItem(
            index,
            journal_indices[index],
            bytes[index],
            [probe = probes[index], index, &commit_order, read_evidence_mid_run](const auto &) {
                ++probe->commit_calls;
                commit_order.push_back("/source-" + std::to_string(index) + ".txt");
                // Asked while the first item is already terminal and the rest are not.
                if( index == 1 && *read_evidence_mid_run )
                    (*read_evidence_mid_run)();
                return ProviderConditionalCopyOperationUTSuccess();
            },
            [probe = probes[index]] {
                ++probe->abort_calls;
                return ProviderPublication::NotPublished;
            }));
    }

    auto created = ProviderConditionalCopyOperationTesting::CreateBatch(std::move(items));
    REQUIRE(created);
    auto product = std::move(*created);
    auto &operation = ProviderConditionalCopyOperationUTOperation(product);
    CHECK(operation->Title() == "Copying 3 items");
    CHECK(operation->Statistics().PreferredSource() == Statistics::SourceType::Items);
    CHECK(operation->Statistics().VolumeTotal(Statistics::SourceType::Items) == 3);
    CHECK(operation->Statistics().VolumeTotal(Statistics::SourceType::Bytes) == 3210);
    ProviderConditionalCopyOperationUTCheckPending(ProviderConditionalCopyOperationUTTerminalEvidence(product));

    std::vector<std::string> reported_paths;
    std::vector<ItemStatus> reported_statuses;
    std::vector<std::optional<std::string>> current_paths;
    operation->SetItemStatusCallback([&](ItemStateReport _report) {
        reported_paths.emplace_back(_report.path);
        reported_statuses.push_back(_report.status);
        current_paths.push_back(operation->CurrentItemPath());
    });

    std::optional<CopyOperationTerminalResultError> mid_run_error;
    bool mid_run_had_evidence = false;
    *read_evidence_mid_run = [&, accessor = ProviderConditionalCopyOperationUTTerminalEvidence(product)] {
        const auto observed = accessor();
        mid_run_had_evidence = observed.has_value();
        if( !observed )
            mid_run_error = observed.error();
    };

    operation->Start();
    REQUIRE(operation->Wait(5s));
    CHECK(operation->State() == OperationState::Completed);
    CHECK(reported_statuses ==
          std::vector<ItemStatus>{ItemStatus::Processed, ItemStatus::Processed, ItemStatus::Processed});

    // Half a batch is not an answer: one item was already terminal when this was read, and a partial
    // snapshot would have been latched and recorded as if the run had ended there.
    CHECK_FALSE(mid_run_had_evidence);
    CHECK(mid_run_error == CopyOperationTerminalResultError::Pending);

    const auto evidence = ProviderConditionalCopyOperationUTEvidence(
        ProviderConditionalCopyOperationUTTerminalEvidence(product));
    CHECK(evidence.state == OperationJournalState::Completed);
    REQUIRE(evidence.item_results.size() == 3);
    for( size_t index = 0; index != probes.size(); ++index ) {
        CHECK(evidence.item_results[index] ==
              ProviderConditionalCopyOperationUTSuccessItemAt(journal_indices[index], bytes[index]));
        CHECK(probes[index]->commit_calls == 1);
        CHECK(probes[index]->abort_calls == 0);
    }
    CHECK(commit_order == std::vector<std::string>{"/source-0.txt", "/source-1.txt", "/source-2.txt"});
    CHECK(reported_paths == commit_order);
    // The file being copied travels on the current-item channel, which is what a progress line reads.
    CHECK(current_paths == std::vector<std::optional<std::string>>{
                               std::optional<std::string>{"/source-0.txt"},
                               std::optional<std::string>{"/source-1.txt"},
                               std::optional<std::string>{"/source-2.txt"}});
    CHECK(operation->Statistics().VolumeProcessed(Statistics::SourceType::Items) == 3);
    CHECK(operation->Statistics().VolumeProcessed(Statistics::SourceType::Bytes) == 3210);
}

TEST_CASE(PREFIX "ends a batch at the first failure and leaves the untouched items unrecorded",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    std::array<std::shared_ptr<ProviderConditionalCopyOperationUTItemProbe>, 3> probes;
    std::vector<ProviderConditionalCopyOperationItem> items;
    for( size_t index = 0; index != probes.size(); ++index ) {
        probes[index] = std::make_shared<ProviderConditionalCopyOperationUTItemProbe>();
        items.push_back(ProviderConditionalCopyOperationUTItem(
            index,
            100,
            [probe = probes[index], index](const auto &) {
                ++probe->commit_calls;
                return index == 1 ? ProviderConditionalCopyOperationUTFailure()
                                  : ProviderConditionalCopyOperationUTSuccess();
            },
            [probe = probes[index]] {
                ++probe->abort_calls;
                return ProviderPublication::NotPublished;
            }));
    }

    auto created = ProviderConditionalCopyOperationTesting::CreateBatch(std::move(items));
    REQUIRE(created);
    auto product = std::move(*created);
    auto &operation = ProviderConditionalCopyOperationUTOperation(product);
    int reports = 0;
    operation->SetItemStatusCallback([&reports](ItemStateReport) { ++reports; });

    operation->Start();
    REQUIRE(operation->Wait(5s));

    const auto evidence = ProviderConditionalCopyOperationUTEvidence(
        ProviderConditionalCopyOperationUTTerminalEvidence(product));
    CHECK(evidence.state == OperationJournalState::Failed);
    // Two results, not three: the third item was never attempted, and a journal entry that reports
    // a failure may say nothing at all about the items behind it. Calling them cancelled instead
    // would be a status this entry cannot legally carry.
    REQUIRE(evidence.item_results.size() == 2);
    CHECK(evidence.item_results[0] == ProviderConditionalCopyOperationUTSuccessItemAt(0, 100));
    CHECK(evidence.item_results[1] == ProviderConditionalCopyOperationUTFailureItemAt(1));
    CHECK(probes[2]->commit_calls == 0);
    CHECK(probes[2]->abort_calls == 1);
    CHECK(reports == 3);
    CHECK(operation->Statistics().VolumeProcessed(Statistics::SourceType::Items) == 1);
}

TEST_CASE(PREFIX "cancels the items behind a cancelled one",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    std::array<std::shared_ptr<ProviderConditionalCopyOperationUTItemProbe>, 3> probes;
    std::vector<ProviderConditionalCopyOperationItem> items;
    for( size_t index = 0; index != probes.size(); ++index ) {
        probes[index] = std::make_shared<ProviderConditionalCopyOperationUTItemProbe>();
        items.push_back(ProviderConditionalCopyOperationUTItem(
            index,
            100,
            [probe = probes[index], index](const auto &) -> CommitResult {
                ++probe->commit_calls;
                if( index != 1 )
                    return ProviderConditionalCopyOperationUTSuccess();
                return {.publication = ProviderPublication::NotPublished,
                        .failure = CommitFailure::Cancelled,
                        .system_error = 0,
                        .filesystem_sync_status = ProviderSync::NotAttempted,
                        .filesystem_sync_system_error = 0};
            },
            [probe = probes[index]] {
                ++probe->abort_calls;
                return ProviderPublication::NotPublished;
            }));
    }

    auto created = ProviderConditionalCopyOperationTesting::CreateBatch(std::move(items));
    REQUIRE(created);
    auto product = std::move(*created);
    auto &operation = ProviderConditionalCopyOperationUTOperation(product);

    operation->Start();
    REQUIRE(operation->Wait(5s));
    CHECK(operation->State() == OperationState::Stopped);

    const auto evidence = ProviderConditionalCopyOperationUTEvidence(
        ProviderConditionalCopyOperationUTTerminalEvidence(product));
    CHECK(evidence.state == OperationJournalState::Cancelled);
    REQUIRE(evidence.item_results.size() == 3);
    CHECK(evidence.item_results[0] == ProviderConditionalCopyOperationUTSuccessItemAt(0, 100));
    CHECK(evidence.item_results[1] == ProviderConditionalCopyOperationUTCancelledItemAt(1));
    // The committed item stays committed and the rest say so themselves - a cancelled item is what
    // the per-item result exists to express, and it is why the tail is cancelled rather than aborted.
    CHECK(evidence.item_results[2] == ProviderConditionalCopyOperationUTCancelledItemAt(2));
    CHECK(probes[2]->commit_calls == 0);
    CHECK(probes[2]->abort_calls == 1);
}

TEST_CASE(PREFIX "lets a failure outrank the cancellation that uncovered it",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    std::vector<ProviderConditionalCopyOperationItem> items;
    items.push_back(ProviderConditionalCopyOperationUTItem(
        0, 100, [](const auto &) { return ProviderConditionalCopyOperationUTSuccess(); },
        [] { return ProviderPublication::NotPublished; }));
    items.push_back(ProviderConditionalCopyOperationUTItem(
        1,
        100,
        [](const auto &) -> CommitResult {
            return {.publication = ProviderPublication::NotPublished,
                    .failure = CommitFailure::Cancelled,
                    .system_error = 0,
                    .filesystem_sync_status = ProviderSync::NotAttempted,
                    .filesystem_sync_system_error = 0};
        },
        [] { return ProviderPublication::NotPublished; }));
    // The tail item winds down, and its rollback cannot say whether anything was published.
    items.push_back(ProviderConditionalCopyOperationUTItem(
        2, 100, [](const auto &) { return ProviderConditionalCopyOperationUTSuccess(); },
        [] { return ProviderPublication::Unknown; }));

    auto created = ProviderConditionalCopyOperationTesting::CreateBatch(std::move(items));
    REQUIRE(created);
    auto product = std::move(*created);
    auto &operation = ProviderConditionalCopyOperationUTOperation(product);

    operation->Start();
    REQUIRE(operation->Wait(5s));

    const auto evidence = ProviderConditionalCopyOperationUTEvidence(
        ProviderConditionalCopyOperationUTTerminalEvidence(product));
    // One entry cannot hold a failure and a cancellation, and of the two only the failure has a
    // consequence: a cancelled item published nothing, so leaving it out withholds nothing about
    // what is on disk, while dropping the failure would hide a destination that may exist.
    CHECK(evidence.state == OperationJournalState::Failed);
    REQUIRE(evidence.item_results.size() == 2);
    CHECK(evidence.item_results[0] == ProviderConditionalCopyOperationUTSuccessItemAt(0, 100));
    CHECK(evidence.item_results[1] == ProviderConditionalCopyOperationUTUnknownItemAt(2));
}

TEST_CASE(PREFIX "refuses a batch it could not record",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    const auto succeed = [](const auto &) { return ProviderConditionalCopyOperationUTSuccess(); };
    const auto abort = [] { return ProviderPublication::NotPublished; };

    SECTION("nothing to do")
    {
        const auto created = ProviderConditionalCopyOperationTesting::CreateBatch({});
        REQUIRE_FALSE(created);
        CHECK(created.error() == ProviderConditionalCopyOperationConstructionError::EmptyBatch);
    }

    SECTION("two items claiming one journal index")
    {
        std::vector<ProviderConditionalCopyOperationItem> items;
        items.push_back(ProviderConditionalCopyOperationUTItem(1, 100, succeed, abort));
        items.push_back(ProviderConditionalCopyOperationUTItem(1, 100, succeed, abort));
        const auto created = ProviderConditionalCopyOperationTesting::CreateBatch(std::move(items));
        REQUIRE_FALSE(created);
        CHECK(created.error() == ProviderConditionalCopyOperationConstructionError::InvalidJournalIndices);
    }

    SECTION("indices that do not increase")
    {
        std::vector<ProviderConditionalCopyOperationItem> items;
        items.push_back(ProviderConditionalCopyOperationUTItem(1, 100, succeed, abort));
        items.push_back(ProviderConditionalCopyOperationUTItem(0, 100, succeed, abort));
        const auto created = ProviderConditionalCopyOperationTesting::CreateBatch(std::move(items));
        REQUIRE_FALSE(created);
        CHECK(created.error() == ProviderConditionalCopyOperationConstructionError::InvalidJournalIndices);
    }

    SECTION("an item with no transaction")
    {
        std::vector<ProviderConditionalCopyOperationItem> items;
        items.push_back(ProviderConditionalCopyOperationUTItem(0, 100, succeed, abort));
        items.push_back(ProviderConditionalCopyOperationItem{
            .transaction = nullptr,
            .journal_context = {.item_index = 1, .exact_source_bytes = 100},
            .presentation = ProviderConditionalCopyOperationUTPresentationAt(1),
        });
        const auto created = ProviderConditionalCopyOperationTesting::CreateBatch(std::move(items));
        REQUIRE_FALSE(created);
        CHECK(created.error() == ProviderConditionalCopyOperationConstructionError::MissingTransaction);
    }

    SECTION("an item that cannot be shown")
    {
        std::vector<ProviderConditionalCopyOperationItem> items;
        items.push_back(ProviderConditionalCopyOperationUTItem(0, 100, succeed, abort));
        auto second = ProviderConditionalCopyOperationUTItem(1, 100, succeed, abort);
        second.presentation.destination_path.clear();
        items.push_back(std::move(second));
        const auto created = ProviderConditionalCopyOperationTesting::CreateBatch(std::move(items));
        REQUIRE_FALSE(created);
        CHECK(created.error() == ProviderConditionalCopyOperationConstructionError::InvalidPresentation);
    }
}

TEST_CASE(PREFIX "terminates every item of a batch that never runs",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    std::array<std::shared_ptr<ProviderConditionalCopyOperationUTItemProbe>, 3> probes;
    const auto build = [&probes] {
        std::vector<ProviderConditionalCopyOperationItem> items;
        for( size_t index = 0; index != probes.size(); ++index ) {
            probes[index] = std::make_shared<ProviderConditionalCopyOperationUTItemProbe>();
            items.push_back(ProviderConditionalCopyOperationUTItem(
                index,
                100,
                [probe = probes[index]](const auto &) {
                    ++probe->commit_calls;
                    return ProviderConditionalCopyOperationUTSuccess();
                },
                [probe = probes[index]] {
                    ++probe->abort_calls;
                    return ProviderPublication::NotPublished;
                }));
        }
        return items;
    };

    SECTION("a cold stop cancels all of them")
    {
        auto created = ProviderConditionalCopyOperationTesting::CreateBatch(build());
        REQUIRE(created);
        auto product = std::move(*created);
        auto &operation = ProviderConditionalCopyOperationUTOperation(product);

        CHECK(operation->Stop());
        const auto evidence = ProviderConditionalCopyOperationUTEvidence(
            ProviderConditionalCopyOperationUTTerminalEvidence(product));
        CHECK(evidence.state == OperationJournalState::Cancelled);
        REQUIRE(evidence.item_results.size() == 3);
        for( size_t index = 0; index != probes.size(); ++index ) {
            CHECK(evidence.item_results[index] == ProviderConditionalCopyOperationUTCancelledItemAt(index));
            CHECK(probes[index]->commit_calls == 0);
            CHECK(probes[index]->abort_calls == 1);
        }
    }

    SECTION("dropping it aborts all of them and reports no run at all")
    {
        auto created = ProviderConditionalCopyOperationTesting::CreateBatch(build());
        REQUIRE(created);
        auto product = std::move(*created);
        auto accessor = ProviderConditionalCopyOperationUTTerminalEvidence(product);
        ProviderConditionalCopyOperationUTOperation(product).reset();

        const auto evidence = accessor();
        REQUIRE_FALSE(evidence);
        // No item ever executed, so there is no terminal outcome to report - which is what the
        // application boundary reads as its integration blocker rather than as a finished run.
        CHECK(evidence.error() == CopyOperationTerminalResultError::Inconsistent);
        for( const auto &probe : probes ) {
            CHECK(probe->commit_calls == 0);
            CHECK(probe->abort_calls == 1);
        }
    }
}

TEST_CASE(PREFIX "accepts a stop while items remain unstarted and cancels exactly those",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    auto stopper = std::make_shared<std::function<void()>>();
    auto stop_accepted = std::make_shared<std::atomic_bool>(false);
    std::array<std::shared_ptr<ProviderConditionalCopyOperationUTItemProbe>, 3> probes;
    std::vector<ProviderConditionalCopyOperationItem> items;
    for( size_t index = 0; index != probes.size(); ++index ) {
        probes[index] = std::make_shared<ProviderConditionalCopyOperationUTItemProbe>();
        items.push_back(ProviderConditionalCopyOperationUTItem(
            index,
            100,
            [probe = probes[index], index, stopper](const auto &) {
                ++probe->commit_calls;
                // The stop arrives while the first item is irreversibly committing.
                if( index == 0 && *stopper )
                    (*stopper)();
                return ProviderConditionalCopyOperationUTSuccess();
            },
            [probe = probes[index]] {
                ++probe->abort_calls;
                return ProviderPublication::NotPublished;
            }));
    }

    auto created = ProviderConditionalCopyOperationTesting::CreateBatch(std::move(items));
    REQUIRE(created);
    auto product = std::move(*created);
    auto &operation = ProviderConditionalCopyOperationUTOperation(product);
    *stopper = [&operation, stop_accepted] { stop_accepted->store(operation->Stop()); };

    operation->Start();
    REQUIRE(operation->Wait(5s));
    // Accepted, not refused: the sequence still had items nobody had touched.
    CHECK(stop_accepted->load());
    CHECK(operation->State() == OperationState::Stopped);

    const auto evidence = ProviderConditionalCopyOperationUTEvidence(
        ProviderConditionalCopyOperationUTTerminalEvidence(product));
    CHECK(evidence.state == OperationJournalState::Cancelled);
    REQUIRE(evidence.item_results.size() == 3);
    // The commit in flight is irreversible and keeps its outcome; the two behind it were never
    // begun, which is exactly what the stop was accepted on the strength of.
    CHECK(evidence.item_results[0] == ProviderConditionalCopyOperationUTSuccessItemAt(0, 100));
    CHECK(evidence.item_results[1] == ProviderConditionalCopyOperationUTCancelledItemAt(1));
    CHECK(evidence.item_results[2] == ProviderConditionalCopyOperationUTCancelledItemAt(2));
    CHECK(probes[0]->commit_calls == 1);
    CHECK(probes[1]->commit_calls == 0);
    CHECK(probes[2]->commit_calls == 0);
}

TEST_CASE(PREFIX "will not call a batch completed while an item has no result to give",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    std::vector<ProviderConditionalCopyOperationItem> items;
    items.push_back(ProviderConditionalCopyOperationUTItem(
        0, 100, [](const auto &) { return ProviderConditionalCopyOperationUTSuccess(); },
        [] { return ProviderPublication::NotPublished; }));
    // A terminal that precedes execution - what a commit on an already-aborted transaction replays.
    // It maps to no journal result at all, so this item cannot appear in the entry.
    items.push_back(ProviderConditionalCopyOperationUTItem(
        1,
        100,
        [](const auto &) -> CommitResult {
            return {.publication = ProviderPublication::NotPublished,
                    .failure = CommitFailure::Aborted,
                    .system_error = 0,
                    .filesystem_sync_status = ProviderSync::NotAttempted,
                    .filesystem_sync_system_error = 0};
        },
        [] { return ProviderPublication::NotPublished; }));

    auto created = ProviderConditionalCopyOperationTesting::CreateBatch(std::move(items));
    REQUIRE(created);
    auto product = std::move(*created);
    auto &operation = ProviderConditionalCopyOperationUTOperation(product);

    operation->Start();
    REQUIRE(operation->Wait(5s));

    const auto evidence = ProviderConditionalCopyOperationUTTerminalEvidence(product)();
    REQUIRE_FALSE(evidence);
    // A failed or cancelled entry may omit the items behind it; a completed one may not, and an entry
    // the journal refuses is not a visible error but a slot latched into contract violation. The
    // single-item path has always answered this same event the same way.
    CHECK(evidence.error() == CopyOperationTerminalResultError::Inconsistent);
}

TEST_CASE(PREFIX "cancels an item the stop was accepted ahead of, rather than committing it",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    // The stop lands in the one window where the worker has decided nothing yet: it is accepted on
    // the strength of "this item has not started", so this item must not then be committed.
    auto stopper = std::make_shared<std::function<void()>>();
    auto stop_accepted = std::make_shared<std::atomic_bool>(false);
    std::array<std::shared_ptr<ProviderConditionalCopyOperationUTItemProbe>, 2> probes;
    std::vector<ProviderConditionalCopyOperationItem> items;
    for( size_t index = 0; index != probes.size(); ++index ) {
        probes[index] = std::make_shared<ProviderConditionalCopyOperationUTItemProbe>();
        items.push_back(ProviderConditionalCopyOperationUTItem(
            index,
            100,
            [probe = probes[index]](const auto &) {
                ++probe->commit_calls;
                return ProviderConditionalCopyOperationUTSuccess();
            },
            [probe = probes[index]] {
                ++probe->abort_calls;
                return ProviderPublication::NotPublished;
            }));
    }

    ProviderConditionalCopyOperationTestHooks hooks;
    hooks.before_item_start = [stopper](size_t _index) {
        if( _index == 0 && *stopper )
            (*stopper)();
    };
    auto created = ProviderConditionalCopyOperationTesting::CreateBatch(std::move(items), {}, std::move(hooks));
    REQUIRE(created);
    auto product = std::move(*created);
    auto &operation = ProviderConditionalCopyOperationUTOperation(product);
    *stopper = [&operation, stop_accepted] { stop_accepted->store(operation->Stop()); };

    operation->Start();
    REQUIRE(operation->Wait(5s));
    CHECK(stop_accepted->load());
    CHECK(operation->State() == OperationState::Stopped);

    const auto evidence = ProviderConditionalCopyOperationUTEvidence(
        ProviderConditionalCopyOperationUTTerminalEvidence(product));
    CHECK(evidence.state == OperationJournalState::Cancelled);
    REQUIRE(evidence.item_results.size() == 2);
    CHECK(evidence.item_results[0] == ProviderConditionalCopyOperationUTCancelledItemAt(0));
    CHECK(evidence.item_results[1] == ProviderConditionalCopyOperationUTCancelledItemAt(1));
    for( const auto &probe : probes ) {
        CHECK(probe->commit_calls == 0);
        CHECK(probe->abort_calls == 1);
    }
}

TEST_CASE(PREFIX "settles every item when the run is thrown out of, instead of stranding the evidence",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    std::array<std::shared_ptr<ProviderConditionalCopyOperationUTItemProbe>, 3> probes;
    std::vector<ProviderConditionalCopyOperationItem> items;
    for( size_t index = 0; index != probes.size(); ++index ) {
        probes[index] = std::make_shared<ProviderConditionalCopyOperationUTItemProbe>();
        items.push_back(ProviderConditionalCopyOperationUTItem(
            index,
            100,
            [probe = probes[index]](const auto &) {
                ++probe->commit_calls;
                return ProviderConditionalCopyOperationUTSuccess();
            },
            [probe = probes[index]] {
                ++probe->abort_calls;
                return ProviderPublication::NotPublished;
            }));
    }

    ProviderConditionalCopyOperationTestHooks hooks;
    hooks.before_item_start = [](size_t _index) {
        if( _index == 1 )
            throw std::runtime_error{"between items"};
    };
    auto created = ProviderConditionalCopyOperationTesting::CreateBatch(std::move(items), {}, std::move(hooks));
    REQUIRE(created);
    auto product = std::move(*created);
    auto &operation = ProviderConditionalCopyOperationUTOperation(product);

    operation->Start();
    REQUIRE(operation->Wait(5s));

    const auto evidence = ProviderConditionalCopyOperationUTEvidence(
        ProviderConditionalCopyOperationUTTerminalEvidence(product));
    // Not `Pending`: an unresolved item is evidence that never arrives, and nothing after the worker
    // can resolve it - a stop finds the sequence claimed and the destructor only acts on an untouched
    // job, so the Pool would hold the operation forever waiting.
    CHECK(evidence.state == OperationJournalState::Cancelled);
    REQUIRE(evidence.item_results.size() == 3);
    CHECK(evidence.item_results[0] == ProviderConditionalCopyOperationUTSuccessItemAt(0, 100));
    CHECK(evidence.item_results[1] == ProviderConditionalCopyOperationUTCancelledItemAt(1));
    CHECK(evidence.item_results[2] == ProviderConditionalCopyOperationUTCancelledItemAt(2));
    CHECK(probes[0]->commit_calls == 1);
    CHECK(probes[1]->commit_calls == 0);
    CHECK(probes[2]->commit_calls == 0);
}

TEST_CASE(PREFIX "keeps destruction behind the completion callback lifetime",
          "[provider-conditional-copy][provider-conditional-copy-operation]")
{
    auto probe = std::make_shared<ProviderConditionalCopyOperationUTProbe>();
    auto transaction = ProviderConditionalCopyOperationUTTransaction(
        [probe](const auto &) {
            ++probe->commit_calls;
            return ProviderConditionalCopyOperationUTSuccess();
        },
        [probe] {
            ++probe->abort_calls;
            return ProviderPublication::NotPublished;
        });
    auto created = ProviderConditionalCopyOperationTesting::Create(std::move(transaction),
                                                                   g_ProviderConditionalCopyOperationUTContext,
                                                                   ProviderConditionalCopyOperationUTPresentation());
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
    ProviderConditionalCopyOperationUTCheckTerminal(accessor, ProviderConditionalCopyOperationUTSuccessItem());
    CHECK(probe->commit_calls == 1);
    CHECK(probe->abort_calls == 0);
    CHECK(ticket);
}

} // namespace nc::ops

#undef PREFIX
