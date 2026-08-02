// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include <Operations/OperationPlan.h>

#include <catch2/catch_all.hpp>
#include <concepts>
#include <type_traits>
#include <utility>

using namespace nc::ops;
using namespace std::chrono_literals;

#define PREFIX "OperationPlan: "

namespace {

constexpr auto g_AskThis =
    OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem};
constexpr auto g_CreatedAt = OperationPlan::TimePoint{1'700'000'000s};

auto Source(std::string _provider = "local", std::string _path = "/source/item.txt")
{
    return OperationPlanSourceInput{std::move(_provider), std::move(_path)};
}

auto Destination(OperationPlanDestinationKind _kind = OperationPlanDestinationKind::Directory,
                 std::string _provider = "local",
                 std::string _path = "/destination")
{
    return OperationPlanDestinationInput{std::move(_provider), std::move(_path), _kind};
}

auto Create(OperationPlanType _type,
            std::vector<OperationPlanSourceInput> _sources = {Source()},
            std::optional<OperationPlanDestinationInput> _destination = Destination(),
            std::optional<OperationPlanConflictPolicy> _policy = g_AskThis)
{
    return OperationPlan::Create({.plan_id = "plan-1",
                                  .type = _type,
                                  .sources = std::move(_sources),
                                  .destination = std::move(_destination),
                                  .conflict_policy = _policy,
                                  .created_at = g_CreatedAt});
}

template <class T>
concept ExecutablePlan = requires(T &_value) { _value.Execute(); };

template <class T>
concept AcceptablePlan = requires(T &_value) { _value.Accept(); };

} // namespace

TEST_CASE(PREFIX "is a non-default-constructible structural value", "[operation-plan]")
{
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<OperationPlan>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<OperationPlanId>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<OperationProviderId>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<OperationPlanDestinationInput>);
    STATIC_REQUIRE(std::is_aggregate_v<OperationPlanInput>);
    STATIC_REQUIRE(std::is_copy_constructible_v<OperationPlan>);
    STATIC_REQUIRE_FALSE(ExecutablePlan<OperationPlan>);
    STATIC_REQUIRE_FALSE(AcceptablePlan<OperationPlan>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<const OperationPlan &>().Sources()),
                                  const std::vector<OperationPlanSource> &>);

    const auto result = OperationPlan::Create(
        {.plan_id = "plan-copy-42",
         .type = OperationPlanType::Copy,
         .sources = {Source("native-main", "/Users/example/report.txt")},
         .destination = Destination(OperationPlanDestinationKind::Directory, "native-backup", "/Volumes/Backup"),
         .conflict_policy = OperationPlanConflictPolicy{OperationPlanConflictDecision::KeepBoth,
                                                        OperationPlanConflictScope::SameFolder},
         .created_at = g_CreatedAt});

    REQUIRE(result);
    const auto &plan = *result;
    CHECK(plan.Id().Value() == "plan-copy-42");
    CHECK(plan.Type() == OperationPlanType::Copy);
    REQUIRE(plan.Sources().size() == 1);
    CHECK(plan.Sources().front().ProviderId().Value() == "native-main");
    CHECK(plan.Sources().front().AbsolutePath() == "/Users/example/report.txt");
    REQUIRE(plan.Destination());
    CHECK(plan.Destination()->ProviderId().Value() == "native-backup");
    CHECK(plan.Destination()->AbsolutePath() == "/Volumes/Backup");
    CHECK(plan.Destination()->Kind() == OperationPlanDestinationKind::Directory);
    REQUIRE(plan.ConflictPolicy());
    CHECK(*plan.ConflictPolicy() ==
          OperationPlanConflictPolicy{OperationPlanConflictDecision::KeepBoth,
                                      OperationPlanConflictScope::SameFolder});
    CHECK(plan.CreatedAt() == g_CreatedAt);

    const OperationPlan copy = plan;
    CHECK(copy == plan);
}

