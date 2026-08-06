// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include <Operations/OperationJournal.h>

#include "../source/OperationJournalTesting.h"

#include <catch2/catch_all.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unistd.h>

using namespace nc::ops;
using namespace std::chrono_literals;

namespace {

struct OperationJournalUTDirectory final {
    OperationJournalUTDirectory()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "operation-journal-ut-XXXXXX").string();
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        path = std::filesystem::canonical(pattern).string();
    }
    ~OperationJournalUTDirectory() { std::filesystem::remove_all(path); }
    std::string path;
};

struct OperationJournalUTFDTracker final {
    OperationJournalUTFDTracker()
    {
        syscalls = OperationJournalTesting::DefaultSyscalls();
        const auto real_open = syscalls->open;
        syscalls->open = [this, real_open](const char *_path, int _flags, mode_t _mode) {
            const int fd = real_open(_path, _flags, _mode);
            if( fd >= 0 )
                live.emplace(fd);
            return fd;
        };
        const auto real_open_at = syscalls->open_at;
        syscalls->open_at = [this, real_open_at](int _directory, const char *_path, int _flags, mode_t _mode) {
            const int fd = real_open_at(_directory, _path, _flags, _mode);
            if( fd >= 0 )
                live.emplace(fd);
            return fd;
        };
        const auto real_close = syscalls->close;
        syscalls->close = [this, real_close](int _fd) {
            live.erase(_fd);
            return real_close(_fd);
        };
    }

    ~OperationJournalUTFDTracker()
    {
        for( const int fd : live )
            ::close(fd);
    }

    std::shared_ptr<OperationJournalSyscalls> syscalls;
    std::set<int> live;
};

