// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "OperationJournal.h"
#include "OperationJournalTesting.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>
#include <utility>

namespace nc::ops {
namespace {

using OperationJournalJSONValue = rapidjson::Value;
using OperationJournalFailureResult = std::unexpected<OperationJournalError>;

OperationJournalFailureResult OperationJournalFailure(OperationJournalErrorCode _code, int _system_error = 0)
{
    return std::unexpected(OperationJournalError{.code = _code,
                                                  .system_error = _system_error,
                                                  .plan_codec_error = std::nullopt});
}

OperationJournalFailureResult OperationJournalCodecFailure(OperationPlanCodecError _error)
{
    return std::unexpected(OperationJournalError{.code = OperationJournalErrorCode::PlanCodecFailed,
                                                  .system_error = 0,
                                                  .plan_codec_error = std::move(_error)});
}

uint64_t OperationJournalOperationIdSequence(const OperationId &_operation_id) noexcept
{
    const auto serialized = _operation_id.ToString();
    constexpr std::string_view prefix{"op-"};
    uint64_t sequence = 0;
    const auto [parsed_until, error] = std::from_chars(
        serialized.data() + prefix.size(), serialized.data() + serialized.size(), sequence);
    const bool valid = error == std::errc{} && parsed_until == serialized.data() + serialized.size() && sequence != 0;
    assert(valid);
    if( !valid )
        return 0;
    return sequence;
}

class OperationJournalDescriptor final
{
public:
    OperationJournalDescriptor() noexcept = default;
    OperationJournalDescriptor(int _fd, const OperationJournalSyscalls &_syscalls) noexcept
        : m_Fd{_fd}, m_Syscalls{&_syscalls}
    {
    }
    ~OperationJournalDescriptor() noexcept { CloseNoThrow(); }

    OperationJournalDescriptor(const OperationJournalDescriptor &) = delete;
    OperationJournalDescriptor &operator=(const OperationJournalDescriptor &) = delete;

    OperationJournalDescriptor(OperationJournalDescriptor &&_other) noexcept
        : m_Fd{std::exchange(_other.m_Fd, -1)}, m_Syscalls{std::exchange(_other.m_Syscalls, nullptr)}
    {
    }

    OperationJournalDescriptor &operator=(OperationJournalDescriptor &&_other) noexcept
    {
        if( this != &_other ) {
            CloseNoThrow();
            m_Fd = std::exchange(_other.m_Fd, -1);
            m_Syscalls = std::exchange(_other.m_Syscalls, nullptr);
        }
        return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return m_Fd >= 0; }
    [[nodiscard]] int Get() const noexcept
    {
        assert(m_Fd >= 0);
        return m_Fd;
    }

    int Close()
    {
        assert(m_Fd >= 0 && m_Syscalls != nullptr);
        const int fd = std::exchange(m_Fd, -1);
        const auto *syscalls = std::exchange(m_Syscalls, nullptr);
        return syscalls->close(fd);
    }

private:
    void CloseNoThrow() noexcept
    {
        if( m_Fd < 0 )
            return;
        const int fd = std::exchange(m_Fd, -1);
        const auto *syscalls = std::exchange(m_Syscalls, nullptr);
        try {
            (void)syscalls->close(fd);
        }
        catch( ... ) {
        }
    }

    int m_Fd{-1};
    const OperationJournalSyscalls *m_Syscalls{nullptr};
};

bool OperationJournalValidSyscalls(const OperationJournalSyscalls &_syscalls) noexcept
{
    return _syscalls.open && _syscalls.open_at && _syscalls.read && _syscalls.write && _syscalls.fsync &&
           _syscalls.fstat && _syscalls.flock && _syscalls.rename_at && _syscalls.close && _syscalls.random_bytes;
}

template <size_t N>
std::expected<std::array<const OperationJournalJSONValue *, N>, OperationJournalError>
OperationJournalMembers(const OperationJournalJSONValue &_object, const std::array<std::string_view, N> &_names)
{
    if( !_object.IsObject() )
        return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);

