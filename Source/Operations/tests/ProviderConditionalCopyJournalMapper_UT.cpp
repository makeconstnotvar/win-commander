// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "../source/ProviderConditionalCopyJournalMapper.h"

#include <catch2/catch_all.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#define PREFIX "ProviderConditionalCopyJournalMapper: "

namespace nc::ops {
namespace {

using CommitFailure = vfs::ProviderConditionalCopyCommitFailure;
using CommitResult = vfs::ProviderConditionalCopyCommitResult;
using ProviderPublication = vfs::ProviderConditionalCopyPublicationState;
using ProviderSync = vfs::ProviderConditionalCopyFilesystemSyncStatus;

struct ProviderConditionalCopyJournalMapperUTDirectory final {
    ProviderConditionalCopyJournalMapperUTDirectory()
    {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "provider-copy-journal-mapper-ut-XXXXXX").string();
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        path = std::filesystem::canonical(pattern).string();
    }

    ~ProviderConditionalCopyJournalMapperUTDirectory() { std::filesystem::remove_all(path); }

    std::string path;
};

OperationPlan ProviderConditionalCopyJournalMapperUTPlan(std::string _id, size_t _source_count = 1)
{
    std::vector<OperationPlanSourceInput> sources;
    sources.reserve(_source_count);
    for( size_t index = 0; index < _source_count; ++index )
        sources.emplace_back(OperationPlanSourceInput{
            "native", "/provider-copy-journal-mapper-source-" + std::to_string(index)});

    auto plan = OperationPlan::Create({
        .plan_id = std::move(_id),
        .type = OperationPlanType::Copy,
        .sources = std::move(sources),
        .destination = OperationPlanDestinationInput{
            "native", "/provider-copy-journal-mapper-destination", OperationPlanDestinationKind::Directory},
        .conflict_policy = OperationPlanConflictPolicy{
            OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem},
        .created_at = OperationPlan::TimePoint{std::chrono::seconds{1'700'000'000}}});
    REQUIRE(plan);
    return std::move(*plan);
}

OperationJournalItemResult ProviderConditionalCopyJournalMapperUTExpected(
    OperationJournalItemStatus _status,
    OperationJournalItemError _error,
    int _system_error,
    uint64_t _bytes,
    OperationJournalPublicationState _publication,
    OperationJournalFilesystemSyncStatus _sync,
    int _sync_system_error,
    OperationJournalRecoveryAction _recovery,
    size_t _item_index = 0)
{
    return OperationJournalItemResult{.item_index = _item_index,
                                      .status = _status,
                                      .error = _error,
                                      .system_error = _system_error,
                                      .prior_error = OperationJournalItemError::None,
                                      .prior_system_error = 0,
                                      .bytes = _bytes,
                                      .destination_publication = _publication,
                                      .filesystem_sync_status = _sync,
                                      .filesystem_sync_system_error = _sync_system_error,
                                      .recovery_action = _recovery};
}

struct ProviderConditionalCopyJournalMapperUTValidCase final {
    std::string_view name;
    CommitResult provider_result;
    uint64_t exact_source_bytes;
    OperationJournalItemResult expected;
    OperationJournalState terminal_state;
};

std::array<ProviderConditionalCopyJournalMapperUTValidCase, 12>
ProviderConditionalCopyJournalMapperUTValidCases()
{
    using ItemStatus = OperationJournalItemStatus;
    using ItemError = OperationJournalItemError;
    using JournalPublication = OperationJournalPublicationState;
    using JournalSync = OperationJournalFilesystemSyncStatus;
    using Recovery = OperationJournalRecoveryAction;

    return {{
        {"cancelled before publication",
         {.publication = ProviderPublication::NotPublished,
          .failure = CommitFailure::Cancelled,
          .system_error = 0,
          .filesystem_sync_status = ProviderSync::NotAttempted,
          .filesystem_sync_system_error = 0},
         101,
         ProviderConditionalCopyJournalMapperUTExpected(ItemStatus::Cancelled,
                                                        ItemError::Cancelled,
                                                        0,
                                                        0,
                                                        JournalPublication::NotPublished,
                                                        JournalSync::NotAttempted,
                                                        0,
                                                        Recovery::None),
         OperationJournalState::Cancelled},
        {"stale source before publication",
         {.publication = ProviderPublication::NotPublished,
          .failure = CommitFailure::SourceStale,
          .system_error = ESTALE,
          .filesystem_sync_status = ProviderSync::NotAttempted,
          .filesystem_sync_system_error = 0},
         102,
         ProviderConditionalCopyJournalMapperUTExpected(ItemStatus::Failed,
                                                        ItemError::SourceChanged,
                                                        ESTALE,
                                                        0,
                                                        JournalPublication::NotPublished,
                                                        JournalSync::NotAttempted,
                                                        0,
                                                        Recovery::Retry),
         OperationJournalState::Failed},
        {"stale destination parent before publication",
         {.publication = ProviderPublication::NotPublished,
          .failure = CommitFailure::DestinationParentStale,
          .system_error = ESTALE,
          .filesystem_sync_status = ProviderSync::NotAttempted,
          .filesystem_sync_system_error = 0},
         103,
         ProviderConditionalCopyJournalMapperUTExpected(ItemStatus::Failed,
                                                        ItemError::DestinationChanged,
                                                        ESTALE,
                                                        0,
                                                        JournalPublication::NotPublished,
                                                        JournalSync::NotAttempted,
                                                        0,
                                                        Recovery::Retry),
         OperationJournalState::Failed},
        {"destination appeared before publication",
         {.publication = ProviderPublication::NotPublished,
          .failure = CommitFailure::DestinationExists,
          .system_error = EEXIST,
          .filesystem_sync_status = ProviderSync::NotAttempted,
          .filesystem_sync_system_error = 0},
         104,
         ProviderConditionalCopyJournalMapperUTExpected(ItemStatus::Failed,
                                                        ItemError::DestinationChanged,
                                                        EEXIST,
                                                        0,
                                                        JournalPublication::NotPublished,
                                                        JournalSync::NotAttempted,
                                                        0,
                                                        Recovery::Retry),
         OperationJournalState::Failed},
        {"provider failure before publication",
         {.publication = ProviderPublication::NotPublished,
          .failure = CommitFailure::ProviderFailure,
          .system_error = EACCES,
          .filesystem_sync_status = ProviderSync::NotAttempted,
          .filesystem_sync_system_error = 0},
         105,
         ProviderConditionalCopyJournalMapperUTExpected(ItemStatus::Failed,
                                                        ItemError::Unknown,
                                                        EACCES,
                                                        0,
                                                        JournalPublication::NotPublished,
                                                        JournalSync::NotAttempted,
                                                        0,
                                                        Recovery::Retry),
         OperationJournalState::Failed},
        {"provider failure with unknown publication",
         {.publication = ProviderPublication::Unknown,
          .failure = CommitFailure::ProviderFailure,
          .system_error = EIO,
          .filesystem_sync_status = ProviderSync::NotAttempted,
          .filesystem_sync_system_error = 0},
         106,
         ProviderConditionalCopyJournalMapperUTExpected(ItemStatus::Failed,
                                                        ItemError::Commit,
                                                        EIO,
                                                        0,
                                                        JournalPublication::Unknown,
                                                        JournalSync::NotAttempted,
                                                        0,
                                                        Recovery::InspectDestination),
         OperationJournalState::Failed},
        {"successful publication",
         {.publication = ProviderPublication::Published,
          .failure = CommitFailure::None,
          .system_error = 0,
          .filesystem_sync_status = ProviderSync::Confirmed,
          .filesystem_sync_system_error = 0},
         107,
         ProviderConditionalCopyJournalMapperUTExpected(ItemStatus::Succeeded,
                                                        ItemError::None,
                                                        0,
                                                        107,
                                                        JournalPublication::Published,
                                                        JournalSync::Confirmed,
                                                        0,
                                                        Recovery::None),
         OperationJournalState::Completed},
        {"metadata failure with confirmed filesystem sync",
         {.publication = ProviderPublication::Published,
          .failure = CommitFailure::MetadataFailed,
          .system_error = ENOTSUP,
          .filesystem_sync_status = ProviderSync::Confirmed,
          .filesystem_sync_system_error = 0},
         108,
         ProviderConditionalCopyJournalMapperUTExpected(ItemStatus::Failed,
                                                        ItemError::Metadata,
                                                        ENOTSUP,
                                                        108,
                                                        JournalPublication::Published,
                                                        JournalSync::Confirmed,
                                                        0,
                                                        Recovery::InspectDestination),
         OperationJournalState::Failed},
        {"metadata failure with failed filesystem sync",
         {.publication = ProviderPublication::Published,
          .failure = CommitFailure::MetadataFailed,
          .system_error = EPERM,
          .filesystem_sync_status = ProviderSync::Failed,
          .filesystem_sync_system_error = ENOSPC},
         109,
         ProviderConditionalCopyJournalMapperUTExpected(ItemStatus::Failed,
                                                        ItemError::Metadata,
                                                        EPERM,
                                                        109,
                                                        JournalPublication::Published,
                                                        JournalSync::Failed,
                                                        ENOSPC,
                                                        Recovery::InspectDestination),
         OperationJournalState::Failed},
        {"filesystem sync failure",
         {.publication = ProviderPublication::Published,
          .failure = CommitFailure::FileSystemSyncFailed,
          .system_error = EROFS,
          .filesystem_sync_status = ProviderSync::Failed,
          .filesystem_sync_system_error = EROFS},
         110,
         ProviderConditionalCopyJournalMapperUTExpected(ItemStatus::Failed,
                                                        ItemError::Commit,
                                                        EROFS,
                                                        110,
                                                        JournalPublication::Published,
                                                        JournalSync::Failed,
                                                        EROFS,
                                                        Recovery::InspectDestination),
         OperationJournalState::Failed},
        {"provider failure with confirmed filesystem sync",
         {.publication = ProviderPublication::Published,
          .failure = CommitFailure::ProviderFailure,
          .system_error = EIO,
          .filesystem_sync_status = ProviderSync::Confirmed,
          .filesystem_sync_system_error = 0},
         111,
         ProviderConditionalCopyJournalMapperUTExpected(ItemStatus::Failed,
                                                        ItemError::Commit,
                                                        EIO,
                                                        111,
                                                        JournalPublication::Published,
                                                        JournalSync::Confirmed,
                                                        0,
                                                        Recovery::InspectDestination),
         OperationJournalState::Failed},
        {"provider failure with failed filesystem sync",
         {.publication = ProviderPublication::Published,
          .failure = CommitFailure::ProviderFailure,
          .system_error = EIO,
          .filesystem_sync_status = ProviderSync::Failed,
          .filesystem_sync_system_error = ENOSPC},
         112,
         ProviderConditionalCopyJournalMapperUTExpected(ItemStatus::Failed,
                                                        ItemError::Commit,
                                                        EIO,
                                                        112,
                                                        JournalPublication::Published,
                                                        JournalSync::Failed,
                                                        ENOSPC,
                                                        Recovery::InspectDestination),
         OperationJournalState::Failed},
    }};
}

void ProviderConditionalCopyJournalMapperUTRoundTrip(
    const ProviderConditionalCopyJournalMapperUTValidCase &_case,
    size_t _ordinal)
{
    const auto mapped = MapProviderConditionalCopyCommitResultToJournalItemResult(
        _case.provider_result,
        ProviderConditionalCopyJournalContext{.item_index = 0,
                                              .exact_source_bytes = _case.exact_source_bytes});
    REQUIRE(mapped);
    CHECK(*mapped == _case.expected);

    ProviderConditionalCopyJournalMapperUTDirectory directory;
    const auto plan = ProviderConditionalCopyJournalMapperUTPlan(
        "provider-copy-journal-mapper-plan-" + std::to_string(_ordinal));
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        auto admission = journal->Admit(plan);
        REQUIRE(admission);
        auto run = journal->TransitionToRunning(std::move(*admission));
        REQUIRE(run);
        const auto finalized = journal->Finalize(std::move(*run), *mapped, _case.terminal_state);
        REQUIRE(finalized);
    }
    {
        auto reopened = OperationJournal::Open(directory.path);
        REQUIRE(reopened);
        const auto snapshot = reopened->Snapshot();
        REQUIRE(snapshot.size() == 1);
        CHECK(snapshot.front().plan == plan);
        CHECK(snapshot.front().state == _case.terminal_state);
        REQUIRE(snapshot.front().item_results.size() == 1);
        CHECK(snapshot.front().item_results.front() == _case.expected);
    }
}

} // namespace

TEST_CASE(PREFIX "maps every executable terminal category and round-trips exact journal evidence",
          "[provider-conditional-copy][provider-conditional-copy-journal-mapper]")
{
    const auto cases = ProviderConditionalCopyJournalMapperUTValidCases();
    for( size_t index = 0; index < cases.size(); ++index ) {
        DYNAMIC_SECTION(cases[index].name)
        {
            ProviderConditionalCopyJournalMapperUTRoundTrip(cases[index], index);
        }
    }
}

TEST_CASE(PREFIX "preserves context values without deriving new evidence",
          "[provider-conditional-copy][provider-conditional-copy-journal-mapper]")
{
    constexpr auto max_index = std::numeric_limits<size_t>::max();
    constexpr auto max_bytes = std::numeric_limits<uint64_t>::max();
    const auto result = MapProviderConditionalCopyCommitResultToJournalItemResult(
        CommitResult{.publication = ProviderPublication::Published,
                     .failure = CommitFailure::None,
                     .system_error = 0,
                     .filesystem_sync_status = ProviderSync::Confirmed,
                     .filesystem_sync_system_error = 0},
        ProviderConditionalCopyJournalContext{.item_index = max_index,
                                              .exact_source_bytes = max_bytes});
    REQUIRE(result);
    CHECK(result->item_index == max_index);
    CHECK(result->bytes == max_bytes);
    CHECK(result->prior_error == OperationJournalItemError::None);
    CHECK(result->prior_system_error == 0);
}

TEST_CASE(PREFIX "keeps an aborted provider terminal outside the execution journal",
          "[provider-conditional-copy][provider-conditional-copy-journal-mapper]")
{
    const auto result = MapProviderConditionalCopyCommitResultToJournalItemResult(
        CommitResult{.publication = ProviderPublication::NotPublished,
                     .failure = CommitFailure::Aborted,
                     .system_error = 0,
                     .filesystem_sync_status = ProviderSync::NotAttempted,
                     .filesystem_sync_system_error = 0},
        ProviderConditionalCopyJournalContext{.item_index = 0, .exact_source_bytes = 42});
    REQUIRE_FALSE(result);
    CHECK(result.error() == ProviderConditionalCopyJournalMappingError::NonExecutionTerminal);
}

TEST_CASE(PREFIX "rejects contradictory and unrecognized provider evidence",
          "[provider-conditional-copy][provider-conditional-copy-journal-mapper]")
{
    const CommitResult valid_unpublished{
        .publication = ProviderPublication::NotPublished,
        .failure = CommitFailure::ProviderFailure,
        .system_error = EIO,
        .filesystem_sync_status = ProviderSync::NotAttempted,
        .filesystem_sync_system_error = 0};
    const CommitResult valid_success{
        .publication = ProviderPublication::Published,
        .failure = CommitFailure::None,
        .system_error = 0,
        .filesystem_sync_status = ProviderSync::Confirmed,
        .filesystem_sync_system_error = 0};
    const CommitResult valid_sync_failure{
        .publication = ProviderPublication::Published,
        .failure = CommitFailure::ProviderFailure,
        .system_error = EIO,
        .filesystem_sync_status = ProviderSync::Failed,
        .filesystem_sync_system_error = ENOSPC};

    struct InvalidCase final {
        std::string_view name;
        CommitResult result;
    };
    std::vector<InvalidCase> cases;
    const auto add = [&](std::string_view _name, CommitResult _result) {
        cases.emplace_back(InvalidCase{_name, _result});
    };
    const auto mutate = [&](std::string_view _name, CommitResult _result, const auto &_change) {
        _change(_result);
        add(_name, _result);
    };

    mutate("negative primary errno", valid_unpublished, [](auto &_value) { _value.system_error = -1; });
    mutate("negative sync errno", valid_sync_failure,
           [](auto &_value) { _value.filesystem_sync_system_error = -1; });
    mutate("not-attempted sync with errno", valid_unpublished,
           [](auto &_value) { _value.filesystem_sync_system_error = EIO; });
    mutate("confirmed sync with errno", valid_success,
           [](auto &_value) { _value.filesystem_sync_system_error = EIO; });
    mutate("failed sync without errno", valid_sync_failure,
           [](auto &_value) { _value.filesystem_sync_system_error = 0; });
    mutate("unrecognized sync status", valid_unpublished, [](auto &_value) {
        _value.filesystem_sync_status = static_cast<ProviderSync>(255);
    });
    mutate("unrecognized publication state", valid_unpublished, [](auto &_value) {
        _value.publication = static_cast<ProviderPublication>(255);
    });
    mutate("unrecognized failure", valid_unpublished,
           [](auto &_value) { _value.failure = static_cast<CommitFailure>(255); });

    add("unpublished success",
        {.publication = ProviderPublication::NotPublished,
         .failure = CommitFailure::None,
         .system_error = 0,
         .filesystem_sync_status = ProviderSync::NotAttempted,
         .filesystem_sync_system_error = 0});
    add("unpublished metadata failure",
        {.publication = ProviderPublication::NotPublished,
         .failure = CommitFailure::MetadataFailed,
         .system_error = EIO,
         .filesystem_sync_status = ProviderSync::NotAttempted,
         .filesystem_sync_system_error = 0});
    add("unpublished filesystem sync failure",
        {.publication = ProviderPublication::NotPublished,
         .failure = CommitFailure::FileSystemSyncFailed,
         .system_error = EIO,
         .filesystem_sync_status = ProviderSync::NotAttempted,
         .filesystem_sync_system_error = 0});
    mutate("unpublished result with sync evidence", valid_unpublished,
           [](auto &_value) { _value.filesystem_sync_status = ProviderSync::Confirmed; });
    add("aborted with errno",
        {.publication = ProviderPublication::NotPublished,
         .failure = CommitFailure::Aborted,
         .system_error = EIO,
         .filesystem_sync_status = ProviderSync::NotAttempted,
         .filesystem_sync_system_error = 0});
    add("cancelled with errno",
        {.publication = ProviderPublication::NotPublished,
         .failure = CommitFailure::Cancelled,
         .system_error = ECANCELED,
         .filesystem_sync_status = ProviderSync::NotAttempted,
         .filesystem_sync_system_error = 0});
    add("source stale without ESTALE",
        {.publication = ProviderPublication::NotPublished,
         .failure = CommitFailure::SourceStale,
         .system_error = EIO,
         .filesystem_sync_status = ProviderSync::NotAttempted,
         .filesystem_sync_system_error = 0});
    add("destination parent stale without ESTALE",
        {.publication = ProviderPublication::NotPublished,
         .failure = CommitFailure::DestinationParentStale,
         .system_error = EIO,
         .filesystem_sync_status = ProviderSync::NotAttempted,
         .filesystem_sync_system_error = 0});
    add("destination exists without EEXIST",
        {.publication = ProviderPublication::NotPublished,
         .failure = CommitFailure::DestinationExists,
         .system_error = EIO,
         .filesystem_sync_status = ProviderSync::NotAttempted,
         .filesystem_sync_system_error = 0});
    mutate("provider failure without errno", valid_unpublished,
           [](auto &_value) { _value.system_error = 0; });

    add("unknown publication with non-provider failure",
        {.publication = ProviderPublication::Unknown,
         .failure = CommitFailure::DestinationExists,
         .system_error = EEXIST,
         .filesystem_sync_status = ProviderSync::NotAttempted,
         .filesystem_sync_system_error = 0});
    add("unknown publication without errno",
        {.publication = ProviderPublication::Unknown,
         .failure = CommitFailure::ProviderFailure,
         .system_error = 0,
         .filesystem_sync_status = ProviderSync::NotAttempted,
         .filesystem_sync_system_error = 0});
    add("unknown publication with sync evidence",
        {.publication = ProviderPublication::Unknown,
         .failure = CommitFailure::ProviderFailure,
         .system_error = EIO,
         .filesystem_sync_status = ProviderSync::Confirmed,
         .filesystem_sync_system_error = 0});

    mutate("published success with errno", valid_success, [](auto &_value) { _value.system_error = EIO; });
    mutate("published success without confirmed sync", valid_success,
           [](auto &_value) { _value.filesystem_sync_status = ProviderSync::NotAttempted; });
    add("published metadata failure without errno",
        {.publication = ProviderPublication::Published,
         .failure = CommitFailure::MetadataFailed,
         .system_error = 0,
         .filesystem_sync_status = ProviderSync::Confirmed,
         .filesystem_sync_system_error = 0});
    add("published metadata failure without sync evidence",
        {.publication = ProviderPublication::Published,
         .failure = CommitFailure::MetadataFailed,
         .system_error = EIO,
         .filesystem_sync_status = ProviderSync::NotAttempted,
         .filesystem_sync_system_error = 0});
    add("published filesystem sync failure without primary errno",
        {.publication = ProviderPublication::Published,
         .failure = CommitFailure::FileSystemSyncFailed,
         .system_error = 0,
         .filesystem_sync_status = ProviderSync::Failed,
         .filesystem_sync_system_error = EIO});
    add("published filesystem sync failure with confirmed sync",
        {.publication = ProviderPublication::Published,
         .failure = CommitFailure::FileSystemSyncFailed,
         .system_error = EIO,
         .filesystem_sync_status = ProviderSync::Confirmed,
         .filesystem_sync_system_error = 0});
    add("published filesystem sync failure with mismatched errno",
        {.publication = ProviderPublication::Published,
         .failure = CommitFailure::FileSystemSyncFailed,
         .system_error = EIO,
         .filesystem_sync_status = ProviderSync::Failed,
         .filesystem_sync_system_error = ENOSPC});
    add("published provider failure without errno",
        {.publication = ProviderPublication::Published,
         .failure = CommitFailure::ProviderFailure,
         .system_error = 0,
         .filesystem_sync_status = ProviderSync::Confirmed,
         .filesystem_sync_system_error = 0});
    add("published provider failure without sync evidence",
        {.publication = ProviderPublication::Published,
         .failure = CommitFailure::ProviderFailure,
         .system_error = EIO,
         .filesystem_sync_status = ProviderSync::NotAttempted,
         .filesystem_sync_system_error = 0});

    for( const auto failure : {CommitFailure::Aborted,
                               CommitFailure::Cancelled,
                               CommitFailure::SourceStale,
                               CommitFailure::DestinationParentStale,
                               CommitFailure::DestinationExists} ) {
        add("published pre-publication-only terminal",
            {.publication = ProviderPublication::Published,
             .failure = failure,
             .system_error = failure == CommitFailure::Aborted || failure == CommitFailure::Cancelled
                                 ? 0
                                 : failure == CommitFailure::DestinationExists ? EEXIST : ESTALE,
             .filesystem_sync_status = ProviderSync::Confirmed,
             .filesystem_sync_system_error = 0});
    }

    for( const auto &test : cases ) {
        DYNAMIC_SECTION(test.name)
        {
            const auto result = MapProviderConditionalCopyCommitResultToJournalItemResult(
                test.result,
                ProviderConditionalCopyJournalContext{.item_index = 0, .exact_source_bytes = 42});
            REQUIRE_FALSE(result);
            CHECK(result.error() == ProviderConditionalCopyJournalMappingError::InconsistentResult);
        }
    }
}

} // namespace nc::ops

#undef PREFIX