OperationPlan OperationJournalUTPlan(std::string _id = "plan-1",
                                     size_t _source_count = 1,
                                     std::string_view _source_prefix = "/source-")
{
    std::vector<OperationPlanSourceInput> sources;
    for( size_t index = 0; index < _source_count; ++index )
        sources.emplace_back(OperationPlanSourceInput{
            "native", std::string{_source_prefix} + std::to_string(index)});
    auto plan = OperationPlan::Create({.plan_id = std::move(_id),
                                       .type = OperationPlanType::Copy,
                                       .sources = std::move(sources),
                                       .destination = OperationPlanDestinationInput{
                                           "native", "/destination", OperationPlanDestinationKind::Directory},
                                       .conflict_policy = OperationPlanConflictPolicy{
                                           OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem},
                                       .created_at = OperationPlan::TimePoint{1'700'000'000s}});
    REQUIRE(plan);
    return std::move(*plan);
}

OperationPlan OperationJournalUTPermanentDeletePlan(std::string _id)
{
    auto plan = OperationPlan::Create({.plan_id = std::move(_id),
                                       .type = OperationPlanType::PermanentDelete,
                                       .sources = {OperationPlanSourceInput{"native", "/source"}},
                                       .created_at = OperationPlan::TimePoint{1'700'000'000s}});
    REQUIRE(plan);
    return std::move(*plan);
}

OperationJournalItemResult OperationJournalUTSuccess(size_t _index = 0)
{
    return {.item_index = _index,
            .status = OperationJournalItemStatus::Succeeded,
            .error = OperationJournalItemError::None,
            .system_error = 0,
            .prior_error = OperationJournalItemError::None,
            .prior_system_error = 0,
            .bytes = 42,
            .destination_publication = OperationJournalPublicationState::Published,
            .filesystem_sync_status = OperationJournalFilesystemSyncStatus::Confirmed,
            .filesystem_sync_system_error = 0,
            .recovery_action = OperationJournalRecoveryAction::None};
}

OperationJournalItemResult OperationJournalUTSkipped(size_t _index = 0)
{
    return {.item_index = _index,
            .status = OperationJournalItemStatus::Skipped,
            .error = OperationJournalItemError::None,
            .system_error = 0,
            .prior_error = OperationJournalItemError::None,
            .prior_system_error = 0,
            .bytes = 0,
            .destination_publication = OperationJournalPublicationState::NotPublished,
            .filesystem_sync_status = OperationJournalFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
            .recovery_action = OperationJournalRecoveryAction::None};
}

OperationJournalItemResult OperationJournalUTCancelled(size_t _index = 0)
{
    return {.item_index = _index,
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

OperationJournalItemResult OperationJournalUTFailure(
    size_t _index = 0,
    OperationJournalItemError _error = OperationJournalItemError::Write,
    bool _destination_is_published = false,
    OperationJournalRecoveryAction _recovery_action = OperationJournalRecoveryAction::Retry)
{
    return {.item_index = _index,
            .status = OperationJournalItemStatus::Failed,
            .error = _error,
            .system_error = EIO,
            .prior_error = OperationJournalItemError::None,
            .prior_system_error = 0,
            .bytes = 7,
            .destination_publication = _destination_is_published
                                           ? OperationJournalPublicationState::Published
                                           : OperationJournalPublicationState::NotPublished,
            .filesystem_sync_status = _destination_is_published
                                          ? OperationJournalFilesystemSyncStatus::Confirmed
                                          : OperationJournalFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
            .recovery_action = _recovery_action};
}

OperationJournalItemResult OperationJournalUTCleanupFailure(size_t _index = 0)
{
    return {.item_index = _index,
            .status = OperationJournalItemStatus::Failed,
            .error = OperationJournalItemError::Cleanup,
            .system_error = 0,
            .prior_error = OperationJournalItemError::Write,
            .prior_system_error = EIO,
            .bytes = 7,
            .destination_publication = OperationJournalPublicationState::NotPublished,
            .filesystem_sync_status = OperationJournalFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
            .recovery_action = OperationJournalRecoveryAction::None};
}

OperationJournalItemResult OperationJournalUTUnknownPublicationFailure(size_t _index = 0)
{
    return {.item_index = _index,
            .status = OperationJournalItemStatus::Failed,
            .error = OperationJournalItemError::Commit,
            .system_error = EIO,
            .prior_error = OperationJournalItemError::None,
            .prior_system_error = 0,
            .bytes = 7,
            .destination_publication = OperationJournalPublicationState::Unknown,
            .filesystem_sync_status = OperationJournalFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
            .recovery_action = OperationJournalRecoveryAction::InspectDestination};
}

std::expected<OperationJournal, OperationJournalError>
OperationJournalUTOpen(const OperationJournalUTDirectory &_directory,
                       std::shared_ptr<OperationJournalSyscalls> _syscalls,
                       OperationPlan::TimePoint _now = OperationPlan::TimePoint{1'700'000'100s})
{
    return OperationJournalTesting::Open(_directory.path, std::move(_syscalls), [_now] { return _now; });
}

std::string OperationJournalUTReadFile(const OperationJournalUTDirectory &_directory)
{
    std::ifstream stream{std::filesystem::path{_directory.path} / OperationJournal::Filename};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void OperationJournalUTWriteFile(const OperationJournalUTDirectory &_directory, std::string_view _contents)
{
    const auto path = std::filesystem::path{_directory.path} / OperationJournal::Filename;
    const int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC);
    REQUIRE(fd >= 0);
    REQUIRE(::write(fd, _contents.data(), _contents.size()) == static_cast<ssize_t>(_contents.size()));
    REQUIRE(::fsync(fd) == 0);
    REQUIRE(::close(fd) == 0);
}

OperationId OperationJournalUTId(const uint64_t _sequence)
{
    const auto operation_id = OperationId::Parse("op-" + std::to_string(_sequence));
    if( !operation_id )
        throw std::logic_error{"invalid operation ID test fixture"};
    return *operation_id;
}

std::string OperationJournalUTAsV1(std::string _v3)
{
    const auto version = _v3.find("\"version\":3");
    if( version == std::string::npos )
        throw std::logic_error{"missing v3 version"};
    _v3.replace(version, std::string_view{"\"version\":3"}.size(), "\"version\":1");
    const auto high_water = _v3.find("\"next_operation_sequence\":");
    if( high_water == std::string::npos )
        throw std::logic_error{"missing v3 operation ID high-water"};
    const auto high_water_comma = _v3.find(',', high_water);
    if( high_water_comma == std::string::npos )
        throw std::logic_error{"malformed v3 operation ID high-water"};
    _v3.erase(high_water, high_water_comma - high_water + 1);
    while( true ) {
        const auto field = _v3.find("\"operation_id\":\"");
        if( field == std::string::npos )
            break;
        const auto comma = _v3.find(',', field);
        if( comma == std::string::npos )
            throw std::logic_error{"malformed v3 operation ID"};
        _v3.erase(field, comma - field + 1);
    }
    return _v3;
}

std::string OperationJournalUTAsV2(std::string _v3)
{
    const auto version = _v3.find("\"version\":3");
    if( version == std::string::npos )
        throw std::logic_error{"missing v3 version"};
    _v3.replace(version, std::string_view{"\"version\":3"}.size(), "\"version\":2");
    const auto high_water = _v3.find("\"next_operation_sequence\":");
    if( high_water == std::string::npos )
        throw std::logic_error{"missing v3 operation ID high-water"};
    const auto high_water_comma = _v3.find(',', high_water);
    if( high_water_comma == std::string::npos )
        throw std::logic_error{"malformed v3 operation ID high-water"};
    _v3.erase(high_water, high_water_comma - high_water + 1);
    return _v3;
}

template <class T>
concept OperationJournalUTExecutionAuthority = requires(T &_value) {
    _value.Execute();
    _value.Enqueue();
    _value.Operation();
};

template <class T>
concept OperationJournalUTIdAddressedMutation = requires(T &_journal) {
    _journal.Transition("plan", OperationJournalState::Failed);
    _journal.RecordItemResult("plan", OperationJournalItemResult{});
};

template <class T>
concept OperationJournalUTRawIdAdmission = requires(T &_journal, const OperationPlan &_plan) {
    _journal.Admit(OperationJournalUTId(1), _plan);
};

} // namespace

TEST_CASE("OperationJournal: durable admission receipt carries no execution authority", "[operation-journal]")
{
    STATIC_REQUIRE(OperationJournal::SchemaVersion == 3);
    STATIC_REQUIRE_FALSE(OperationJournalUTExecutionAuthority<OperationJournalAdmissionReceipt>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<OperationJournalAdmissionReceipt>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<OperationJournalAdmissionReceipt>);
    STATIC_REQUIRE(std::is_move_constructible_v<OperationJournalAdmissionReceipt>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<OperationJournalAdmissionReceipt>);
    STATIC_REQUIRE_FALSE(OperationJournalUTExecutionAuthority<OperationJournalRunReceipt>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<OperationJournalRunReceipt>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<OperationJournalRunReceipt>);
    STATIC_REQUIRE(std::is_move_constructible_v<OperationJournalRunReceipt>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<OperationJournalRunReceipt>);
    STATIC_REQUIRE_FALSE(OperationJournalUTIdAddressedMutation<OperationJournal>);
    STATIC_REQUIRE_FALSE(OperationJournalUTRawIdAdmission<OperationJournal>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<OperationJournal::AdmissionReservation>);
    STATIC_REQUIRE(std::is_move_constructible_v<OperationJournal::AdmissionReservation>);

    OperationJournalUTDirectory directory;
    auto journal = OperationJournal::Open(directory.path);
    REQUIRE(journal);
    const auto admitted = journal->Admit(OperationJournalUTPlan());
    REQUIRE(admitted);
    CHECK(admitted->PlanId() == "plan-1");
    CHECK(admitted->OperationId().ToString() == "op-1");
    REQUIRE(journal->Snapshot().size() == 1);
    CHECK(journal->Snapshot()[0].operation_id == admitted->OperationId());
    CHECK(journal->Snapshot()[0].state == OperationJournalState::Admitted);
    CHECK(OperationJournalUTReadFile(directory).find("\"state\":\"admitted\"") != std::string::npos);
    struct stat state{};
    const auto journal_path = std::filesystem::path{directory.path} / OperationJournal::Filename;
    REQUIRE(::stat(journal_path.c_str(), &state) == 0);
    CHECK((state.st_mode & 0777) == 0600);
}

TEST_CASE("OperationJournal: reservations allocate exact durable IDs without raw admission", "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        auto first = journal->ReserveOperationId();
        auto abandoned = journal->ReserveOperationId();
        REQUIRE(first);
        REQUIRE(abandoned);
        CHECK(first->Id().ToString() == "op-1");
        CHECK(abandoned->Id().ToString() == "op-2");
        auto admitted = journal->Admit(std::move(*first), OperationJournalUTPlan("reserved"));
        REQUIRE(admitted);
        CHECK(admitted->OperationId().ToString() == "op-1");

        auto third = journal->ReserveOperationId();
        REQUIRE(third);
        CHECK(third->Id().ToString() == "op-3");
        auto second_admission = journal->Admit(std::move(*third), OperationJournalUTPlan("reserved-next"));
        REQUIRE(second_admission);
        CHECK(second_admission->OperationId().ToString() == "op-3");
    }

    auto reopened = OperationJournal::Open(directory.path);
    REQUIRE(reopened);
    const auto next = reopened->ReserveOperationId();
    REQUIRE(next);
    CHECK(next->Id().ToString() == "op-4");
    const auto json = OperationJournalUTReadFile(directory);
    CHECK(json.find("\"version\":3") != std::string::npos);
    CHECK(json.find("\"next_operation_sequence\":4") != std::string::npos);
}

TEST_CASE("OperationJournal: schema-v2 migration derives and persists the operation ID high-water", "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        REQUIRE(OperationJournalTesting::AdmitWithOperationId(
            *journal, OperationJournalUTId(41), OperationJournalUTPlan("v2-existing")));
    }
    OperationJournalUTWriteFile(directory, OperationJournalUTAsV2(OperationJournalUTReadFile(directory)));

    {
        auto migrated = OperationJournal::Open(directory.path);
        REQUIRE(migrated);
        auto reservation = migrated->ReserveOperationId();
        REQUIRE(reservation);
        CHECK(reservation->Id().ToString() == "op-42");
        const auto admission = migrated->Admit(std::move(*reservation), OperationJournalUTPlan("v3-next"));
        REQUIRE(admission);
        CHECK(admission->OperationId().ToString() == "op-42");
        CHECK(OperationJournalUTReadFile(directory).find("\"version\":3") != std::string::npos);
        CHECK(OperationJournalUTReadFile(directory).find("\"next_operation_sequence\":43") != std::string::npos);
    }

    auto reopened = OperationJournal::Open(directory.path);
    REQUIRE(reopened);
    const auto next = reopened->ReserveOperationId();
    REQUIRE(next);
    CHECK(next->Id().ToString() == "op-43");
}

TEST_CASE("OperationJournal: explicit execution IDs survive durable admission and restart", "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    const auto operation_id = OperationJournalUTId(41);
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        auto admission = OperationJournalTesting::AdmitWithOperationId(*journal, operation_id, OperationJournalUTPlan("explicit-id"));
        REQUIRE(admission);
        CHECK(admission->OperationId() == operation_id);
        CHECK(journal->Snapshot()[0].operation_id == operation_id);

        const auto duplicate =
            OperationJournalTesting::AdmitWithOperationId(*journal, operation_id, OperationJournalUTPlan("other-plan"));
        REQUIRE_FALSE(duplicate);
        CHECK(duplicate.error().code == OperationJournalErrorCode::DuplicateOperationId);
    }
    const auto json = OperationJournalUTReadFile(directory);
    CHECK(json.find("\"version\":3") != std::string::npos);
    CHECK(json.find("\"operation_id\":\"op-41\"") != std::string::npos);

    auto reopened = OperationJournal::Open(directory.path);
    REQUIRE(reopened);
    REQUIRE(reopened->Snapshot().size() == 1);
    CHECK(reopened->Snapshot()[0].operation_id == operation_id);
    CHECK(reopened->Snapshot()[0].state == OperationJournalState::Interrupted);
}

TEST_CASE("OperationJournal: migrates schema-v1 IDs atomically before exposing a snapshot", "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        auto completed =
            OperationJournalTesting::AdmitWithOperationId(*journal, OperationJournalUTId(41), OperationJournalUTPlan("completed"));
        REQUIRE(completed);
        auto completed_run = journal->TransitionToRunning(std::move(*completed));
        REQUIRE(completed_run);
        REQUIRE(journal->Finalize(
            std::move(*completed_run), OperationJournalUTSuccess(), OperationJournalState::Completed));

        auto running =
            OperationJournalTesting::AdmitWithOperationId(*journal, OperationJournalUTId(99), OperationJournalUTPlan("running"));
        REQUIRE(running);
        REQUIRE(journal->TransitionToRunning(std::move(*running)));
    }
    OperationJournalUTWriteFile(directory, OperationJournalUTAsV1(OperationJournalUTReadFile(directory)));

    {
        auto migrated = OperationJournal::Open(directory.path);
        REQUIRE(migrated);
        const auto snapshot = migrated->Snapshot();
        REQUIRE(snapshot.size() == 2);
        CHECK(snapshot[0].operation_id == OperationJournalUTId(1));
        CHECK(snapshot[0].state == OperationJournalState::Completed);
        CHECK(snapshot[1].operation_id == OperationJournalUTId(2));
        CHECK(snapshot[1].state == OperationJournalState::Interrupted);
        CHECK(OperationJournalUTReadFile(directory).find("\"version\":3") != std::string::npos);
    }
    auto reopened = OperationJournal::Open(directory.path);
    REQUIRE(reopened);
    REQUIRE(reopened->Snapshot().size() == 2);
    CHECK(reopened->Snapshot()[0].operation_id == OperationJournalUTId(1));
    CHECK(reopened->Snapshot()[1].operation_id == OperationJournalUTId(2));
}

TEST_CASE("OperationJournal: schema-v1 migration fails closed after post-rename uncertainty", "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        auto admission = OperationJournalTesting::AdmitWithOperationId(
            *journal, OperationJournalUTId(41), OperationJournalUTPlan("migration-fault"));
        REQUIRE(admission);
        auto run = journal->TransitionToRunning(std::move(*admission));
        REQUIRE(run);
        REQUIRE(journal->Finalize(std::move(*run), OperationJournalUTSuccess(), OperationJournalState::Completed));
    }
    OperationJournalUTWriteFile(directory, OperationJournalUTAsV1(OperationJournalUTReadFile(directory)));

    auto syscalls = OperationJournalTesting::DefaultSyscalls();
    const auto real_fsync = syscalls->fsync;
    size_t fsync_calls = 0;
    syscalls->fsync = [real_fsync, &fsync_calls](const int fd) {
        ++fsync_calls;
        if( fsync_calls == 2 ) {
            errno = EIO;
            return -1;
        }
        return real_fsync(fd);
    };
    const auto migration = OperationJournalUTOpen(directory, std::move(syscalls));
    REQUIRE_FALSE(migration);
    CHECK(migration.error().code == OperationJournalErrorCode::DurabilityUncertain);

    auto reopened = OperationJournal::Open(directory.path);
    REQUIRE(reopened);
    REQUIRE(reopened->Snapshot().size() == 1);
    CHECK(reopened->Snapshot()[0].operation_id == OperationJournalUTId(1));
    CHECK(OperationJournalUTReadFile(directory).find("\"version\":3") != std::string::npos);
}

TEST_CASE("OperationJournal: rejects malformed and duplicated schema-v3 operation IDs", "[operation-journal]")
{
    SECTION("high-water does not cover persisted IDs")
    {
        OperationJournalUTDirectory directory;
        {
            auto journal = OperationJournal::Open(directory.path);
            REQUIRE(journal);
            REQUIRE(journal->Admit(OperationJournalUTPlan()));
        }
        auto json = OperationJournalUTReadFile(directory);
        const auto high_water = json.find("\"next_operation_sequence\":2");
        REQUIRE(high_water != std::string::npos);
        json.replace(high_water,
                     std::string_view{"\"next_operation_sequence\":2"}.size(),
                     "\"next_operation_sequence\":1");
        OperationJournalUTWriteFile(directory, json);
        const auto reopened = OperationJournal::Open(directory.path);
        REQUIRE_FALSE(reopened);
        CHECK(reopened.error().code == OperationJournalErrorCode::CorruptJournal);
    }

    SECTION("missing or noncanonical ID")
    {
        OperationJournalUTDirectory directory;
        {
            auto journal = OperationJournal::Open(directory.path);
            REQUIRE(journal);
            REQUIRE(journal->Admit(OperationJournalUTPlan()));
        }
        auto json = OperationJournalUTReadFile(directory);
        const auto field = json.find("\"operation_id\":\"op-1\",");
        REQUIRE(field != std::string::npos);

        SECTION("missing") { json.erase(field, std::string_view{"\"operation_id\":\"op-1\","}.size()); }
        SECTION("zero") { json.replace(field, std::string_view{"\"operation_id\":\"op-1\""}.size(), "\"operation_id\":\"op-0\""); }
        SECTION("leading zero") { json.replace(field, std::string_view{"\"operation_id\":\"op-1\""}.size(), "\"operation_id\":\"op-01\""); }
        OperationJournalUTWriteFile(directory, json);
        const auto reopened = OperationJournal::Open(directory.path);
        REQUIRE_FALSE(reopened);
        CHECK(reopened.error().code == OperationJournalErrorCode::CorruptJournal);
    }

    SECTION("duplicate")
    {
        OperationJournalUTDirectory directory;
        {
            auto journal = OperationJournal::Open(directory.path);
            REQUIRE(journal);
            REQUIRE(OperationJournalTesting::AdmitWithOperationId(
                *journal, OperationJournalUTId(1), OperationJournalUTPlan("first")));
            REQUIRE(OperationJournalTesting::AdmitWithOperationId(
                *journal, OperationJournalUTId(2), OperationJournalUTPlan("second")));
        }
        auto json = OperationJournalUTReadFile(directory);
        const auto second = json.find("\"operation_id\":\"op-2\"");
        REQUIRE(second != std::string::npos);
        json.replace(second, std::string_view{"\"operation_id\":\"op-2\""}.size(), "\"operation_id\":\"op-1\"");
        OperationJournalUTWriteFile(directory, json);
        const auto reopened = OperationJournal::Open(directory.path);
        REQUIRE_FALSE(reopened);
        CHECK(reopened.error().code == OperationJournalErrorCode::DuplicateOperationId);
    }
}

TEST_CASE("OperationJournal: moving exact receipts deterministically invalidates the source authority",
          "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    auto journal = OperationJournal::Open(directory.path);
    REQUIRE(journal);

    auto admission = journal->Admit(OperationJournalUTPlan("move-invalidates"));
    REQUIRE(admission);
    OperationJournalAdmissionReceipt moved_admission{std::move(*admission)};
    const auto moved_from_admission = journal->TransitionToRunning(std::move(*admission));
    REQUIRE_FALSE(moved_from_admission);
    CHECK(moved_from_admission.error().code ==
          OperationJournalErrorCode::AdmissionReceiptAlreadyConsumed);

    auto run = journal->TransitionToRunning(std::move(moved_admission));
    REQUIRE(run);
    OperationJournalRunReceipt moved_run{std::move(*run)};
    const auto moved_from_run = journal->Finalize(
        std::move(*run), OperationJournalUTSuccess(), OperationJournalState::Completed);
    REQUIRE_FALSE(moved_from_run);
    CHECK(moved_from_run.error().code == OperationJournalErrorCode::RunReceiptAlreadyConsumed);
    REQUIRE(journal->Finalize(
        std::move(moved_run), OperationJournalUTSuccess(), OperationJournalState::Completed));
}

TEST_CASE("OperationJournal: admission receipt is exact journal-bound and consumed once", "[operation-journal]")
{
    OperationJournalUTDirectory first_directory;
    OperationJournalUTDirectory second_directory;
    auto first = OperationJournal::Open(first_directory.path);
    auto second = OperationJournal::Open(second_directory.path);
    REQUIRE(first);
    REQUIRE(second);

    auto first_receipt = first->Admit(OperationJournalUTPlan("same-id", 1, "/first-"));
    auto second_receipt = second->Admit(OperationJournalUTPlan("same-id", 1, "/second-"));
    REQUIRE(first_receipt);
    REQUIRE(second_receipt);

    auto wrong_exact_plan = OperationJournalTesting::ForgeAdmissionReceipt(
        *first, first->Snapshot()[0].operation_id, OperationJournalUTPlan("same-id", 1, "/forged-"));
    const auto exact_mismatch = first->TransitionToRunning(std::move(wrong_exact_plan));
    CHECK(exact_mismatch == std::unexpected(OperationJournalError{
                                .code = OperationJournalErrorCode::InvalidAdmissionReceipt,
                                .system_error = 0,
                                .plan_codec_error = std::nullopt}));

    auto wrong_exact_id = OperationJournalTesting::ForgeAdmissionReceipt(
        *first, OperationJournalUTId(999), first->Snapshot()[0].plan);
    const auto id_mismatch = first->TransitionToRunning(std::move(wrong_exact_id));
    REQUIRE_FALSE(id_mismatch);
    CHECK(id_mismatch.error().code == OperationJournalErrorCode::InvalidAdmissionReceipt);

    const auto cross_journal = second->TransitionToRunning(std::move(*first_receipt));
    CHECK(cross_journal == std::unexpected(OperationJournalError{
                               .code = OperationJournalErrorCode::InvalidAdmissionReceipt,
                               .system_error = 0,
                               .plan_codec_error = std::nullopt}));
    REQUIRE(first->TransitionToRunning(std::move(*first_receipt)));

    const auto duplicate = first->TransitionToRunning(std::move(*first_receipt));
    CHECK(duplicate == std::unexpected(OperationJournalError{
                           .code = OperationJournalErrorCode::AdmissionReceiptAlreadyConsumed,
                           .system_error = 0,
                           .plan_codec_error = std::nullopt}));
    REQUIRE(second->TransitionToRunning(std::move(*second_receipt)));
}

TEST_CASE("OperationJournal: receipt from a closed journal cannot start a reopened entry", "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    std::optional<OperationJournalAdmissionReceipt> stale_receipt;
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        auto receipt = journal->Admit(OperationJournalUTPlan("stale"));
        REQUIRE(receipt);
        stale_receipt.emplace(std::move(*receipt));
    }

    auto reopened = OperationJournal::Open(directory.path);
    REQUIRE(reopened);
    REQUIRE(reopened->Snapshot().size() == 1);
    CHECK(reopened->Snapshot()[0].state == OperationJournalState::Interrupted);
    const auto stale = reopened->TransitionToRunning(std::move(*stale_receipt));
    REQUIRE_FALSE(stale);
    CHECK(stale.error().code == OperationJournalErrorCode::InvalidAdmissionReceipt);
}

