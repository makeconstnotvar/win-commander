// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include <Operations/OperationPlanCodec.h>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace nc::ops;
using namespace std::chrono_literals;

namespace {

constexpr auto OperationPlanCodecUTCreatedAt = OperationPlan::TimePoint{1'700'000'000s};

OperationPlanSourceInput OperationPlanCodecUTSource(std::string _provider = "native",
                                                    std::string _path = "/source")
{
    return OperationPlanSourceInput{std::move(_provider), std::move(_path)};
}

OperationPlanDestinationInput
OperationPlanCodecUTDestination(OperationPlanDestinationKind _kind = OperationPlanDestinationKind::Directory,
                                std::string _provider = "native",
                                std::string _path = "/destination")
{
    return OperationPlanDestinationInput{std::move(_provider), std::move(_path), _kind};
}

OperationPlan OperationPlanCodecUTPlan(OperationPlanType _type,
                                       std::vector<OperationPlanSourceInput> _sources,
                                       std::optional<OperationPlanDestinationInput> _destination,
                                       std::optional<OperationPlanConflictPolicy> _policy,
                                       std::string _plan_id = "plan-1",
                                       OperationPlan::TimePoint _created_at = OperationPlanCodecUTCreatedAt)
{
    auto plan = OperationPlan::Create({.plan_id = std::move(_plan_id),
                                       .type = _type,
                                       .sources = std::move(_sources),
                                       .destination = std::move(_destination),
                                       .conflict_policy = _policy,
                                       .created_at = _created_at});
    REQUIRE(plan);
    return std::move(*plan);
}

std::string OperationPlanCodecUTReplace(std::string _value, std::string_view _from, std::string_view _to)
{
    const auto position = _value.find(_from);
    REQUIRE(position != std::string::npos);
    _value.replace(position, _from.size(), _to);
    return _value;
}

std::string OperationPlanCodecUTValidJSON()
{
    return R"({"version":1,"plan_id_base64":"cGxhbi0x","type":"copy","sources":[{"provider_id_base64":"bmF0aXZl","absolute_path_base64":"L3NvdXJjZQ=="}],"destination":{"provider_id_base64":"bmF0aXZl","absolute_path_base64":"L2Rlc3RpbmF0aW9u","kind":"directory"},"conflict_policy":{"decision":"ask","scope":"this_item"},"created_at_epoch_nanoseconds":1700000000000000000})";
}

template <class T>
concept OperationPlanCodecUTHasExecutionAuthority = requires(T &_value) {
    _value.Execute();
    _value.Enqueue();
    _value.Persist();
};

} // namespace

TEST_CASE("OperationPlanCodec: is a pure typed value codec", "[operation-plan-codec]")
{
    STATIC_REQUIRE(OperationPlanCodec::SchemaVersion == 1);
    STATIC_REQUIRE(OperationPlanCodec::MaxSources >= 10'000);
    STATIC_REQUIRE_FALSE(OperationPlanCodecUTHasExecutionAuthority<OperationPlanCodec>);
    STATIC_REQUIRE(std::is_same_v<decltype(OperationPlanCodec::Encode(std::declval<const OperationPlan &>())),
                                  std::expected<std::string, OperationPlanCodecError>>);
    STATIC_REQUIRE(std::is_same_v<decltype(OperationPlanCodec::Decode(std::declval<std::string_view>())),
                                  std::expected<OperationPlan, OperationPlanCodecError>>);
}

TEST_CASE("OperationPlanCodec: emits deterministic schema version 1 JSON", "[operation-plan-codec]")
{
    const auto plan = OperationPlanCodecUTPlan(
        OperationPlanType::Copy,
        {OperationPlanCodecUTSource("native", "/source")},
        OperationPlanCodecUTDestination(OperationPlanDestinationKind::Directory, "native", "/destination"),
        OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem});

    const auto encoded = OperationPlanCodec::Encode(plan);
    REQUIRE(encoded);
    CHECK(*encoded == OperationPlanCodecUTValidJSON());
    CHECK(OperationPlanCodec::Encode(plan) == encoded);
}

TEST_CASE("OperationPlanCodec: round-trips every structural plan type", "[operation-plan-codec]")
{
    const std::array plans{
        OperationPlanCodecUTPlan(
            OperationPlanType::Copy,
            {OperationPlanCodecUTSource("source-a", "/one"), OperationPlanCodecUTSource("source-b", "/two")},
            OperationPlanCodecUTDestination(OperationPlanDestinationKind::Directory, "target", "/copies"),
            OperationPlanConflictPolicy{OperationPlanConflictDecision::KeepBoth,
                                        OperationPlanConflictScope::SameFolder},
            "copy-plan"),
        OperationPlanCodecUTPlan(
            OperationPlanType::Move,
            {OperationPlanCodecUTSource("source", "/one")},
            OperationPlanCodecUTDestination(OperationPlanDestinationKind::ExactItem, "target", "/moved"),
            OperationPlanConflictPolicy{OperationPlanConflictDecision::Replace,
                                        OperationPlanConflictScope::AllItems},
            "move-plan"),
        OperationPlanCodecUTPlan(
            OperationPlanType::Rename,
            {OperationPlanCodecUTSource("native", "/old")},
            OperationPlanCodecUTDestination(OperationPlanDestinationKind::ExactItem, "native", "/new"),
            OperationPlanConflictPolicy{OperationPlanConflictDecision::Skip,
                                        OperationPlanConflictScope::ThisItem},
            "rename-plan"),
        OperationPlanCodecUTPlan(OperationPlanType::Trash,
                                 {OperationPlanCodecUTSource("native", "/trash-me")},
                                 std::nullopt,
                                 std::nullopt,
                                 "trash-plan"),
        OperationPlanCodecUTPlan(OperationPlanType::PermanentDelete,
                                 {OperationPlanCodecUTSource("native", "/delete-me")},
                                 std::nullopt,
                                 std::nullopt,
                                 "delete-plan"),
    };

    for( const auto &plan : plans ) {
        const auto encoded = OperationPlanCodec::Encode(plan);
        REQUIRE(encoded);
        const auto decoded = OperationPlanCodec::Decode(*encoded);
        REQUIRE(decoded);
        CHECK(*decoded == plan);
        CHECK(OperationPlanCodec::Encode(*decoded) == encoded);
    }
}

TEST_CASE("OperationPlanCodec: uses stable tokens for every policy enum", "[operation-plan-codec]")
{
    const std::array decisions{
        std::pair{OperationPlanConflictDecision::Ask, std::string_view{"\"decision\":\"ask\""}},
        std::pair{OperationPlanConflictDecision::Replace, std::string_view{"\"decision\":\"replace\""}},
        std::pair{OperationPlanConflictDecision::Skip, std::string_view{"\"decision\":\"skip\""}},
        std::pair{OperationPlanConflictDecision::KeepBoth, std::string_view{"\"decision\":\"keep_both\""}},
        std::pair{OperationPlanConflictDecision::RenameNew, std::string_view{"\"decision\":\"rename_new\""}},
        std::pair{OperationPlanConflictDecision::RenameExisting,
                  std::string_view{"\"decision\":\"rename_existing\""}},
        std::pair{OperationPlanConflictDecision::MergeFolders,
                  std::string_view{"\"decision\":\"merge_folders\""}},
    };
    for( const auto &[decision, token] : decisions ) {
        const auto plan = OperationPlanCodecUTPlan(
            OperationPlanType::Copy,
            {OperationPlanCodecUTSource()},
            OperationPlanCodecUTDestination(),
            OperationPlanConflictPolicy{decision, OperationPlanConflictScope::ThisItem});
        const auto encoded = OperationPlanCodec::Encode(plan);
        REQUIRE(encoded);
        CHECK(encoded->find(token) != std::string::npos);
        CHECK(OperationPlanCodec::Decode(*encoded) == plan);
    }

    const std::array scopes{
        std::pair{OperationPlanConflictScope::ThisItem, std::string_view{"\"scope\":\"this_item\""}},
        std::pair{OperationPlanConflictScope::AllItems, std::string_view{"\"scope\":\"all_items\""}},
        std::pair{OperationPlanConflictScope::SameExtension,
                  std::string_view{"\"scope\":\"same_extension\""}},
        std::pair{OperationPlanConflictScope::SameFolder, std::string_view{"\"scope\":\"same_folder\""}},
    };
    for( const auto &[scope, token] : scopes ) {
        const auto plan = OperationPlanCodecUTPlan(
            OperationPlanType::Copy,
            {OperationPlanCodecUTSource()},
            OperationPlanCodecUTDestination(),
            OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask, scope});
        const auto encoded = OperationPlanCodec::Encode(plan);
        REQUIRE(encoded);
        CHECK(encoded->find(token) != std::string::npos);
        CHECK(OperationPlanCodec::Decode(*encoded) == plan);
    }
}