    std::array<const OperationJournalJSONValue *, N> result{};
    for( auto member = _object.MemberBegin(); member != _object.MemberEnd(); ++member ) {
        const std::string_view name{member->name.GetString(), member->name.GetStringLength()};
        size_t index = N;
        for( size_t candidate = 0; candidate < N; ++candidate ) {
            if( name == _names[candidate] ) {
                index = candidate;
                break;
            }
        }
        if( index == N || result[index] != nullptr )
            return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
        result[index] = &member->value;
    }
    for( const auto *member : result ) {
        if( member == nullptr )
            return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
    }
    return result;
}

std::optional<std::string_view> OperationJournalStateToken(OperationJournalState _state) noexcept
{
    switch( _state ) {
        case OperationJournalState::Admitted:
            return "admitted";
        case OperationJournalState::Running:
            return "running";
        case OperationJournalState::Interrupted:
            return "interrupted";
        case OperationJournalState::Completed:
            return "completed";
        case OperationJournalState::Failed:
            return "failed";
        case OperationJournalState::Cancelled:
            return "cancelled";
    }
    return std::nullopt;
}

std::optional<std::string_view> OperationJournalItemStatusToken(OperationJournalItemStatus _status) noexcept
{
    switch( _status ) {
        case OperationJournalItemStatus::Succeeded:
            return "succeeded";
        case OperationJournalItemStatus::Failed:
            return "failed";
        case OperationJournalItemStatus::Cancelled:
            return "cancelled";
        case OperationJournalItemStatus::Skipped:
            return "skipped";
    }
    return std::nullopt;
}

std::optional<std::string_view> OperationJournalItemErrorToken(OperationJournalItemError _error) noexcept
{
    switch( _error ) {
        case OperationJournalItemError::None:
            return "none";
        case OperationJournalItemError::SourceChanged:
            return "source_changed";
        case OperationJournalItemError::DestinationChanged:
            return "destination_changed";
        case OperationJournalItemError::PermissionDenied:
            return "permission_denied";
        case OperationJournalItemError::Read:
            return "read";
        case OperationJournalItemError::Write:
            return "write";
        case OperationJournalItemError::Metadata:
            return "metadata";
        case OperationJournalItemError::Commit:
            return "commit";
        case OperationJournalItemError::Cleanup:
            return "cleanup";
        case OperationJournalItemError::Cancelled:
            return "cancelled";
        case OperationJournalItemError::Unknown:
            return "unknown";
    }
    return std::nullopt;
}

std::optional<std::string_view>
OperationJournalRecoveryActionToken(OperationJournalRecoveryAction _action) noexcept
{
    switch( _action ) {
        case OperationJournalRecoveryAction::None:
            return "none";
        case OperationJournalRecoveryAction::Retry:
            return "retry";
        case OperationJournalRecoveryAction::InspectDestination:
            return "inspect_destination";
        case OperationJournalRecoveryAction::RemoveTemporaryItem:
            return "remove_temporary_item";
        case OperationJournalRecoveryAction::RestoreSource:
            return "restore_source";
    }
    return std::nullopt;
}

std::optional<std::string_view>
OperationJournalFilesystemSyncStatusToken(OperationJournalFilesystemSyncStatus _status) noexcept
{
    switch( _status ) {
        case OperationJournalFilesystemSyncStatus::NotAttempted:
            return "not_attempted";
        case OperationJournalFilesystemSyncStatus::Confirmed:
            return "confirmed";
        case OperationJournalFilesystemSyncStatus::Failed:
            return "failed";
    }
    return std::nullopt;
}

std::optional<std::string_view>
OperationJournalPublicationStateToken(OperationJournalPublicationState _state) noexcept
{
    switch( _state ) {
        case OperationJournalPublicationState::NotPublished:
            return "not_published";
        case OperationJournalPublicationState::Published:
            return "published";
        case OperationJournalPublicationState::Unknown:
            return "unknown";
    }
    return std::nullopt;
}

template <class Enum, size_t N>
std::expected<Enum, OperationJournalError>
OperationJournalParseToken(const OperationJournalJSONValue &_value,
                           const std::array<std::pair<std::string_view, Enum>, N> &_tokens)
{
    if( !_value.IsString() )
        return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
    const std::string_view token{_value.GetString(), _value.GetStringLength()};
    for( const auto &[name, value] : _tokens ) {
        if( token == name )
            return value;
    }
    return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
}

std::expected<OperationJournalState, OperationJournalError>
OperationJournalParseState(const OperationJournalJSONValue &_value)
{
    static constexpr std::array tokens{
        std::pair{std::string_view{"admitted"}, OperationJournalState::Admitted},
        std::pair{std::string_view{"running"}, OperationJournalState::Running},
        std::pair{std::string_view{"interrupted"}, OperationJournalState::Interrupted},
        std::pair{std::string_view{"completed"}, OperationJournalState::Completed},
        std::pair{std::string_view{"failed"}, OperationJournalState::Failed},
        std::pair{std::string_view{"cancelled"}, OperationJournalState::Cancelled},
    };
    return OperationJournalParseToken(_value, tokens);
}

std::expected<OperationJournalItemStatus, OperationJournalError>
OperationJournalParseItemStatus(const OperationJournalJSONValue &_value)
{
    static constexpr std::array tokens{
        std::pair{std::string_view{"succeeded"}, OperationJournalItemStatus::Succeeded},
        std::pair{std::string_view{"failed"}, OperationJournalItemStatus::Failed},
        std::pair{std::string_view{"cancelled"}, OperationJournalItemStatus::Cancelled},
        std::pair{std::string_view{"skipped"}, OperationJournalItemStatus::Skipped},
    };
    return OperationJournalParseToken(_value, tokens);
}

std::expected<OperationJournalItemError, OperationJournalError>
OperationJournalParseItemError(const OperationJournalJSONValue &_value)
{
    static constexpr std::array tokens{
        std::pair{std::string_view{"none"}, OperationJournalItemError::None},
        std::pair{std::string_view{"source_changed"}, OperationJournalItemError::SourceChanged},
        std::pair{std::string_view{"destination_changed"}, OperationJournalItemError::DestinationChanged},
        std::pair{std::string_view{"permission_denied"}, OperationJournalItemError::PermissionDenied},
        std::pair{std::string_view{"read"}, OperationJournalItemError::Read},
        std::pair{std::string_view{"write"}, OperationJournalItemError::Write},
        std::pair{std::string_view{"metadata"}, OperationJournalItemError::Metadata},
        std::pair{std::string_view{"commit"}, OperationJournalItemError::Commit},
        std::pair{std::string_view{"cleanup"}, OperationJournalItemError::Cleanup},
        std::pair{std::string_view{"cancelled"}, OperationJournalItemError::Cancelled},
        std::pair{std::string_view{"unknown"}, OperationJournalItemError::Unknown},
    };
    return OperationJournalParseToken(_value, tokens);
}

std::expected<OperationJournalRecoveryAction, OperationJournalError>
OperationJournalParseRecoveryAction(const OperationJournalJSONValue &_value)
{
    static constexpr std::array tokens{
        std::pair{std::string_view{"none"}, OperationJournalRecoveryAction::None},
        std::pair{std::string_view{"retry"}, OperationJournalRecoveryAction::Retry},
        std::pair{std::string_view{"inspect_destination"}, OperationJournalRecoveryAction::InspectDestination},
        std::pair{std::string_view{"remove_temporary_item"},
                  OperationJournalRecoveryAction::RemoveTemporaryItem},
        std::pair{std::string_view{"restore_source"}, OperationJournalRecoveryAction::RestoreSource},
    };
    return OperationJournalParseToken(_value, tokens);
}

std::expected<OperationJournalFilesystemSyncStatus, OperationJournalError>
OperationJournalParseFilesystemSyncStatus(const OperationJournalJSONValue &_value)
{
    static constexpr std::array tokens{
        std::pair{std::string_view{"not_attempted"}, OperationJournalFilesystemSyncStatus::NotAttempted},
        std::pair{std::string_view{"confirmed"}, OperationJournalFilesystemSyncStatus::Confirmed},
        std::pair{std::string_view{"failed"}, OperationJournalFilesystemSyncStatus::Failed},
    };
    return OperationJournalParseToken(_value, tokens);
}

std::expected<OperationJournalPublicationState, OperationJournalError>
OperationJournalParsePublicationState(const OperationJournalJSONValue &_value)
{
    static constexpr std::array tokens{
        std::pair{std::string_view{"not_published"}, OperationJournalPublicationState::NotPublished},
        std::pair{std::string_view{"published"}, OperationJournalPublicationState::Published},
        std::pair{std::string_view{"unknown"}, OperationJournalPublicationState::Unknown},
    };
    return OperationJournalParseToken(_value, tokens);
}

bool OperationJournalValidItemResult(const OperationPlan &_plan, const OperationJournalItemResult &_result)
{
    if( _result.item_index >= _plan.Sources().size() || _result.system_error < 0 ||
        _result.prior_system_error < 0 || _result.filesystem_sync_system_error < 0 )
        return false;

    switch( _result.destination_publication ) {
        case OperationJournalPublicationState::NotPublished:
        case OperationJournalPublicationState::Unknown:
            if( _result.filesystem_sync_status != OperationJournalFilesystemSyncStatus::NotAttempted ||
                _result.filesystem_sync_system_error != 0 )
                return false;
            break;
        case OperationJournalPublicationState::Published:
            if( _result.filesystem_sync_status == OperationJournalFilesystemSyncStatus::Confirmed ) {
                if( _result.filesystem_sync_system_error != 0 )
                    return false;
            }
            else if( _result.filesystem_sync_status == OperationJournalFilesystemSyncStatus::Failed ) {
                if( _result.filesystem_sync_system_error == 0 )
                    return false;
            }
            else {
                return false;
            }
            break;
        default:
            return false;
    }

    if( _result.error == OperationJournalItemError::Cleanup ) {
        if( _result.destination_publication != OperationJournalPublicationState::NotPublished )
            return false;
        if( _result.prior_error == OperationJournalItemError::None ||
            _result.prior_error == OperationJournalItemError::Cleanup )
            return false;
        if( _result.prior_error == OperationJournalItemError::Cancelled ) {
            if( _result.prior_system_error != 0 )
                return false;
        }
        else if( _result.prior_system_error == 0 ) {
            return false;
        }
    }
    else if( _result.prior_error != OperationJournalItemError::None || _result.prior_system_error != 0 ) {
        return false;
    }

    switch( _result.status ) {
        case OperationJournalItemStatus::Succeeded: {
            const bool publishes_destination = _plan.Type() == OperationPlanType::Copy ||
                                               _plan.Type() == OperationPlanType::Move ||
                                               _plan.Type() == OperationPlanType::Rename;
            const auto expected_publication = publishes_destination
                                                  ? OperationJournalPublicationState::Published
                                                  : OperationJournalPublicationState::NotPublished;
            return _result.error == OperationJournalItemError::None && _result.system_error == 0 &&
                   _result.destination_publication == expected_publication &&
                   (!publishes_destination ||
                    _result.filesystem_sync_status == OperationJournalFilesystemSyncStatus::Confirmed) &&
                   _result.recovery_action == OperationJournalRecoveryAction::None;
        }
        case OperationJournalItemStatus::Skipped:
            return _result.error == OperationJournalItemError::None && _result.system_error == 0 &&
                   _result.bytes == 0 &&
                   _result.destination_publication == OperationJournalPublicationState::NotPublished &&
                   _result.recovery_action == OperationJournalRecoveryAction::None;
        case OperationJournalItemStatus::Cancelled:
            return _result.error == OperationJournalItemError::Cancelled && _result.system_error == 0 &&
                   _result.destination_publication == OperationJournalPublicationState::NotPublished &&
                   _result.recovery_action == OperationJournalRecoveryAction::None;
        case OperationJournalItemStatus::Failed: {
            if( _result.error == OperationJournalItemError::None ||
                _result.error == OperationJournalItemError::Cancelled )
                return false;
            if( _result.error != OperationJournalItemError::Cleanup && _result.system_error == 0 )
                return false;
            if( _result.destination_publication == OperationJournalPublicationState::Unknown ) {
                if( _result.error != OperationJournalItemError::Commit ||
                    _result.recovery_action != OperationJournalRecoveryAction::InspectDestination )
                    return false;
            }
            switch( _result.recovery_action ) {
                case OperationJournalRecoveryAction::None:
                    return _result.destination_publication != OperationJournalPublicationState::Unknown;
                case OperationJournalRecoveryAction::Retry:
                case OperationJournalRecoveryAction::RemoveTemporaryItem:
                    return _result.destination_publication == OperationJournalPublicationState::NotPublished;
                case OperationJournalRecoveryAction::InspectDestination:
                    return _result.destination_publication != OperationJournalPublicationState::NotPublished;
                case OperationJournalRecoveryAction::RestoreSource:
                    return _result.destination_publication == OperationJournalPublicationState::Published;
            }
            return false;
        }
    }
    return false;
}

bool OperationJournalValidEntryLifecycle(const OperationJournalEntry &_entry)
{
    const auto complete = _entry.item_results.size() == _entry.plan.Sources().size();
    const auto has_status = [&](OperationJournalItemStatus _status) {
        return std::ranges::any_of(_entry.item_results,
                                   [&](const auto &item) { return item.status == _status; });
    };
    switch( _entry.state ) {
        case OperationJournalState::Admitted:
            return _entry.item_results.empty();
        case OperationJournalState::Running:
        case OperationJournalState::Interrupted:
            return true;
        case OperationJournalState::Completed:
            return complete && !has_status(OperationJournalItemStatus::Failed) &&
                   !has_status(OperationJournalItemStatus::Cancelled);
        case OperationJournalState::Failed:
            return _entry.item_results.empty() ||
                   (has_status(OperationJournalItemStatus::Failed) &&
                    !has_status(OperationJournalItemStatus::Cancelled));
        case OperationJournalState::Cancelled:
            return _entry.item_results.empty() ||
                   (has_status(OperationJournalItemStatus::Cancelled) &&
                    !has_status(OperationJournalItemStatus::Failed));
    }
    return false;
}

std::expected<int64_t, OperationJournalError>
OperationJournalEpochNanoseconds(OperationPlan::TimePoint _time_point)
{
    using Duration = OperationPlan::Clock::duration;
    using Period = Duration::period;
    const __int128 ticks = static_cast<__int128>(_time_point.time_since_epoch().count());
    const __int128 numerator = ticks * static_cast<__int128>(Period::num) * 1'000'000'000;
    const __int128 denominator = static_cast<__int128>(Period::den);
    if( numerator % denominator != 0 )
        return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
    const __int128 value = numerator / denominator;
    if( value < std::numeric_limits<int64_t>::min() || value > std::numeric_limits<int64_t>::max() )
        return OperationJournalFailure(OperationJournalErrorCode::ResourceLimitExceeded);
    return static_cast<int64_t>(value);
}

std::expected<OperationPlan::TimePoint, OperationJournalError>
OperationJournalTimePoint(int64_t _epoch_nanoseconds)
{
    using Duration = OperationPlan::Clock::duration;
    using Period = Duration::period;
    const __int128 numerator =
        static_cast<__int128>(_epoch_nanoseconds) * static_cast<__int128>(Period::den);
    const __int128 denominator = static_cast<__int128>(Period::num) * 1'000'000'000;
    if( numerator % denominator != 0 )
        return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
    const __int128 ticks = numerator / denominator;
    if( ticks < std::numeric_limits<Duration::rep>::min() || ticks > std::numeric_limits<Duration::rep>::max() )
        return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
    return OperationPlan::TimePoint{Duration{static_cast<Duration::rep>(ticks)}};
}

template <class Writer>
bool OperationJournalWriteToken(Writer &_writer, std::string_view _token)
{
    return _writer.String(_token.data(), static_cast<rapidjson::SizeType>(_token.size()));
}

std::expected<std::string, OperationJournalError>
OperationJournalEncode(const std::vector<OperationJournalEntry> &_entries, const uint64_t _next_operation_sequence)
{
    if( _entries.size() > OperationJournal::MaxEntries || _next_operation_sequence == 0 )
        return OperationJournalFailure(OperationJournalErrorCode::ResourceLimitExceeded);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer{buffer};
    writer.StartObject();
    writer.Key("version");
    writer.Uint(OperationJournal::SchemaVersion);
    writer.Key("next_operation_sequence");
    writer.Uint64(_next_operation_sequence);
    writer.Key("entries");
    writer.StartArray();
    size_t total_item_results = 0;
    for( const auto &entry : _entries ) {
        if( entry.item_results.size() > OperationJournal::MaxItemResults - total_item_results )
            return OperationJournalFailure(OperationJournalErrorCode::ResourceLimitExceeded);
        total_item_results += entry.item_results.size();
        if( !OperationJournalValidEntryLifecycle(entry) )
            return OperationJournalFailure(OperationJournalErrorCode::InvalidTransition);

        const auto encoded_plan = OperationPlanCodec::Encode(entry.plan);
        if( !encoded_plan )
            return OperationJournalCodecFailure(encoded_plan.error());
        rapidjson::Document plan_document;
        plan_document.Parse(encoded_plan->data(), encoded_plan->size());
        if( plan_document.HasParseError() || !plan_document.IsObject() )
            return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
        const auto state = OperationJournalStateToken(entry.state);
        const auto updated_at = OperationJournalEpochNanoseconds(entry.updated_at);
        if( !state || !updated_at )
            return updated_at ? OperationJournalFailure(OperationJournalErrorCode::CorruptJournal)
                              : std::unexpected(updated_at.error());

        writer.StartObject();
        writer.Key("operation_id");
        const auto operation_id = entry.operation_id.ToString();
        OperationJournalWriteToken(writer, operation_id);
        writer.Key("plan");
        plan_document.Accept(writer);
        writer.Key("state");
        OperationJournalWriteToken(writer, *state);
        writer.Key("updated_at_epoch_nanoseconds");
        writer.Int64(*updated_at);
        writer.Key("item_results");
        writer.StartArray();
        for( const auto &item : entry.item_results ) {
            const auto status = OperationJournalItemStatusToken(item.status);
            const auto error = OperationJournalItemErrorToken(item.error);
            const auto prior_error = OperationJournalItemErrorToken(item.prior_error);
            const auto destination_publication =
                OperationJournalPublicationStateToken(item.destination_publication);
            const auto filesystem_sync_status =
                OperationJournalFilesystemSyncStatusToken(item.filesystem_sync_status);
            const auto recovery = OperationJournalRecoveryActionToken(item.recovery_action);
            if( !status || !error || !prior_error || !destination_publication ||
                !filesystem_sync_status || !recovery ||
                !OperationJournalValidItemResult(entry.plan, item) )
                return OperationJournalFailure(OperationJournalErrorCode::InvalidItemResult);
            writer.StartObject();
            writer.Key("item_index");
            writer.Uint64(item.item_index);
            writer.Key("status");
            OperationJournalWriteToken(writer, *status);
            writer.Key("error");
            OperationJournalWriteToken(writer, *error);
            writer.Key("system_error");
            writer.Int(item.system_error);
            writer.Key("prior_error");
            OperationJournalWriteToken(writer, *prior_error);
            writer.Key("prior_system_error");
            writer.Int(item.prior_system_error);
            writer.Key("bytes");
            writer.Uint64(item.bytes);
            writer.Key("destination_publication");
            OperationJournalWriteToken(writer, *destination_publication);
            writer.Key("filesystem_sync_status");
            OperationJournalWriteToken(writer, *filesystem_sync_status);
            writer.Key("filesystem_sync_system_error");
            writer.Int(item.filesystem_sync_system_error);
            writer.Key("recovery_action");
            OperationJournalWriteToken(writer, *recovery);
            writer.EndObject();
        }
        writer.EndArray();
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
    if( !writer.IsComplete() || buffer.GetSize() > OperationJournal::MaxJournalBytes )
        return OperationJournalFailure(OperationJournalErrorCode::ResourceLimitExceeded);
    return std::string{buffer.GetString(), buffer.GetSize()};
}

struct OperationJournalDecoded final {
    std::vector<OperationJournalEntry> entries;
    uint64_t next_operation_sequence{1};
    bool requires_migration{false};
};

std::expected<OperationJournalDecoded, OperationJournalError>
OperationJournalDecode(std::string_view _json)
{
    if( _json.size() > OperationJournal::MaxJournalBytes )
        return OperationJournalFailure(OperationJournalErrorCode::JournalTooLarge);
    rapidjson::Document document;
    document.Parse<rapidjson::kParseValidateEncodingFlag>(_json.data(), _json.size());
    if( document.HasParseError() || !document.IsObject() )
        return OperationJournalFailure(OperationJournalErrorCode::MalformedJournal);
    const auto version_member = document.FindMember("version");
    if( version_member == document.MemberEnd() || !version_member->value.IsUint() )
        return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
    const auto version = version_member->value.GetUint();
    if( version != 1 && version != 2 && version != OperationJournal::SchemaVersion )
        return OperationJournalFailure(OperationJournalErrorCode::UnsupportedSchemaVersion);
    const OperationJournalJSONValue *entries_value = nullptr;
    uint64_t next_operation_sequence = 1;
    if( version == OperationJournal::SchemaVersion ) {
        const auto root = OperationJournalMembers(
            document, std::array<std::string_view, 3>{"version", "next_operation_sequence", "entries"});
        if( !root )
            return std::unexpected(root.error());
        const auto *const high_water = (*root)[1];
        if( !high_water->IsUint64() || high_water->GetUint64() == 0 )
            return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
        next_operation_sequence = high_water->GetUint64();
        entries_value = (*root)[2];
    }
    else {
        const auto root = OperationJournalMembers(document, std::array<std::string_view, 2>{"version", "entries"});
        if( !root )
            return std::unexpected(root.error());
        entries_value = (*root)[1];
    }
    if( !entries_value->IsArray() )
        return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
    if( entries_value->Size() > OperationJournal::MaxEntries )
        return OperationJournalFailure(OperationJournalErrorCode::ResourceLimitExceeded);

    std::vector<OperationJournalEntry> entries;
    entries.reserve(entries_value->Size());
    size_t total_item_results = 0;
    for( const auto &entry_value : entries_value->GetArray() ) {
        const OperationJournalJSONValue *operation_id_value = nullptr;
        const OperationJournalJSONValue *plan_value = nullptr;
        const OperationJournalJSONValue *state_value = nullptr;
        const OperationJournalJSONValue *updated_at_value = nullptr;
        const OperationJournalJSONValue *item_results_value = nullptr;
        std::optional<OperationId> operation_id;
        if( version == 1 ) {
            const auto members = OperationJournalMembers(
                entry_value,
                std::array<std::string_view, 4>{"plan", "state", "updated_at_epoch_nanoseconds", "item_results"});
            if( !members )
                return std::unexpected(members.error());
            plan_value = (*members)[0];
            state_value = (*members)[1];
            updated_at_value = (*members)[2];
            item_results_value = (*members)[3];
            operation_id = OperationId::Parse("op-" + std::to_string(entries.size() + 1));
        }
        else {
            const auto members = OperationJournalMembers(
                entry_value,
                std::array<std::string_view, 5>{"operation_id",
                                                 "plan",
                                                 "state",
                                                 "updated_at_epoch_nanoseconds",
                                                 "item_results"});
            if( !members )
                return std::unexpected(members.error());
            operation_id_value = (*members)[0];
            plan_value = (*members)[1];
            state_value = (*members)[2];
            updated_at_value = (*members)[3];
            item_results_value = (*members)[4];
            if( !operation_id_value->IsString() )
                return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
            operation_id = OperationId::Parse(
                {operation_id_value->GetString(), operation_id_value->GetStringLength()});
        }
        if( !operation_id )
            return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);

        rapidjson::StringBuffer plan_buffer;
        rapidjson::Writer<rapidjson::StringBuffer> plan_writer{plan_buffer};
        plan_value->Accept(plan_writer);
        const auto plan = OperationPlanCodec::Decode({plan_buffer.GetString(), plan_buffer.GetSize()});
        if( !plan )
            return OperationJournalCodecFailure(plan.error());
        if( std::ranges::any_of(entries, [&](const auto &existing) { return existing.plan.Id() == plan->Id(); }) )
            return OperationJournalFailure(OperationJournalErrorCode::DuplicatePlanId);
        if( std::ranges::any_of(entries, [&](const auto &existing) { return existing.operation_id == *operation_id; }) )
            return OperationJournalFailure(OperationJournalErrorCode::DuplicateOperationId);

        const auto state = OperationJournalParseState(*state_value);
        if( !state || !updated_at_value->IsInt64() || !item_results_value->IsArray() )
            return state ? OperationJournalFailure(OperationJournalErrorCode::CorruptJournal)
                         : std::unexpected(state.error());
        const auto updated_at = OperationJournalTimePoint(updated_at_value->GetInt64());
        if( !updated_at )
            return std::unexpected(updated_at.error());
        if( item_results_value->Size() > OperationJournal::MaxItemResults - total_item_results )
            return OperationJournalFailure(OperationJournalErrorCode::ResourceLimitExceeded);
        total_item_results += item_results_value->Size();

        std::vector<OperationJournalItemResult> item_results;
        item_results.reserve(item_results_value->Size());
        for( const auto &item_value : item_results_value->GetArray() ) {
            const auto item_members = OperationJournalMembers(
                item_value,
                std::array<std::string_view, 11>{"item_index",
                                                 "status",
                                                 "error",
                                                 "system_error",
                                                 "prior_error",
                                                 "prior_system_error",
                                                 "bytes",
                                                 "destination_publication",
                                                 "filesystem_sync_status",
                                                 "filesystem_sync_system_error",
                                                 "recovery_action"});
            if( !item_members || !(*item_members)[0]->IsUint64() || !(*item_members)[3]->IsInt() ||
                !(*item_members)[5]->IsInt() || !(*item_members)[6]->IsUint64() ||
                !(*item_members)[9]->IsInt() ||
                (*item_members)[0]->GetUint64() > std::numeric_limits<size_t>::max() )
                return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
            const auto status = OperationJournalParseItemStatus(*(*item_members)[1]);
            const auto error = OperationJournalParseItemError(*(*item_members)[2]);
            const auto prior_error = OperationJournalParseItemError(*(*item_members)[4]);
            const auto destination_publication =
                OperationJournalParsePublicationState(*(*item_members)[7]);
            const auto filesystem_sync_status =
                OperationJournalParseFilesystemSyncStatus(*(*item_members)[8]);
            const auto recovery = OperationJournalParseRecoveryAction(*(*item_members)[10]);
            if( !status || !error || !prior_error || !destination_publication ||
                !filesystem_sync_status || !recovery )
                return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
            OperationJournalItemResult result{.item_index = static_cast<size_t>((*item_members)[0]->GetUint64()),
                                              .status = *status,
                                              .error = *error,
                                              .system_error = (*item_members)[3]->GetInt(),
                                              .prior_error = *prior_error,
                                              .prior_system_error = (*item_members)[5]->GetInt(),
                                              .bytes = (*item_members)[6]->GetUint64(),
                                              .destination_publication = *destination_publication,
                                              .filesystem_sync_status = *filesystem_sync_status,
                                              .filesystem_sync_system_error = (*item_members)[9]->GetInt(),
                                              .recovery_action = *recovery};
            if( !OperationJournalValidItemResult(*plan, result) ||
                (!item_results.empty() && item_results.back().item_index >= result.item_index) )
                return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
            item_results.emplace_back(result);
        }
        OperationJournalEntry entry{.operation_id = std::move(*operation_id),
                                    .plan = std::move(*plan),
                                    .state = *state,
                                    .updated_at = *updated_at,
                                    .item_results = std::move(item_results)};
        if( !OperationJournalValidEntryLifecycle(entry) )
            return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
        entries.emplace_back(std::move(entry));
    }
    uint64_t maximum_operation_sequence = 0;
    for( const auto &entry : entries )
        maximum_operation_sequence = std::max(maximum_operation_sequence, OperationJournalOperationIdSequence(entry.operation_id));
    if( maximum_operation_sequence == std::numeric_limits<uint64_t>::max() )
        return OperationJournalFailure(OperationJournalErrorCode::ResourceLimitExceeded);
    const auto minimum_next_operation_sequence = maximum_operation_sequence + 1;
    if( version == OperationJournal::SchemaVersion ) {
        if( next_operation_sequence < minimum_next_operation_sequence )
            return OperationJournalFailure(OperationJournalErrorCode::CorruptJournal);
    }
    else {
        next_operation_sequence = minimum_next_operation_sequence;
    }
    return OperationJournalDecoded{.entries = std::move(entries),
                                   .next_operation_sequence = next_operation_sequence,
                                   .requires_migration = version != OperationJournal::SchemaVersion};
}

bool OperationJournalValidParentPath(std::string_view _path)
{
    if( _path.empty() || _path.front() != '/' || (_path.size() > 1 && _path.back() == '/') )
        return false;
    size_t begin = 1;
    while( begin < _path.size() ) {
        const size_t end = _path.find('/', begin);
        const std::string_view component =
            _path.substr(begin, end == std::string_view::npos ? _path.size() - begin : end - begin);
        if( component.empty() || component == "." || component == ".." )
            return false;
        if( end == std::string_view::npos )
            break;
        begin = end + 1;
    }
    return true;
}

std::expected<OperationJournalDescriptor, OperationJournalError>
OperationJournalOpenParent(std::string_view _path, const OperationJournalSyscalls &_syscalls)
{
    if( !OperationJournalValidParentPath(_path) )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidParentPath);
    const int root = _syscalls.open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
    if( root < 0 )
        return OperationJournalFailure(OperationJournalErrorCode::ParentOpenFailed, errno);
    OperationJournalDescriptor directory{root, _syscalls};
    if( _path == "/" )
        return directory;

