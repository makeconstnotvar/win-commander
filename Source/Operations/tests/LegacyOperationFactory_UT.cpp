// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"

#include <Operations/Copying.h>
#include "../source/Copying/CopyingJobCallbacks.h"
#include <Operations/LegacyOperationFactory.h>
#include <Operations/Operation.h>
#include <Operations/OperationPlan.h>
#include <Operations/VFSOperationPlanningProbes.h>

#include <filesystem>
#include <fstream>

#define PREFIX "LegacyOperationFactory: "

namespace nc::ops {
namespace {

class NonNativePlanningHost final : public VFSHost
{
public:
    explicit NonNativePlanningHost(std::shared_ptr<VFSHost> _delegate)
        : VFSHost{"", nullptr, UniqueTag}, m_Delegate{std::move(_delegate)}
    {
        AddFeatures(vfs::HostFeatures::Read | vfs::HostFeatures::CreateFile |
                    vfs::HostFeatures::CreateDirectory | vfs::HostFeatures::Unlink |
                    vfs::HostFeatures::RemoveDirectory);
    }

    bool IsWritable() const override { return true; }
    std::optional<bool> CaseSensitivityAtPath(std::string_view _path) const override
    {
        return m_Delegate->CaseSensitivityAtPath(_path);
    }
    std::optional<std::string> SemanticNamespaceIdentity() const override
    {
        return "legacy-factory-non-native-test";
    }
    std::expected<VFSStat, Error> Stat(std::string_view _path,
                                       unsigned long _flags,
                                       const VFSCancelChecker &_cancel_checker = {}) override
    {
        return m_Delegate->Stat(_path, _flags, _cancel_checker);
    }
    std::expected<VFSStatFS, Error>
    StatFS(std::string_view _path, const VFSCancelChecker &_cancel_checker = {}) override
    {
        return m_Delegate->StatFS(_path, _cancel_checker);
    }

private:
    static constexpr const char *UniqueTag = "legacy-factory-non-native-test";
    std::shared_ptr<VFSHost> m_Delegate;
};

OperationPlan CopyPlan(std::string _source,
                       std::string _destination,
                       OperationPlanDestinationKind _destination_kind,
                       OperationPlanConflictDecision _decision = OperationPlanConflictDecision::Ask)
{
    OperationPlanInput input{
        .plan_id = "legacy-copy-factory",
        .type = OperationPlanType::Copy,
        .sources = {{"local", std::move(_source)}},
        .destination = OperationPlanDestinationInput{
            "local", std::move(_destination), _destination_kind},
        .conflict_policy = OperationPlanConflictPolicy{_decision, OperationPlanConflictScope::AllItems},
        .created_at = OperationPlan::TimePoint{std::chrono::seconds{1}},
    };
    auto plan = OperationPlan::Create(std::move(input));
    REQUIRE(plan);
    return std::move(*plan);
}

OperationPlan CopyBatchPlan(std::string _first_source,
                            std::string _second_source,
                            std::string _destination)
{
    OperationPlanInput input{
        .plan_id = "legacy-copy-factory-batch",
        .type = OperationPlanType::Copy,
        .sources = {{"local", std::move(_first_source)}, {"local", std::move(_second_source)}},
        .destination = OperationPlanDestinationInput{
            "local", std::move(_destination), OperationPlanDestinationKind::Directory},
        .conflict_policy = OperationPlanConflictPolicy{
            OperationPlanConflictDecision::Ask, OperationPlanConflictScope::AllItems},
        .created_at = OperationPlan::TimePoint{std::chrono::seconds{1}},
    };
    auto plan = OperationPlan::Create(std::move(input));
    REQUIRE(plan);
    return std::move(*plan);
}

VFSOperationPlanningProbes NativeProbes(const std::shared_ptr<VFSHost> &_host)
{
    auto bindings = VFSOperationPlanningBindings::Create({{"local", _host}});
    REQUIRE(bindings);
    auto probes = VFSOperationPlanningProbes::Create(
        *bindings,
        [](const OperationPlanningPath &, OperationPlanningRequiredAccess, nc::vfs::Host &)
            -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    REQUIRE(probes);
    return std::move(*probes);
}

ReviewedVFSOperationPreflight Review(OperationPlan _plan,
                                     VFSOperationPlanningProbes &_probes,
                                     VFSOperationPreflightReviewDecision _decision =
                                         VFSOperationPreflightReviewDecision::Approved)
{
    auto reviewed = ReviewedVFSOperationPreflight::Review(
        _probes.Preflight(std::move(_plan)), _decision);
    REQUIRE(reviewed);
    return std::move(*reviewed);
}

std::string ReadFile(const std::filesystem::path &_path)
{
    std::ifstream stream{_path};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE(PREFIX "requires reviewed exact bindings and preserves Copy intent", "[legacy-operation-factory]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    {
        std::ofstream stream{source};
        stream << "first";
    }

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = NativeProbes(host);

    SECTION("destination directory disappearance fails runtime revalidation")
    {
        auto reviewed = Review(
            CopyPlan(source.native(),
                     destination_directory.native(),
                     OperationPlanDestinationKind::Directory),
            probes);
        const auto operation = LegacyOperationFactory::Create(std::move(reviewed));
        REQUIRE(operation);
        CHECK(std::dynamic_pointer_cast<Copying>(*operation));

        std::filesystem::remove(destination_directory);
        (*operation)->Start();
        (*operation)->Wait();
        CHECK((*operation)->State() == OperationState::Stopped);
        CHECK_FALSE(std::filesystem::exists(destination_directory / source.filename()));
    }

    SECTION("destructive replacement requires explicit review confirmation")
    {
        const auto destination = destination_directory / "existing.txt";
        {
            std::ofstream stream{destination};
            stream << "old";
        }

        auto unconfirmed = ReviewedVFSOperationPreflight::Review(
            probes.Preflight(CopyPlan(source.native(),
                                      destination.native(),
                                      OperationPlanDestinationKind::ExactItem,
                                      OperationPlanConflictDecision::Replace)),
            VFSOperationPreflightReviewDecision::Approved);
        REQUIRE_FALSE(unconfirmed);
        CHECK(unconfirmed.error() ==
              VFSOperationPreflightReviewError::DestructiveConfirmationRequired);

        auto reviewed = Review(
            CopyPlan(source.native(),
                     destination.native(),
                     OperationPlanDestinationKind::ExactItem,
                     OperationPlanConflictDecision::Replace),
            probes,
            VFSOperationPreflightReviewDecision::ApprovedWithDestructiveConfirmation);
        const auto operation = LegacyOperationFactory::Create(std::move(reviewed));
        REQUIRE(operation);
        (*operation)->Start();
        (*operation)->Wait();
        CHECK((*operation)->State() == OperationState::Completed);
        CHECK(ReadFile(destination) == "first");
    }

    SECTION("replace policy requires confirmation before a conflict exists")
    {
        const auto destination = destination_directory / "missing.txt";
        auto reviewed = ReviewedVFSOperationPreflight::Review(
            probes.Preflight(CopyPlan(source.native(),
                                      destination.native(),
                                      OperationPlanDestinationKind::ExactItem,
                                      OperationPlanConflictDecision::Replace)),
            VFSOperationPreflightReviewDecision::Approved);
        REQUIRE_FALSE(reviewed);
        CHECK(reviewed.error() ==
              VFSOperationPreflightReviewError::DestructiveConfirmationRequired);
    }

    SECTION("exact-item intent does not turn into copy-inside after a directory race")
    {
        const auto exact_destination = destination_directory / "exact-race";
        auto reviewed = Review(
            CopyPlan(source.native(),
                     exact_destination.native(),
                     OperationPlanDestinationKind::ExactItem),
            probes);
        const auto operation = LegacyOperationFactory::Create(std::move(reviewed));
        REQUIRE(operation);
        auto copying = std::dynamic_pointer_cast<Copying>(*operation);
        REQUIRE(copying);

        std::filesystem::create_directory(exact_destination);
        CopyingJobCallbacks callbacks;
        callbacks.m_OnCopyDestinationAlreadyExists =
            [](const struct stat &, const struct stat &, const std::string &) {
                return CopyingJobCallbacks::CopyDestExistsResolution::Stop;
            };
        copying->SetCallbackHooks(&callbacks);
        copying->Start();
        copying->Wait();
        CHECK_FALSE(std::filesystem::exists(exact_destination / source.filename()));
    }

    SECTION("source kind swap fails immediately before mutation")
    {
        auto reviewed = Review(
            CopyPlan(source.native(),
                     destination_directory.native(),
                     OperationPlanDestinationKind::Directory),
            probes);
        const auto operation = LegacyOperationFactory::Create(std::move(reviewed));
        REQUIRE(operation);

        std::filesystem::remove(source);
        std::filesystem::create_directory(source);
        {
            std::ofstream stream{source / "nested.txt"};
            stream << "must-not-copy";
        }
        (*operation)->Start();
        (*operation)->Wait();
        CHECK((*operation)->State() == OperationState::Stopped);
        CHECK_FALSE(std::filesystem::exists(destination_directory / source.filename()));
    }

    SECTION("runtime revalidation rejects a destination symlink present before execution")
    {
        const auto exact_destination = destination_directory / "symlink-race.txt";
        const auto victim = temporary.directory / "victim.txt";
        {
            std::ofstream stream{victim};
            stream << "victim";
        }
        auto reviewed = Review(
            CopyPlan(source.native(),
                     exact_destination.native(),
                     OperationPlanDestinationKind::ExactItem),
            probes);
        const auto operation = LegacyOperationFactory::Create(std::move(reviewed));
        REQUIRE(operation);
        auto copying = std::dynamic_pointer_cast<Copying>(*operation);
        REQUIRE(copying);

        std::filesystem::create_symlink(victim, exact_destination);
        CopyingJobCallbacks callbacks;
        callbacks.m_OnCopyDestinationAlreadyExists =
            [](const struct stat &, const struct stat &, const std::string &) {
                return CopyingJobCallbacks::CopyDestExistsResolution::Overwrite;
            };
        copying->SetCallbackHooks(&callbacks);
        copying->Start();
        copying->Wait();
        CHECK(copying->State() == OperationState::Stopped);
        CHECK(ReadFile(victim) == "victim");
    }

    SECTION("legacy dereference mode remains compatible with source symlinks")
    {
        const auto source_target = temporary.directory / "source-target.txt";
        const auto source_symlink = temporary.directory / "source-symlink.txt";
        {
            std::ofstream stream{source_target};
            stream << "followed";
        }
        std::filesystem::create_symlink(source_target.filename(), source_symlink);

        CopyingOptions options;
        options.preserve_symlinks = false;
        auto source_listing = host->FetchSingleItemListing(source_symlink.native(), VFSFlags::F_NoFollow);
        REQUIRE(source_listing);
        Copying operation{{(*source_listing)->Item(0)}, destination_directory.native() + "/", host, options};
        operation.Start();
        operation.Wait();

        CHECK(operation.State() == OperationState::Completed);
        CHECK(ReadFile(destination_directory / source_symlink.filename()) == "followed");
    }
}

// Split from the SECTIONs above into its own TEST_CASE: as one function this test's frame grew past
// the project's 32 KiB limit once `OperationPreflightReport` gained a second item vector for reviewed
// Delete (`Docs/Design/reviewed_delete_execution.md`) - not because any SECTION here changed, but
// because Catch2 SECTIONs share one function frame in an unoptimized build, and every local in every
// SECTION of a single TEST_CASE competes for space in it regardless of which one runs. A second
// TEST_CASE is a second frame, which is the same fix already used in Theme_UT.mm and
// PanelPresentationGeometry_UT.mm for the identical limit. No assertion moved or changed.
TEST_CASE(PREFIX "requires reviewed exact bindings and preserves Copy intent, continued",
          "[legacy-operation-factory]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    {
        std::ofstream stream{source};
        stream << "first";
    }

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = NativeProbes(host);

    SECTION("source directory trailing slash materializes through canonical item form")
    {
        const auto source_directory = temporary.directory / "source-directory";
        std::filesystem::create_directory(source_directory);
        {
            std::ofstream stream{source_directory / "nested.txt"};
            stream << "nested";
        }
        auto reviewed = Review(
            CopyPlan(source_directory.native() + "/",
                     destination_directory.native(),
                     OperationPlanDestinationKind::Directory),
            probes);
        const auto operation = LegacyOperationFactory::Create(std::move(reviewed));
        REQUIRE(operation);
        CHECK(std::dynamic_pointer_cast<Copying>(*operation));
    }

    SECTION("blocked preflight cannot become a review token")
    {
        auto reviewed = ReviewedVFSOperationPreflight::Review(
            probes.Preflight(CopyPlan(
                source.native(), source.native(), OperationPlanDestinationKind::ExactItem)),
            VFSOperationPreflightReviewDecision::Approved);
        REQUIRE_FALSE(reviewed);
        CHECK(reviewed.error() == VFSOperationPreflightReviewError::Blocked);
    }

    SECTION("invalid review decision fails closed")
    {
        auto reviewed = ReviewedVFSOperationPreflight::Review(
            probes.Preflight(CopyPlan(source.native(),
                                      destination_directory.native(),
                                      OperationPlanDestinationKind::Directory)),
            static_cast<VFSOperationPreflightReviewDecision>(255));
        REQUIRE_FALSE(reviewed);
        CHECK(reviewed.error() == VFSOperationPreflightReviewError::InvalidDecision);
    }

    SECTION("factory revalidates source materialization through the retained host")
    {
        const auto vanishing_source = temporary.directory / "vanishing.txt";
        {
            std::ofstream stream{vanishing_source};
            stream << "gone";
        }
        auto reviewed = Review(
            CopyPlan(vanishing_source.native(),
                     destination_directory.native(),
                     OperationPlanDestinationKind::Directory),
            probes);
        std::filesystem::remove(vanishing_source);
        const auto operation = LegacyOperationFactory::Create(std::move(reviewed));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code ==
              LegacyOperationFactoryErrorCode::SourceMaterializationFailed);
        CHECK(operation.error().path == OperationPlanningPath{"local", vanishing_source.native()});
        CHECK(operation.error().cause.has_value());
    }