TEST_CASE("OperationPlanCodec: preserves non-UTF-8 opaque bytes through canonical base64",
          "[operation-plan-codec]")
{
    std::string every_non_nul_byte;
    every_non_nul_byte.reserve(255);
    for( unsigned value = 1; value <= 255; ++value )
        every_non_nul_byte.push_back(static_cast<char>(value));

    const auto plan = OperationPlanCodecUTPlan(
        OperationPlanType::Copy,
        {OperationPlanCodecUTSource(every_non_nul_byte, "/" + every_non_nul_byte)},
        OperationPlanCodecUTDestination(OperationPlanDestinationKind::ExactItem,
                                        every_non_nul_byte,
                                        "/target/" + every_non_nul_byte),
        OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem},
        every_non_nul_byte);

    const auto encoded = OperationPlanCodec::Encode(plan);
    REQUIRE(encoded);
    CHECK(std::ranges::all_of(*encoded,
                              [](char _byte) { return static_cast<unsigned char>(_byte) < 0x80u; }));
    const auto decoded = OperationPlanCodec::Decode(*encoded);
    REQUIRE(decoded);
    CHECK(*decoded == plan);
}

TEST_CASE("OperationPlanCodec: rejects malformed JSON and invalid root shape", "[operation-plan-codec]")
{
    const auto malformed = OperationPlanCodec::Decode("{");
    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().code == OperationPlanCodecErrorCode::MalformedJSON);

    const auto invalid_utf8 = OperationPlanCodec::Decode(std::string{"\"\xFF\"", 3});
    REQUIRE_FALSE(invalid_utf8);
    CHECK(invalid_utf8.error().code == OperationPlanCodecErrorCode::MalformedJSON);

    const auto array = OperationPlanCodec::Decode("[]");
    REQUIRE_FALSE(array);
    CHECK(array.error().code == OperationPlanCodecErrorCode::RootNotObject);
}