    size_t begin = 1;
    while( begin < _path.size() ) {
        const size_t end = _path.find('/', begin);
        const auto component = _path.substr(begin, end == std::string_view::npos ? _path.size() - begin : end - begin);
        const std::string name{component};
        const int next =
            _syscalls.open_at(directory.Get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
        const int saved_error = errno;
        if( next < 0 )
            return OperationJournalFailure(OperationJournalErrorCode::ParentOpenFailed, saved_error);
        directory = OperationJournalDescriptor{next, _syscalls};
        if( end == std::string_view::npos )
            break;
        begin = end + 1;
    }
    return directory;
}

std::expected<std::optional<std::string>, OperationJournalError>
OperationJournalRead(int _parent_fd, const OperationJournalSyscalls &_syscalls)
{
    const std::string filename{OperationJournal::Filename};
    const int opened =
        _syscalls.open_at(_parent_fd, filename.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0);
    if( opened < 0 ) {
        if( errno == ENOENT )
            return std::nullopt;
        return OperationJournalFailure(OperationJournalErrorCode::JournalOpenFailed, errno);
    }
    OperationJournalDescriptor fd{opened, _syscalls};
    struct stat state{};
    if( _syscalls.fstat(fd.Get(), &state) != 0 ) {
        const int saved_error = errno;
        return OperationJournalFailure(OperationJournalErrorCode::JournalReadFailed, saved_error);
    }
    if( !S_ISREG(state.st_mode) || state.st_nlink != 1 || state.st_uid != ::geteuid() ||
        (state.st_mode & 077) != 0 || state.st_size < 0 ) {
        return OperationJournalFailure(OperationJournalErrorCode::JournalReadFailed, EPERM);
    }
    if( static_cast<uint64_t>(state.st_size) > OperationJournal::MaxJournalBytes ) {
        return OperationJournalFailure(OperationJournalErrorCode::JournalTooLarge);
    }

    std::string contents;
    contents.reserve(static_cast<size_t>(state.st_size));
    std::array<char, 16 * 1024> buffer{};
    while( true ) {
        const ssize_t count = _syscalls.read(fd.Get(), buffer.data(), buffer.size());
        if( count < 0 ) {
            if( errno == EINTR )
                continue;
            const int saved_error = errno;
            return OperationJournalFailure(OperationJournalErrorCode::JournalReadFailed, saved_error);
        }
        if( count == 0 )
            break;
        if( static_cast<size_t>(count) > OperationJournal::MaxJournalBytes - contents.size() )
            return OperationJournalFailure(OperationJournalErrorCode::JournalTooLarge);
        contents.append(buffer.data(), static_cast<size_t>(count));
    }
    if( fd.Close() != 0 )
        return OperationJournalFailure(OperationJournalErrorCode::JournalReadFailed, errno);
    return contents;
}

std::expected<void, OperationJournalError>
OperationJournalPersist(int _parent_fd,
                        const std::vector<OperationJournalEntry> &_entries,
                        const uint64_t _next_operation_sequence,
                        const OperationJournalSyscalls &_syscalls,
                        bool &_rename_committed)
{
    _rename_committed = false;
    const auto encoded = OperationJournalEncode(_entries, _next_operation_sequence);
    if( !encoded )
        return std::unexpected(encoded.error());
    const std::string filename{OperationJournal::Filename};
    std::string temporary;
    OperationJournalDescriptor fd;
    for( size_t attempt = 0; attempt < 8; ++attempt ) {
        std::array<uint8_t, 16> random{};
        try {
            _syscalls.random_bytes(random.data(), random.size());
        }
        catch( ... ) {
            return OperationJournalFailure(OperationJournalErrorCode::TemporaryCreateFailed, EIO);
        }
        static constexpr char hexadecimal[] = "0123456789abcdef";
        temporary = ".operation-journal-v1.json.tmp.";
        temporary.reserve(temporary.size() + random.size() * 2);
        for( const uint8_t byte : random ) {
            temporary.push_back(hexadecimal[byte >> 4u]);
            temporary.push_back(hexadecimal[byte & 0x0Fu]);
        }
        const int opened = _syscalls.open_at(
            _parent_fd, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if( opened >= 0 ) {
            fd = OperationJournalDescriptor{opened, _syscalls};
            break;
        }
        if( errno != EEXIST )
            return OperationJournalFailure(OperationJournalErrorCode::TemporaryCreateFailed, errno);
    }
    if( !fd )
        return OperationJournalFailure(OperationJournalErrorCode::TemporaryCreateFailed, EEXIST);
    // A same-UID actor can rebind a pathname before unlink. Pre-rename failures therefore retain the
    // random exclusive file; bounded reclamation requires a conditional descriptor-bound primitive.

    size_t offset = 0;
    while( offset < encoded->size() ) {
        const ssize_t count = _syscalls.write(fd.Get(), encoded->data() + offset, encoded->size() - offset);
        if( count < 0 ) {
            if( errno == EINTR )
                continue;
            return OperationJournalFailure(OperationJournalErrorCode::WriteFailed, errno);
        }
        if( count == 0 )
            return OperationJournalFailure(OperationJournalErrorCode::WriteFailed, EIO);
        offset += static_cast<size_t>(count);
    }
    while( _syscalls.fsync(fd.Get()) != 0 ) {
        if( errno == EINTR )
            continue;
        return OperationJournalFailure(OperationJournalErrorCode::FileSyncFailed, errno);
    }
    if( fd.Close() != 0 ) {
        const int saved_error = errno;
        return OperationJournalFailure(OperationJournalErrorCode::CloseFailed, saved_error);
    }

    // From this point any failure is conservatively post-commit: rename-like providers can report or throw
    // after changing the namespace, so callers must poison the handle and reconcile by reopening.
    _rename_committed = true;
    int rename_result = -1;
    try {
        rename_result = _syscalls.rename_at(_parent_fd, temporary.c_str(), _parent_fd, filename.c_str());
    }
    catch( ... ) {
        return OperationJournalFailure(OperationJournalErrorCode::DurabilityUncertain, EIO);
    }
    if( rename_result != 0 ) {
        const int saved_error = errno;
        return OperationJournalFailure(OperationJournalErrorCode::DurabilityUncertain, saved_error);
    }
    while( true ) {
        int sync_result = -1;
        try {
            sync_result = _syscalls.fsync(_parent_fd);
        }
        catch( ... ) {
            return OperationJournalFailure(OperationJournalErrorCode::DurabilityUncertain, EIO);
        }
        if( sync_result == 0 )
            break;
        if( errno == EINTR )
            continue;
        return OperationJournalFailure(OperationJournalErrorCode::DurabilityUncertain, errno);
    }
    return {};
}

bool OperationJournalLegalTransition(OperationJournalState _from, OperationJournalState _to) noexcept
{
    switch( _from ) {
        case OperationJournalState::Admitted:
            return _to == OperationJournalState::Failed || _to == OperationJournalState::Cancelled;
        case OperationJournalState::Running:
            return _to == OperationJournalState::Completed || _to == OperationJournalState::Failed ||
                   _to == OperationJournalState::Cancelled;
        case OperationJournalState::Interrupted:
            return _to == OperationJournalState::Failed || _to == OperationJournalState::Cancelled;
        case OperationJournalState::Completed:
        case OperationJournalState::Failed:
        case OperationJournalState::Cancelled:
            return false;
    }
    return false;
}

} // namespace

struct OperationJournal::Impl final {
    std::shared_ptr<OperationJournalSyscalls> syscalls;
    OperationJournalTesting::Clock clock;
    OperationJournalDescriptor parent_fd;
    OperationJournalDescriptor lock_fd;
    mutable std::mutex mutex;
    std::vector<OperationJournalEntry> entries;
    uint64_t next_operation_sequence{1};
    uint64_t next_reservation_nonce{1};
    std::unordered_map<uint64_t, uint64_t> reservations;
    uint64_t parent_device{0};
    uint64_t parent_inode{0};
    bool usable = true;
};

OperationJournal::AdmissionReservation::AdmissionReservation(nc::ops::OperationId _operation_id,
                                                              const uint64_t _nonce,
                                                              std::weak_ptr<Impl> _impl) noexcept
    : m_OperationId{std::move(_operation_id)}, m_Nonce{_nonce}, m_Impl{std::move(_impl)}
{
}

OperationJournal::AdmissionReservation::AdmissionReservation(AdmissionReservation &&_other) noexcept
    : m_OperationId{std::move(_other.m_OperationId)},
      m_Nonce{_other.m_Nonce},
      m_Impl{std::move(_other.m_Impl)},
      m_Consumed{std::exchange(_other.m_Consumed, true)}
{
}

OperationJournal::AdmissionReservation &
OperationJournal::AdmissionReservation::operator=(AdmissionReservation &&_other) noexcept
{
    if( this == &_other )
        return *this;
    Release();
    m_OperationId = std::move(_other.m_OperationId);
    m_Nonce = _other.m_Nonce;
    m_Impl = std::move(_other.m_Impl);
    m_Consumed = std::exchange(_other.m_Consumed, true);
    return *this;
}

OperationJournal::AdmissionReservation::~AdmissionReservation()
{
    Release();
}

void OperationJournal::AdmissionReservation::Release() noexcept
{
    if( m_Consumed )
        return;
    if( const auto impl = m_Impl.lock() ) {
        const auto guard = std::lock_guard{impl->mutex};
        const auto found = impl->reservations.find(OperationJournalOperationIdSequence(m_OperationId));
        if( found != impl->reservations.end() && found->second == m_Nonce )
            impl->reservations.erase(found);
    }
    m_Consumed = true;
    m_Impl.reset();
}

OperationJournalAdmissionReceipt::OperationJournalAdmissionReceipt(
    OperationJournalAdmissionReceipt &&_other) noexcept
    : m_JournalInstance{std::move(_other.m_JournalInstance)},
      m_OperationId{std::move(_other.m_OperationId)},
      m_Plan{std::move(_other.m_Plan)},
      m_Consumed{std::exchange(_other.m_Consumed, true)}
{
    _other.m_JournalInstance.reset();
}

OperationJournalRunReceipt::OperationJournalRunReceipt(OperationJournalRunReceipt &&_other) noexcept
    : m_JournalInstance{std::move(_other.m_JournalInstance)},
      m_OperationId{std::move(_other.m_OperationId)},
      m_Plan{std::move(_other.m_Plan)},
      m_Consumed{std::exchange(_other.m_Consumed, true)}
{
    _other.m_JournalInstance.reset();
}

std::shared_ptr<OperationJournalSyscalls> OperationJournalTesting::DefaultSyscalls()
{
    auto syscalls = std::make_shared<OperationJournalSyscalls>();
    syscalls->open = [](const char *_path, int _flags, mode_t _mode) { return ::open(_path, _flags, _mode); };
    syscalls->open_at =
        [](int _directory, const char *_path, int _flags, mode_t _mode) { return ::openat(_directory, _path, _flags, _mode); };
    syscalls->read = [](int _fd, void *_buffer, size_t _size) { return ::read(_fd, _buffer, _size); };
    syscalls->write = [](int _fd, const void *_buffer, size_t _size) { return ::write(_fd, _buffer, _size); };
    syscalls->fsync = [](int _fd) { return ::fsync(_fd); };
    syscalls->fstat = [](int _fd, struct stat *_state) { return ::fstat(_fd, _state); };
    syscalls->flock = [](int _fd, int _operation) { return ::flock(_fd, _operation); };
    syscalls->rename_at = [](int _from_directory, const char *_from, int _to_directory, const char *_to) {
        return ::renameat(_from_directory, _from, _to_directory, _to);
    };
    syscalls->close = [](int _fd) { return ::close(_fd); };
    syscalls->random_bytes = [](void *_buffer, size_t _size) { ::arc4random_buf(_buffer, _size); };
    return syscalls;
}

OperationJournalAdmissionReceipt
OperationJournalTesting::ForgeAdmissionReceipt(const OperationJournal &_journal,
                                               OperationId _operation_id,
                                               OperationPlan _plan)
{
    return OperationJournalAdmissionReceipt{
        std::weak_ptr<const void>{_journal.m_Impl}, std::move(_operation_id), std::move(_plan)};
}

OperationJournalRunReceipt
OperationJournalTesting::ForgeRunReceipt(const OperationJournal &_journal,
                                         OperationId _operation_id,
                                         OperationPlan _plan)
{
    return OperationJournalRunReceipt{
        std::weak_ptr<const void>{_journal.m_Impl}, std::move(_operation_id), std::move(_plan)};
}

std::expected<OperationJournalAdmissionReceipt, OperationJournalError>
OperationJournalTesting::AdmitWithOperationId(OperationJournal &_journal,
                                              OperationId _operation_id,
                                              const OperationPlan &_plan)
{
    return _journal.AdmitWithOperationIdForTesting(std::move(_operation_id), _plan);
}

std::expected<void, OperationJournalError>
OperationJournalTesting::Transition(OperationJournal &_journal,
                                    std::string_view _plan_id,
                                    OperationJournalState _state)
{
    return _journal.Transition(_plan_id, _state);
}

std::expected<void, OperationJournalError>
OperationJournalTesting::RecordItemResult(OperationJournal &_journal,
                                          std::string_view _plan_id,
                                          OperationJournalItemResult _result)
{
    return _journal.RecordItemResult(_plan_id, std::move(_result));
}

std::expected<std::vector<OperationJournalEntry>, OperationJournalError>
OperationJournalTesting::InspectPersistedReadOnly(const int _parent_directory_fd)
{
    const auto syscalls = DefaultSyscalls();
    struct stat parent_state{};
    if( _parent_directory_fd < 0 )
        return OperationJournalFailure(OperationJournalErrorCode::ParentOpenFailed, EBADF);
    if( syscalls->fstat(_parent_directory_fd, &parent_state) != 0 )
        return OperationJournalFailure(OperationJournalErrorCode::ParentOpenFailed, errno);
    if( !S_ISDIR(parent_state.st_mode) || parent_state.st_uid != ::geteuid() || (parent_state.st_mode & 0022) != 0 )
        return OperationJournalFailure(OperationJournalErrorCode::ParentOpenFailed, EPERM);

    const auto persisted = OperationJournalRead(_parent_directory_fd, *syscalls);
    if( !persisted )
        return std::unexpected(persisted.error());
    if( !*persisted )
        return OperationJournalFailure(OperationJournalErrorCode::JournalOpenFailed, ENOENT);
    const auto decoded = OperationJournalDecode(**persisted);
    if( !decoded )
        return std::unexpected(decoded.error());
    return std::move(decoded->entries);
}

std::expected<OperationJournal, OperationJournalError>
OperationJournalTesting::Open(std::string_view _absolute_existing_parent,
                              std::shared_ptr<OperationJournalSyscalls> _syscalls,
                              Clock _clock)
{
    if( !_syscalls || !_clock || !OperationJournalValidSyscalls(*_syscalls) )
        return OperationJournalFailure(OperationJournalErrorCode::JournalOpenFailed, EINVAL);
    auto parent_fd = OperationJournalOpenParent(_absolute_existing_parent, *_syscalls);
    if( !parent_fd )
        return std::unexpected(parent_fd.error());

    struct stat parent_state{};
    const int parent_stat_result = _syscalls->fstat(parent_fd->Get(), &parent_state);
    if( parent_stat_result != 0 || !S_ISDIR(parent_state.st_mode) || parent_state.st_uid != ::geteuid() ||
        (parent_state.st_mode & 0022) != 0 ) {
        const int saved_error = parent_stat_result != 0 ? errno : EPERM;
        return OperationJournalFailure(OperationJournalErrorCode::ParentOpenFailed, saved_error);
    }

    auto impl = std::make_shared<OperationJournal::Impl>();
    impl->syscalls = std::move(_syscalls);
    impl->clock = std::move(_clock);
    impl->parent_fd = std::move(*parent_fd);
    impl->parent_device = static_cast<uint64_t>(parent_state.st_dev);
    impl->parent_inode = static_cast<uint64_t>(parent_state.st_ino);

    constexpr std::string_view lock_filename = "operation-journal-v1.lock";
    const int lock_fd = impl->syscalls->open_at(impl->parent_fd.Get(),
                                                lock_filename.data(),
                                                O_RDWR | O_CREAT | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW,
                                                0600);
    if( lock_fd < 0 )
        return OperationJournalFailure(OperationJournalErrorCode::LockOpenFailed, errno);
    impl->lock_fd = OperationJournalDescriptor{lock_fd, *impl->syscalls};
    struct stat lock_state{};
    const int lock_stat_result = impl->syscalls->fstat(impl->lock_fd.Get(), &lock_state);
    if( lock_stat_result != 0 || !S_ISREG(lock_state.st_mode) || lock_state.st_nlink != 1 ||
        lock_state.st_uid != ::geteuid() || (lock_state.st_mode & 077) != 0 )
        return OperationJournalFailure(OperationJournalErrorCode::LockOpenFailed,
                                       lock_stat_result != 0 ? errno : EPERM);
    if( impl->syscalls->flock(impl->lock_fd.Get(), LOCK_EX | LOCK_NB) != 0 )
        return OperationJournalFailure(OperationJournalErrorCode::JournalAlreadyOpen, errno);

    bool requires_migration = false;
    const auto persisted = OperationJournalRead(impl->parent_fd.Get(), *impl->syscalls);
    if( !persisted )
        return std::unexpected(persisted.error());
    if( *persisted ) {
        const auto decoded = OperationJournalDecode(**persisted);
        if( !decoded )
            return std::unexpected(decoded.error());
        requires_migration = decoded->requires_migration;
        impl->entries = std::move(decoded->entries);
        impl->next_operation_sequence = decoded->next_operation_sequence;
    }
    else {
        bool rename_committed = false;
        const auto created =
            OperationJournalPersist(
                impl->parent_fd.Get(), impl->entries, impl->next_operation_sequence, *impl->syscalls, rename_committed);
        if( !created )
            return std::unexpected(created.error());
    }

    auto interrupted = impl->entries;
    const auto now = impl->clock();
    bool changed = false;
    for( auto &entry : interrupted ) {
        if( entry.state == OperationJournalState::Admitted || entry.state == OperationJournalState::Running ) {
            entry.state = OperationJournalState::Interrupted;
            entry.updated_at = now;
            changed = true;
        }
    }
    if( requires_migration || changed ) {
        bool rename_committed = false;
        const auto stored =
            OperationJournalPersist(impl->parent_fd.Get(),
                                    interrupted,
                                    impl->next_operation_sequence,
                                    *impl->syscalls,
                                    rename_committed);
        if( !stored )
            return std::unexpected(stored.error());
        impl->entries = std::move(interrupted);
    }
    return OperationJournal{std::move(impl)};
}

std::expected<OperationJournal, OperationJournalError>
OperationJournal::Open(std::string_view _absolute_existing_parent)
{
    return OperationJournalTesting::Open(_absolute_existing_parent,
                                         OperationJournalTesting::DefaultSyscalls(),
                                         [] { return OperationPlan::Clock::now(); });
}

std::expected<OperationJournal::AdmissionReservation, OperationJournalError> OperationJournal::ReserveOperationId()
{
    const auto guard = std::lock_guard{m_Impl->mutex};
    if( !m_Impl->usable )
        return OperationJournalFailure(OperationJournalErrorCode::JournalUnusable);
    if( m_Impl->next_operation_sequence == 0 ||
        m_Impl->next_operation_sequence == std::numeric_limits<uint64_t>::max() ||
        m_Impl->next_reservation_nonce == 0 )
        return OperationJournalFailure(OperationJournalErrorCode::ResourceLimitExceeded);

    const auto sequence = m_Impl->next_operation_sequence++;
    const auto nonce = m_Impl->next_reservation_nonce++;
    const auto [_, inserted] = m_Impl->reservations.emplace(sequence, nonce);
    if( !inserted )
        return OperationJournalFailure(OperationJournalErrorCode::JournalUnusable);
    return AdmissionReservation{OperationId{sequence}, nonce, m_Impl};
}

std::expected<OperationJournalAdmissionReceipt, OperationJournalError>
OperationJournal::Admit(const OperationPlan &_plan)
{
    auto reservation = ReserveOperationId();
    if( !reservation )
        return std::unexpected(reservation.error());
    return Admit(std::move(*reservation), _plan);
}

std::expected<OperationJournalAdmissionReceipt, OperationJournalError>
OperationJournal::Admit(AdmissionReservation &&_reservation, const OperationPlan &_plan)
{
    std::lock_guard lock{m_Impl->mutex};
    return AdmitLocked(_reservation, _plan);
}

std::expected<OperationJournalAdmissionReceipt, OperationJournalError>
OperationJournal::AdmitLocked(AdmissionReservation &_reservation, const OperationPlan &_plan)
{
    if( !m_Impl->usable )
        return OperationJournalFailure(OperationJournalErrorCode::JournalUnusable);
    if( _reservation.m_Consumed )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidAdmissionReceipt);
    const auto reservation_impl = _reservation.m_Impl.lock();
    if( !reservation_impl || reservation_impl.get() != m_Impl.get() )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidAdmissionReceipt);
    const auto reservation = m_Impl->reservations.find(_reservation.m_OperationId.m_Sequence);
    if( reservation == m_Impl->reservations.end() || reservation->second != _reservation.m_Nonce )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidAdmissionReceipt);
    if( m_Impl->entries.size() >= MaxEntries )
        return OperationJournalFailure(OperationJournalErrorCode::ResourceLimitExceeded);
    if( std::ranges::any_of(m_Impl->entries,
                            [&](const auto &entry) { return entry.operation_id == _reservation.m_OperationId; }) )
        return OperationJournalFailure(OperationJournalErrorCode::DuplicateOperationId);
    if( std::ranges::any_of(m_Impl->entries, [&](const auto &entry) { return entry.plan.Id() == _plan.Id(); }) )
        return OperationJournalFailure(OperationJournalErrorCode::PlanAlreadyAdmitted);

    OperationJournalAdmissionReceipt receipt{std::weak_ptr<const void>{m_Impl}, _reservation.m_OperationId, _plan};
    auto candidate = m_Impl->entries;
    candidate.emplace_back(OperationJournalEntry{.operation_id = _reservation.m_OperationId,
                                                 .plan = _plan,
                                                 .state = OperationJournalState::Admitted,
                                                 .updated_at = m_Impl->clock(),
                                                 .item_results = {}});
    const auto durable_next_sequence = m_Impl->next_operation_sequence;
    bool rename_committed = false;
    const auto stored =
        OperationJournalPersist(m_Impl->parent_fd.Get(),
                                candidate,
                                durable_next_sequence,
                                *m_Impl->syscalls,
                                rename_committed);
    if( !stored ) {
        if( rename_committed )
            m_Impl->usable = false;
        return std::unexpected(stored.error());
    }
    m_Impl->entries = std::move(candidate);
    m_Impl->reservations.erase(reservation);
    _reservation.m_Consumed = true;
    _reservation.m_Impl.reset();
    return receipt;
}

