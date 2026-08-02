// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "OperationPlanCodec.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <array>
#include <limits>
#include <type_traits>
#include <utility>

namespace nc::ops {
namespace {

using OperationPlanCodecJSONValue = rapidjson::Value;
using OperationPlanCodecErrorResult = std::unexpected<OperationPlanCodecError>;

constexpr std::string_view OperationPlanCodecBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

OperationPlanCodecErrorResult OperationPlanCodecFailure(OperationPlanCodecErrorCode _code)
{
    return std::unexpected(OperationPlanCodecError{.code = _code, .plan_validation_error = std::nullopt});
}

OperationPlanCodecErrorResult OperationPlanCodecValidationFailure(OperationPlanValidationError _error)
{
    return std::unexpected(OperationPlanCodecError{.code = OperationPlanCodecErrorCode::PlanValidationFailed,
                                                    .plan_validation_error = _error});
}

template <size_t N>
std::expected<std::array<const OperationPlanCodecJSONValue *, N>, OperationPlanCodecError>
OperationPlanCodecMembers(const OperationPlanCodecJSONValue &_object,
                          const std::array<std::string_view, N> &_names)
{
    if( !_object.IsObject() )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::InvalidMemberType);

    std::array<const OperationPlanCodecJSONValue *, N> members{};
    for( auto member = _object.MemberBegin(); member != _object.MemberEnd(); ++member ) {
        const std::string_view name{member->name.GetString(), member->name.GetStringLength()};
        size_t index = N;
        for( size_t candidate = 0; candidate < N; ++candidate ) {
            if( name == _names[candidate] ) {
                index = candidate;
                break;
            }
        }
        if( index == N )
            return OperationPlanCodecFailure(OperationPlanCodecErrorCode::UnexpectedMember);
        if( members[index] != nullptr )
            return OperationPlanCodecFailure(OperationPlanCodecErrorCode::DuplicateMember);
        members[index] = &member->value;
    }

    for( const auto *member : members ) {
        if( member == nullptr )
            return OperationPlanCodecFailure(OperationPlanCodecErrorCode::MissingMember);
    }
    return members;
}

std::expected<std::string, OperationPlanCodecError> OperationPlanCodecEncodeBase64(std::string_view _bytes)
{
    if( _bytes.size() > OperationPlanCodec::MaxOpaqueFieldBytes )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::ResourceLimitExceeded);
    if( _bytes.size() > (std::numeric_limits<size_t>::max() - 2) / 4 * 3 )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::ResourceLimitExceeded);

    std::string encoded;
    encoded.reserve(((_bytes.size() + 2) / 3) * 4);
    for( size_t offset = 0; offset < _bytes.size(); offset += 3 ) {
        const auto first = static_cast<uint8_t>(_bytes[offset]);
        const bool has_second = offset + 1 < _bytes.size();
        const bool has_third = offset + 2 < _bytes.size();
        const auto second = has_second ? static_cast<uint8_t>(_bytes[offset + 1]) : uint8_t{0};
        const auto third = has_third ? static_cast<uint8_t>(_bytes[offset + 2]) : uint8_t{0};

        encoded.push_back(OperationPlanCodecBase64Alphabet[first >> 2]);
        encoded.push_back(OperationPlanCodecBase64Alphabet[((first & 0x03u) << 4u) | (second >> 4u)]);
        encoded.push_back(has_second
                              ? OperationPlanCodecBase64Alphabet[((second & 0x0Fu) << 2u) | (third >> 6u)]
                              : '=');
        encoded.push_back(has_third ? OperationPlanCodecBase64Alphabet[third & 0x3Fu] : '=');
    }
    return encoded;
}

int OperationPlanCodecBase64Value(char _character) noexcept
{
    if( _character >= 'A' && _character <= 'Z' )
        return _character - 'A';
    if( _character >= 'a' && _character <= 'z' )
        return _character - 'a' + 26;
    if( _character >= '0' && _character <= '9' )
        return _character - '0' + 52;
    if( _character == '+' )
        return 62;
    if( _character == '/' )
        return 63;
    return -1;
}

