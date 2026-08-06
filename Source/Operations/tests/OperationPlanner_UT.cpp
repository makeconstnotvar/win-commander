// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include <Operations/OperationPlanner.h>

#include <catch2/catch_all.hpp>
#include <algorithm>
#include <concepts>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace nc::ops;
using namespace std::chrono_literals;

#define PREFIX "OperationPlanner: "

namespace {

std::string Key(const OperationPlanningPath &_path)
{
    return _path.provider_id + "\n" + _path.absolute_path;
}

struct FakeProbes final : OperationPlanningProbes {
    OperationPlanningProviderEvidence default_provider{
        true, true, OperationPlanningPathIdentitySemantics::ExactBytes, true, true, true};
    OperationPlanningAccessEvidence default_access{OperationPlanningAccessState::Granted};
    OperationPlanningSpaceEvidence default_space{std::numeric_limits<uint64_t>::max()};
    std::map<std::string, OperationPlanningProbeResult<OperationPlanningProviderEvidence>> providers;
    std::map<std::string, OperationPlanningProbeResult<OperationPlanningItemEvidence>> items;
    std::map<std::string, OperationPlanningProbeResult<OperationPlanningNameEvidence>> names;
    std::map<std::string, OperationPlanningProbeResult<OperationPlanningAccessEvidence>> access;
    std::map<std::string, OperationPlanningProbeResult<OperationPlanningEstimateEvidence>> estimates;
    std::map<std::string, OperationPlanningProbeResult<OperationPlanningSpaceEvidence>> spaces;
    std::vector<std::string> calls;
    std::optional<std::string> throw_on;

    OperationPlanningProbeResult<OperationPlanningProviderEvidence>
    ProbeProvider(const OperationPlanningPath &_path) override
    {
        return Lookup("provider",
                      _path,
                      providers,
                      OperationPlanningProbeResult<OperationPlanningProviderEvidence>{default_provider});
    }

    OperationPlanningProbeResult<OperationPlanningItemEvidence>
    ProbeItem(const OperationPlanningPath &_path) override
    {
        return Lookup("item",
                      _path,
                      items,
                      OperationPlanningProbeResult<OperationPlanningItemEvidence>{OperationPlanningItemEvidence{}});
    }

    OperationPlanningProbeResult<OperationPlanningNameEvidence>
    ProbeDestinationName(const OperationPlanningPath &_path) override
    {
        return Lookup("name",
                      _path,
                      names,
                      OperationPlanningProbeResult<OperationPlanningNameEvidence>{
                          OperationPlanningNameEvidence{true}});
    }

    OperationPlanningProbeResult<OperationPlanningAccessEvidence>
    ProbeAccess(const OperationPlanningPath &_path, OperationPlanningRequiredAccess _required) override
    {
        const auto name = [&] {
            switch( _required ) {
                case OperationPlanningRequiredAccess::Read:
                    return "access-read";
                case OperationPlanningRequiredAccess::Write:
                    return "access-write";
                case OperationPlanningRequiredAccess::Rename:
                    return "access-rename";
                case OperationPlanningRequiredAccess::ReplaceFile:
                    return "access-replace-file";
                case OperationPlanningRequiredAccess::ReplaceDirectory:
                    return "access-replace-directory";
            }
            return "access-invalid";
        }();
        return Lookup(name,
                      _path,
                      access,
                      OperationPlanningProbeResult<OperationPlanningAccessEvidence>{default_access});
    }

    OperationPlanningProbeResult<OperationPlanningEstimateEvidence>
    ProbeEstimate(const OperationPlanningPath &_path,
                  const OperationPlanningPath &) override
    {
        return Lookup("estimate",
                      _path,
                      estimates,
                      OperationPlanningProbeResult<OperationPlanningEstimateEvidence>{
                          std::unexpected(OperationPlanningProbeError::Unsupported)});
    }