TEST_CASE("OperationJournal: run receipt is exact journal-bound and consumed once", "[operation-journal]")
{
    OperationJournalUTDirectory first_directory;
    OperationJournalUTDirectory second_directory;
    auto first = OperationJournal::Open(first_directory.path);
    auto second = OperationJournal::Open(second_directory.path);
    REQUIRE(first);
    REQUIRE(second);

    auto first_admission = first->Admit(OperationJournalUTPlan("same-run-id", 1, "/first-"));
    auto second_admission = second->Admit(OperationJournalUTPlan("same-run-id", 1, "/second-"));
    REQUIRE(first_admission);
    REQUIRE(second_admission);
    auto first_run = first->TransitionToRunning(std::move(*first_admission));
    auto second_run = second->TransitionToRunning(std::move(*second_admission));
    REQUIRE(first_run);
    REQUIRE(second_run);
    CHECK(first_run->PlanId() == "same-run-id");
    CHECK(first_run->OperationId() == first->Snapshot()[0].operation_id);

    auto wrong_exact_plan = OperationJournalTesting::ForgeRunReceipt(
        *first, first_run->OperationId(), OperationJournalUTPlan("same-run-id", 1, "/forged-"));
    const auto exact_mismatch = first->Finalize(
        std::move(wrong_exact_plan), OperationJournalUTFailure(), OperationJournalState::Failed);
    REQUIRE_FALSE(exact_mismatch);
    CHECK(exact_mismatch.error().code == OperationJournalErrorCode::InvalidRunReceipt);

    auto wrong_exact_id = OperationJournalTesting::ForgeRunReceipt(
        *first, OperationJournalUTId(999), first->Snapshot()[0].plan);
    const auto id_mismatch = first->Finalize(
        std::move(wrong_exact_id), OperationJournalUTFailure(), OperationJournalState::Failed);
    REQUIRE_FALSE(id_mismatch);
    CHECK(id_mismatch.error().code == OperationJournalErrorCode::InvalidRunReceipt);

    const auto cross_journal = second->Finalize(
        std::move(*first_run), OperationJournalUTSuccess(), OperationJournalState::Completed);
    REQUIRE_FALSE(cross_journal);
    CHECK(cross_journal.error().code == OperationJournalErrorCode::InvalidRunReceipt);

    REQUIRE(first->Finalize(
        std::move(*first_run), OperationJournalUTSuccess(), OperationJournalState::Completed));
    const auto duplicate = first->Finalize(
        std::move(*first_run), OperationJournalUTSuccess(), OperationJournalState::Completed);
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == OperationJournalErrorCode::RunReceiptAlreadyConsumed);
    REQUIRE(second->Finalize(
        std::move(*second_run), OperationJournalUTFailure(), OperationJournalState::Failed));
}

TEST_CASE("OperationJournal: run receipt from a closed journal cannot finalize a reopened entry",
          "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    std::optional<OperationJournalRunReceipt> stale_receipt;
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        auto admission = journal->Admit(OperationJournalUTPlan("stale-run"));
        REQUIRE(admission);
        auto run = journal->TransitionToRunning(std::move(*admission));
        REQUIRE(run);
        stale_receipt.emplace(std::move(*run));
    }

    auto reopened = OperationJournal::Open(directory.path);
    REQUIRE(reopened);
    REQUIRE(reopened->Snapshot().size() == 1);
    CHECK(reopened->Snapshot()[0].state == OperationJournalState::Interrupted);
    const auto stale = reopened->Finalize(
        std::move(*stale_receipt), OperationJournalUTSuccess(), OperationJournalState::Completed);
    REQUIRE_FALSE(stale);
    CHECK(stale.error().code == OperationJournalErrorCode::InvalidRunReceipt);
}

TEST_CASE("OperationJournal: admission finalization is exact journal-bound and consumed once",
          "[operation-journal]")
{
    OperationJournalUTDirectory first_directory;
    OperationJournalUTDirectory second_directory;
    auto first = OperationJournal::Open(first_directory.path);
    auto second = OperationJournal::Open(second_directory.path);
    REQUIRE(first);
    REQUIRE(second);

    auto first_receipt = first->Admit(OperationJournalUTPlan("same-admission-id", 1, "/first-"));
    auto second_receipt = second->Admit(OperationJournalUTPlan("same-admission-id", 1, "/second-"));
    REQUIRE(first_receipt);
    REQUIRE(second_receipt);

    auto wrong_exact_plan = OperationJournalTesting::ForgeAdmissionReceipt(
        *first,
        first->Snapshot()[0].operation_id,
        OperationJournalUTPlan("same-admission-id", 1, "/forged-"));
    const auto exact_mismatch = first->FinalizeAdmission(
        std::move(wrong_exact_plan), OperationJournalState::Failed);
    REQUIRE_FALSE(exact_mismatch);
    CHECK(exact_mismatch.error().code == OperationJournalErrorCode::InvalidAdmissionReceipt);

    const auto cross_journal = second->FinalizeAdmission(
        std::move(*first_receipt), OperationJournalState::Failed);
    REQUIRE_FALSE(cross_journal);
    CHECK(cross_journal.error().code == OperationJournalErrorCode::InvalidAdmissionReceipt);

    REQUIRE(first->FinalizeAdmission(std::move(*first_receipt), OperationJournalState::Failed));
    const auto duplicate = first->FinalizeAdmission(
        std::move(*first_receipt), OperationJournalState::Cancelled);
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == OperationJournalErrorCode::AdmissionReceiptAlreadyConsumed);
    REQUIRE(second->FinalizeAdmission(std::move(*second_receipt), OperationJournalState::Cancelled));

    CHECK(first->Snapshot()[0].state == OperationJournalState::Failed);
    CHECK(second->Snapshot()[0].state == OperationJournalState::Cancelled);
}