std::expected<std::string, OperationPlanCodecError>
OperationPlanCodecDecodeBase64(const OperationPlanCodecJSONValue &_value, size_t &_total_decoded_bytes)
{
    if( !_value.IsString() )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::InvalidMemberType);

    const std::string_view encoded{_value.GetString(), _value.GetStringLength()};
    if( encoded.size() % 4 != 0 )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::InvalidBase64);

    size_t padding = 0;
    if( !encoded.empty() && encoded.back() == '=' ) {
        padding = 1;
        if( encoded.size() >= 2 && encoded[encoded.size() - 2] == '=' )
            padding = 2;
    }
    const size_t decoded_size = encoded.size() / 4 * 3 - padding;
    if( decoded_size > OperationPlanCodec::MaxOpaqueFieldBytes ||
        decoded_size > OperationPlanCodec::MaxDecodedOpaqueBytes - _total_decoded_bytes )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::ResourceLimitExceeded);

    std::string decoded;
    decoded.reserve(decoded_size);
    for( size_t offset = 0; offset < encoded.size(); offset += 4 ) {
        const bool final_group = offset + 4 == encoded.size();
        const char third_character = encoded[offset + 2];
        const char fourth_character = encoded[offset + 3];
        const int first = OperationPlanCodecBase64Value(encoded[offset]);
        const int second = OperationPlanCodecBase64Value(encoded[offset + 1]);
        const int third = third_character == '=' ? 0 : OperationPlanCodecBase64Value(third_character);
        const int fourth = fourth_character == '=' ? 0 : OperationPlanCodecBase64Value(fourth_character);

        if( first < 0 || second < 0 || third < 0 || fourth < 0 ||
            (!final_group && (third_character == '=' || fourth_character == '=')) ||
            (third_character == '=' && fourth_character != '=') ||
            (third_character == '=' && (second & 0x0F) != 0) ||
            (fourth_character == '=' && third_character != '=' && (third & 0x03) != 0) )
            return OperationPlanCodecFailure(OperationPlanCodecErrorCode::InvalidBase64);

        decoded.push_back(static_cast<char>((first << 2) | (second >> 4)));
        if( third_character != '=' )
            decoded.push_back(static_cast<char>(((second & 0x0F) << 4) | (third >> 2)));
        if( fourth_character != '=' )
            decoded.push_back(static_cast<char>(((third & 0x03) << 6) | fourth));
    }

    _total_decoded_bytes += decoded.size();
    return decoded;
}

std::optional<std::string_view> OperationPlanCodecTypeToken(OperationPlanType _type) noexcept
{
    switch( _type ) {
        case OperationPlanType::Copy:
            return "copy";
        case OperationPlanType::Move:
            return "move";
        case OperationPlanType::Rename:
            return "rename";
        case OperationPlanType::Trash:
            return "trash";
        case OperationPlanType::PermanentDelete:
            return "permanent_delete";
    }
    return std::nullopt;
}

std::optional<std::string_view> OperationPlanCodecDestinationKindToken(OperationPlanDestinationKind _kind) noexcept
{
    switch( _kind ) {
        case OperationPlanDestinationKind::Directory:
            return "directory";
        case OperationPlanDestinationKind::ExactItem:
            return "exact_item";
    }
    return std::nullopt;
}

std::optional<std::string_view> OperationPlanCodecDecisionToken(OperationPlanConflictDecision _decision) noexcept
{
    switch( _decision ) {
        case OperationPlanConflictDecision::Ask:
            return "ask";
        case OperationPlanConflictDecision::Replace:
            return "replace";
        case OperationPlanConflictDecision::Skip:
            return "skip";
        case OperationPlanConflictDecision::KeepBoth:
            return "keep_both";
        case OperationPlanConflictDecision::RenameNew:
            return "rename_new";
        case OperationPlanConflictDecision::RenameExisting:
            return "rename_existing";
        case OperationPlanConflictDecision::MergeFolders:
            return "merge_folders";
    }
    return std::nullopt;
}