    OperationPlanningProbeResult<OperationPlanningSpaceEvidence>
    ProbeSpace(const OperationPlanningPath &_path) override
    {
        return Lookup("space",
                      _path,
                      spaces,
                      OperationPlanningProbeResult<OperationPlanningSpaceEvidence>{default_space});
    }

private:
    template <class T>
    OperationPlanningProbeResult<T> Lookup(std::string_view _kind,
                                           const OperationPlanningPath &_path,
                                           const std::map<std::string, OperationPlanningProbeResult<T>> &_values,
                                           OperationPlanningProbeResult<T> _fallback)
    {
        const auto call = std::string{_kind} + ':' + Key(_path);
        calls.emplace_back(call);
        if( throw_on && *throw_on == call )
            throw std::runtime_error{"probe failure"};
        if( const auto found = _values.find(Key(_path)); found != _values.end() )
            return found->second;
        return _fallback;
    }
};

OperationPlanningPath Path(std::string _provider, std::string _path)
{
    return {std::move(_provider), std::move(_path)};
}

OperationPlan MakeCopy(std::vector<OperationPlanSourceInput> _sources = {{"source", "/src/a.txt"}},
                       OperationPlanDestinationInput _destination = {"destination",
                                                                     "/dst",
                                                                     OperationPlanDestinationKind::Directory},
                       OperationPlanConflictPolicy _policy = {OperationPlanConflictDecision::Ask,
                                                               OperationPlanConflictScope::ThisItem})
{
    auto plan = OperationPlan::Create({.plan_id = "copy-plan",
                                       .type = OperationPlanType::Copy,
                                       .sources = std::move(_sources),
                                       .destination = std::move(_destination),
                                       .conflict_policy = _policy,
                                       .created_at = OperationPlan::TimePoint{1'700'000'000s}});
    REQUIRE(plan);
    return std::move(*plan);
}

OperationPlan MakeMove(
    std::vector<OperationPlanSourceInput> _sources = {{"local", "/src/a.txt"}},
    OperationPlanDestinationInput _destination = {"local", "/dst/moved.txt", OperationPlanDestinationKind::ExactItem},
    OperationPlanConflictPolicy _policy = {OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem})
{
    auto plan = OperationPlan::Create({.plan_id = "move-plan",
                                       .type = OperationPlanType::Move,
                                       .sources = std::move(_sources),
                                       .destination = std::move(_destination),
                                       .conflict_policy = _policy,
                                       .created_at = OperationPlan::TimePoint{1'700'000'000s}});
    REQUIRE(plan);
    return std::move(*plan);
}

OperationPlan MakeTrash()
{
    auto plan = OperationPlan::Create({.plan_id = "trash-plan",
                                       .type = OperationPlanType::Trash,
                                       .sources = {{"source", "/src/a.txt"}},
                                       .destination = std::nullopt,
                                       .conflict_policy = std::nullopt,
                                       .created_at = OperationPlan::TimePoint{1'700'000'000s}});
    REQUIRE(plan);
    return std::move(*plan);
}

OperationPlan MakeRename()
{
    auto plan = OperationPlan::Create(
        {.plan_id = "rename-plan",
         .type = OperationPlanType::Rename,
         .sources = {{"source", "/src/a.txt"}},
         .destination =
             OperationPlanDestinationInput{"source", "/src/renamed.txt", OperationPlanDestinationKind::ExactItem},
         .conflict_policy =
             OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem},
         .created_at = OperationPlan::TimePoint{1'700'000'000s}});
    REQUIRE(plan);
    return std::move(*plan);
}

OperationPlan MakePermanentDelete()
{
    auto plan = OperationPlan::Create({.plan_id = "permanent-delete-plan",
                                       .type = OperationPlanType::PermanentDelete,
                                       .sources = {{"source", "/src/a.txt"}},
                                       .destination = std::nullopt,
                                       .conflict_policy = std::nullopt,
                                       .created_at = OperationPlan::TimePoint{1'700'000'000s}});
    REQUIRE(plan);
    return std::move(*plan);
}

void ScriptFileCopy(FakeProbes &_probes,
                    std::string _source_provider = "source",
                    std::string _source_path = "/src/a.txt",
                    std::string _destination_provider = "destination",
                    std::string _destination_directory = "/dst",
                    uint64_t _bytes = 10)
{
    _probes.items.emplace(Key(Path(_destination_provider, _destination_directory)),
                          OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
    _probes.items.emplace(Key(Path(_source_provider, _source_path)),
                          OperationPlanningItemEvidence{OperationPlanningItemKind::File, _bytes});
}

void ScriptFileMove(FakeProbes &_probes,
                    std::string _source_provider = "local",
                    std::string _source_path = "/src/a.txt",
                    std::string _destination_provider = "local",
                    std::string _destination_path = "/dst/moved.txt",
                    uint64_t _bytes = 10)
{
    _probes.default_provider.can_rename = true;
    const auto source_parent = std::filesystem::path{_source_path}.parent_path().native();
    const auto destination_parent = std::filesystem::path{_destination_path}.parent_path().native();
    _probes.items.emplace(Key(Path(_source_provider, source_parent)),
                          OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
    _probes.items.emplace(Key(Path(_destination_provider, destination_parent)),
                          OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
    _probes.items.emplace(Key(Path(_source_provider, _source_path)),
                          OperationPlanningItemEvidence{OperationPlanningItemKind::File, _bytes});
    _probes.items.emplace(Key(Path(_destination_provider, _destination_path)),
                          OperationPlanningItemEvidence{OperationPlanningItemKind::Missing, std::nullopt});
}

const AcceptedOperationPlan &Accepted(const OperationPreflightResult &_result)
{
    REQUIRE(std::holds_alternative<AcceptedOperationPlan>(_result));
    return std::get<AcceptedOperationPlan>(_result);
}

const BlockedOperationPlan &Blocked(const OperationPreflightResult &_result)
{
    REQUIRE(std::holds_alternative<BlockedOperationPlan>(_result));
    return std::get<BlockedOperationPlan>(_result);
}

bool HasBlocker(const BlockedOperationPlan &_blocked, OperationPlanningBlockerCode _code)
{
    return std::ranges::any_of(_blocked.Blockers(), [_code](const auto &_blocker) { return _blocker.code == _code; });
}

bool HasWarning(const OperationPreflightReport &_report, OperationPlanningWarningCode _code)
{
    return std::ranges::any_of(_report.warnings, [_code](const auto &_warning) { return _warning.code == _code; });
}

template <class T>
concept Executable = requires(T &_value) { _value.Execute(); };

} // namespace

TEST_CASE(PREFIX "accepts an owning cross-provider file copy report", "[operation-planner]")
{
    OperationPreflightResult result = [&] {
        FakeProbes probes;
        ScriptFileCopy(probes);
        probes.items.at(Key(Path("source", "/src/a.txt"))).value().native_identity =
            OperationPlanningNativeObjectIdentityEvidence{
                .device = 7,
                .inode = 42,
                .birth_time = {.seconds = 100, .nanoseconds = 200},
            };
        probes.items.at(Key(Path("source", "/src/a.txt"))).value().native_version =
            OperationPlanningNativeObjectVersionEvidence{
                .mode = 0100644,
                .byte_size = 10,
                .modification_time = {.seconds = 300, .nanoseconds = 400},
                .status_change_time = {.seconds = 500, .nanoseconds = 600},
            };
        probes.default_space.available_bytes = 10;
        return OperationPlanner::Preflight(MakeCopy(), probes);
    }();

    const auto &accepted = Accepted(result);
    CHECK(accepted.Plan().Id().Value() == "copy-plan");
    REQUIRE(accepted.Report().items.size() == 1);
    CHECK(accepted.Report().items.front().source == Path("source", "/src/a.txt"));
    CHECK(accepted.Report().items.front().destination == Path("destination", "/dst/a.txt"));
    REQUIRE(accepted.Report().items.front().estimate);
    CHECK(accepted.Report().items.front().estimate->files == 1);
    CHECK(accepted.Report().items.front().estimate->bytes == 10);
    CHECK(accepted.Report().estimated_files == 1);
    CHECK(accepted.Report().estimated_bytes == 10);
    CHECK_FALSE(accepted.Report().requires_confirmation);
    CHECK(HasWarning(accepted.Report(), OperationPlanningWarningCode::RuntimeRevalidationRequired));
    CHECK_FALSE(HasWarning(accepted.Report(), OperationPlanningWarningCode::EstimateUnavailable));
    REQUIRE(accepted.Report().item_evidence.size() == 3);
    CHECK(accepted.Report().item_evidence[0] == OperationPlanningItemSnapshot{
                                                     Path("destination", "/dst"),
                                                     {OperationPlanningItemKind::Directory, std::nullopt}});
    CHECK(accepted.Report().item_evidence[1].path == Path("source", "/src/a.txt"));
    REQUIRE(accepted.Report().item_evidence[1].evidence.native_identity);
    CHECK(accepted.Report().item_evidence[1].evidence.native_identity->inode == 42);
    REQUIRE(accepted.Report().item_evidence[1].evidence.native_version);
    CHECK(accepted.Report().item_evidence[1].evidence.native_version->status_change_time ==
          OperationPlanningTimestampEvidence{500, 600});
    CHECK(accepted.Report().item_evidence[2] == OperationPlanningItemSnapshot{
                                                     Path("destination", "/dst/a.txt"),
                                                     {OperationPlanningItemKind::Missing, std::nullopt}});
}

TEST_CASE(PREFIX "blocks non-copy plans before probing", "[operation-planner]")
{
    const auto check = [](OperationPlan _plan) {
        FakeProbes probes;
        const auto result = OperationPlanner::Preflight(std::move(_plan), probes);

        CHECK(probes.calls.empty());
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::UnsupportedPlanType));
    };

    SECTION("Trash")
    {
        check(MakeTrash());
    }
    SECTION("Rename")
    {
        check(MakeRename());
    }
    SECTION("PermanentDelete")
    {
        check(MakePermanentDelete());
    }
}

TEST_CASE(PREFIX "Move accepts the exact one-file same-provider intent", "[operation-planner][move-preflight]")
{
    FakeProbes probes;
    ScriptFileMove(probes);
    probes.default_space.available_bytes = 0;

    const auto result = OperationPlanner::Preflight(MakeMove(), probes);
    const auto &accepted = Accepted(result);
    CHECK(accepted.Plan().Type() == OperationPlanType::Move);
    REQUIRE(accepted.Report().items.size() == 1);
    const auto &item = accepted.Report().items.front();
    CHECK(item.source == Path("local", "/src/a.txt"));
    CHECK(item.destination == Path("local", "/dst/moved.txt"));
    CHECK(item.source_kind == OperationPlanningItemKind::File);
    CHECK(item.estimate == OperationPlanningEstimateEvidence{.files = 1, .bytes = 10});
    CHECK(accepted.Report().estimated_files == 1);
    CHECK(accepted.Report().estimated_bytes == 10);
    CHECK_FALSE(accepted.Report().destination_space);
    CHECK_FALSE(accepted.Report().requires_confirmation);
    CHECK(HasWarning(accepted.Report(), OperationPlanningWarningCode::RuntimeRevalidationRequired));
    REQUIRE(accepted.Report().access_evidence.size() == 2);
    CHECK(accepted.Report().access_evidence[0].required == OperationPlanningRequiredAccess::Rename);
    CHECK(accepted.Report().access_evidence[1].required == OperationPlanningRequiredAccess::Rename);
    CHECK(std::ranges::find(probes.calls, "access-read:local\n/src/a.txt") == probes.calls.end());
    CHECK(std::ranges::none_of(probes.calls, [](const std::string &_call) {
        return _call.starts_with("space:") || _call.starts_with("estimate:");
    }));
    CHECK(std::ranges::find(probes.calls, "access-rename:local\n/src") != probes.calls.end());
    CHECK(std::ranges::find(probes.calls, "access-rename:local\n/dst") != probes.calls.end());
    CHECK(std::ranges::none_of(probes.calls,
                               [](const std::string &_call) { return _call.starts_with("access-write:"); }));
}

TEST_CASE(PREFIX "Move deduplicates same-parent evidence and namespace access", "[operation-planner][move-preflight]")
{
    FakeProbes probes;
    ScriptFileMove(probes, "local", "/src/a.txt", "local", "/src/moved.txt");

    const auto result = OperationPlanner::Preflight(
        MakeMove({{"local", "/src/a.txt"}}, {"local", "/src/moved.txt", OperationPlanDestinationKind::ExactItem}),
        probes);
    REQUIRE(std::holds_alternative<AcceptedOperationPlan>(result));
    CHECK(std::ranges::count(probes.calls, "provider:local\n/src") == 1);
    CHECK(std::ranges::count(probes.calls, "item:local\n/src") == 1);
    CHECK(std::ranges::count(probes.calls, "access-rename:local\n/src") == 1);
}

TEST_CASE(PREFIX "Move fails closed outside its narrow intent", "[operation-planner][move-preflight]")
{
    SECTION("a cross-provider move is unsupported before probing")
    {
        FakeProbes probes;
        const auto result = OperationPlanner::Preflight(
            MakeMove({{"source", "/src/a.txt"}},
                     {"destination", "/dst/moved.txt", OperationPlanDestinationKind::ExactItem}),
            probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::ProviderCapabilityUnsupported));
        CHECK(probes.calls.empty());
    }

    SECTION("a directory source is unsupported")
    {
        FakeProbes probes;
        ScriptFileMove(probes);
        probes.items[Key(Path("local", "/src/a.txt"))] =
            OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt};
        const auto result = OperationPlanner::Preflight(MakeMove(), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::ProviderCapabilityUnsupported));
    }

    SECTION("a symlink source is unsupported")
    {
        FakeProbes probes;
        ScriptFileMove(probes);
        probes.items[Key(Path("local", "/src/a.txt"))] =
            OperationPlanningItemEvidence{OperationPlanningItemKind::Symlink, 4};
        const auto result = OperationPlanner::Preflight(MakeMove(), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::ProviderCapabilityUnsupported));
    }

    SECTION("a batch plan is unsupported before probing")
    {
        FakeProbes probes;
        const auto result =
            OperationPlanner::Preflight(MakeMove({{"local", "/src/a.txt"}, {"local", "/src/b.txt"}},
                                                 {"local", "/dst", OperationPlanDestinationKind::Directory}),
                                        probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::UnsupportedPlanType));
        CHECK(probes.calls.empty());
    }

    SECTION("a directory destination is unsupported before probing")
    {
        FakeProbes probes;
        const auto result = OperationPlanner::Preflight(
            MakeMove({{"local", "/src/a.txt"}}, {"local", "/dst", OperationPlanDestinationKind::Directory}), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::UnsupportedPlanType));
        CHECK(probes.calls.empty());
    }

    SECTION("a non-Ask policy is rejected before probing")
    {
        FakeProbes probes;
        const auto result = OperationPlanner::Preflight(
            MakeMove({{"local", "/src/a.txt"}},
                     {"local", "/dst/moved.txt", OperationPlanDestinationKind::ExactItem},
                     {OperationPlanConflictDecision::Replace, OperationPlanConflictScope::ThisItem}),
            probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::ConflictPolicyUnsupported));
        CHECK(probes.calls.empty());
    }
}

TEST_CASE(PREFIX "Move requires rename capability and both parent namespaces", "[operation-planner][move-preflight]")
{
    SECTION("source rename capability")
    {
        FakeProbes probes;
        ScriptFileMove(probes);
        probes.providers.emplace(
            Key(Path("local", "/src")),
            OperationPlanningProviderEvidence{
                true, true, OperationPlanningPathIdentitySemantics::ExactBytes, true, true, true, false});
        const auto result = OperationPlanner::Preflight(MakeMove(), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::ProviderCapabilityUnsupported));
    }

    SECTION("destination rename capability")
    {
        FakeProbes probes;
        ScriptFileMove(probes);
        probes.providers.emplace(
            Key(Path("local", "/dst")),
            OperationPlanningProviderEvidence{
                true, true, OperationPlanningPathIdentitySemantics::ExactBytes, true, true, true, false});
        const auto result = OperationPlanner::Preflight(MakeMove(), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::DestinationNotWritable));
    }

    SECTION("source-parent access")
    {
        FakeProbes probes;
        ScriptFileMove(probes);
        probes.access.emplace(Key(Path("local", "/src")),
                              OperationPlanningAccessEvidence{OperationPlanningAccessState::Denied});
        const auto result = OperationPlanner::Preflight(MakeMove(), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::PermissionDenied));
    }

    SECTION("destination-parent access")
    {
        FakeProbes probes;
        ScriptFileMove(probes);
        probes.access.emplace(Key(Path("local", "/dst")),
                              OperationPlanningAccessEvidence{OperationPlanningAccessState::Denied});
        const auto result = OperationPlanner::Preflight(MakeMove(), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::PermissionDenied));
    }
}

TEST_CASE(PREFIX "Move retains exact conflict and path guards", "[operation-planner][move-preflight]")
{
    SECTION("existing destination requires a later explicit conflict flow")
    {
        FakeProbes probes;
        ScriptFileMove(probes);
        probes.items[Key(Path("local", "/dst/moved.txt"))] =
            OperationPlanningItemEvidence{OperationPlanningItemKind::File, 5};
        const auto result = OperationPlanner::Preflight(MakeMove(), probes);
        const auto &blocked = Blocked(result);
        CHECK(HasBlocker(blocked, OperationPlanningBlockerCode::ConflictDecisionRequired));
        REQUIRE(blocked.Report().conflicts.size() == 1);
        CHECK(blocked.Report().conflicts.front().source == Path("local", "/src/a.txt"));
        CHECK(blocked.Report().conflicts.front().destination == Path("local", "/dst/moved.txt"));
    }

    SECTION("same normalized path is rejected")
    {
        FakeProbes probes;
        ScriptFileMove(probes, "local", "/src/a.txt", "local", "/src/dir/../a.txt");
        const auto result = OperationPlanner::Preflight(
            MakeMove({{"local", "/src/a.txt"}},
                     {"local", "/src/dir/../a.txt", OperationPlanDestinationKind::ExactItem}),
            probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::SamePath));
    }
}

TEST_CASE(PREFIX "fails closed and contains probe failures", "[operation-planner]")
{
    SECTION("cancelled destination provider stops dependent probes")
    {
        FakeProbes probes;
        probes.providers.emplace(Key(Path("destination", "/dst")),
                                 std::unexpected(OperationPlanningProbeError::Cancelled));
        const auto result = OperationPlanner::Preflight(MakeCopy(), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::ProbeCancelled));
        REQUIRE(probes.calls.size() == 1);
        CHECK(probes.calls.front() == "provider:destination\n/dst");
    }

    SECTION("exception becomes a typed blocker and skips source dependencies")
    {
        FakeProbes probes;
        ScriptFileCopy(probes);
        probes.throw_on = "provider:source\n/src/a.txt";
        const auto result = OperationPlanner::Preflight(MakeCopy(), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::ProbeFailed));
        CHECK(std::ranges::find(probes.calls, "item:source\n/src/a.txt") == probes.calls.end());
    }

    SECTION("missing source and read-only destination remain distinct")
    {
        FakeProbes missing;
        missing.items.emplace(Key(Path("destination", "/dst")),
                              OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        const auto missing_result = OperationPlanner::Preflight(MakeCopy(), missing);
        CHECK(HasBlocker(Blocked(missing_result), OperationPlanningBlockerCode::SourceMissing));

        FakeProbes read_only;
        ScriptFileCopy(read_only);
        read_only.providers.emplace(
            Key(Path("destination", "/dst")),
            OperationPlanningProviderEvidence{
                true, false, OperationPlanningPathIdentitySemantics::ExactBytes});
        const auto read_only_result = OperationPlanner::Preflight(MakeCopy(), read_only);
        CHECK(HasBlocker(Blocked(read_only_result), OperationPlanningBlockerCode::DestinationNotWritable));
    }

    SECTION("invalid probe enum payloads become ProbeFailed")
    {
        FakeProbes invalid_provider;
        ScriptFileCopy(invalid_provider);
        invalid_provider.providers.emplace(
            Key(Path("destination", "/dst")),
            OperationPlanningProviderEvidence{
                true, true, static_cast<OperationPlanningPathIdentitySemantics>(255)});
        CHECK(HasBlocker(Blocked(OperationPlanner::Preflight(MakeCopy(), invalid_provider)),
                         OperationPlanningBlockerCode::ProbeFailed));

        FakeProbes invalid_item;
        ScriptFileCopy(invalid_item);
        invalid_item.items[Key(Path("source", "/src/a.txt"))] =
            OperationPlanningItemEvidence{static_cast<OperationPlanningItemKind>(255), std::nullopt};
        CHECK(HasBlocker(Blocked(OperationPlanner::Preflight(MakeCopy(), invalid_item)),
                         OperationPlanningBlockerCode::ProbeFailed));

        FakeProbes invalid_access;
        ScriptFileCopy(invalid_access);
        invalid_access.access.emplace(
            Key(Path("source", "/src/a.txt")),
            OperationPlanningAccessEvidence{static_cast<OperationPlanningAccessState>(255)});
        CHECK(HasBlocker(Blocked(OperationPlanner::Preflight(MakeCopy(), invalid_access)),
                         OperationPlanningBlockerCode::ProbeFailed));

        FakeProbes invalid_error;
        invalid_error.providers.emplace(
            Key(Path("destination", "/dst")),
            std::unexpected(static_cast<OperationPlanningProbeError>(255)));
        CHECK(HasBlocker(Blocked(OperationPlanner::Preflight(MakeCopy(), invalid_error)),
                         OperationPlanningBlockerCode::ProbeFailed));
    }

    SECTION("known special item kinds remain unsupported")
    {
        FakeProbes special_source;
        ScriptFileCopy(special_source);
        special_source.items[Key(Path("source", "/src/a.txt"))] =
            OperationPlanningItemEvidence{OperationPlanningItemKind::Other, std::nullopt};
        CHECK(HasBlocker(Blocked(OperationPlanner::Preflight(MakeCopy(), special_source)),
                         OperationPlanningBlockerCode::ProviderCapabilityUnsupported));

        FakeProbes special_destination;
        ScriptFileCopy(special_destination);
        special_destination.items.emplace(
            Key(Path("destination", "/dst/a.txt")),
            OperationPlanningItemEvidence{OperationPlanningItemKind::Other, std::nullopt});
        CHECK(HasBlocker(Blocked(OperationPlanner::Preflight(MakeCopy(), special_destination)),
                         OperationPlanningBlockerCode::ProviderCapabilityUnsupported));
    }

    SECTION("invalid destination name is evidence-backed")
    {
        FakeProbes probes;
        ScriptFileCopy(probes);
        probes.names.emplace(Key(Path("destination", "/dst/a.txt")),
                             OperationPlanningNameEvidence{false});
        const auto result = OperationPlanner::Preflight(MakeCopy(), probes);
        const auto &blocked = Blocked(result);
        CHECK(HasBlocker(blocked, OperationPlanningBlockerCode::InvalidDestinationName));
        REQUIRE(blocked.Report().name_evidence.size() == 1);
        CHECK_FALSE(blocked.Report().name_evidence.front().evidence.valid);
    }

    SECTION("same-provider directory destination preserves an existing source name")
    {
        FakeProbes probes;
        probes.items.emplace(Key(Path("local", "/dst")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("local", "/src/a:b")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});
        probes.names.emplace(Key(Path("local", "/dst/a:b")), OperationPlanningNameEvidence{false});
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"local", "/src/a:b"}},
                     {"local", "/dst", OperationPlanDestinationKind::Directory}),
            probes);
        REQUIRE(std::holds_alternative<AcceptedOperationPlan>(result));
        CHECK(std::get<AcceptedOperationPlan>(result).Report().name_evidence.empty());
    }
}