TEST_CASE("OperationJournal: atomic finalization preserves receipts across retryable persist faults",
          "[operation-journal]")
{
    SECTION("running item and terminal")
    {
        OperationJournalUTDirectory directory;
        auto syscalls = OperationJournalTesting::DefaultSyscalls();
        auto journal = OperationJournalUTOpen(directory, syscalls);
        REQUIRE(journal);
        auto admission = journal->Admit(OperationJournalUTPlan("retry-running"));
        REQUIRE(admission);
        auto run = journal->TransitionToRunning(std::move(*admission));
        REQUIRE(run);
        const auto persisted_running = OperationJournalUTReadFile(directory);

        const auto real_open_at = syscalls->open_at;
        bool fail_once = true;
        syscalls->open_at = [real_open_at, &fail_once](int _directory,
                                                       const char *_path,
                                                       int _flags,
                                                       mode_t _mode) {
            if( fail_once && std::string_view{_path}.starts_with(".operation-journal-v1.json.tmp.") ) {
                fail_once = false;
                errno = EACCES;
                return -1;
            }
            return real_open_at(_directory, _path, _flags, _mode);
        };

        const auto failed = journal->Finalize(
            std::move(*run), OperationJournalUTSuccess(), OperationJournalState::Completed);
        REQUIRE_FALSE(failed);
        CHECK(failed.error().code == OperationJournalErrorCode::TemporaryCreateFailed);
        REQUIRE(journal->Snapshot().size() == 1);
        CHECK(journal->Snapshot()[0].state == OperationJournalState::Running);
        CHECK(journal->Snapshot()[0].item_results.empty());
        CHECK(OperationJournalUTReadFile(directory) == persisted_running);

        REQUIRE(journal->Finalize(
            std::move(*run), OperationJournalUTSuccess(), OperationJournalState::Completed));
        REQUIRE(journal->Snapshot().size() == 1);
        CHECK(journal->Snapshot()[0].state == OperationJournalState::Completed);
        CHECK(journal->Snapshot()[0].item_results == std::vector{OperationJournalUTSuccess()});
    }

    SECTION("admitted terminal")
    {
        OperationJournalUTDirectory directory;
        auto syscalls = OperationJournalTesting::DefaultSyscalls();
        auto journal = OperationJournalUTOpen(directory, syscalls);
        REQUIRE(journal);
        auto admission = journal->Admit(OperationJournalUTPlan("retry-admitted"));
        REQUIRE(admission);
        const auto persisted_admitted = OperationJournalUTReadFile(directory);

        const auto real_open_at = syscalls->open_at;
        bool fail_once = true;
        syscalls->open_at = [real_open_at, &fail_once](int _directory,
                                                       const char *_path,
                                                       int _flags,
                                                       mode_t _mode) {
            if( fail_once && std::string_view{_path}.starts_with(".operation-journal-v1.json.tmp.") ) {
                fail_once = false;
                errno = EACCES;
                return -1;
            }
            return real_open_at(_directory, _path, _flags, _mode);
        };

        const auto failed = journal->FinalizeAdmission(
            std::move(*admission), OperationJournalState::Cancelled);
        REQUIRE_FALSE(failed);
        CHECK(failed.error().code == OperationJournalErrorCode::TemporaryCreateFailed);
        REQUIRE(journal->Snapshot().size() == 1);
        CHECK(journal->Snapshot()[0].state == OperationJournalState::Admitted);
        CHECK(OperationJournalUTReadFile(directory) == persisted_admitted);

        REQUIRE(journal->FinalizeAdmission(
            std::move(*admission), OperationJournalState::Cancelled));
        CHECK(journal->Snapshot()[0].state == OperationJournalState::Cancelled);
    }
}

TEST_CASE("OperationJournal: batch durable terminal evidence is canonical and atomic",
          "[operation-journal][batch-durable-terminal]")
{
    SECTION("a canonical complete vector finalizes and round-trips")
    {
        OperationJournalUTDirectory directory;
        const std::vector expected{OperationJournalUTSuccess(0),
                                   OperationJournalUTSkipped(1),
                                   OperationJournalUTSuccess(2)};
        {
            auto journal = OperationJournal::Open(directory.path);
            REQUIRE(journal);
            auto admission = journal->Admit(OperationJournalUTPlan("batch-completed", 3));
            REQUIRE(admission);
            auto run = journal->TransitionToRunning(std::move(*admission));
            REQUIRE(run);

            REQUIRE(journal->Finalize(std::move(*run), expected, OperationJournalState::Completed));
            const auto snapshot = journal->Snapshot();
            REQUIRE(snapshot.size() == 1);
            CHECK(snapshot[0].state == OperationJournalState::Completed);
            CHECK(snapshot[0].item_results == expected);
        }

        const auto reopened = OperationJournal::Open(directory.path);
        REQUIRE(reopened);
        const auto snapshot = reopened->Snapshot();
        REQUIRE(snapshot.size() == 1);
        CHECK(snapshot[0].state == OperationJournalState::Completed);
        CHECK(snapshot[0].item_results == expected);
    }

    SECTION("rejects invalid terminal evidence without consuming the receipt")
    {
        const auto exercise = [](const std::vector<OperationJournalItemResult> &rejected_evidence,
                                 const OperationJournalState rejected_state,
                                 const OperationJournalErrorCode expected_error) {
            OperationJournalUTDirectory directory;
            auto journal = OperationJournal::Open(directory.path);
            REQUIRE(journal);
            auto admission = journal->Admit(OperationJournalUTPlan("batch-rejected", 3));
            REQUIRE(admission);
            auto run = journal->TransitionToRunning(std::move(*admission));
            REQUIRE(run);
            const auto persisted_running = OperationJournalUTReadFile(directory);

            const auto rejected = journal->Finalize(std::move(*run), rejected_evidence, rejected_state);
            REQUIRE_FALSE(rejected);
            CHECK(rejected.error().code == expected_error);
            const auto running = journal->Snapshot();
            REQUIRE(running.size() == 1);
            CHECK(running[0].state == OperationJournalState::Running);
            CHECK(running[0].item_results.empty());
            CHECK(OperationJournalUTReadFile(directory) == persisted_running);

            const std::vector complete{OperationJournalUTSuccess(0),
                                       OperationJournalUTSkipped(1),
                                       OperationJournalUTSuccess(2)};
            REQUIRE(journal->Finalize(std::move(*run), complete, OperationJournalState::Completed));
        };

        SECTION("out of order")
        {
            exercise({OperationJournalUTSuccess(1), OperationJournalUTSkipped(0), OperationJournalUTSuccess(2)},
                     OperationJournalState::Completed,
                     OperationJournalErrorCode::InvalidItemResult);
        }
        SECTION("duplicate")
        {
            exercise({OperationJournalUTSuccess(0), OperationJournalUTSkipped(0), OperationJournalUTSuccess(2)},
                     OperationJournalState::Completed,
                     OperationJournalErrorCode::InvalidItemResult);
        }
        SECTION("invalid source index")
        {
            exercise({OperationJournalUTSuccess(0), OperationJournalUTSkipped(1), OperationJournalUTSuccess(3)},
                     OperationJournalState::Completed,
                     OperationJournalErrorCode::InvalidItemResult);
        }
        SECTION("incomplete completion")
        {
            exercise({OperationJournalUTSuccess(0), OperationJournalUTSkipped(1)},
                     OperationJournalState::Completed,
                     OperationJournalErrorCode::InvalidTransition);
        }
        SECTION("failed state without failed item")
        {
            exercise({OperationJournalUTSuccess(0), OperationJournalUTSkipped(1), OperationJournalUTSuccess(2)},
                     OperationJournalState::Failed,
                     OperationJournalErrorCode::InvalidTransition);
        }
        SECTION("completed state with failed item")
        {
            exercise({OperationJournalUTFailure(0), OperationJournalUTSkipped(1), OperationJournalUTSuccess(2)},
                     OperationJournalState::Completed,
                     OperationJournalErrorCode::InvalidTransition);
        }
    }

    SECTION("bulk finalization does not merge test-only incremental evidence")
    {
        OperationJournalUTDirectory directory;
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        auto admission = journal->Admit(OperationJournalUTPlan("batch-no-merge", 3));
        REQUIRE(admission);
        auto run = journal->TransitionToRunning(std::move(*admission));
        REQUIRE(run);
        REQUIRE(OperationJournalTesting::RecordItemResult(*journal, "batch-no-merge", OperationJournalUTSuccess(0)));

        const std::vector remaining{OperationJournalUTSkipped(1), OperationJournalUTSuccess(2)};
        const auto rejected = journal->Finalize(std::move(*run), remaining, OperationJournalState::Completed);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == OperationJournalErrorCode::InvalidTransition);
        REQUIRE(OperationJournalTesting::RecordItemResult(*journal, "batch-no-merge", OperationJournalUTSkipped(1)));
        REQUIRE(OperationJournalTesting::RecordItemResult(*journal, "batch-no-merge", OperationJournalUTSuccess(2)));
        REQUIRE(OperationJournalTesting::Transition(*journal, "batch-no-merge", OperationJournalState::Completed));
    }

    SECTION("failed and cancelled may durably finalize with empty evidence")
    {
        const auto finalize_empty = [](std::string_view plan_id, const OperationJournalState terminal_state) {
            OperationJournalUTDirectory directory;
            auto journal = OperationJournal::Open(directory.path);
            REQUIRE(journal);
            auto admission = journal->Admit(OperationJournalUTPlan(std::string{plan_id}, 3));
            REQUIRE(admission);
            auto run = journal->TransitionToRunning(std::move(*admission));
            REQUIRE(run);

            const std::vector<OperationJournalItemResult> empty;
            REQUIRE(journal->Finalize(std::move(*run), empty, terminal_state));
            const auto snapshot = journal->Snapshot();
            REQUIRE(snapshot.size() == 1);
            CHECK(snapshot[0].state == terminal_state);
            CHECK(snapshot[0].item_results.empty());
        };

        finalize_empty("batch-failed-empty", OperationJournalState::Failed);
        finalize_empty("batch-cancelled-empty", OperationJournalState::Cancelled);
    }

    SECTION("pre-rename persist failure keeps the vector evidence and receipt retryable")
    {
        OperationJournalUTDirectory directory;
        auto syscalls = OperationJournalTesting::DefaultSyscalls();
        auto journal = OperationJournalUTOpen(directory, syscalls);
        REQUIRE(journal);
        auto admission = journal->Admit(OperationJournalUTPlan("batch-retry", 3));
        REQUIRE(admission);
        auto run = journal->TransitionToRunning(std::move(*admission));
        REQUIRE(run);
        const auto persisted_running = OperationJournalUTReadFile(directory);
        const std::vector evidence{OperationJournalUTSuccess(0),
                                   OperationJournalUTSkipped(1),
                                   OperationJournalUTSuccess(2)};

        const auto real_open_at = syscalls->open_at;
        bool fail_once = true;
        syscalls->open_at = [real_open_at, &fail_once](int _directory,
                                                       const char *_path,
                                                       int _flags,
                                                       mode_t _mode) {
            if( fail_once && std::string_view{_path}.starts_with(".operation-journal-v1.json.tmp.") ) {
                fail_once = false;
                errno = EACCES;
                return -1;
            }
            return real_open_at(_directory, _path, _flags, _mode);
        };

        const auto failed = journal->Finalize(std::move(*run), evidence, OperationJournalState::Completed);
        REQUIRE_FALSE(failed);
        CHECK(failed.error().code == OperationJournalErrorCode::TemporaryCreateFailed);
        const auto running = journal->Snapshot();
        REQUIRE(running.size() == 1);
        CHECK(running[0].state == OperationJournalState::Running);
        CHECK(running[0].item_results.empty());
        CHECK(OperationJournalUTReadFile(directory) == persisted_running);

        REQUIRE(journal->Finalize(std::move(*run), evidence, OperationJournalState::Completed));
        const auto completed = journal->Snapshot();
        REQUIRE(completed.size() == 1);
        CHECK(completed[0].state == OperationJournalState::Completed);
        CHECK(completed[0].item_results == evidence);
    }
}