std::expected<OperationJournalAdmissionReceipt, OperationJournalError>
OperationJournal::AdmitWithOperationIdForTesting(OperationId _operation_id, const OperationPlan &_plan)
{
    std::lock_guard lock{m_Impl->mutex};
    if( !m_Impl->usable )
        return OperationJournalFailure(OperationJournalErrorCode::JournalUnusable);
    if( m_Impl->entries.size() >= MaxEntries )
        return OperationJournalFailure(OperationJournalErrorCode::ResourceLimitExceeded);
    if( std::ranges::any_of(m_Impl->entries,
                            [&](const auto &entry) { return entry.operation_id == _operation_id; }) )
        return OperationJournalFailure(OperationJournalErrorCode::DuplicateOperationId);
    if( std::ranges::any_of(m_Impl->entries, [&](const auto &entry) { return entry.plan.Id() == _plan.Id(); }) )
        return OperationJournalFailure(OperationJournalErrorCode::PlanAlreadyAdmitted);
    if( _operation_id.m_Sequence == std::numeric_limits<uint64_t>::max() )
        return OperationJournalFailure(OperationJournalErrorCode::ResourceLimitExceeded);

    const auto durable_next_sequence = std::max(m_Impl->next_operation_sequence, _operation_id.m_Sequence + 1);
    OperationJournalAdmissionReceipt receipt{std::weak_ptr<const void>{m_Impl}, _operation_id, _plan};
    auto candidate = m_Impl->entries;
    candidate.emplace_back(OperationJournalEntry{.operation_id = _operation_id,
                                                 .plan = _plan,
                                                 .state = OperationJournalState::Admitted,
                                                 .updated_at = m_Impl->clock(),
                                                 .item_results = {}});
    bool rename_committed = false;
    const auto stored = OperationJournalPersist(
        m_Impl->parent_fd.Get(), candidate, durable_next_sequence, *m_Impl->syscalls, rename_committed);
    if( !stored ) {
        if( rename_committed )
            m_Impl->usable = false;
        return std::unexpected(stored.error());
    }
    m_Impl->entries = std::move(candidate);
    m_Impl->next_operation_sequence = durable_next_sequence;
    return receipt;
}