TEST_CASE(PREFIX "blocks same-path and recursive directory destinations", "[operation-planner]")
{
    SECTION("case-insensitive same path")
    {
        FakeProbes probes;
        probes.default_provider.path_identity = OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive;
        probes.items.emplace(Key(Path("local", "/src")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("local", "/SRC/A.txt")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});
        const auto plan = MakeCopy({{"local", "/SRC/A.txt"}},
                                   {"local", "/src/a.txt", OperationPlanDestinationKind::ExactItem});
        const auto result = OperationPlanner::Preflight(plan, probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::SamePath));
    }

    SECTION("directory into its descendant")
    {
        FakeProbes probes;
        probes.items.emplace(Key(Path("local", "/src/dir/inside")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("local", "/src/dir")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        const auto plan = MakeCopy({{"local", "/src/dir"}},
                                   {"local", "/src/dir/inside", OperationPlanDestinationKind::Directory});
        const auto result = OperationPlanner::Preflight(plan, probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::RecursiveDestination));
    }

    SECTION("unsupported Unicode identity comparison fails closed")
    {
        FakeProbes probes;
        probes.default_provider.path_identity = OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive;
        probes.items.emplace(Key(Path("local", "/src")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("local", "/src/Ä.txt")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"local", "/src/Ä.txt"}},
                     {"local", "/src/ä.txt", OperationPlanDestinationKind::ExactItem}),
            probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::PathIdentityUnavailable));
    }

    SECTION("case-sensitive ASCII-only evidence rejects Unicode comparison")
    {
        FakeProbes probes;
        probes.default_provider.path_identity = OperationPlanningPathIdentitySemantics::ASCIICaseSensitive;
        probes.items.emplace(Key(Path("local", "/src")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("local", "/src/é.txt")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"local", "/src/é.txt"}},
                     {"local", "/src/é.txt", OperationPlanDestinationKind::ExactItem}),
            probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::PathIdentityUnavailable));
    }

    SECTION("same-provider identity evidence combines conservatively")
    {
        FakeProbes probes;
        probes.providers.emplace(
            Key(Path("local", "/src")),
            OperationPlanningProviderEvidence{
                true, true, OperationPlanningPathIdentitySemantics::Unavailable});
        probes.providers.emplace(
            Key(Path("local", "/src/a.txt")),
            OperationPlanningProviderEvidence{
                true, true, OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive});
        probes.items.emplace(Key(Path("local", "/src")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("local", "/src/a.txt")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"local", "/src/a.txt"}},
                     {"local", "/src/b.txt", OperationPlanDestinationKind::ExactItem}),
            probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::PathIdentityUnavailable));
    }
}