TEST_CASE("OperationPlanCodec: requires exact object members without duplicates", "[operation-plan-codec]")
{
    SECTION("missing root member")
    {
        const auto result = OperationPlanCodec::Decode(OperationPlanCodecUTReplace(
            OperationPlanCodecUTValidJSON(), ",\"conflict_policy\":{\"decision\":\"ask\",\"scope\":\"this_item\"}", ""));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == OperationPlanCodecErrorCode::MissingMember);
    }

    SECTION("unexpected root member")
    {
        const auto result = OperationPlanCodec::Decode(OperationPlanCodecUTReplace(
            OperationPlanCodecUTValidJSON(), "{\"version\":1", "{\"unknown\":0,\"version\":1"));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == OperationPlanCodecErrorCode::UnexpectedMember);
    }

    SECTION("duplicate root member")
    {
        const auto result = OperationPlanCodec::Decode(OperationPlanCodecUTReplace(
            OperationPlanCodecUTValidJSON(), "{\"version\":1", "{\"version\":1,\"version\":1"));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == OperationPlanCodecErrorCode::DuplicateMember);
    }

    SECTION("unexpected nested member")
    {
        const auto result = OperationPlanCodec::Decode(OperationPlanCodecUTReplace(
            OperationPlanCodecUTValidJSON(),
            "\"absolute_path_base64\":\"L3NvdXJjZQ==\"}",
            "\"absolute_path_base64\":\"L3NvdXJjZQ==\",\"extra\":true}"));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == OperationPlanCodecErrorCode::UnexpectedMember);
    }
}

TEST_CASE("OperationPlanCodec: validates schema types version and enum tokens", "[operation-plan-codec]")
{
    const std::array cases{
        std::pair{OperationPlanCodecUTReplace(OperationPlanCodecUTValidJSON(), "\"version\":1", "\"version\":\"1\""),
                  OperationPlanCodecErrorCode::InvalidMemberType},
        std::pair{OperationPlanCodecUTReplace(OperationPlanCodecUTValidJSON(), "\"version\":1", "\"version\":2"),
                  OperationPlanCodecErrorCode::UnsupportedSchemaVersion},
        std::pair{OperationPlanCodecUTReplace(OperationPlanCodecUTValidJSON(), "\"type\":\"copy\"", "\"type\":1"),
                  OperationPlanCodecErrorCode::InvalidMemberType},
        std::pair{OperationPlanCodecUTReplace(OperationPlanCodecUTValidJSON(),
                                              "\"type\":\"copy\"",
                                              "\"type\":\"future_copy\""),
                  OperationPlanCodecErrorCode::InvalidEnumToken},
        std::pair{OperationPlanCodecUTReplace(
                      OperationPlanCodecUTValidJSON(),
                      "\"sources\":[{\"provider_id_base64\":\"bmF0aXZl\",\"absolute_path_base64\":\"L3NvdXJjZQ==\"}]",
                      "\"sources\":true"),
                  OperationPlanCodecErrorCode::InvalidMemberType},
    };

    for( const auto &[json, expected_error] : cases ) {
        const auto result = OperationPlanCodec::Decode(json);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == expected_error);
    }
}