std::expected<void, OperationJournalError>
OperationJournal::ValidateAdmissionReceiptForOrchestration(const OperationJournalAdmissionReceipt &_receipt) const
{
    const auto guard = std::lock_guard{m_Impl->mutex};
    if( !m_Impl->usable )
        return OperationJournalFailure(OperationJournalErrorCode::JournalUnusable);
    if( _receipt.m_Consumed )
        return OperationJournalFailure(OperationJournalErrorCode::AdmissionReceiptAlreadyConsumed);
    const auto receipt_owner = _receipt.m_JournalInstance.lock();
    if( !receipt_owner || receipt_owner.get() != static_cast<const void *>(m_Impl.get()) )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidAdmissionReceipt);
    const auto entry = std::ranges::find_if(m_Impl->entries, [&](const auto &value) {
        return value.operation_id == _receipt.m_OperationId && value.plan.Id() == _receipt.m_Plan.Id();
    });
    if( entry == m_Impl->entries.end() || entry->state != OperationJournalState::Admitted ||
        entry->operation_id != _receipt.m_OperationId || entry->plan != _receipt.m_Plan )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidAdmissionReceipt);
    return {};
}

std::expected<OperationJournalRunReceipt, OperationJournalError>
OperationJournal::TransitionToRunning(OperationJournalAdmissionReceipt &&_receipt)
{
    std::lock_guard lock{m_Impl->mutex};
    if( !m_Impl->usable )
        return OperationJournalFailure(OperationJournalErrorCode::JournalUnusable);
    if( _receipt.m_Consumed )
        return OperationJournalFailure(OperationJournalErrorCode::AdmissionReceiptAlreadyConsumed);

    const auto receipt_owner = _receipt.m_JournalInstance.lock();
    if( !receipt_owner || receipt_owner.get() != static_cast<const void *>(m_Impl.get()) )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidAdmissionReceipt);

    auto candidate = m_Impl->entries;
    const auto entry = std::ranges::find_if(candidate, [&](const auto &value) {
        return value.operation_id == _receipt.m_OperationId && value.plan.Id() == _receipt.m_Plan.Id();
    });
    if( entry == candidate.end() || entry->state != OperationJournalState::Admitted ||
        entry->operation_id != _receipt.m_OperationId || entry->plan != _receipt.m_Plan )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidAdmissionReceipt);
    OperationJournalRunReceipt run_receipt{
        std::weak_ptr<const void>{m_Impl}, _receipt.m_OperationId, _receipt.m_Plan};
    entry->state = OperationJournalState::Running;
    entry->updated_at = m_Impl->clock();

    bool rename_committed = false;
    const auto stored =
        OperationJournalPersist(
            m_Impl->parent_fd.Get(), candidate, m_Impl->next_operation_sequence, *m_Impl->syscalls, rename_committed);
    if( !stored ) {
        if( rename_committed ) {
            m_Impl->usable = false;
            _receipt.m_Consumed = true;
        }
        return std::unexpected(stored.error());
    }
    m_Impl->entries = std::move(candidate);
    _receipt.m_Consumed = true;
    return run_receipt;
}