TEST_CASE(PREFIX "applies the supported conflict policies explicitly", "[operation-planner]")
{
    auto run = [](OperationPlanConflictDecision _decision,
                  OperationPlanningItemKind _source_kind = OperationPlanningItemKind::File,
                  OperationPlanningItemKind _destination_kind = OperationPlanningItemKind::File) {
        FakeProbes probes;
        probes.items.emplace(Key(Path("destination", "/dst")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("source", "/src/a.txt")),
                             OperationPlanningItemEvidence{_source_kind, 10});
        probes.items.emplace(Key(Path("destination", "/dst/a.txt")),
                             OperationPlanningItemEvidence{_destination_kind, 5});
        if( _source_kind == OperationPlanningItemKind::Directory )
            probes.estimates.emplace(Key(Path("source", "/src/a.txt")),
                                     OperationPlanningEstimateEvidence{2, 10});
        auto plan = MakeCopy({{"source", "/src/a.txt"}},
                             {"destination", "/dst", OperationPlanDestinationKind::Directory},
                             {_decision, OperationPlanConflictScope::ThisItem});
        return OperationPlanner::Preflight(std::move(plan), probes);
    };

    CHECK(HasBlocker(Blocked(run(OperationPlanConflictDecision::Ask)),
                     OperationPlanningBlockerCode::ConflictDecisionRequired));

    const auto replace = run(OperationPlanConflictDecision::Replace);
    const auto &accepted_replace = Accepted(replace);
    CHECK(accepted_replace.Report().requires_confirmation);
    REQUIRE(accepted_replace.Report().destructive_effects.size() == 1);
    CHECK(HasWarning(accepted_replace.Report(), OperationPlanningWarningCode::DestructiveReplacement));

    SECTION("replace requires target capability")
    {
        FakeProbes probes;
        probes.default_provider.can_replace_file = false;
        ScriptFileCopy(probes);
        probes.items.emplace(Key(Path("destination", "/dst/a.txt")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 5});
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"source", "/src/a.txt"}},
                     {"destination", "/dst", OperationPlanDestinationKind::Directory},
                     {OperationPlanConflictDecision::Replace, OperationPlanConflictScope::ThisItem}),
            probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::ProviderCapabilityUnsupported));
    }

    SECTION("replace rejects source and destination kind changes")
    {
        CHECK(HasBlocker(Blocked(run(OperationPlanConflictDecision::Replace,
                                     OperationPlanningItemKind::File,
                                     OperationPlanningItemKind::Directory)),
                         OperationPlanningBlockerCode::ConflictPolicyUnsupported));
        CHECK(HasBlocker(Blocked(run(OperationPlanConflictDecision::Replace,
                                     OperationPlanningItemKind::Directory,
                                     OperationPlanningItemKind::File)),
                         OperationPlanningBlockerCode::ConflictPolicyUnsupported));
        CHECK(HasBlocker(Blocked(run(OperationPlanConflictDecision::Replace,
                                     OperationPlanningItemKind::Symlink,
                                     OperationPlanningItemKind::File)),
                         OperationPlanningBlockerCode::ConflictPolicyUnsupported));
        CHECK(HasBlocker(Blocked(run(OperationPlanConflictDecision::Replace,
                                     OperationPlanningItemKind::File,
                                     OperationPlanningItemKind::Symlink)),
                         OperationPlanningBlockerCode::ConflictPolicyUnsupported));
        CHECK(HasBlocker(Blocked(run(OperationPlanConflictDecision::Replace,
                                     OperationPlanningItemKind::Symlink,
                                     OperationPlanningItemKind::Directory)),
                         OperationPlanningBlockerCode::ConflictPolicyUnsupported));
        CHECK(HasBlocker(Blocked(run(OperationPlanConflictDecision::Replace,
                                     OperationPlanningItemKind::Directory,
                                     OperationPlanningItemKind::Directory)),
                         OperationPlanningBlockerCode::ConflictPolicyUnsupported));
    }

    SECTION("replace requires access to the existing target")
    {
        FakeProbes probes;
        ScriptFileCopy(probes);
        probes.items.emplace(Key(Path("destination", "/dst/a.txt")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 5});
        probes.access.emplace(Key(Path("destination", "/dst/a.txt")),
                              OperationPlanningAccessEvidence{OperationPlanningAccessState::Denied});
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"source", "/src/a.txt"}},
                     {"destination", "/dst", OperationPlanDestinationKind::Directory},
                     {OperationPlanConflictDecision::Replace, OperationPlanConflictScope::ThisItem}),
            probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::PermissionDenied));
    }

    CHECK(HasBlocker(Blocked(run(OperationPlanConflictDecision::Skip)),
                     OperationPlanningBlockerCode::NothingToDo));
    CHECK(HasBlocker(Blocked(run(OperationPlanConflictDecision::KeepBoth)),
                     OperationPlanningBlockerCode::ConflictPolicyUnsupported));
    CHECK(HasBlocker(Blocked(run(OperationPlanConflictDecision::MergeFolders,
                                 OperationPlanningItemKind::Directory,
                                 OperationPlanningItemKind::Directory)),
                     OperationPlanningBlockerCode::ConflictPolicyUnsupported));
}