TEST_CASE(PREFIX "derives baseline effects from type", "[operation-plan]")
{
    SECTION("copy preserves sources and creates destination content")
    {
        const auto plan = Create(OperationPlanType::Copy);
        REQUIRE(plan);
        CHECK(plan->IntrinsicEffects().Source() == OperationPlanSourceEffect::Unchanged);
        CHECK(plan->IntrinsicEffects().Destination() == OperationPlanDestinationEffect::CreateOrUpdate);
        CHECK(plan->IntrinsicEffects().DataLossRisk() == OperationPlanDataLossRisk::None);
    }

    SECTION("move and rename relocate their source")
    {
        const auto move = Create(OperationPlanType::Move);
        const auto rename = Create(OperationPlanType::Rename,
                                   {Source()},
                                   Destination(OperationPlanDestinationKind::ExactItem,
                                               "local",
                                               "/source/renamed.txt"));
        REQUIRE(move);
        REQUIRE(rename);
        CHECK(move->IntrinsicEffects().Source() == OperationPlanSourceEffect::Relocated);
        CHECK(rename->IntrinsicEffects().Source() == OperationPlanSourceEffect::Relocated);
    }

    SECTION("requested conflict policy does not change baseline effects")
    {
        const auto ask = Create(OperationPlanType::Copy);
        const auto replace = Create(OperationPlanType::Copy,
                                    {Source()},
                                    Destination(),
                                    OperationPlanConflictPolicy{OperationPlanConflictDecision::Replace,
                                                                OperationPlanConflictScope::AllItems});
        REQUIRE(ask);
        REQUIRE(replace);
        CHECK(replace->IntrinsicEffects() == ask->IntrinsicEffects());
    }

    SECTION("trash is recoverable and permanent delete is irreversible")
    {
        const auto trash = Create(OperationPlanType::Trash, {Source()}, std::nullopt, std::nullopt);
        const auto deletion = Create(OperationPlanType::PermanentDelete, {Source()}, std::nullopt, std::nullopt);
        REQUIRE(trash);
        REQUIRE(deletion);
        CHECK_FALSE(trash->ConflictPolicy());
        CHECK_FALSE(deletion->ConflictPolicy());
        CHECK(trash->IntrinsicEffects().Source() == OperationPlanSourceEffect::Relocated);
        CHECK(trash->IntrinsicEffects().Destination() == OperationPlanDestinationEffect::None);
        CHECK(trash->IntrinsicEffects().DataLossRisk() == OperationPlanDataLossRisk::Recoverable);
        CHECK(deletion->IntrinsicEffects().Source() == OperationPlanSourceEffect::Deleted);
        CHECK(deletion->IntrinsicEffects().Destination() == OperationPlanDestinationEffect::None);
        CHECK(deletion->IntrinsicEffects().DataLossRisk() == OperationPlanDataLossRisk::Irreversible);
    }
}

TEST_CASE(PREFIX "requires opaque nonempty identities and nonempty unique sources", "[operation-plan]")
{
    SECTION("plan id")
    {
        for( const auto &id : {std::string{}, std::string{"plan\0id", 7}} ) {
            const auto result = OperationPlan::Create({.plan_id = id,
                                                       .type = OperationPlanType::Copy,
                                                       .sources = {Source()},
                                                       .destination = Destination(),
                                                       .conflict_policy = g_AskThis,
                                                       .created_at = g_CreatedAt});
            REQUIRE_FALSE(result);
            CHECK(result.error() == OperationPlanValidationError::InvalidPlanId);
        }
    }

    SECTION("source collection")
    {
        const auto result = Create(OperationPlanType::Copy, {});
        REQUIRE_FALSE(result);
        CHECK(result.error() == OperationPlanValidationError::EmptySources);
    }

    SECTION("source provider")
    {
        for( const auto &provider : {std::string{}, std::string{"provider\0id", 11}} ) {
            const auto result = Create(OperationPlanType::Copy, {Source(provider, "/source")});
            REQUIRE_FALSE(result);
            CHECK(result.error() == OperationPlanValidationError::InvalidSourceProviderId);
        }
    }

    SECTION("duplicate identity is provider and path")
    {
        const auto duplicate = Create(OperationPlanType::Copy,
                                      {Source("p1", "/same"), Source("p1", "/same")});
        REQUIRE_FALSE(duplicate);
        CHECK(duplicate.error() == OperationPlanValidationError::DuplicateSource);

        const auto distinct = Create(OperationPlanType::Copy,
                                     {Source("p1", "/same"), Source("p2", "/same")});
        REQUIRE(distinct);
    }
}