std::expected<void, OperationJournalError>
OperationJournal::FinalizeAdmission(OperationJournalAdmissionReceipt &&_receipt,
                                    OperationJournalState _terminal_state)
{
    std::lock_guard lock{m_Impl->mutex};
    if( !m_Impl->usable )
        return OperationJournalFailure(OperationJournalErrorCode::JournalUnusable);
    if( _receipt.m_Consumed )
        return OperationJournalFailure(OperationJournalErrorCode::AdmissionReceiptAlreadyConsumed);

    const auto receipt_owner = _receipt.m_JournalInstance.lock();
    if( !receipt_owner || receipt_owner.get() != static_cast<const void *>(m_Impl.get()) )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidAdmissionReceipt);
    if( !OperationJournalLegalTransition(OperationJournalState::Admitted, _terminal_state) )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidTransition);

    auto candidate = m_Impl->entries;
    const auto entry = std::ranges::find_if(candidate, [&](const auto &value) {
        return value.operation_id == _receipt.m_OperationId && value.plan.Id() == _receipt.m_Plan.Id();
    });
    if( entry == candidate.end() || entry->state != OperationJournalState::Admitted ||
        entry->operation_id != _receipt.m_OperationId || entry->plan != _receipt.m_Plan )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidAdmissionReceipt);
    entry->state = _terminal_state;
    entry->updated_at = m_Impl->clock();
    if( !OperationJournalValidEntryLifecycle(*entry) )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidTransition);

    bool rename_committed = false;
    const auto stored =
        OperationJournalPersist(
            m_Impl->parent_fd.Get(), candidate, m_Impl->next_operation_sequence, *m_Impl->syscalls, rename_committed);
    if( !stored ) {
        if( rename_committed ) {
            m_Impl->usable = false;
            _receipt.m_Consumed = true;
        }
        return std::unexpected(stored.error());
    }
    m_Impl->entries = std::move(candidate);
    _receipt.m_Consumed = true;
    return {};
}