std::optional<std::string_view> OperationPlanCodecScopeToken(OperationPlanConflictScope _scope) noexcept
{
    switch( _scope ) {
        case OperationPlanConflictScope::ThisItem:
            return "this_item";
        case OperationPlanConflictScope::AllItems:
            return "all_items";
        case OperationPlanConflictScope::SameExtension:
            return "same_extension";
        case OperationPlanConflictScope::SameFolder:
            return "same_folder";
    }
    return std::nullopt;
}

template <class Enum, size_t N>
std::expected<Enum, OperationPlanCodecError>
OperationPlanCodecParseEnum(const OperationPlanCodecJSONValue &_value,
                            const std::array<std::pair<std::string_view, Enum>, N> &_tokens)
{
    if( !_value.IsString() )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::InvalidMemberType);
    const std::string_view token{_value.GetString(), _value.GetStringLength()};
    for( const auto &[name, value] : _tokens ) {
        if( token == name )
            return value;
    }
    return OperationPlanCodecFailure(OperationPlanCodecErrorCode::InvalidEnumToken);
}

std::expected<OperationPlanType, OperationPlanCodecError>
OperationPlanCodecParseType(const OperationPlanCodecJSONValue &_value)
{
    static constexpr std::array tokens{
        std::pair{std::string_view{"copy"}, OperationPlanType::Copy},
        std::pair{std::string_view{"move"}, OperationPlanType::Move},
        std::pair{std::string_view{"rename"}, OperationPlanType::Rename},
        std::pair{std::string_view{"trash"}, OperationPlanType::Trash},
        std::pair{std::string_view{"permanent_delete"}, OperationPlanType::PermanentDelete},
    };
    return OperationPlanCodecParseEnum(_value, tokens);
}

std::expected<OperationPlanDestinationKind, OperationPlanCodecError>
OperationPlanCodecParseDestinationKind(const OperationPlanCodecJSONValue &_value)
{
    static constexpr std::array tokens{
        std::pair{std::string_view{"directory"}, OperationPlanDestinationKind::Directory},
        std::pair{std::string_view{"exact_item"}, OperationPlanDestinationKind::ExactItem},
    };
    return OperationPlanCodecParseEnum(_value, tokens);
}

std::expected<OperationPlanConflictDecision, OperationPlanCodecError>
OperationPlanCodecParseDecision(const OperationPlanCodecJSONValue &_value)
{
    static constexpr std::array tokens{
        std::pair{std::string_view{"ask"}, OperationPlanConflictDecision::Ask},
        std::pair{std::string_view{"replace"}, OperationPlanConflictDecision::Replace},
        std::pair{std::string_view{"skip"}, OperationPlanConflictDecision::Skip},
        std::pair{std::string_view{"keep_both"}, OperationPlanConflictDecision::KeepBoth},
        std::pair{std::string_view{"rename_new"}, OperationPlanConflictDecision::RenameNew},
        std::pair{std::string_view{"rename_existing"}, OperationPlanConflictDecision::RenameExisting},
        std::pair{std::string_view{"merge_folders"}, OperationPlanConflictDecision::MergeFolders},
    };
    return OperationPlanCodecParseEnum(_value, tokens);
}

std::expected<OperationPlanConflictScope, OperationPlanCodecError>
OperationPlanCodecParseScope(const OperationPlanCodecJSONValue &_value)
{
    static constexpr std::array tokens{
        std::pair{std::string_view{"this_item"}, OperationPlanConflictScope::ThisItem},
        std::pair{std::string_view{"all_items"}, OperationPlanConflictScope::AllItems},
        std::pair{std::string_view{"same_extension"}, OperationPlanConflictScope::SameExtension},
        std::pair{std::string_view{"same_folder"}, OperationPlanConflictScope::SameFolder},
    };
    return OperationPlanCodecParseEnum(_value, tokens);
}