TEST_CASE("OperationJournal: requires an absolute private journal parent", "[operation-journal]")
{
    const auto relative = OperationJournal::Open("relative/journal");
    REQUIRE_FALSE(relative);
    CHECK(relative.error().code == OperationJournalErrorCode::InvalidParentPath);

    OperationJournalUTDirectory directory;
    REQUIRE(::chmod(directory.path.c_str(), 0777) == 0);
    const auto shared_parent = OperationJournal::Open(directory.path);
    REQUIRE_FALSE(shared_parent);
    CHECK(shared_parent.error().code == OperationJournalErrorCode::ParentOpenFailed);
    REQUIRE(::chmod(directory.path.c_str(), 0700) == 0);
}

TEST_CASE("OperationJournal: holds an exclusive journal lock", "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    {
        auto first = OperationJournal::Open(directory.path);
        REQUIRE(first);
        const auto concurrent = OperationJournal::Open(directory.path);
        REQUIRE_FALSE(concurrent);
        CHECK(concurrent.error().code == OperationJournalErrorCode::JournalAlreadyOpen);
    }
    CHECK(OperationJournal::Open(directory.path));
}

TEST_CASE("OperationJournal: emits deterministic JSON for the same candidate snapshot", "[operation-journal]")
{
    OperationJournalUTDirectory first_directory;
    OperationJournalUTDirectory second_directory;
    auto first = OperationJournalUTOpen(first_directory, OperationJournalTesting::DefaultSyscalls());
    auto second = OperationJournalUTOpen(second_directory, OperationJournalTesting::DefaultSyscalls());
    REQUIRE(first);
    REQUIRE(second);
    auto first_receipt = first->Admit(OperationJournalUTPlan("deterministic", 2));
    auto second_receipt = second->Admit(OperationJournalUTPlan("deterministic", 2));
    REQUIRE(first_receipt);
    REQUIRE(second_receipt);
    REQUIRE(first->TransitionToRunning(std::move(*first_receipt)));
    REQUIRE(second->TransitionToRunning(std::move(*second_receipt)));
    REQUIRE(OperationJournalTesting::RecordItemResult(*first, "deterministic", OperationJournalUTSuccess(1)));
    REQUIRE(OperationJournalTesting::RecordItemResult(*first, "deterministic", OperationJournalUTSuccess(0)));
    REQUIRE(OperationJournalTesting::RecordItemResult(*second, "deterministic", OperationJournalUTSuccess(0)));
    REQUIRE(OperationJournalTesting::RecordItemResult(*second, "deterministic", OperationJournalUTSuccess(1)));
    CHECK(OperationJournalUTReadFile(first_directory) == OperationJournalUTReadFile(second_directory));
}

TEST_CASE("OperationJournal: round-trips item linkage and legal terminal lifecycle", "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        auto receipt = journal->Admit(OperationJournalUTPlan("linked", 2));
        REQUIRE(receipt);
        REQUIRE(journal->TransitionToRunning(std::move(*receipt)));
        REQUIRE(OperationJournalTesting::RecordItemResult(*journal, "linked", OperationJournalUTSuccess(1)));
        REQUIRE(OperationJournalTesting::RecordItemResult(*journal, "linked", OperationJournalUTSuccess(0)));
        REQUIRE(OperationJournalTesting::Transition(*journal, "linked", OperationJournalState::Completed));
    }
    auto reopened = OperationJournal::Open(directory.path);
    REQUIRE(reopened);
    const auto snapshot = reopened->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].plan == OperationJournalUTPlan("linked", 2));
    CHECK(snapshot[0].state == OperationJournalState::Completed);
    CHECK(snapshot[0].item_results ==
          std::vector{OperationJournalUTSuccess(0), OperationJournalUTSuccess(1)});
}

TEST_CASE("OperationJournal: round-trips strict ordered item evidence without dropping errno or sync state",
          "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    const auto cleanup = OperationJournalUTCleanupFailure(0);
    auto published_failure = OperationJournalUTFailure(
        1, OperationJournalItemError::Metadata, true, OperationJournalRecoveryAction::InspectDestination);
    const auto unknown_publication = OperationJournalUTUnknownPublicationFailure(2);
    published_failure.system_error = EPERM;
    published_failure.filesystem_sync_status = OperationJournalFilesystemSyncStatus::Failed;
    published_failure.filesystem_sync_system_error = EIO;
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        auto receipt = journal->Admit(OperationJournalUTPlan("evidence", 3));
        REQUIRE(receipt);
        REQUIRE(journal->TransitionToRunning(std::move(*receipt)));
        REQUIRE(OperationJournalTesting::RecordItemResult(*journal, "evidence", published_failure));
        REQUIRE(OperationJournalTesting::RecordItemResult(*journal, "evidence", cleanup));
        REQUIRE(OperationJournalTesting::RecordItemResult(*journal, "evidence", unknown_publication));
        REQUIRE(OperationJournalTesting::Transition(*journal, "evidence", OperationJournalState::Failed));
    }

    const auto json = OperationJournalUTReadFile(directory);
    CHECK(json.find("\"destination_publication\":\"unknown\"") != std::string::npos);
    const std::array ordered_members{
        std::string_view{"\"item_index\":"},
        std::string_view{"\"status\":"},
        std::string_view{"\"error\":"},
        std::string_view{"\"system_error\":"},
        std::string_view{"\"prior_error\":"},
        std::string_view{"\"prior_system_error\":"},
        std::string_view{"\"bytes\":"},
        std::string_view{"\"destination_publication\":"},
        std::string_view{"\"filesystem_sync_status\":"},
        std::string_view{"\"filesystem_sync_system_error\":"},
        std::string_view{"\"recovery_action\":"},
    };
    size_t position = json.find("\"item_results\":[{");
    REQUIRE(position != std::string::npos);
    for( const auto member : ordered_members ) {
        position = json.find(member, position);
        REQUIRE(position != std::string::npos);
        position += member.size();
    }

    auto reopened = OperationJournal::Open(directory.path);
    REQUIRE(reopened);
    const auto snapshot = reopened->Snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].state == OperationJournalState::Failed);
    CHECK(snapshot[0].item_results == std::vector{cleanup, published_failure, unknown_publication});
}

TEST_CASE("OperationJournal: rejects missing and unknown strict item evidence members", "[operation-journal]")
{
    const auto persisted_cleanup = [](const OperationJournalUTDirectory &_directory) {
        auto journal = OperationJournal::Open(_directory.path);
        REQUIRE(journal);
        auto receipt = journal->Admit(OperationJournalUTPlan("strict-members"));
        REQUIRE(receipt);
        REQUIRE(journal->TransitionToRunning(std::move(*receipt)));
        REQUIRE(OperationJournalTesting::RecordItemResult(
            *journal, "strict-members", OperationJournalUTCleanupFailure()));
        REQUIRE(OperationJournalTesting::Transition(
            *journal, "strict-members", OperationJournalState::Failed));
    };

    SECTION("missing")
    {
        OperationJournalUTDirectory directory;
        persisted_cleanup(directory);
        auto json = OperationJournalUTReadFile(directory);
        const std::string member = "\"prior_system_error\":" + std::to_string(EIO) + ",";
        const auto position = json.find(member);
        REQUIRE(position != std::string::npos);
        json.erase(position, member.size());
        OperationJournalUTWriteFile(directory, json);
        const auto reopened = OperationJournal::Open(directory.path);
        REQUIRE_FALSE(reopened);
        CHECK(reopened.error().code == OperationJournalErrorCode::CorruptJournal);
    }

    SECTION("unknown")
    {
        OperationJournalUTDirectory directory;
        persisted_cleanup(directory);
        auto json = OperationJournalUTReadFile(directory);
        const auto position = json.find("\"recovery_action\":");
        REQUIRE(position != std::string::npos);
        json.insert(position, "\"unexpected_evidence\":0,");
        OperationJournalUTWriteFile(directory, json);
        const auto reopened = OperationJournal::Open(directory.path);
        REQUIRE_FALSE(reopened);
        CHECK(reopened.error().code == OperationJournalErrorCode::CorruptJournal);
    }

    SECTION("legacy boolean publication evidence")
    {
        OperationJournalUTDirectory directory;
        persisted_cleanup(directory);
        auto json = OperationJournalUTReadFile(directory);
        const std::string token = "\"destination_publication\":\"not_published\"";
        const auto position = json.find(token);
        REQUIRE(position != std::string::npos);
        json.replace(position, token.size(), "\"destination_publication\":false");
        OperationJournalUTWriteFile(directory, json);
        const auto reopened = OperationJournal::Open(directory.path);
        REQUIRE_FALSE(reopened);
        CHECK(reopened.error().code == OperationJournalErrorCode::CorruptJournal);
    }

    SECTION("unknown publication token")
    {
        OperationJournalUTDirectory directory;
        persisted_cleanup(directory);
        auto json = OperationJournalUTReadFile(directory);
        const std::string token = "\"destination_publication\":\"not_published\"";
        const auto position = json.find(token);
        REQUIRE(position != std::string::npos);
        json.replace(position, token.size(), "\"destination_publication\":\"ambiguous\"");
        OperationJournalUTWriteFile(directory, json);
        const auto reopened = OperationJournal::Open(directory.path);
        REQUIRE_FALSE(reopened);
        CHECK(reopened.error().code == OperationJournalErrorCode::CorruptJournal);
    }
}