TEST_CASE("OperationPlanCodec: accepts only canonical base64", "[operation-plan-codec]")
{
    for( const auto invalid : {"Zg", "Zh==", "Zg=A", "Zg__", "===="} ) {
        const auto json = OperationPlanCodecUTReplace(OperationPlanCodecUTValidJSON(), "cGxhbi0x", invalid);
        const auto result = OperationPlanCodec::Decode(json);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == OperationPlanCodecErrorCode::InvalidBase64);
    }
}

TEST_CASE("OperationPlanCodec: retains OperationPlan structural validation errors", "[operation-plan-codec]")
{
    const auto result = OperationPlanCodec::Decode(OperationPlanCodecUTReplace(
        OperationPlanCodecUTValidJSON(), "\"plan_id_base64\":\"cGxhbi0x\"", "\"plan_id_base64\":\"\""));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == OperationPlanCodecErrorCode::PlanValidationFailed);
    CHECK(result.error().plan_validation_error == OperationPlanValidationError::InvalidPlanId);
}

TEST_CASE("OperationPlanCodec: enforces opaque field resource limits on encode and decode",
          "[operation-plan-codec]")
{
    const auto oversized_plan_id = std::string(OperationPlanCodec::MaxOpaqueFieldBytes + 1, 'p');
    const auto plan = OperationPlanCodecUTPlan(
        OperationPlanType::Copy,
        {OperationPlanCodecUTSource()},
        OperationPlanCodecUTDestination(),
        OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem},
        oversized_plan_id);
    const auto encoded = OperationPlanCodec::Encode(plan);
    REQUIRE_FALSE(encoded);
    CHECK(encoded.error().code == OperationPlanCodecErrorCode::ResourceLimitExceeded);

    const size_t oversized_base64_length = 4 * (OperationPlanCodec::MaxOpaqueFieldBytes / 3 + 1);
    const auto json = OperationPlanCodecUTReplace(OperationPlanCodecUTValidJSON(),
                                                  "cGxhbi0x",
                                                  std::string(oversized_base64_length, 'A'));
    const auto decoded = OperationPlanCodec::Decode(json);
    REQUIRE_FALSE(decoded);
    CHECK(decoded.error().code == OperationPlanCodecErrorCode::ResourceLimitExceeded);
}

TEST_CASE("OperationPlanCodec: requires a checked signed epoch-nanosecond timestamp",
          "[operation-plan-codec]")
{
    const auto unsigned_overflow = OperationPlanCodec::Decode(OperationPlanCodecUTReplace(
        OperationPlanCodecUTValidJSON(), "1700000000000000000", "18446744073709551615"));
    REQUIRE_FALSE(unsigned_overflow);
    CHECK(unsigned_overflow.error().code == OperationPlanCodecErrorCode::TimestampOutOfRange);

    const auto one_nanosecond = OperationPlanCodec::Decode(
        OperationPlanCodecUTReplace(OperationPlanCodecUTValidJSON(), "1700000000000000000", "1"));
    if( std::chrono::duration_cast<OperationPlan::Clock::duration>(1ns) == OperationPlan::Clock::duration{0} ) {
        REQUIRE_FALSE(one_nanosecond);
        CHECK(one_nanosecond.error().code == OperationPlanCodecErrorCode::TimestampNotRepresentable);
    }
    else {
        REQUIRE(one_nanosecond);
        CHECK(one_nanosecond->CreatedAt().time_since_epoch() == 1ns);
    }

    for( const auto boundary : {OperationPlan::TimePoint::min(), OperationPlan::TimePoint::max()} ) {
        const auto plan = OperationPlanCodecUTPlan(OperationPlanType::Trash,
                                                   {OperationPlanCodecUTSource()},
                                                   std::nullopt,
                                                   std::nullopt,
                                                   "boundary-time",
                                                   boundary);
        const auto encoded = OperationPlanCodec::Encode(plan);
        if( encoded ) {
            const auto decoded = OperationPlanCodec::Decode(*encoded);
            REQUIRE(decoded);
            CHECK(decoded->CreatedAt() == boundary);
        }
        else {
            CHECK((encoded.error().code == OperationPlanCodecErrorCode::TimestampOutOfRange ||
                   encoded.error().code == OperationPlanCodecErrorCode::TimestampNotRepresentable));
        }
    }
}