template <class Writer>
bool OperationPlanCodecWriteString(Writer &_writer, std::string_view _value)
{
    return _writer.String(_value.data(), static_cast<rapidjson::SizeType>(_value.size()));
}

std::expected<int64_t, OperationPlanCodecError>
OperationPlanCodecEpochNanoseconds(OperationPlan::TimePoint _time_point)
{
    using Duration = OperationPlan::Clock::duration;
    using Period = Duration::period;
    static_assert(std::is_integral_v<Duration::rep>);

    const __int128 ticks = static_cast<__int128>(_time_point.time_since_epoch().count());
    const __int128 numerator = ticks * static_cast<__int128>(Period::num) * 1'000'000'000;
    const __int128 denominator = static_cast<__int128>(Period::den);
    if( numerator % denominator != 0 )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::TimestampNotRepresentable);
    const __int128 nanoseconds = numerator / denominator;
    if( nanoseconds < std::numeric_limits<int64_t>::min() || nanoseconds > std::numeric_limits<int64_t>::max() )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::TimestampOutOfRange);
    return static_cast<int64_t>(nanoseconds);
}

std::expected<OperationPlan::TimePoint, OperationPlanCodecError>
OperationPlanCodecTimePoint(int64_t _epoch_nanoseconds)
{
    using Duration = OperationPlan::Clock::duration;
    using Period = Duration::period;
    static_assert(std::is_integral_v<Duration::rep>);

    const __int128 numerator =
        static_cast<__int128>(_epoch_nanoseconds) * static_cast<__int128>(Period::den);
    const __int128 denominator = static_cast<__int128>(Period::num) * 1'000'000'000;
    if( numerator % denominator != 0 )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::TimestampNotRepresentable);
    const __int128 ticks = numerator / denominator;
    if( ticks < std::numeric_limits<Duration::rep>::min() || ticks > std::numeric_limits<Duration::rep>::max() )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::TimestampOutOfRange);
    return OperationPlan::TimePoint{Duration{static_cast<Duration::rep>(ticks)}};
}

bool OperationPlanCodecAddOpaqueBytes(size_t _bytes, size_t &_total) noexcept
{
    if( _bytes > OperationPlanCodec::MaxOpaqueFieldBytes ||
        _bytes > OperationPlanCodec::MaxDecodedOpaqueBytes - _total )
        return false;
    _total += _bytes;
    return true;
}

} // namespace