TEST_CASE("OperationJournal: rejects duplicate admission illegal transitions and invalid item linkage",
          "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    auto journal = OperationJournal::Open(directory.path);
    REQUIRE(journal);
    auto receipt = journal->Admit(OperationJournalUTPlan());
    REQUIRE(receipt);

    const auto duplicate = journal->Admit(OperationJournalUTPlan());
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == OperationJournalErrorCode::PlanAlreadyAdmitted);
    const auto premature_complete =
        OperationJournalTesting::Transition(*journal, "plan-1", OperationJournalState::Completed);
    REQUIRE_FALSE(premature_complete);
    CHECK(premature_complete.error().code == OperationJournalErrorCode::InvalidTransition);
    const auto premature_result =
        OperationJournalTesting::RecordItemResult(*journal, "plan-1", OperationJournalUTSuccess());
    REQUIRE_FALSE(premature_result);
    CHECK(premature_result.error().code == OperationJournalErrorCode::InvalidTransition);
    const auto legacy_start =
        OperationJournalTesting::Transition(*journal, "plan-1", OperationJournalState::Running);
    REQUIRE_FALSE(legacy_start);
    CHECK(legacy_start.error().code == OperationJournalErrorCode::InvalidTransition);
    REQUIRE(journal->TransitionToRunning(std::move(*receipt)));

    auto invalid = OperationJournalUTSuccess(1);
    const auto wrong_index = OperationJournalTesting::RecordItemResult(*journal, "plan-1", invalid);
    REQUIRE_FALSE(wrong_index);
    CHECK(wrong_index.error().code == OperationJournalErrorCode::InvalidItemResult);
    REQUIRE(OperationJournalTesting::RecordItemResult(*journal, "plan-1", OperationJournalUTSuccess()));
    const auto repeated =
        OperationJournalTesting::RecordItemResult(*journal, "plan-1", OperationJournalUTSuccess());
    REQUIRE_FALSE(repeated);
    CHECK(repeated.error().code == OperationJournalErrorCode::InvalidItemResult);
    REQUIRE(OperationJournalTesting::Transition(*journal, "plan-1", OperationJournalState::Completed));
    const auto terminal =
        OperationJournalTesting::Transition(*journal, "plan-1", OperationJournalState::Failed);
    REQUIRE_FALSE(terminal);
    CHECK(terminal.error().code == OperationJournalErrorCode::InvalidTransition);
}

TEST_CASE("OperationJournal: enforces item result and terminal lifecycle matrices", "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    auto journal = OperationJournal::Open(directory.path);
    REQUIRE(journal);
    const auto start = [&](const OperationPlan &_plan) {
        auto receipt = journal->Admit(_plan);
        REQUIRE(receipt);
        REQUIRE(journal->TransitionToRunning(std::move(*receipt)));
    };

    start(OperationJournalUTPlan("matrix"));
    std::vector<OperationJournalItemResult> invalid_results;
    {
        auto result = OperationJournalUTSuccess();
        result.error = OperationJournalItemError::Write;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTSuccess();
        result.destination_publication = OperationJournalPublicationState::NotPublished;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTSuccess();
        result.destination_publication = OperationJournalPublicationState::Unknown;
        result.filesystem_sync_status = OperationJournalFilesystemSyncStatus::NotAttempted;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTSuccess();
        result.recovery_action = OperationJournalRecoveryAction::Retry;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTSuccess();
        result.filesystem_sync_status = OperationJournalFilesystemSyncStatus::NotAttempted;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTSuccess();
        result.filesystem_sync_status = OperationJournalFilesystemSyncStatus::Failed;
        result.filesystem_sync_system_error = EIO;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTSkipped();
        result.bytes = 1;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTSkipped();
        result.destination_publication = OperationJournalPublicationState::Published;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTCancelled();
        result.error = OperationJournalItemError::None;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTCancelled();
        result.destination_publication = OperationJournalPublicationState::Published;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTCancelled();
        result.destination_publication = OperationJournalPublicationState::Unknown;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTCancelled();
        result.recovery_action = OperationJournalRecoveryAction::Retry;
        invalid_results.emplace_back(result);
    }
    invalid_results.emplace_back(OperationJournalUTFailure(0, OperationJournalItemError::None));
    invalid_results.emplace_back(OperationJournalUTFailure(0, OperationJournalItemError::Cancelled));
    invalid_results.emplace_back(OperationJournalUTFailure(
        0, OperationJournalItemError::Write, true, OperationJournalRecoveryAction::Retry));
    invalid_results.emplace_back(OperationJournalUTFailure(
        0, OperationJournalItemError::Commit, false, OperationJournalRecoveryAction::InspectDestination));
    invalid_results.emplace_back(OperationJournalUTFailure(
        0, OperationJournalItemError::Cleanup, true, OperationJournalRecoveryAction::RemoveTemporaryItem));
    invalid_results.emplace_back(OperationJournalUTFailure(
        0, OperationJournalItemError::Write, false, OperationJournalRecoveryAction::RestoreSource));
    {
        auto result = OperationJournalUTFailure();
        result.system_error = 0;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTFailure();
        result.prior_error = OperationJournalItemError::Read;
        result.prior_system_error = EIO;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTFailure();
        result.filesystem_sync_system_error = EIO;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTUnknownPublicationFailure();
        result.recovery_action = OperationJournalRecoveryAction::Retry;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTUnknownPublicationFailure();
        result.error = OperationJournalItemError::Metadata;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTUnknownPublicationFailure();
        result.filesystem_sync_status = OperationJournalFilesystemSyncStatus::Confirmed;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTUnknownPublicationFailure();
        result.filesystem_sync_system_error = EIO;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTFailure(
            0, OperationJournalItemError::Commit, true, OperationJournalRecoveryAction::InspectDestination);
        result.filesystem_sync_status = OperationJournalFilesystemSyncStatus::Failed;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTCleanupFailure();
        result.prior_error = OperationJournalItemError::None;
        result.prior_system_error = 0;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTCleanupFailure();
        result.destination_publication = OperationJournalPublicationState::Unknown;
        result.recovery_action = OperationJournalRecoveryAction::InspectDestination;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTCleanupFailure();
        result.prior_system_error = 0;
        invalid_results.emplace_back(result);
    }
    {
        auto result = OperationJournalUTCleanupFailure();
        result.prior_error = OperationJournalItemError::Cancelled;
        invalid_results.emplace_back(result);
    }
    for( const auto &result : invalid_results ) {
        const auto recorded = OperationJournalTesting::RecordItemResult(*journal, "matrix", result);
        REQUIRE_FALSE(recorded);
        CHECK(recorded.error().code == OperationJournalErrorCode::InvalidItemResult);
    }
    REQUIRE(OperationJournalTesting::RecordItemResult(*journal, "matrix", OperationJournalUTFailure()));
    REQUIRE(OperationJournalTesting::Transition(*journal, "matrix", OperationJournalState::Failed));

    start(OperationJournalUTPlan("published-confirmed"));
    REQUIRE(OperationJournalTesting::RecordItemResult(
        *journal,
        "published-confirmed",
        OperationJournalUTFailure(
            0, OperationJournalItemError::Metadata, true, OperationJournalRecoveryAction::InspectDestination)));
    REQUIRE(OperationJournalTesting::Transition(
        *journal, "published-confirmed", OperationJournalState::Failed));

    start(OperationJournalUTPlan("published-sync-failed"));
    auto published_sync_failed = OperationJournalUTFailure(
        0, OperationJournalItemError::Metadata, true, OperationJournalRecoveryAction::InspectDestination);
    published_sync_failed.system_error = EPERM;
    published_sync_failed.filesystem_sync_status = OperationJournalFilesystemSyncStatus::Failed;
    published_sync_failed.filesystem_sync_system_error = EIO;
    REQUIRE(OperationJournalTesting::RecordItemResult(
        *journal, "published-sync-failed", published_sync_failed));
    REQUIRE(OperationJournalTesting::Transition(
        *journal, "published-sync-failed", OperationJournalState::Failed));

    start(OperationJournalUTPlan("unknown-publication"));
    REQUIRE(OperationJournalTesting::RecordItemResult(
        *journal, "unknown-publication", OperationJournalUTUnknownPublicationFailure()));
    REQUIRE(OperationJournalTesting::Transition(
        *journal, "unknown-publication", OperationJournalState::Failed));

    start(OperationJournalUTPlan("cleanup-prior"));
    REQUIRE(OperationJournalTesting::RecordItemResult(
        *journal, "cleanup-prior", OperationJournalUTCleanupFailure()));
    REQUIRE(OperationJournalTesting::Transition(*journal, "cleanup-prior", OperationJournalState::Failed));

    start(OperationJournalUTPlan("partial-failure", 3));
    REQUIRE(OperationJournalTesting::RecordItemResult(
        *journal, "partial-failure", OperationJournalUTSuccess(0)));
    REQUIRE(OperationJournalTesting::RecordItemResult(
        *journal, "partial-failure", OperationJournalUTFailure(2)));
    REQUIRE(OperationJournalTesting::Transition(
        *journal, "partial-failure", OperationJournalState::Failed));

    start(OperationJournalUTPlan("partial-cancel", 3));
    REQUIRE(OperationJournalTesting::RecordItemResult(
        *journal, "partial-cancel", OperationJournalUTSuccess(0)));
    REQUIRE(OperationJournalTesting::RecordItemResult(
        *journal, "partial-cancel", OperationJournalUTCancelled(2)));
    REQUIRE(OperationJournalTesting::Transition(
        *journal, "partial-cancel", OperationJournalState::Cancelled));

    start(OperationJournalUTPlan("wrong-failure"));
    REQUIRE(OperationJournalTesting::RecordItemResult(
        *journal, "wrong-failure", OperationJournalUTSuccess()));
    const auto wrong_failure =
        OperationJournalTesting::Transition(*journal, "wrong-failure", OperationJournalState::Failed);
    REQUIRE_FALSE(wrong_failure);
    CHECK(wrong_failure.error().code == OperationJournalErrorCode::InvalidTransition);

    start(OperationJournalUTPlan("wrong-cancel"));
    REQUIRE(OperationJournalTesting::RecordItemResult(
        *journal, "wrong-cancel", OperationJournalUTFailure()));
    const auto wrong_cancel =
        OperationJournalTesting::Transition(*journal, "wrong-cancel", OperationJournalState::Cancelled);
    REQUIRE_FALSE(wrong_cancel);
    CHECK(wrong_cancel.error().code == OperationJournalErrorCode::InvalidTransition);

    start(OperationJournalUTPlan("completed", 2));
    REQUIRE(OperationJournalTesting::RecordItemResult(
        *journal, "completed", OperationJournalUTSuccess(0)));
    const auto incomplete =
        OperationJournalTesting::Transition(*journal, "completed", OperationJournalState::Completed);
    REQUIRE_FALSE(incomplete);
    CHECK(incomplete.error().code == OperationJournalErrorCode::InvalidTransition);
    REQUIRE(OperationJournalTesting::RecordItemResult(
        *journal, "completed", OperationJournalUTSkipped(1)));
    REQUIRE(OperationJournalTesting::Transition(*journal, "completed", OperationJournalState::Completed));

    start(OperationJournalUTPermanentDeletePlan("delete"));
    const auto wrongly_published =
        OperationJournalTesting::RecordItemResult(*journal, "delete", OperationJournalUTSuccess());
    REQUIRE_FALSE(wrongly_published);
    CHECK(wrongly_published.error().code == OperationJournalErrorCode::InvalidItemResult);
    auto deleted = OperationJournalUTSuccess();
    deleted.destination_publication = OperationJournalPublicationState::NotPublished;
    deleted.filesystem_sync_status = OperationJournalFilesystemSyncStatus::NotAttempted;
    REQUIRE(OperationJournalTesting::RecordItemResult(*journal, "delete", deleted));
    REQUIRE(OperationJournalTesting::Transition(*journal, "delete", OperationJournalState::Completed));

    auto queued_cancel = journal->Admit(OperationJournalUTPlan("queued-cancel"));
    REQUIRE(queued_cancel);
    REQUIRE(OperationJournalTesting::Transition(
        *journal, "queued-cancel", OperationJournalState::Cancelled));
    const auto stale_queued_receipt = journal->TransitionToRunning(std::move(*queued_cancel));
    REQUIRE_FALSE(stale_queued_receipt);
    CHECK(stale_queued_receipt.error().code == OperationJournalErrorCode::InvalidAdmissionReceipt);

    REQUIRE(journal->Admit(OperationJournalUTPlan("queued-failure")));
    REQUIRE(OperationJournalTesting::Transition(
        *journal, "queued-failure", OperationJournalState::Failed));
}