TEST_CASE(PREFIX "rejects unsupported conflict policy before target discovery", "[operation-planner]")
{
    SECTION("unsupported decision with a missing target")
    {
        FakeProbes probes;
        ScriptFileCopy(probes);
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"source", "/src/a.txt"}},
                     {"destination", "/dst", OperationPlanDestinationKind::Directory},
                     {OperationPlanConflictDecision::KeepBoth, OperationPlanConflictScope::AllItems}),
            probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::ConflictPolicyUnsupported));
        CHECK(probes.calls.empty());
    }

    SECTION("unsupported scope with currently missing targets")
    {
        FakeProbes probes;
        ScriptFileCopy(probes);
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"source", "/src/a.txt"}},
                     {"destination", "/dst", OperationPlanDestinationKind::Directory},
                     {OperationPlanConflictDecision::Replace, OperationPlanConflictScope::SameFolder}),
            probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::ConflictPolicyUnsupported));
        CHECK(probes.calls.empty());
    }
}

TEST_CASE(PREFIX "requires symlink creation support for direct and nested links", "[operation-planner]")
{
    SECTION("direct symlink")
    {
        FakeProbes probes;
        probes.default_provider.can_copy_symlink_to = false;
        probes.items.emplace(Key(Path("destination", "/dst")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("source", "/src/link")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Symlink, 4});
        const auto result = OperationPlanner::Preflight(MakeCopy({{"source", "/src/link"}}), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::ProviderCapabilityUnsupported));
    }

    SECTION("nested symlink")
    {
        FakeProbes probes;
        probes.default_provider.can_copy_symlink_to = false;
        probes.items.emplace(Key(Path("destination", "/dst")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("source", "/src/tree")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.estimates.emplace(Key(Path("source", "/src/tree")),
                                 OperationPlanningEstimateEvidence{2, 10, true});
        const auto result = OperationPlanner::Preflight(MakeCopy({{"source", "/src/tree"}}), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::ProviderCapabilityUnsupported));
    }

    SECTION("unknown directory contents")
    {
        FakeProbes probes;
        probes.default_provider.can_copy_symlink_to = false;
        probes.items.emplace(Key(Path("destination", "/dst")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("source", "/src/tree")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        const auto result = OperationPlanner::Preflight(MakeCopy({{"source", "/src/tree"}}), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::ProviderCapabilityUnsupported));
        CHECK(HasWarning(Blocked(result).Report(), OperationPlanningWarningCode::EstimateUnavailable));
    }
}

TEST_CASE(PREFIX "requires nested destination-name evidence for cross-provider directories",
          "[operation-planner]")
{
    FakeProbes probes;
    probes.items.emplace(Key(Path("destination", "/dst")),
                         OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
    probes.items.emplace(Key(Path("source", "/src/tree")),
                         OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
    const auto result = OperationPlanner::Preflight(MakeCopy({{"source", "/src/tree"}}), probes);
    const auto &blocked = Blocked(result);
    CHECK(HasBlocker(blocked, OperationPlanningBlockerCode::DestinationNameEvidenceUnavailable));
    CHECK(HasWarning(blocked.Report(), OperationPlanningWarningCode::EstimateUnavailable));
}

TEST_CASE(PREFIX "reports estimates, insufficient space, unknown evidence, and overflow", "[operation-planner]")
{
    SECTION("known one-byte deficit blocks")
    {
        FakeProbes probes;
        probes.items.emplace(Key(Path("destination", "/dst")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("source", "/src/dir")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.estimates.emplace(Key(Path("source", "/src/dir")), OperationPlanningEstimateEvidence{5, 100});
        probes.default_space.available_bytes = 99;
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"source", "/src/dir"}}), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::InsufficientSpace));
    }

    SECTION("unsupported estimate and unknown space warn but can be reviewed")
    {
        FakeProbes probes;
        probes.items.emplace(Key(Path("local", "/dst")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("local", "/src/dir")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.default_space.available_bytes = std::nullopt;
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"local", "/src/dir"}},
                     {"local", "/dst", OperationPlanDestinationKind::Directory}),
            probes);
        const auto &report = Accepted(result).Report();
        CHECK_FALSE(report.estimated_bytes);
        CHECK(HasWarning(report, OperationPlanningWarningCode::EstimateUnavailable));
        CHECK(HasWarning(report, OperationPlanningWarningCode::SpaceUnknown));
    }

    SECTION("checked totals reject overflow")
    {
        FakeProbes probes;
        probes.items.emplace(Key(Path("destination", "/dst")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        for( const auto &path : {"/src/one", "/src/two"} ) {
            probes.items.emplace(Key(Path("source", path)),
                                 OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
            probes.estimates.emplace(
                Key(Path("source", path)),
                OperationPlanningEstimateEvidence{1, path == std::string_view{"/src/one"}
                                                           ? std::numeric_limits<uint64_t>::max()
                                                           : 1});
        }
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"source", "/src/one"}, {"source", "/src/two"}}), probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::EstimateOverflow));
    }
}