    SECTION("throwing cancellation is contained and fails closed")
    {
        auto reviewed = Review(
            CopyPlan(source.native(),
                     destination_directory.native(),
                     OperationPlanDestinationKind::Directory),
            probes);
        const auto operation =
            LegacyOperationFactory::Create(std::move(reviewed), []() -> bool { throw 1; });
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == LegacyOperationFactoryErrorCode::Cancelled);
    }

    SECTION("review authority is consumed once")
    {
        auto reviewed = Review(
            CopyPlan(source.native(),
                     destination_directory.native(),
                     OperationPlanDestinationKind::Directory),
            probes);
        const auto first = LegacyOperationFactory::Create(std::move(reviewed));
        REQUIRE(first);
        const auto second = LegacyOperationFactory::Create(std::move(reviewed));
        REQUIRE_FALSE(second);
        CHECK(second.error().code == LegacyOperationFactoryErrorCode::MissingBindings);
    }
}

TEST_CASE(PREFIX "rejects an unanchored non-native provider scope",
          "[legacy-operation-factory]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    {
        std::ofstream stream{source};
        stream << "source";
    }

    const auto native = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto non_native = std::make_shared<NonNativePlanningHost>(native);
    auto probes = NativeProbes(non_native);
    auto reviewed = Review(
        CopyPlan(source.native(),
                 destination_directory.native(),
                 OperationPlanDestinationKind::Directory),
        probes);
    const auto operation = LegacyOperationFactory::Create(std::move(reviewed));

    REQUIRE_FALSE(operation);
    CHECK(operation.error().code == LegacyOperationFactoryErrorCode::UnsupportedProviderScope);
}

TEST_CASE(PREFIX "rejects a reviewed batch before operation construction",
          "[legacy-operation-factory]")
{
    TempTestDir temporary;
    const auto first_source = temporary.directory / "first.txt";
    const auto second_source = temporary.directory / "second.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    {
        std::ofstream first{first_source};
        first << "first";
        std::ofstream second{second_source};
        second << "second";
    }

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = NativeProbes(host);
    auto reviewed = Review(
        CopyBatchPlan(first_source.native(),
                      second_source.native(),
                      destination_directory.native()),
        probes);
    const auto operation = LegacyOperationFactory::Create(std::move(reviewed));

    REQUIRE_FALSE(operation);
    CHECK(operation.error().code == LegacyOperationFactoryErrorCode::BatchUnsupported);
    CHECK_FALSE(std::filesystem::exists(destination_directory / first_source.filename()));
    CHECK_FALSE(std::filesystem::exists(destination_directory / second_source.filename()));
}

} // namespace nc::ops

#undef PREFIX