TEST_CASE("OperationJournal: startup durably classifies admitted and running plans as interrupted",
          "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        REQUIRE(journal->Admit(OperationJournalUTPlan("admitted")));
        auto running_receipt = journal->Admit(OperationJournalUTPlan("running", 2));
        REQUIRE(running_receipt);
        REQUIRE(journal->TransitionToRunning(std::move(*running_receipt)));
        REQUIRE(OperationJournalTesting::RecordItemResult(
            *journal, "running", OperationJournalUTSuccess(0)));
    }
    {
        auto reopened = OperationJournal::Open(directory.path);
        REQUIRE(reopened);
        REQUIRE(reopened->Snapshot().size() == 2);
        CHECK(reopened->Snapshot()[0].state == OperationJournalState::Interrupted);
        CHECK(reopened->Snapshot()[1].state == OperationJournalState::Interrupted);
        const auto result_after_restart = OperationJournalTesting::RecordItemResult(
            *reopened, "running", OperationJournalUTSuccess(1));
        REQUIRE_FALSE(result_after_restart);
        CHECK(result_after_restart.error().code == OperationJournalErrorCode::InvalidTransition);
    }
    CHECK(OperationJournalUTReadFile(directory).find("\"state\":\"admitted\"") == std::string::npos);
    CHECK(OperationJournalUTReadFile(directory).find("\"state\":\"running\"") == std::string::npos);
}

TEST_CASE("OperationJournal: descriptor-bound read-only inspection preserves unfinished persisted state",
          "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        REQUIRE(journal->Admit(OperationJournalUTPlan("persisted-admitted")));
        auto running = journal->Admit(OperationJournalUTPlan("persisted-running"));
        REQUIRE(running);
        REQUIRE(journal->TransitionToRunning(std::move(*running)));
    }

    const auto lock_path = std::filesystem::path{directory.path} / "operation-journal-v1.lock";
    REQUIRE(::unlink(lock_path.c_str()) == 0);
    const int parent_fd = ::open(directory.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(parent_fd >= 0);
    const auto inspected = OperationJournalTesting::InspectPersistedReadOnly(parent_fd);
    REQUIRE(::close(parent_fd) == 0);

    REQUIRE(inspected);
    REQUIRE(inspected->size() == 2);
    CHECK((*inspected)[0].plan.Id().Value() == "persisted-admitted");
    CHECK((*inspected)[0].state == OperationJournalState::Admitted);
    CHECK((*inspected)[1].plan.Id().Value() == "persisted-running");
    CHECK((*inspected)[1].state == OperationJournalState::Running);
    CHECK_FALSE(std::filesystem::exists(lock_path));
    CHECK(OperationJournalUTReadFile(directory).find("\"state\":\"admitted\"") != std::string::npos);
    CHECK(OperationJournalUTReadFile(directory).find("\"state\":\"running\"") != std::string::npos);
}

TEST_CASE("OperationJournal: fails closed on corruption version mismatch and duplicate persisted plan IDs",
          "[operation-journal]")
{
    SECTION("malformed")
    {
        OperationJournalUTDirectory directory;
        REQUIRE(OperationJournal::Open(directory.path));
        OperationJournalUTWriteFile(directory, "{");
        const auto result = OperationJournal::Open(directory.path);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == OperationJournalErrorCode::MalformedJournal);
    }
    SECTION("unknown version")
    {
        OperationJournalUTDirectory directory;
        REQUIRE(OperationJournal::Open(directory.path));
        auto contents = OperationJournalUTReadFile(directory);
        const auto position = contents.find("\"version\":3");
        REQUIRE(position != std::string::npos);
        contents.replace(position, std::string_view{"\"version\":3"}.size(), "\"version\":4");
        OperationJournalUTWriteFile(directory, contents);
        const auto result = OperationJournal::Open(directory.path);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == OperationJournalErrorCode::UnsupportedSchemaVersion);
    }
    SECTION("duplicate plan ID")
    {
        OperationJournalUTDirectory directory;
        {
            auto journal = OperationJournal::Open(directory.path);
            REQUIRE(journal);
            REQUIRE(journal->Admit(OperationJournalUTPlan()));
        }
        auto contents = OperationJournalUTReadFile(directory);
        const auto array_begin = contents.find('[');
        const auto array_end = contents.rfind(']');
        REQUIRE(array_begin != std::string::npos);
        REQUIRE(array_end != std::string::npos);
        const auto entry = contents.substr(array_begin + 1, array_end - array_begin - 1);
        contents.insert(array_end, "," + entry);
        OperationJournalUTWriteFile(directory, contents);
        const auto result = OperationJournal::Open(directory.path);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == OperationJournalErrorCode::DuplicatePlanId);
    }
    SECTION("persisted lifecycle contradiction")
    {
        OperationJournalUTDirectory directory;
        {
            auto journal = OperationJournal::Open(directory.path);
            REQUIRE(journal);
            auto receipt = journal->Admit(OperationJournalUTPlan());
            REQUIRE(receipt);
            REQUIRE(journal->TransitionToRunning(std::move(*receipt)));
            REQUIRE(OperationJournalTesting::RecordItemResult(
                *journal, "plan-1", OperationJournalUTSuccess()));
            REQUIRE(OperationJournalTesting::Transition(
                *journal, "plan-1", OperationJournalState::Completed));
        }
        auto contents = OperationJournalUTReadFile(directory);
        const auto position = contents.find("\"state\":\"completed\"");
        REQUIRE(position != std::string::npos);
        contents.replace(position, std::string_view{"\"state\":\"completed\""}.size(),
                         "\"state\":\"admitted\"");
        OperationJournalUTWriteFile(directory, contents);
        const auto result = OperationJournal::Open(directory.path);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == OperationJournalErrorCode::CorruptJournal);
    }
    SECTION("entry resource limit")
    {
        OperationJournalUTDirectory directory;
        REQUIRE(OperationJournal::Open(directory.path));
        std::string contents = "{\"version\":3,\"next_operation_sequence\":1,\"entries\":[";
        for( size_t index = 0; index <= OperationJournal::MaxEntries; ++index ) {
            if( index != 0 )
                contents.push_back(',');
            contents += "null";
        }
        contents += "]}";
        OperationJournalUTWriteFile(directory, contents);
        const auto result = OperationJournal::Open(directory.path);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == OperationJournalErrorCode::ResourceLimitExceeded);
    }
}