std::expected<void, OperationJournalError>
OperationJournal::Finalize(OperationJournalRunReceipt &&_receipt,
                           OperationJournalItemResult _result,
                           OperationJournalState _terminal_state)
{
    std::lock_guard lock{m_Impl->mutex};
    if( !m_Impl->usable )
        return OperationJournalFailure(OperationJournalErrorCode::JournalUnusable);
    if( _receipt.m_Consumed )
        return OperationJournalFailure(OperationJournalErrorCode::RunReceiptAlreadyConsumed);

    const auto receipt_owner = _receipt.m_JournalInstance.lock();
    if( !receipt_owner || receipt_owner.get() != static_cast<const void *>(m_Impl.get()) )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidRunReceipt);
    if( !OperationJournalLegalTransition(OperationJournalState::Running, _terminal_state) )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidTransition);

    auto candidate = m_Impl->entries;
    const auto entry = std::ranges::find_if(candidate, [&](const auto &value) {
        return value.operation_id == _receipt.m_OperationId && value.plan.Id() == _receipt.m_Plan.Id();
    });
    if( entry == candidate.end() || entry->state != OperationJournalState::Running ||
        entry->operation_id != _receipt.m_OperationId || entry->plan != _receipt.m_Plan )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidRunReceipt);
    if( !OperationJournalValidItemResult(entry->plan, _result) ||
        std::ranges::any_of(entry->item_results,
                            [&](const auto &existing) { return existing.item_index == _result.item_index; }) )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidItemResult);
    size_t total_results = 0;
    for( const auto &value : candidate )
        total_results += value.item_results.size();
    if( total_results >= MaxItemResults )
        return OperationJournalFailure(OperationJournalErrorCode::ResourceLimitExceeded);

    const auto insertion = std::ranges::lower_bound(
        entry->item_results, _result.item_index, {}, &OperationJournalItemResult::item_index);
    entry->item_results.insert(insertion, _result);
    entry->state = _terminal_state;
    entry->updated_at = m_Impl->clock();
    if( !OperationJournalValidEntryLifecycle(*entry) )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidTransition);

    bool rename_committed = false;
    const auto stored =
        OperationJournalPersist(
            m_Impl->parent_fd.Get(), candidate, m_Impl->next_operation_sequence, *m_Impl->syscalls, rename_committed);
    if( !stored ) {
        if( rename_committed ) {
            m_Impl->usable = false;
            _receipt.m_Consumed = true;
        }
        return std::unexpected(stored.error());
    }
    m_Impl->entries = std::move(candidate);
    _receipt.m_Consumed = true;
    return {};
}