std::expected<std::string, OperationPlanCodecError> OperationPlanCodec::Encode(const OperationPlan &_plan)
{
    if( _plan.Sources().size() > MaxSources )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::ResourceLimitExceeded);

    size_t total_opaque_bytes = 0;
    if( !OperationPlanCodecAddOpaqueBytes(_plan.Id().Value().size(), total_opaque_bytes) )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::ResourceLimitExceeded);
    for( const auto &source : _plan.Sources() ) {
        if( !OperationPlanCodecAddOpaqueBytes(source.ProviderId().Value().size(), total_opaque_bytes) ||
            !OperationPlanCodecAddOpaqueBytes(source.AbsolutePath().size(), total_opaque_bytes) )
            return OperationPlanCodecFailure(OperationPlanCodecErrorCode::ResourceLimitExceeded);
    }
    if( _plan.Destination() &&
        (!OperationPlanCodecAddOpaqueBytes(_plan.Destination()->ProviderId().Value().size(), total_opaque_bytes) ||
         !OperationPlanCodecAddOpaqueBytes(_plan.Destination()->AbsolutePath().size(), total_opaque_bytes)) )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::ResourceLimitExceeded);

    const auto plan_id = OperationPlanCodecEncodeBase64(_plan.Id().Value());
    const auto type = OperationPlanCodecTypeToken(_plan.Type());
    const auto timestamp = OperationPlanCodecEpochNanoseconds(_plan.CreatedAt());
    if( !plan_id )
        return std::unexpected(plan_id.error());
    if( !type )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::InvalidEnumToken);
    if( !timestamp )
        return std::unexpected(timestamp.error());

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer{buffer};
    writer.StartObject();
    writer.Key("version");
    writer.Uint(SchemaVersion);
    writer.Key("plan_id_base64");
    OperationPlanCodecWriteString(writer, *plan_id);
    writer.Key("type");
    OperationPlanCodecWriteString(writer, *type);
    writer.Key("sources");
    writer.StartArray();
    for( const auto &source : _plan.Sources() ) {
        const auto provider = OperationPlanCodecEncodeBase64(source.ProviderId().Value());
        const auto path = OperationPlanCodecEncodeBase64(source.AbsolutePath());
        if( !provider )
            return std::unexpected(provider.error());
        if( !path )
            return std::unexpected(path.error());
        writer.StartObject();
        writer.Key("provider_id_base64");
        OperationPlanCodecWriteString(writer, *provider);
        writer.Key("absolute_path_base64");
        OperationPlanCodecWriteString(writer, *path);
        writer.EndObject();
    }
    writer.EndArray();
    writer.Key("destination");
    if( _plan.Destination() ) {
        const auto provider = OperationPlanCodecEncodeBase64(_plan.Destination()->ProviderId().Value());
        const auto path = OperationPlanCodecEncodeBase64(_plan.Destination()->AbsolutePath());
        const auto kind = OperationPlanCodecDestinationKindToken(_plan.Destination()->Kind());
        if( !provider )
            return std::unexpected(provider.error());
        if( !path )
            return std::unexpected(path.error());
        if( !kind )
            return OperationPlanCodecFailure(OperationPlanCodecErrorCode::InvalidEnumToken);
        writer.StartObject();
        writer.Key("provider_id_base64");
        OperationPlanCodecWriteString(writer, *provider);
        writer.Key("absolute_path_base64");
        OperationPlanCodecWriteString(writer, *path);
        writer.Key("kind");
        OperationPlanCodecWriteString(writer, *kind);
        writer.EndObject();
    }
    else {
        writer.Null();
    }
    writer.Key("conflict_policy");
    if( _plan.ConflictPolicy() ) {
        const auto decision = OperationPlanCodecDecisionToken(_plan.ConflictPolicy()->Decision());
        const auto scope = OperationPlanCodecScopeToken(_plan.ConflictPolicy()->Scope());
        if( !decision || !scope )
            return OperationPlanCodecFailure(OperationPlanCodecErrorCode::InvalidEnumToken);
        writer.StartObject();
        writer.Key("decision");
        OperationPlanCodecWriteString(writer, *decision);
        writer.Key("scope");
        OperationPlanCodecWriteString(writer, *scope);
        writer.EndObject();
    }
    else {
        writer.Null();
    }
    writer.Key("created_at_epoch_nanoseconds");
    writer.Int64(*timestamp);
    writer.EndObject();

    if( buffer.GetSize() > MaxJSONBytes )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::ResourceLimitExceeded);
    return std::string{buffer.GetString(), buffer.GetSize()};
}