TEST_CASE(PREFIX "requires absolute source and destination paths", "[operation-plan]")
{
    SECTION("source")
    {
        for( const auto &path : {std::string{}, std::string{"relative/item"}, std::string{"/valid\0suffix", 13}} ) {
            const auto result = Create(OperationPlanType::Copy, {Source("local", path)});
            REQUIRE_FALSE(result);
            CHECK(result.error() == OperationPlanValidationError::InvalidSourcePath);
        }
    }

    SECTION("destination")
    {
        const auto relative = Create(OperationPlanType::Copy,
                                     {Source()},
                                     Destination(OperationPlanDestinationKind::Directory, "local", "relative"));
        REQUIRE_FALSE(relative);
        CHECK(relative.error() == OperationPlanValidationError::InvalidDestinationPath);

        const auto nul_path = Create(
            OperationPlanType::Copy,
            {Source()},
            Destination(OperationPlanDestinationKind::Directory, "local", std::string{"/target\0item", 12}));
        REQUIRE_FALSE(nul_path);
        CHECK(nul_path.error() == OperationPlanValidationError::InvalidDestinationPath);

        const auto exact_trailing_slash = Create(
            OperationPlanType::Copy,
            {Source()},
            Destination(OperationPlanDestinationKind::ExactItem, "local", "/target/item/"));
        REQUIRE_FALSE(exact_trailing_slash);
        CHECK(exact_trailing_slash.error() == OperationPlanValidationError::InvalidDestinationPath);

        const auto exact_root = Create(
            OperationPlanType::Copy,
            {Source()},
            Destination(OperationPlanDestinationKind::ExactItem, "local", "/"));
        REQUIRE_FALSE(exact_root);
        CHECK(exact_root.error() == OperationPlanValidationError::InvalidDestinationPath);

        const auto empty_provider = Create(OperationPlanType::Copy,
                                           {Source()},
                                           Destination(OperationPlanDestinationKind::Directory, "", "/destination"));
        REQUIRE_FALSE(empty_provider);
        CHECK(empty_provider.error() == OperationPlanValidationError::InvalidDestinationProviderId);

        const auto nul_provider = Create(
            OperationPlanType::Copy,
            {Source()},
            Destination(OperationPlanDestinationKind::Directory, std::string{"provider\0id", 11}, "/destination"));
        REQUIRE_FALSE(nul_provider);
        CHECK(nul_provider.error() == OperationPlanValidationError::InvalidDestinationProviderId);
    }
}

TEST_CASE(PREFIX "validates operation and destination enum values", "[operation-plan]")
{
    const auto invalid_type = Create(static_cast<OperationPlanType>(255));
    REQUIRE_FALSE(invalid_type);
    CHECK(invalid_type.error() == OperationPlanValidationError::InvalidType);

    const auto invalid_destination = Create(OperationPlanType::Copy,
                                            {Source()},
                                            Destination(static_cast<OperationPlanDestinationKind>(255)));
    REQUIRE_FALSE(invalid_destination);
    CHECK(invalid_destination.error() == OperationPlanValidationError::InvalidDestinationKind);

    const auto omitted_type = OperationPlan::Create({.plan_id = "plan-without-type",
                                                     .sources = {Source()},
                                                     .destination = Destination(),
                                                     .conflict_policy = g_AskThis,
                                                     .created_at = g_CreatedAt});
    REQUIRE_FALSE(omitted_type);
    CHECK(omitted_type.error() == OperationPlanValidationError::InvalidType);
}

TEST_CASE(PREFIX "requires an injected creation timestamp", "[operation-plan]")
{
    const auto result = OperationPlan::Create({.plan_id = "plan-without-timestamp",
                                               .type = OperationPlanType::Copy,
                                               .sources = {Source()},
                                               .destination = Destination(),
                                               .conflict_policy = g_AskThis});
    REQUIRE_FALSE(result);
    CHECK(result.error() == OperationPlanValidationError::MissingCreatedAt);
}