TEST_CASE("OperationJournal: an orphaned exclusive temporary file does not wedge later admission",
          "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    auto syscalls = OperationJournalTesting::DefaultSyscalls();
    auto journal = OperationJournalUTOpen(directory, syscalls);
    REQUIRE(journal);
    const std::string orphan_name = ".operation-journal-v1.json.tmp." + std::string(32, '0');
    const auto orphan_path = std::filesystem::path{directory.path} / orphan_name;
    const int orphan = ::open(orphan_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    REQUIRE(orphan >= 0);
    REQUIRE(::close(orphan) == 0);

    size_t random_calls = 0;
    syscalls->random_bytes = [&random_calls](void *_buffer, size_t _size) {
        std::memset(_buffer, random_calls++ == 0 ? 0 : 1, _size);
    };
    REQUIRE(journal->Admit(OperationJournalUTPlan()));
    CHECK(random_calls == 2);
    CHECK(std::filesystem::exists(orphan_path));
}

TEST_CASE("OperationJournal: descriptor ownership survives callback exceptions", "[operation-journal]")
{
    SECTION("parent validation")
    {
        OperationJournalUTDirectory directory;
        OperationJournalUTFDTracker tracker;
        tracker.syscalls->fstat = [](int, struct stat *) -> int { throw std::runtime_error{"fstat"}; };

        CHECK_THROWS_AS(OperationJournalUTOpen(directory, tracker.syscalls), std::runtime_error);
        CHECK(tracker.live.empty());
    }

    SECTION("journal read")
    {
        OperationJournalUTDirectory directory;
        OperationJournalUTFDTracker tracker;
        {
            auto journal = OperationJournalUTOpen(directory, tracker.syscalls);
            REQUIRE(journal);
        }
        REQUIRE(tracker.live.empty());
        tracker.syscalls->read = [](int, void *, size_t) -> ssize_t { throw std::runtime_error{"read"}; };

        CHECK_THROWS_AS(OperationJournalUTOpen(directory, tracker.syscalls), std::runtime_error);
        CHECK(tracker.live.empty());
    }

    SECTION("staging write")
    {
        OperationJournalUTDirectory directory;
        OperationJournalUTFDTracker tracker;
        {
            auto journal = OperationJournalUTOpen(directory, tracker.syscalls);
            REQUIRE(journal);
            const auto persistent_descriptors = tracker.live.size();
            REQUIRE(persistent_descriptors == 2);
            tracker.syscalls->write = [](int, const void *, size_t) -> ssize_t {
                throw std::runtime_error{"write"};
            };

            CHECK_THROWS_AS(journal->Admit(OperationJournalUTPlan()), std::runtime_error);
            CHECK(tracker.live.size() == persistent_descriptors);
            CHECK(journal->Snapshot().empty());
        }
        CHECK(tracker.live.empty());
    }
}

TEST_CASE("OperationJournal: retries interrupted and partial IO", "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    auto syscalls = OperationJournalTesting::DefaultSyscalls();
    {
        auto journal = OperationJournalUTOpen(directory, syscalls);
        REQUIRE(journal);

        const auto real_write = syscalls->write;
        size_t writes = 0;
        syscalls->write = [real_write, &writes](int _fd, const void *_buffer, size_t _size) {
            ++writes;
            if( writes == 1 ) {
                errno = EINTR;
                return ssize_t{-1};
            }
            return real_write(_fd, _buffer, std::min<size_t>(_size, 7));
        };
        const auto real_fsync = syscalls->fsync;
        size_t syncs = 0;
        syscalls->fsync = [real_fsync, &syncs](int _fd) {
            ++syncs;
            if( syncs == 1 ) {
                errno = EINTR;
                return -1;
            }
            return real_fsync(_fd);
        };
        REQUIRE(journal->Admit(OperationJournalUTPlan()));
        CHECK(writes > 2);
        CHECK(syncs >= 3);
        syscalls->write = real_write;
        syscalls->fsync = real_fsync;
    }

    const auto real_read = syscalls->read;
    size_t reads = 0;
    syscalls->read = [real_read, &reads](int _fd, void *_buffer, size_t _size) {
        ++reads;
        if( reads == 1 ) {
            errno = EINTR;
            return ssize_t{-1};
        }
        return real_read(_fd, _buffer, std::min<size_t>(_size, 5));
    };
    auto reopened = OperationJournalUTOpen(directory, syscalls);
    REQUIRE(reopened);
    CHECK(reads > 2);
    CHECK(reopened->Snapshot()[0].state == OperationJournalState::Interrupted);
}

TEST_CASE("OperationJournal: durable mutation failures preserve committed in-memory state", "[operation-journal]")
{
    struct FaultCase {
        std::string name;
        OperationJournalErrorCode expected;
        std::function<void(const std::shared_ptr<OperationJournalSyscalls> &)> inject;
    };
    const std::vector<FaultCase> cases{
        {"temporary create", OperationJournalErrorCode::TemporaryCreateFailed, [](const auto &syscalls) {
             const auto real = syscalls->open_at;
             syscalls->open_at = [real](int _directory, const char *_path, int _flags, mode_t _mode) {
                 if( std::string_view{_path}.starts_with(".operation-journal") ) {
                     errno = EACCES;
                     return -1;
                 }
                 return real(_directory, _path, _flags, _mode);
             };
         }},
        {"write", OperationJournalErrorCode::WriteFailed, [](const auto &syscalls) {
             syscalls->write = [](int, const void *, size_t) {
                 errno = EIO;
                 return ssize_t{-1};
             };
         }},
        {"file fsync", OperationJournalErrorCode::FileSyncFailed, [](const auto &syscalls) {
             syscalls->fsync = [](int) {
                 errno = EIO;
                 return -1;
             };
         }},
        {"close", OperationJournalErrorCode::CloseFailed, [](const auto &syscalls) {
             const auto real = syscalls->close;
             syscalls->close = [real](int _fd) {
                 real(_fd);
                 errno = EIO;
                 return -1;
             };
         }},
        {"rename", OperationJournalErrorCode::DurabilityUncertain, [](const auto &syscalls) {
             syscalls->rename_at = [](int, const char *, int, const char *) {
                 errno = EIO;
                 return -1;
             };
         }},
    };

    for( const auto &test : cases ) {
        DYNAMIC_SECTION(test.name)
        {
            OperationJournalUTDirectory directory;
            auto syscalls = OperationJournalTesting::DefaultSyscalls();
            auto journal = OperationJournalUTOpen(directory, syscalls);
            REQUIRE(journal);
            test.inject(syscalls);
            const auto admitted = journal->Admit(OperationJournalUTPlan());
            REQUIRE_FALSE(admitted);
            CHECK(admitted.error().code == test.expected);
            CHECK(journal->Snapshot().empty());
            if( test.expected == OperationJournalErrorCode::DurabilityUncertain ) {
                const auto retry = journal->Admit(OperationJournalUTPlan("retry"));
                REQUIRE_FALSE(retry);
                CHECK(retry.error().code == OperationJournalErrorCode::JournalUnusable);
            }
        }
    }
}

TEST_CASE("OperationJournal: post-rename parent sync failure yields no receipt and poisons the handle",
          "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    auto syscalls = OperationJournalTesting::DefaultSyscalls();
    auto journal = OperationJournalUTOpen(directory, syscalls);
    REQUIRE(journal);
    const auto real_fsync = syscalls->fsync;
    size_t calls = 0;
    syscalls->fsync = [real_fsync, &calls](int _fd) {
        ++calls;
        if( calls == 2 ) {
            errno = EIO;
            return -1;
        }
        return real_fsync(_fd);
    };

    const auto admitted = journal->Admit(OperationJournalUTPlan());
    REQUIRE_FALSE(admitted);
    CHECK(admitted.error().code == OperationJournalErrorCode::DurabilityUncertain);
    CHECK(journal->Snapshot().empty());
    const auto retry = journal->Admit(OperationJournalUTPlan("other"));
    REQUIRE_FALSE(retry);
    CHECK(retry.error().code == OperationJournalErrorCode::JournalUnusable);
}

TEST_CASE("OperationJournal: startup interruption rewrite fails closed on parent sync uncertainty",
          "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    {
        auto journal = OperationJournal::Open(directory.path);
        REQUIRE(journal);
        REQUIRE(journal->Admit(OperationJournalUTPlan()));
    }
    auto syscalls = OperationJournalTesting::DefaultSyscalls();
    const auto real_fsync = syscalls->fsync;
    size_t calls = 0;
    syscalls->fsync = [real_fsync, &calls](int _fd) {
        ++calls;
        if( calls == 2 ) {
            errno = EIO;
            return -1;
        }
        return real_fsync(_fd);
    };
    const auto reopened = OperationJournalUTOpen(directory, syscalls);
    REQUIRE_FALSE(reopened);
    CHECK(reopened.error().code == OperationJournalErrorCode::DurabilityUncertain);
}

TEST_CASE("OperationJournal: ambiguous rename interruption yields no admission receipt", "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    auto syscalls = OperationJournalTesting::DefaultSyscalls();
    auto journal = OperationJournalUTOpen(directory, syscalls);
    REQUIRE(journal);
    syscalls->rename_at = [](int, const char *, int, const char *) {
        errno = EINTR;
        return -1;
    };
    const auto admitted = journal->Admit(OperationJournalUTPlan());
    REQUIRE_FALSE(admitted);
    CHECK(admitted.error().code == OperationJournalErrorCode::DurabilityUncertain);
    CHECK(journal->Snapshot().empty());
    const auto retry = journal->Admit(OperationJournalUTPlan("retry"));
    REQUIRE_FALSE(retry);
    CHECK(retry.error().code == OperationJournalErrorCode::JournalUnusable);
}

TEST_CASE("OperationJournal: committed rename reported as failure poisons and reconciles on reopen",
          "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    auto syscalls = OperationJournalTesting::DefaultSyscalls();
    const auto real_rename = syscalls->rename_at;
    {
        auto journal = OperationJournalUTOpen(directory, syscalls);
        REQUIRE(journal);
        syscalls->rename_at = [real_rename](int _from_directory,
                                           const char *_from,
                                           int _to_directory,
                                           const char *_to) {
            const int result = real_rename(_from_directory, _from, _to_directory, _to);
            if( result != 0 )
                return result;
            errno = EIO;
            return -1;
        };

        const auto admitted = journal->Admit(OperationJournalUTPlan("committed-but-uncertain"));
        REQUIRE_FALSE(admitted);
        CHECK(admitted.error().code == OperationJournalErrorCode::DurabilityUncertain);
        CHECK(journal->Snapshot().empty());
        const auto retry = journal->Admit(OperationJournalUTPlan("retry"));
        REQUIRE_FALSE(retry);
        CHECK(retry.error().code == OperationJournalErrorCode::JournalUnusable);
    }

    syscalls->rename_at = real_rename;
    auto reopened = OperationJournalUTOpen(directory, syscalls);
    REQUIRE(reopened);
    REQUIRE(reopened->Snapshot().size() == 1);
    CHECK(reopened->Snapshot()[0].plan.Id().Value() == "committed-but-uncertain");
    CHECK(reopened->Snapshot()[0].state == OperationJournalState::Interrupted);
}

TEST_CASE("OperationJournal: committed rename followed by an exception preserves uncertainty semantics",
          "[operation-journal]")
{
    OperationJournalUTDirectory directory;
    auto syscalls = OperationJournalTesting::DefaultSyscalls();
    const auto real_rename = syscalls->rename_at;
    {
        auto journal = OperationJournalUTOpen(directory, syscalls);
        REQUIRE(journal);
        syscalls->rename_at = [real_rename](int _from_directory,
                                           const char *_from,
                                           int _to_directory,
                                           const char *_to) -> int {
            const int result = real_rename(_from_directory, _from, _to_directory, _to);
            if( result != 0 )
                return result;
            throw std::runtime_error{"post-rename"};
        };

        const auto admitted = journal->Admit(OperationJournalUTPlan("exception-after-commit"));
        REQUIRE_FALSE(admitted);
        CHECK(admitted.error().code == OperationJournalErrorCode::DurabilityUncertain);
        CHECK(journal->Snapshot().empty());
        const auto retry = journal->Admit(OperationJournalUTPlan("retry"));
        REQUIRE_FALSE(retry);
        CHECK(retry.error().code == OperationJournalErrorCode::JournalUnusable);
    }

    syscalls->rename_at = real_rename;
    auto reopened = OperationJournalUTOpen(directory, syscalls);
    REQUIRE(reopened);
    REQUIRE(reopened->Snapshot().size() == 1);
    CHECK(reopened->Snapshot()[0].plan.Id().Value() == "exception-after-commit");
    CHECK(reopened->Snapshot()[0].state == OperationJournalState::Interrupted);
}