TEST_CASE(PREFIX "deduplicates identical probe keys and preserves deterministic order", "[operation-planner]")
{
    FakeProbes probes;
    probes.items.emplace(Key(Path("local", "/src")),
                         OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
    probes.items.emplace(Key(Path("local", "/src/a")),
                         OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});
    const auto result = OperationPlanner::Preflight(
        MakeCopy({{"local", "/src/a"}}, {"local", "/src/a", OperationPlanDestinationKind::ExactItem}), probes);
    CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::SamePath));
    CHECK(std::ranges::count(probes.calls, "provider:local\n/src/a") == 1);
    CHECK(std::ranges::count(probes.calls, "item:local\n/src/a") == 1);
    REQUIRE(probes.calls.size() >= 4);
    CHECK(probes.calls[0] == "provider:local\n/src");
    CHECK(probes.calls[1] == "item:local\n/src");
    CHECK(probes.calls[2] == "access-write:local\n/src");
}

TEST_CASE(PREFIX "records item evidence once per normalized path", "[operation-planner]")
{
    FakeProbes probes;
    probes.items.emplace(Key(Path("destination", "/dst")),
                         OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
    probes.items.emplace(Key(Path("source", "/src/a.txt")),
                         OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});

    const auto result = OperationPlanner::Preflight(
        MakeCopy({{"source", "/src/a.txt"}, {"source", "/src/dir/../a.txt"}},
                 {"destination", "/dst", OperationPlanDestinationKind::Directory},
                 {OperationPlanConflictDecision::Ask, OperationPlanConflictScope::AllItems}),
        probes);
    const auto &blocked = Blocked(result);
    CHECK(HasBlocker(blocked, OperationPlanningBlockerCode::DuplicateDestination));
    CHECK(std::ranges::count(probes.calls, "item:source\n/src/a.txt") == 1);
    CHECK(std::ranges::count(probes.calls, "item:source\n/src/dir/../a.txt") == 0);
    CHECK(std::ranges::count_if(blocked.Report().item_evidence, [](const auto &_snapshot) {
              return _snapshot.path == Path("source", "/src/a.txt");
          }) == 1);
    REQUIRE(blocked.Report().item_evidence.size() == 3);
    CHECK(blocked.Report().item_evidence[0].path == Path("destination", "/dst"));
    CHECK(blocked.Report().item_evidence[1].path == Path("source", "/src/a.txt"));
    CHECK(blocked.Report().item_evidence[2].path == Path("destination", "/dst/a.txt"));
}