TEST_CASE(PREFIX "enforces destination contracts for each operation type", "[operation-plan]")
{
    SECTION("copy and move require a destination")
    {
        for( const auto type : {OperationPlanType::Copy, OperationPlanType::Move} ) {
            const auto result = Create(type, {Source()}, std::nullopt);
            REQUIRE_FALSE(result);
            CHECK(result.error() == OperationPlanValidationError::MissingDestination);
        }
    }

    SECTION("an exact destination identifies one source item")
    {
        const auto result = Create(OperationPlanType::Copy,
                                   {Source("p", "/one"), Source("p", "/two")},
                                   Destination(OperationPlanDestinationKind::ExactItem, "p", "/result"));
        REQUIRE_FALSE(result);
        CHECK(result.error() == OperationPlanValidationError::ExactDestinationRequiresSingleSource);
    }

    SECTION("rename requires one source and exact same-provider destination")
    {
        const auto multiple = Create(OperationPlanType::Rename,
                                     {Source("p", "/one"), Source("p", "/two")},
                                     Destination(OperationPlanDestinationKind::ExactItem, "p", "/renamed"));
        REQUIRE_FALSE(multiple);
        CHECK(multiple.error() == OperationPlanValidationError::RenameRequiresSingleSource);

        const auto directory = Create(OperationPlanType::Rename,
                                      {Source("p", "/one")},
                                      Destination(OperationPlanDestinationKind::Directory, "p", "/target"));
        REQUIRE_FALSE(directory);
        CHECK(directory.error() == OperationPlanValidationError::RenameRequiresExactDestination);

        const auto cross_provider = Create(OperationPlanType::Rename,
                                           {Source("p1", "/one")},
                                           Destination(OperationPlanDestinationKind::ExactItem, "p2", "/renamed"));
        REQUIRE_FALSE(cross_provider);
        CHECK(cross_provider.error() == OperationPlanValidationError::RenameRequiresSameProvider);
    }

    SECTION("trash and permanent delete reject a destination")
    {
        for( const auto type : {OperationPlanType::Trash, OperationPlanType::PermanentDelete} ) {
            const auto result = Create(type, {Source()}, Destination(), std::nullopt);
            REQUIRE_FALSE(result);
            CHECK(result.error() == OperationPlanValidationError::UnexpectedDestination);
        }
    }
}

TEST_CASE(PREFIX "requires explicit valid conflict decision and scope", "[operation-plan]")
{
    SECTION("copy move and rename require a policy")
    {
        for( const auto type : {OperationPlanType::Copy, OperationPlanType::Move} ) {
            const auto result = Create(type, {Source()}, Destination(), std::nullopt);
            REQUIRE_FALSE(result);
            CHECK(result.error() == OperationPlanValidationError::MissingConflictPolicy);
        }

        const auto rename = Create(OperationPlanType::Rename,
                                   {Source()},
                                   Destination(OperationPlanDestinationKind::ExactItem,
                                               "local",
                                               "/source/renamed.txt"),
                                   std::nullopt);
        REQUIRE_FALSE(rename);
        CHECK(rename.error() == OperationPlanValidationError::MissingConflictPolicy);
    }

    SECTION("trash and permanent delete reject a policy")
    {
        for( const auto type : {OperationPlanType::Trash, OperationPlanType::PermanentDelete} ) {
            const auto result = Create(type, {Source()}, std::nullopt, g_AskThis);
            REQUIRE_FALSE(result);
            CHECK(result.error() == OperationPlanValidationError::UnexpectedConflictPolicy);
        }
    }

    SECTION("policy enum values are validated")
    {
        const auto invalid_decision = Create(
            OperationPlanType::Copy,
            {Source()},
            Destination(),
            OperationPlanConflictPolicy{static_cast<OperationPlanConflictDecision>(255),
                                        OperationPlanConflictScope::ThisItem});
        REQUIRE_FALSE(invalid_decision);
        CHECK(invalid_decision.error() == OperationPlanValidationError::InvalidConflictDecision);

        const auto invalid_scope = Create(
            OperationPlanType::Copy,
            {Source()},
            Destination(),
            OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask,
                                        static_cast<OperationPlanConflictScope>(255)});
        REQUIRE_FALSE(invalid_scope);
        CHECK(invalid_scope.error() == OperationPlanValidationError::InvalidConflictScope);
    }
}

#undef PREFIX