std::expected<void, OperationJournalError>
OperationJournal::Transition(std::string_view _plan_id, OperationJournalState _state)
{
    std::lock_guard lock{m_Impl->mutex};
    if( !m_Impl->usable )
        return OperationJournalFailure(OperationJournalErrorCode::JournalUnusable);
    auto candidate = m_Impl->entries;
    const auto entry = std::ranges::find_if(candidate, [&](const auto &value) { return value.plan.Id().Value() == _plan_id; });
    if( entry == candidate.end() )
        return OperationJournalFailure(OperationJournalErrorCode::PlanNotFound);
    if( !OperationJournalLegalTransition(entry->state, _state) )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidTransition);
    entry->state = _state;
    entry->updated_at = m_Impl->clock();
    if( !OperationJournalValidEntryLifecycle(*entry) )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidTransition);

    bool rename_committed = false;
    const auto stored =
        OperationJournalPersist(
            m_Impl->parent_fd.Get(), candidate, m_Impl->next_operation_sequence, *m_Impl->syscalls, rename_committed);
    if( !stored ) {
        if( rename_committed )
            m_Impl->usable = false;
        return std::unexpected(stored.error());
    }
    m_Impl->entries = std::move(candidate);
    return {};
}

std::expected<void, OperationJournalError>
OperationJournal::RecordItemResult(std::string_view _plan_id, OperationJournalItemResult _result)
{
    std::lock_guard lock{m_Impl->mutex};
    if( !m_Impl->usable )
        return OperationJournalFailure(OperationJournalErrorCode::JournalUnusable);
    auto candidate = m_Impl->entries;
    const auto entry = std::ranges::find_if(candidate, [&](const auto &value) { return value.plan.Id().Value() == _plan_id; });
    if( entry == candidate.end() )
        return OperationJournalFailure(OperationJournalErrorCode::PlanNotFound);
    if( entry->state != OperationJournalState::Running )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidTransition);
    if( !OperationJournalValidItemResult(entry->plan, _result) ||
        std::ranges::any_of(entry->item_results,
                            [&](const auto &existing) { return existing.item_index == _result.item_index; }) )
        return OperationJournalFailure(OperationJournalErrorCode::InvalidItemResult);
    size_t total_results = 0;
    for( const auto &value : candidate )
        total_results += value.item_results.size();
    if( total_results >= MaxItemResults )
        return OperationJournalFailure(OperationJournalErrorCode::ResourceLimitExceeded);
    const auto insertion = std::ranges::lower_bound(
        entry->item_results, _result.item_index, {}, &OperationJournalItemResult::item_index);
    entry->item_results.insert(insertion, _result);
    entry->updated_at = m_Impl->clock();

    bool rename_committed = false;
    const auto stored =
        OperationJournalPersist(
            m_Impl->parent_fd.Get(), candidate, m_Impl->next_operation_sequence, *m_Impl->syscalls, rename_committed);
    if( !stored ) {
        if( rename_committed )
            m_Impl->usable = false;
        return std::unexpected(stored.error());
    }
    m_Impl->entries = std::move(candidate);
    return {};
}

std::vector<OperationJournalEntry> OperationJournal::Snapshot() const
{
    std::lock_guard lock{m_Impl->mutex};
    return m_Impl->entries;
}

std::pair<uint64_t, uint64_t> OperationJournal::StorageIdentityForCustody() const noexcept
{
    return {m_Impl->parent_device, m_Impl->parent_inode};
}

} // namespace nc::ops