TEST_CASE(PREFIX "preserves provider paths and blocks intra-plan destination collisions", "[operation-planner]")
{
    SECTION("probe and report paths preserve the structural plan spelling")
    {
        FakeProbes probes;
        probes.items.emplace(Key(Path("destination", "/dst")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("source", "/src/dir/../a.txt")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"source", "/src/dir/../a.txt"}}), probes);
        const auto &accepted = Accepted(result);
        CHECK(accepted.Report().items.front().source.absolute_path == "/src/dir/../a.txt");
        CHECK(accepted.Report().items.front().destination.absolute_path == "/dst/a.txt");
        CHECK(std::ranges::find(probes.calls, "item:source\n/src/dir/../a.txt") != probes.calls.end());
    }

    SECTION("two sources cannot map to the same effective destination")
    {
        FakeProbes probes;
        probes.items.emplace(Key(Path("destination", "/dst")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("source", "/one/a.txt")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});
        probes.items.emplace(Key(Path("source", "/two/a.txt")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"source", "/one/a.txt"}, {"source", "/two/a.txt"}},
                     {"destination", "/dst", OperationPlanDestinationKind::Directory},
                     {OperationPlanConflictDecision::Ask, OperationPlanConflictScope::AllItems}),
            probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::DuplicateDestination));
    }

    SECTION("destination identity controls cross-provider case folding")
    {
        FakeProbes probes;
        probes.providers.emplace(
            Key(Path("destination", "/dst")),
            OperationPlanningProviderEvidence{
                true, true, OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive});
        probes.items.emplace(Key(Path("destination", "/dst")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("remote-a", "/one/A.txt")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});
        probes.items.emplace(Key(Path("remote-b", "/two/a.txt")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"remote-a", "/one/A.txt"}, {"remote-b", "/two/a.txt"}},
                     {"destination", "/dst", OperationPlanDestinationKind::Directory},
                     {OperationPlanConflictDecision::Ask, OperationPlanConflictScope::AllItems}),
            probes);
        CHECK(HasBlocker(Blocked(result), OperationPlanningBlockerCode::DuplicateDestination));
    }

    SECTION("source directory spelling does not constrain cross-provider destination identity")
    {
        FakeProbes probes;
        probes.providers.emplace(
            Key(Path("destination", "/dst")),
            OperationPlanningProviderEvidence{
                true, true, OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive});
        probes.items.emplace(Key(Path("destination", "/dst")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::Directory, std::nullopt});
        probes.items.emplace(Key(Path("remote-a", "/Документы/a.txt")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});
        probes.items.emplace(Key(Path("remote-b", "/Другое/b.txt")),
                             OperationPlanningItemEvidence{OperationPlanningItemKind::File, 1});
        const auto result = OperationPlanner::Preflight(
            MakeCopy({{"remote-a", "/Документы/a.txt"}, {"remote-b", "/Другое/b.txt"}},
                     {"destination", "/dst", OperationPlanDestinationKind::Directory},
                     {OperationPlanConflictDecision::Ask, OperationPlanConflictScope::AllItems}),
            probes);
        CHECK(std::holds_alternative<AcceptedOperationPlan>(result));
    }
}

TEST_CASE(PREFIX "result types cannot be fabricated or executed", "[operation-planner]")
{
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<AcceptedOperationPlan>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<BlockedOperationPlan>);
    STATIC_REQUIRE_FALSE(std::is_constructible_v<AcceptedOperationPlan, OperationPlan, OperationPreflightReport>);
    STATIC_REQUIRE_FALSE(Executable<AcceptedOperationPlan>);
    STATIC_REQUIRE_FALSE(Executable<BlockedOperationPlan>);
}

#undef PREFIX