std::expected<OperationPlan, OperationPlanCodecError> OperationPlanCodec::Decode(std::string_view _json)
{
    if( _json.size() > MaxJSONBytes )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::ResourceLimitExceeded);

    rapidjson::Document document;
    constexpr unsigned parse_flags = rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag;
    document.Parse<parse_flags>(_json.data(), _json.size());
    if( document.HasParseError() )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::MalformedJSON);
    if( !document.IsObject() )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::RootNotObject);

    static constexpr std::array root_names{
        std::string_view{"version"},
        std::string_view{"plan_id_base64"},
        std::string_view{"type"},
        std::string_view{"sources"},
        std::string_view{"destination"},
        std::string_view{"conflict_policy"},
        std::string_view{"created_at_epoch_nanoseconds"},
    };
    const auto root = OperationPlanCodecMembers(document, root_names);
    if( !root )
        return std::unexpected(root.error());
    if( !(*root)[0]->IsUint() )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::InvalidMemberType);
    if( (*root)[0]->GetUint() != SchemaVersion )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::UnsupportedSchemaVersion);

    size_t total_decoded_bytes = 0;
    auto plan_id = OperationPlanCodecDecodeBase64(*(*root)[1], total_decoded_bytes);
    auto type = OperationPlanCodecParseType(*(*root)[2]);
    if( !plan_id )
        return std::unexpected(plan_id.error());
    if( !type )
        return std::unexpected(type.error());

    if( !(*root)[3]->IsArray() )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::InvalidMemberType);
    if( (*root)[3]->Size() > MaxSources )
        return OperationPlanCodecFailure(OperationPlanCodecErrorCode::ResourceLimitExceeded);

    std::vector<OperationPlanSourceInput> sources;
    sources.reserve((*root)[3]->Size());
    static constexpr std::array source_names{
        std::string_view{"provider_id_base64"},
        std::string_view{"absolute_path_base64"},
    };
    for( const auto &source_value : (*root)[3]->GetArray() ) {
        const auto source = OperationPlanCodecMembers(source_value, source_names);
        if( !source )
            return std::unexpected(source.error());
        auto provider = OperationPlanCodecDecodeBase64(*(*source)[0], total_decoded_bytes);
        auto path = OperationPlanCodecDecodeBase64(*(*source)[1], total_decoded_bytes);
        if( !provider )
            return std::unexpected(provider.error());
        if( !path )
            return std::unexpected(path.error());
        sources.emplace_back(OperationPlanSourceInput{std::move(*provider), std::move(*path)});
    }

    std::optional<OperationPlanDestinationInput> destination;
    if( !(*root)[4]->IsNull() ) {
        static constexpr std::array destination_names{
            std::string_view{"provider_id_base64"},
            std::string_view{"absolute_path_base64"},
            std::string_view{"kind"},
        };
        const auto destination_members = OperationPlanCodecMembers(*(*root)[4], destination_names);
        if( !destination_members )
            return std::unexpected(destination_members.error());
        auto provider = OperationPlanCodecDecodeBase64(*(*destination_members)[0], total_decoded_bytes);
        auto path = OperationPlanCodecDecodeBase64(*(*destination_members)[1], total_decoded_bytes);
        auto kind = OperationPlanCodecParseDestinationKind(*(*destination_members)[2]);
        if( !provider )
            return std::unexpected(provider.error());
        if( !path )
            return std::unexpected(path.error());
        if( !kind )
            return std::unexpected(kind.error());
        destination.emplace(std::move(*provider), std::move(*path), *kind);
    }

    std::optional<OperationPlanConflictPolicy> conflict_policy;
    if( !(*root)[5]->IsNull() ) {
        static constexpr std::array policy_names{
            std::string_view{"decision"},
            std::string_view{"scope"},
        };
        const auto policy = OperationPlanCodecMembers(*(*root)[5], policy_names);
        if( !policy )
            return std::unexpected(policy.error());
        auto decision = OperationPlanCodecParseDecision(*(*policy)[0]);
        auto scope = OperationPlanCodecParseScope(*(*policy)[1]);
        if( !decision )
            return std::unexpected(decision.error());
        if( !scope )
            return std::unexpected(scope.error());
        conflict_policy.emplace(*decision, *scope);
    }

    if( !(*root)[6]->IsInt64() ) {
        return OperationPlanCodecFailure((*root)[6]->IsNumber()
                                             ? OperationPlanCodecErrorCode::TimestampOutOfRange
                                             : OperationPlanCodecErrorCode::InvalidMemberType);
    }
    auto created_at = OperationPlanCodecTimePoint((*root)[6]->GetInt64());
    if( !created_at )
        return std::unexpected(created_at.error());

    auto plan = OperationPlan::Create({.plan_id = std::move(*plan_id),
                                       .type = *type,
                                       .sources = std::move(sources),
                                       .destination = std::move(destination),
                                       .conflict_policy = conflict_policy,
                                       .created_at = *created_at});
    if( !plan )
        return OperationPlanCodecValidationFailure(plan.error());
    return std::move(*plan);
}

} // namespace nc::ops
