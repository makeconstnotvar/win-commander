// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"

#include "../source/NativeCreateCopy/NativeCreateCopy.h"
#include "../source/ReviewedOperationFactoryTesting.h"
#include "../../VFS/source/ProviderCapabilitiesTesting.h"

#include <Operations/Operation.h>
#include <Operations/OperationPlan.h>
#include <Operations/ReviewedOperationFactory.h>
#include <Operations/VFSOperationPlanningProbes.h>

#include <VFS/Host.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unistd.h>

#define PREFIX "ReviewedOperationFactory: "

namespace nc::ops {
namespace {

class ReviewedFactoryNonNativeHost final : public VFSHost
{
public:
    explicit ReviewedFactoryNonNativeHost(std::shared_ptr<VFSHost> _delegate)
        : VFSHost{"", nullptr, UniqueTag}, m_Delegate{std::move(_delegate)}
    {
        AddFeatures(vfs::HostFeatures::Read | vfs::HostFeatures::CreateFile | vfs::HostFeatures::CreateDirectory |
                    vfs::HostFeatures::Unlink | vfs::HostFeatures::RemoveDirectory | vfs::HostFeatures::CreateSymlink);
    }

    bool IsWritable() const override { return true; }
    std::optional<bool> CaseSensitivityAtPath(std::string_view _path) const override
    {
        return m_Delegate->CaseSensitivityAtPath(_path);
    }
    std::optional<std::string> SemanticNamespaceIdentity() const override { return "reviewed-factory-non-native-test"; }
    std::expected<VFSStat, Error>
    Stat(std::string_view _path, unsigned long _flags, const VFSCancelChecker &_cancel_checker = {}) override
    {
        return m_Delegate->Stat(_path, _flags, _cancel_checker);
    }
    std::expected<VFSStatFS, Error> StatFS(std::string_view _path,
                                           const VFSCancelChecker &_cancel_checker = {}) override
    {
        return m_Delegate->StatFS(_path, _cancel_checker);
    }

private:
    static constexpr const char *UniqueTag = "reviewed-factory-non-native-test";
    std::shared_ptr<VFSHost> m_Delegate;
};

OperationPlan ReviewedCopyPlan(std::vector<OperationPlanSourceInput> _sources,
                               std::string _destination,
                               OperationPlanDestinationKind _destination_kind,
                               OperationPlanConflictDecision _decision = OperationPlanConflictDecision::Ask)
{
    OperationPlanInput input{
        .plan_id = "reviewed-create-copy",
        .type = OperationPlanType::Copy,
        .sources = std::move(_sources),
        .destination = OperationPlanDestinationInput{"local", std::move(_destination), _destination_kind},
        .conflict_policy = OperationPlanConflictPolicy{_decision, OperationPlanConflictScope::AllItems},
        .created_at = OperationPlan::TimePoint{std::chrono::seconds{1}},
    };
    auto plan = OperationPlan::Create(std::move(input));
    REQUIRE(plan);
    return std::move(*plan);
}

VFSOperationPlanningProbes
ReviewedNativeProbesWithAccess(const std::shared_ptr<VFSHost> &_host,
                               std::function<OperationPlanningProbeResult<OperationPlanningAccessEvidence>(
                                   const OperationPlanningPath &)> _access)
{
    auto bindings = VFSOperationPlanningBindings::Create({{"local", _host}});
    REQUIRE(bindings);
    auto probes = VFSOperationPlanningProbes::Create(
        *bindings,
        [access = std::move(_access)](const OperationPlanningPath &_path,
                                      OperationPlanningRequiredAccess,
                                      nc::vfs::Host &) -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return access(_path);
        });
    REQUIRE(probes);
    return std::move(*probes);
}

VFSOperationPlanningProbes ReviewedNativeProbes(const std::shared_ptr<VFSHost> &_host)
{
    return ReviewedNativeProbesWithAccess(_host, [](const OperationPlanningPath &) {
        return OperationPlanningProbeResult<OperationPlanningAccessEvidence>{
            OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted}};
    });
}

OperationPlan ReviewedMovePlan(std::string _source, std::string _destination)
{
    OperationPlanInput input{
        .plan_id = "reviewed-move",
        .type = OperationPlanType::Move,
        .sources = {{"local", std::move(_source)}},
        .destination = OperationPlanDestinationInput{"local", std::move(_destination), OperationPlanDestinationKind::ExactItem},
        .conflict_policy =
            OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem},
        .created_at = OperationPlan::TimePoint{std::chrono::seconds{1}},
    };
    auto plan = OperationPlan::Create(std::move(input));
    REQUIRE(plan);
    return std::move(*plan);
}

OperationPlan ReviewedMoveBatchPlan(std::vector<OperationPlanSourceInput> _sources, std::string _destination_directory)
{
    OperationPlanInput input{
        .plan_id = "reviewed-move-batch",
        .type = OperationPlanType::Move,
        .sources = std::move(_sources),
        .destination = OperationPlanDestinationInput{
            "local", std::move(_destination_directory), OperationPlanDestinationKind::Directory},
        .conflict_policy =
            OperationPlanConflictPolicy{OperationPlanConflictDecision::Ask, OperationPlanConflictScope::ThisItem},
        .created_at = OperationPlan::TimePoint{std::chrono::seconds{1}},
    };
    auto plan = OperationPlan::Create(std::move(input));
    REQUIRE(plan);
    return std::move(*plan);
}

ReviewedOperationFactoryTestAccess::ConditionalMoveCommitTransactionResolver
ReviewedStrongConditionalMoveCommitTransaction(
    int *_abort_calls = nullptr,
    nc::vfs::ProviderConditionalCopyPublicationState _abort_publication =
        nc::vfs::ProviderConditionalCopyPublicationState::NotPublished)
{
    return [_abort_calls, _abort_publication](
               nc::vfs::ProviderConditionalMoveReviewedAuthority _authority,
               const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &) {
        REQUIRE(_authority.HasReviewSeal());
        auto destination = _authority.Claims().destination_binding.host;
        return nc::vfs::ProviderConditionalCopyTransactionTestAccess::MintForMove(
            *destination,
            std::move(_authority),
            [](const auto &) {
                return nc::vfs::ProviderConditionalCopyCommitResult{
                    .publication = nc::vfs::ProviderConditionalCopyPublicationState::Published,
                    .failure = nc::vfs::ProviderConditionalCopyCommitFailure::None,
                    .filesystem_sync_status =
                        nc::vfs::ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
                };
            },
            [_abort_calls, _abort_publication] {
                if( _abort_calls != nullptr )
                    ++*_abort_calls;
                return _abort_publication;
            });
    };
}

ReviewedVFSOperationPreflight
ReviewedReview(OperationPlan _plan,
               VFSOperationPlanningProbes &_probes,
               VFSOperationPreflightReviewDecision _decision = VFSOperationPreflightReviewDecision::Approved)
{
    auto reviewed = ReviewedVFSOperationPreflight::Review(_probes.Preflight(std::move(_plan)), _decision);
    REQUIRE(reviewed);
    return std::move(*reviewed);
}

void ReviewedWriteFile(const std::filesystem::path &_path, std::string_view _contents)
{
    std::ofstream stream{_path, std::ios::binary};
    REQUIRE(stream);
    stream.write(_contents.data(), static_cast<std::streamsize>(_contents.size()));
    REQUIRE(stream);
}

std::string ReviewedReadFile(const std::filesystem::path &_path)
{
    std::ifstream stream{_path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

ReviewedOperationFactoryTestAccess::ConditionalCommitTransactionResolver
ReviewedStrongConditionalCommitTransaction(
    int *_abort_calls = nullptr,
    nc::vfs::ProviderConditionalCopyPublicationState _abort_publication =
        nc::vfs::ProviderConditionalCopyPublicationState::NotPublished)
{
    return [_abort_calls, _abort_publication](
               nc::vfs::ProviderConditionalCopyReviewedAuthority _authority,
               const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &) {
        REQUIRE(_authority.HasReviewSeal());
        auto destination = _authority.Claims().destination_binding.host;
        return nc::vfs::ProviderConditionalCopyTransactionTestAccess::Mint(
            *destination,
            std::move(_authority),
            [](const auto &) {
                return nc::vfs::ProviderConditionalCopyCommitResult{
                    .publication = nc::vfs::ProviderConditionalCopyPublicationState::Published,
                    .failure = nc::vfs::ProviderConditionalCopyCommitFailure::None,
                    .filesystem_sync_status =
                        nc::vfs::ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
                };
            },
            [_abort_calls, _abort_publication] {
                if( _abort_calls != nullptr )
                    ++*_abort_calls;
                return _abort_publication;
            });
    };
}

std::expected<std::shared_ptr<Operation>, ReviewedOperationFactoryError>
ReviewedCreateWithStrongTestAuthority(
    ReviewedVFSOperationPreflight _reviewed,
    ReviewedOperationFactory::CancelChecker _cancel_checker = {},
    ReviewedOperationFactoryTestAccess::DirectAccessChecker _direct_access_checker = {},
    ReviewedOperationFactoryTestAccess::SourceOpenAt _source_open_at = {})
{
    return ReviewedOperationFactoryTestAccess::Create(std::move(_reviewed),
                                                      ReviewedStrongConditionalCommitTransaction(),
                                                      std::move(_cancel_checker),
                                                      std::move(_direct_access_checker),
                                                      std::move(_source_open_at));
}

void ReviewedReachConditionalCommitIntegrationBlocker(ReviewedVFSOperationPreflight _reviewed,
                                                       const std::filesystem::path &_destination)
{
    int abort_calls = 0;
    const auto operation = ReviewedOperationFactoryTestAccess::Create(
        std::move(_reviewed), ReviewedStrongConditionalCommitTransaction(&abort_calls));
    REQUIRE_FALSE(operation);
    CHECK(operation.error().code ==
          ReviewedOperationFactoryErrorCode::ConditionalCommitIntegrationUnavailable);
    CHECK(abort_calls == 1);
    CHECK_FALSE(std::filesystem::exists(_destination));
}

} // namespace

static_assert(!std::is_copy_constructible_v<ReviewedVFSOperationPreflight>);
static_assert(!std::is_copy_constructible_v<NativeCreateCopyCapsule>);
static_assert(!std::is_copy_constructible_v<nc::vfs::ProviderConditionalCopyTransaction>);
static_assert(std::is_move_constructible_v<nc::vfs::ProviderConditionalCopyTransaction>);

TEST_CASE(PREFIX "fails closed without expected-version conditional commit authority",
          "[reviewed-operation-factory]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    const auto destination = destination_directory / source.filename();
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(source, "reviewed payload");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);

    SECTION("public Native authority reaches the explicit integration blocker")
    {
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        const auto operation = ReviewedOperationFactory::Create(std::move(reviewed));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code ==
              ReviewedOperationFactoryErrorCode::ConditionalCommitIntegrationUnavailable);
        CHECK_FALSE(operation.error().path);
        CHECK_FALSE(std::filesystem::exists(destination));
    }

    SECTION("a null provider transaction cannot authorize construction")
    {
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        bool direct_access_checked = false;
        const auto operation = ReviewedOperationFactoryTestAccess::Create(
            std::move(reviewed),
            [](nc::vfs::ProviderConditionalCopyReviewedAuthority,
               const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &)
                -> std::expected<std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction>,
                                 nc::vfs::ProviderConditionalCopyTransactionBeginError> { return nullptr; },
            {},
            [&](std::string_view, int) {
                direct_access_checked = true;
                return true;
            });
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code ==
              ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable);
        CHECK_FALSE(operation.error().path);
        CHECK(direct_access_checked);
        CHECK_FALSE(std::filesystem::exists(destination));
    }

    SECTION("uncertain cold abort is an authority failure")
    {
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        int abort_calls = 0;
        const auto operation = ReviewedOperationFactoryTestAccess::Create(
            std::move(reviewed),
            ReviewedStrongConditionalCommitTransaction(
                &abort_calls,
                nc::vfs::ProviderConditionalCopyPublicationState::Unknown));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code ==
              ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable);
        CHECK(abort_calls == 1);
        CHECK_FALSE(std::filesystem::exists(destination));
    }
}

TEST_CASE(PREFIX "private execution product retains exact reviewed transaction context",
          "[reviewed-operation-factory]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(source, "reviewed payload");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);
    auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                    destination_directory.native(),
                                                    OperationPlanDestinationKind::Directory),
                                   probes);
    int abort_calls = 0;
    auto created = ReviewedOperationFactoryTestAccess::CreateExecutionProduct(
        std::move(reviewed), ReviewedStrongConditionalCommitTransaction(&abort_calls));
    REQUIRE(created);
    auto product = std::move(*created);
    auto &operation = ReviewedOperationFactoryTestAccess::Operation(product);
    auto &terminal_item_result = ReviewedOperationFactoryTestAccess::TerminalItemResult(product);
    CHECK(operation->State() == OperationState::Cold);
    const auto pending = terminal_item_result();
    REQUIRE_FALSE(pending);
    CHECK(pending.error() == CopyOperationTerminalResultError::Pending);

    operation->Start();
    REQUIRE(operation->Wait(std::chrono::seconds{5}));
    const auto terminal = terminal_item_result();
    REQUIRE(terminal);
    CHECK(terminal->item_index == 0);
    CHECK(terminal->bytes == std::filesystem::file_size(source));
    CHECK(terminal->destination_publication == OperationJournalPublicationState::Published);
    CHECK(terminal->filesystem_sync_status == OperationJournalFilesystemSyncStatus::Confirmed);
    CHECK(abort_calls == 0);
}

TEST_CASE(PREFIX "provider transaction reaches the explicit Native commit integration blocker",
          "[reviewed-operation-factory]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(source, "reviewed payload");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);

    SECTION("exact item")
    {
        const auto destination = destination_directory / "exact.txt";
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination.native(),
                                                        OperationPlanDestinationKind::ExactItem),
                                       probes);
        ReviewedReachConditionalCommitIntegrationBlocker(std::move(reviewed), destination);
    }

    SECTION("directory derives the source name and accepts Skip while missing")
    {
        const auto destination = destination_directory / source.filename();
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination_directory.native() + "/",
                                                        OperationPlanDestinationKind::Directory,
                                                        OperationPlanConflictDecision::Skip),
                                       probes);
        ReviewedReachConditionalCommitIntegrationBlocker(std::move(reviewed), destination);
    }
}

TEST_CASE(PREFIX "rejects stale source and destination identity before capsule construction",
          "[reviewed-operation-factory]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    const auto destination = destination_directory / "target.txt";
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(source, "first");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);

    SECTION("source object changed")
    {
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination.native(),
                                                        OperationPlanDestinationKind::ExactItem),
                                       probes);
        std::filesystem::remove(source);
        ReviewedWriteFile(source, "replacement object");

        const auto operation = ReviewedCreateWithStrongTestAuthority(std::move(reviewed));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::StaleSource);
        CHECK(operation.error().path == OperationPlanningPath{"local", source.native()});
        CHECK_FALSE(std::filesystem::exists(destination));
    }

    SECTION("destination parent object changed")
    {
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination.native(),
                                                        OperationPlanDestinationKind::ExactItem),
                                       probes);
        const auto old_directory = temporary.directory / "old-destination";
        std::filesystem::rename(destination_directory, old_directory);
        std::filesystem::create_directory(destination_directory);

        const auto operation = ReviewedCreateWithStrongTestAuthority(std::move(reviewed));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::StaleDestination);
        CHECK(operation.error().path == OperationPlanningPath{"local", destination_directory.native()});
        CHECK_FALSE(std::filesystem::exists(destination));
        CHECK_FALSE(std::filesystem::exists(old_directory / destination.filename()));
    }

    SECTION("effective target appeared")
    {
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination.native(),
                                                        OperationPlanDestinationKind::ExactItem),
                                       probes);
        ReviewedWriteFile(destination, "existing");

        const auto operation = ReviewedCreateWithStrongTestAuthority(std::move(reviewed));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::StaleDestination);
        CHECK(operation.error().path == OperationPlanningPath{"local", destination.native()});
        CHECK(ReviewedReadFile(destination) == "existing");
    }

    SECTION("destination ancestor was rebound")
    {
        const auto ancestor = temporary.directory / "ancestor";
        const auto nested_destination = ancestor / "nested";
        const auto nested_target = nested_destination / "target.txt";
        std::filesystem::create_directories(nested_destination);
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        nested_target.native(),
                                                        OperationPlanDestinationKind::ExactItem),
                                       probes);

        const auto reviewed_ancestor = temporary.directory / "reviewed-ancestor";
        std::filesystem::rename(ancestor, reviewed_ancestor);
        std::filesystem::create_directories(nested_destination);

        const auto operation = ReviewedCreateWithStrongTestAuthority(std::move(reviewed));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::StaleDestination);
        CHECK(operation.error().path == OperationPlanningPath{"local", nested_destination.native()});
        CHECK_FALSE(std::filesystem::exists(nested_target));
        CHECK_FALSE(std::filesystem::exists(reviewed_ancestor / "nested" / "target.txt"));
    }
}

TEST_CASE(PREFIX "never sees conflict evidence, because the review refuses it first",
          "[reviewed-operation-factory]")
{
    // Written to reach UnexpectedConflictEvidence and it showed that path is unreachable too: a plan
    // whose destination is already occupied is refused at review, so the factory's own check is
    // defence in depth rather than a case anyone can drive. Worth pinning as the reason - otherwise
    // the next person counting untested paths tries the same thing and learns it again.
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    std::ofstream(source) << "payload";
    std::ofstream(destination_directory / "source.txt") << "occupied";

    auto probes = ReviewedNativeProbes(TestEnv().vfs_native);
    const auto reviewed = ReviewedVFSOperationPreflight::Review(
        probes.Preflight(ReviewedCopyPlan({{"local", source.native()}},
                                          destination_directory.native(),
                                          OperationPlanDestinationKind::Directory)),
        VFSOperationPreflightReviewDecision::Approved);
    REQUIRE_FALSE(reviewed);

    // And the occupant is untouched - nothing got as far as opening anything.
    std::ifstream kept(destination_directory / "source.txt");
    const std::string contents{std::istreambuf_iterator<char>{kept}, std::istreambuf_iterator<char>{}};
    CHECK(contents == "occupied");
}

TEST_CASE(PREFIX "refuses when the evidence for a path is missing or is the wrong shape",
          "[reviewed-operation-factory]")
{
    // These two guard the staleness evidence, and a real planner never produces them - so they are
    // reached through an injected snapshot lookup. That seam, rather than a way to construct an
    // accepted plan, is the deliberate choice: it can only change how already-reviewed evidence is
    // looked up, where a forge-a-plan seam could manufacture a review that never happened.
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    std::ofstream(source) << "payload";

    const auto make_reviewed = [&] {
        auto probes = ReviewedNativeProbes(TestEnv().vfs_native);
        return ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                               destination_directory.native(),
                                               OperationPlanDestinationKind::Directory),
                              probes);
    };

    SECTION("missing")
    {
        const auto operation = ReviewedOperationFactoryTestAccess::Create(
            make_reviewed(), ReviewedStrongConditionalCommitTransaction(), {}, {}, {},
            [](const OperationPreflightReport &, const OperationPlanningPath &) { return nullptr; });
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::MissingEvidence);
    }

    SECTION("wrong shape")
    {
        // Present, but claiming the source is a directory. Accepting it would let the copy proceed
        // against evidence that describes something else entirely.
        OperationPlanningItemSnapshot forged;
        const auto operation = ReviewedOperationFactoryTestAccess::Create(
            make_reviewed(), ReviewedStrongConditionalCommitTransaction(), {}, {}, {},
            [&forged](const OperationPreflightReport &_report, const OperationPlanningPath &_path)
                -> const OperationPlanningItemSnapshot * {
                const OperationPlanningItemSnapshot *real = nullptr;
                for( const auto &snapshot : _report.item_evidence )
                    if( snapshot.path.provider_id == _path.provider_id )
                        if( snapshot.evidence.kind == OperationPlanningItemKind::File )
                            real = &snapshot;
                if( real == nullptr )
                    return real;
                forged = *real;
                forged.evidence.kind = OperationPlanningItemKind::Directory;
                return &forged;
            });
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::InvalidEvidence);
    }
}

TEST_CASE(PREFIX "reports a source that would not open as a failure, not as staleness",
          "[reviewed-operation-factory]")
{
    // OpenFailed is raised at eight places in this factory and was asserted by nothing. It is also
    // exactly what a per-item extraction would move, so leaving it unpinned would mean moving the
    // descriptor handling blind.
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    std::ofstream(source) << "payload";

    auto probes = ReviewedNativeProbes(TestEnv().vfs_native);
    auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                    destination_directory.native(),
                                                    OperationPlanDestinationKind::Directory),
                                   probes);

    // EACCES rather than ENOENT: the missing-file errnos mean the world moved and are reported as
    // staleness, which is a different thing to tell the user than "this could not be opened".
    const auto operation = ReviewedCreateWithStrongTestAuthority(
        std::move(reviewed), {}, {}, [](int, const char *, int) {
            errno = EACCES;
            return -1;
        });
    REQUIRE_FALSE(operation);
    CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::OpenFailed);
    CHECK(operation.error().path == OperationPlanningPath{"local", source.native()});
}

TEST_CASE(PREFIX "rejects destructive policy, unsupported source shapes, and batches", "[reviewed-operation-factory]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto second_source = temporary.directory / "second.txt";
    const auto source_directory = temporary.directory / "source-directory";
    const auto source_symlink = temporary.directory / "source-link";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(source_directory);
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(source, "first");
    ReviewedWriteFile(second_source, "second");
    std::filesystem::create_symlink(source.filename(), source_symlink);

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);

    SECTION("Replace is outside create-only scope even when the target was missing")
    {
        const auto destination = destination_directory / "replace.txt";
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination.native(),
                                                        OperationPlanDestinationKind::ExactItem,
                                                        OperationPlanConflictDecision::Replace),
                                       probes,
                                       VFSOperationPreflightReviewDecision::ApprovedWithDestructiveConfirmation);
        const auto operation = ReviewedOperationFactory::Create(std::move(reviewed));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::UnsupportedConflictPolicy);
    }

    SECTION("directory")
    {
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source_directory.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        const auto operation = ReviewedOperationFactory::Create(std::move(reviewed));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::UnsupportedSourceKind);
    }

    SECTION("symlink")
    {
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source_symlink.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        const auto operation = ReviewedOperationFactory::Create(std::move(reviewed));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::UnsupportedSourceKind);
    }

    SECTION("a directory source is one item, not several")
    {
        // Written to pin the refusal of one source expanded into several items, and it disproved the
        // assumption behind that refusal: a directory source is accepted as ONE item of kind
        // Directory, not as several file items, so it stops at the source-kind gate. Nothing at this
        // layer expands a source, which is why the batch refusal never had a way to be reached and
        // why what replaced it is a completeness rule rather than a limit.
        std::ofstream(source_directory / "a.txt") << "one";
        std::ofstream(source_directory / "b.txt") << "two";
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source_directory.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        const auto operation = ReviewedOperationFactory::Create(std::move(reviewed));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::UnsupportedSourceKind);
    }
}

TEST_CASE(PREFIX "requires concrete Native hosts and a direct access route", "[reviewed-operation-factory]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(source, "source");

    SECTION("non-native binding")
    {
        const auto native = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
        auto non_native = std::make_shared<ReviewedFactoryNonNativeHost>(native);
        auto probes = ReviewedNativeProbes(non_native);
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        const auto operation = ReviewedOperationFactory::Create(std::move(reviewed));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::UnsupportedProviderScope);
    }

    SECTION("routed access decision")
    {
        const auto native = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
        auto probes = ReviewedNativeProbes(native);
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        const auto operation = ReviewedOperationFactoryTestAccess::Create(
            std::move(reviewed),
            ReviewedStrongConditionalCommitTransaction(),
            {},
            [](std::string_view, int) { return false; });
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::UnsupportedAccessRoute);
        CHECK(operation.error().path == OperationPlanningPath{"local", source.native()});
    }
}

TEST_CASE(PREFIX "fails closed for invalid paths, cancellation, and consumed authority", "[reviewed-operation-factory]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(source, "source");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);

    SECTION("non-canonical plan path")
    {
        const auto source_with_dot = source.parent_path() / "." / source.filename();
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source_with_dot.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        const auto operation = ReviewedOperationFactory::Create(std::move(reviewed));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::InvalidPath);
    }

    SECTION("throwing cancellation callback")
    {
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        const auto operation = ReviewedOperationFactory::Create(std::move(reviewed), []() -> bool { throw 1; });
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::Cancelled);
    }

    SECTION("review authority is consumed once")
    {
        const auto destination = destination_directory / source.filename();
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        const auto first = ReviewedCreateWithStrongTestAuthority(std::move(reviewed));
        REQUIRE_FALSE(first);
        CHECK(first.error().code ==
              ReviewedOperationFactoryErrorCode::ConditionalCommitIntegrationUnavailable);
        CHECK_FALSE(std::filesystem::exists(destination));

        const auto second = ReviewedCreateWithStrongTestAuthority(std::move(reviewed));
        REQUIRE_FALSE(second);
        CHECK(second.error().code == ReviewedOperationFactoryErrorCode::MissingBindings);
    }
}

TEST_CASE(PREFIX "opens a raced source leaf without blocking and checks cancellation between EINTR retries",
          "[reviewed-operation-factory]")
{
    using namespace std::chrono_literals;

    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(source, "source");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);

    SECTION("regular file replaced by FIFO")
    {
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        REQUIRE(std::filesystem::remove(source));
        REQUIRE(mkfifo(source.c_str(), 0600) == 0);

        std::mutex watchdog_mutex;
        std::condition_variable watchdog_cv;
        bool factory_finished = false;
        std::thread watchdog{[&] {
            auto lock = std::unique_lock{watchdog_mutex};
            if( watchdog_cv.wait_for(lock, 1s, [&] { return factory_finished; }) )
                return;
            lock.unlock();
            const int fifo_fd = open(source.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if( fifo_fd >= 0 )
                (void)close(fifo_fd);
        }};

        const auto started_at = std::chrono::steady_clock::now();
        const auto operation = ReviewedCreateWithStrongTestAuthority(std::move(reviewed));
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        {
            const auto lock = std::lock_guard{watchdog_mutex};
            factory_finished = true;
        }
        watchdog_cv.notify_one();
        watchdog.join();

        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::StaleSource);
        CHECK(elapsed < 500ms);
    }

    SECTION("cancellation interrupts an EINTR retry")
    {
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        int open_attempts = 0;
        int observed_flags = 0;
        const auto operation = ReviewedOperationFactoryTestAccess::Create(
            std::move(reviewed),
            ReviewedStrongConditionalCommitTransaction(),
            [&] { return open_attempts != 0; },
            [](std::string_view, int) { return true; },
            [&](int, const char *, int _flags) {
                ++open_attempts;
                observed_flags = _flags;
                errno = EINTR;
                return -1;
            });

        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::Cancelled);
        CHECK(open_attempts == 1);
        CHECK((observed_flags & O_ACCMODE) == O_RDONLY);
        CHECK((observed_flags & (O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)) ==
              (O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    }
}

TEST_CASE(PREFIX "carries every source of a reviewed plan through one operation", "[reviewed-operation-factory]")
{
    TempTestDir temporary;
    const auto first = temporary.directory / "first.txt";
    const auto second = temporary.directory / "second.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(first, "first payload");
    ReviewedWriteFile(second, "the second payload is longer");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);
    auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", first.native()}, {"local", second.native()}},
                                                    destination_directory.native(),
                                                    OperationPlanDestinationKind::Directory),
                                   probes);
    REQUIRE(reviewed.AcceptedPlan().Plan().Sources().size() == 2);
    REQUIRE(reviewed.AcceptedPlan().Report().items.size() == 2);

    int abort_calls = 0;
    auto created = ReviewedOperationFactoryTestAccess::CreateExecutionProduct(
        std::move(reviewed), ReviewedStrongConditionalCommitTransaction(&abort_calls));
    REQUIRE(created);
    auto product = std::move(*created);
    auto &operation = ReviewedOperationFactoryTestAccess::Operation(product);
    auto &terminal_evidence = ReviewedOperationFactoryTestAccess::TerminalEvidence(product);

    operation->Start();
    REQUIRE(operation->Wait(std::chrono::seconds{5}));

    const auto evidence = terminal_evidence();
    REQUIRE(evidence);
    CHECK(evidence->state == OperationJournalState::Completed);
    REQUIRE(evidence->item_results.size() == 2);
    // Numbered in the plan's source space, which is the space the journal validates against, and
    // complete - a completed entry that skipped a source could not be recorded at all.
    CHECK(evidence->item_results[0].item_index == 0);
    CHECK(evidence->item_results[1].item_index == 1);
    CHECK(evidence->item_results[0].bytes == std::filesystem::file_size(first));
    CHECK(evidence->item_results[1].bytes == std::filesystem::file_size(second));
    for( const auto &result : evidence->item_results ) {
        CHECK(result.status == OperationJournalItemStatus::Succeeded);
        CHECK(result.destination_publication == OperationJournalPublicationState::Published);
    }
    CHECK(abort_calls == 0);
}

TEST_CASE(PREFIX "completes a real provider batch though its own first item grows the shared destination",
          "[reviewed-operation-factory]")
{
    // Executed against the real Native transaction rather than the test mint, which is the only way
    // this was ever visible: publishing the first item changes the destination directory's size and
    // both its content timestamps (and, confirmed empirically on APFS, its link_count - a directory's
    // link_count there advances for a regular-file child too, not only for subdirectories). Every item
    // shares one reviewed expectation of that directory, so without a second look the provider would
    // read the second item's turn as someone else having tampered with the destination.
    //
    // It is not tampering, and this is a fact about who published, provable without trusting a value
    // carried forward across items: the first item to name a given destination parent is reviewed
    // exactly as ever - nothing but this transaction should find it touched at all - and every later
    // item sharing that same parent is told, structurally, that content growth is expected there.
    // Identity and the whole ownership/permission surface (device, inode, birth time, mode, uid, gid,
    // BSD flags, ACL, extended attributes) still refuse on any change, for every item, always; only
    // size, the two content timestamps, and link_count may advance instead of matching exactly.
    TempTestDir temporary;
    const auto first = temporary.directory / "first.txt";
    const auto second = temporary.directory / "second.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(first, "first payload");
    ReviewedWriteFile(second, "second payload");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);
    auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", first.native()}, {"local", second.native()}},
                                                    destination_directory.native(),
                                                    OperationPlanDestinationKind::Directory),
                                   probes);
    auto created = ReviewedOperationFactoryTestAccess::CreateExecutionProduct(std::move(reviewed), {});
    REQUIRE(created);
    auto product = std::move(*created);
    auto &operation = ReviewedOperationFactoryTestAccess::Operation(product);
    auto &terminal_evidence = ReviewedOperationFactoryTestAccess::TerminalEvidence(product);

    operation->Start();
    REQUIRE(operation->Wait(std::chrono::seconds{5}));

    const auto evidence = terminal_evidence();
    REQUIRE(evidence);
    CHECK(evidence->state == OperationJournalState::Completed);
    REQUIRE(evidence->item_results.size() == 2);
    for( const auto &result : evidence->item_results ) {
        CHECK(result.status == OperationJournalItemStatus::Succeeded);
        CHECK(result.destination_publication == OperationJournalPublicationState::Published);
    }
    CHECK(ReviewedReadFile(destination_directory / "first.txt") == "first payload");
    CHECK(ReviewedReadFile(destination_directory / "second.txt") == "second payload");
}

TEST_CASE(PREFIX "still fails closed on a real provider when the shared destination was touched before review ran",
          "[reviewed-operation-factory]")
{
    // The companion case to the one above: growth is tolerated once it is the batch's own doing, from
    // its first item onward, but the FIRST item is still reviewed exactly as a lone copy always has
    // been - nothing should have touched the destination before this batch ever started publishing.
    // What tolerance does NOT cover - an unrelated write landing between two items, once the batch's
    // own growth has already made the directory a moving target - is a separate, accepted question,
    // tested directly at the provider level in NativeConditionalCopyTransaction_UT.cpp, where it can
    // be driven synchronously instead of raced against a real batch.
    TempTestDir temporary;
    const auto first = temporary.directory / "first.txt";
    const auto second = temporary.directory / "second.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(first, "first payload");
    ReviewedWriteFile(second, "second payload");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);
    auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", first.native()}, {"local", second.native()}},
                                                    destination_directory.native(),
                                                    OperationPlanDestinationKind::Directory),
                                   probes);
    // Written into the destination before the batch runs at all: this is what the review never saw.
    // Every item's own preparation revalidates the destination parent against that original snapshot
    // before any transaction begins, so this is caught before the batch ever starts, not during it.
    ReviewedWriteFile(destination_directory / "unrelated.txt", "not part of this review");

    const auto created = ReviewedOperationFactoryTestAccess::CreateExecutionProduct(std::move(reviewed), {});
    REQUIRE_FALSE(created);
    CHECK(created.error().code == ReviewedOperationFactoryErrorCode::StaleDestination);
    CHECK_FALSE(std::filesystem::exists(destination_directory / "first.txt"));
    CHECK_FALSE(std::filesystem::exists(destination_directory / "second.txt"));
}

TEST_CASE(PREFIX "carries a reviewed Move through one operation, and the source stops existing",
          "[reviewed-operation-factory]")
{
    // Against the real Native transaction, not the test mint - this is the one property that matters
    // for a Move and the test mint cannot exercise it: `renameatx_np` publishes the destination and
    // removes the source in the same indivisible call, so "completed" has to mean the source is gone,
    // not merely that the destination now exists.
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination = temporary.directory / "moved.txt";
    ReviewedWriteFile(source, "move payload");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);
    auto reviewed = ReviewedReview(ReviewedMovePlan(source.native(), destination.native()), probes);
    REQUIRE(reviewed.AcceptedPlan().Plan().Type() == OperationPlanType::Move);
    REQUIRE(reviewed.AcceptedPlan().Report().items.size() == 1);

    auto created = ReviewedOperationFactoryTestAccess::CreateExecutionProduct(std::move(reviewed), {});
    REQUIRE(created);
    auto product = std::move(*created);
    auto &operation = ReviewedOperationFactoryTestAccess::Operation(product);
    auto &terminal_evidence = ReviewedOperationFactoryTestAccess::TerminalEvidence(product);

    operation->Start();
    REQUIRE(operation->Wait(std::chrono::seconds{5}));

    const auto evidence = terminal_evidence();
    REQUIRE(evidence);
    CHECK(evidence->state == OperationJournalState::Completed);
    REQUIRE(evidence->item_results.size() == 1);
    CHECK(evidence->item_results[0].item_index == 0);
    CHECK(evidence->item_results[0].status == OperationJournalItemStatus::Succeeded);
    CHECK(evidence->item_results[0].destination_publication == OperationJournalPublicationState::Published);
    CHECK(ReviewedReadFile(destination) == "move payload");
    CHECK_FALSE(std::filesystem::exists(source));
}

TEST_CASE(PREFIX "completes a real provider Move batch though its own first item shrinks the shared source folder",
          "[reviewed-operation-factory]")
{
    // The Move-only mirror of the batch Copy test above, and the question it answers is the opposite
    // one: that test asks whether a shared *destination* directory growing from the batch's own
    // publications still passes review on later items. This one asks the same thing about a shared
    // *source* directory - `MoveTo` is most often several siblings moved out of one folder, and each
    // rename indivisibly removes an entry from that folder, which is a real, `source_parent`-visible
    // change under the same `renameatx_np` that publishes the destination. If the second item's own
    // `source_parent` expectation is not told about the batch's own first removal, the whole shape a
    // `MoveTo` batch mostly is - siblings out of one folder - would fail closed on every item after the
    // first, which only a real filesystem run below the test mint could ever have shown.
    TempTestDir temporary;
    const auto source_directory = temporary.directory / "source";
    std::filesystem::create_directory(source_directory);
    const auto first = source_directory / "first.txt";
    const auto second = source_directory / "second.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(first, "first payload");
    ReviewedWriteFile(second, "second payload");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);
    auto reviewed = ReviewedReview(
        ReviewedMoveBatchPlan({{"local", first.native()}, {"local", second.native()}}, destination_directory.native()),
        probes);
    REQUIRE(reviewed.AcceptedPlan().Plan().Type() == OperationPlanType::Move);
    REQUIRE(reviewed.AcceptedPlan().Report().items.size() == 2);

    auto created = ReviewedOperationFactoryTestAccess::CreateExecutionProduct(std::move(reviewed), {});
    REQUIRE(created);
    auto product = std::move(*created);
    auto &operation = ReviewedOperationFactoryTestAccess::Operation(product);
    auto &terminal_evidence = ReviewedOperationFactoryTestAccess::TerminalEvidence(product);

    operation->Start();
    REQUIRE(operation->Wait(std::chrono::seconds{5}));

    const auto evidence = terminal_evidence();
    REQUIRE(evidence);
    CHECK(evidence->state == OperationJournalState::Completed);
    REQUIRE(evidence->item_results.size() == 2);
    for( const auto &result : evidence->item_results ) {
        CHECK(result.status == OperationJournalItemStatus::Succeeded);
        CHECK(result.destination_publication == OperationJournalPublicationState::Published);
    }
    CHECK(ReviewedReadFile(destination_directory / "first.txt") == "first payload");
    CHECK(ReviewedReadFile(destination_directory / "second.txt") == "second payload");
    CHECK_FALSE(std::filesystem::exists(first));
    CHECK_FALSE(std::filesystem::exists(second));
}

TEST_CASE(PREFIX "mints a Move authority carrying the source's own folder as a claim",
          "[reviewed-operation-factory]")
{
    // Through the injected resolver rather than the real provider, so the claims the factory built can
    // be inspected directly instead of only inferred from the outcome on disk.
    TempTestDir temporary;
    const auto source_directory = temporary.directory / "source";
    std::filesystem::create_directory(source_directory);
    const auto source = source_directory / "source.txt";
    const auto destination = temporary.directory / "moved.txt";
    ReviewedWriteFile(source, "move payload");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);
    auto reviewed = ReviewedReview(ReviewedMovePlan(source.native(), destination.native()), probes);

    std::optional<nc::vfs::ProviderConditionalMoveReviewedClaims> observed_claims;
    const auto observing_resolver =
        [&](nc::vfs::ProviderConditionalMoveReviewedAuthority _authority,
           const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker)
        -> std::expected<std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction>,
                         nc::vfs::ProviderConditionalMoveTransactionBeginError> {
        observed_claims = _authority.Claims();
        return ReviewedStrongConditionalMoveCommitTransaction()(std::move(_authority), _cancel_checker);
    };

    auto created = ReviewedOperationFactoryTestAccess::CreateExecutionProduct(
        std::move(reviewed), {}, {}, {}, {}, observing_resolver);
    REQUIRE(created);
    REQUIRE(observed_claims);
    CHECK(observed_claims->plan_id == "reviewed-move");
    CHECK(observed_claims->source.absolute_path == source.native());
    CHECK(observed_claims->source_parent.absolute_path == source_directory.native());
    CHECK(observed_claims->destination.absolute_path == destination.native());

    auto product = std::move(*created);
    auto &operation = ReviewedOperationFactoryTestAccess::Operation(product);
    operation->Start();
    REQUIRE(operation->Wait(std::chrono::seconds{5}));
}

TEST_CASE(PREFIX "fails closed on a real provider when the source's own folder was touched after review ran",
          "[reviewed-operation-factory]")
{
    // The Move-only counterpart of the destination-parent staleness case above: the folder holding the
    // source is part of what a Move claim names (a rename acts on a name inside a directory), so it
    // gets the same fail-closed treatment the destination parent already has, and its own distinct
    // error code - `StaleSourceParent`, not `StaleSource` - because the more specific fact is that the
    // directory moved, not the file believed to be inside it.
    TempTestDir temporary;
    const auto source_directory = temporary.directory / "source";
    std::filesystem::create_directory(source_directory);
    const auto source = source_directory / "source.txt";
    const auto destination = temporary.directory / "moved.txt";
    ReviewedWriteFile(source, "move payload");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);
    auto reviewed = ReviewedReview(ReviewedMovePlan(source.native(), destination.native()), probes);
    // Written into the source's own folder after the review snapshot was taken: this is what the
    // review never saw, so the folder no longer matches what it was reviewed as.
    ReviewedWriteFile(source_directory / "unrelated.txt", "not part of this review");

    const auto created = ReviewedOperationFactoryTestAccess::CreateExecutionProduct(std::move(reviewed), {});
    REQUIRE_FALSE(created);
    CHECK(created.error().code == ReviewedOperationFactoryErrorCode::StaleSourceParent);
    CHECK(std::filesystem::exists(source));
    CHECK_FALSE(std::filesystem::exists(destination));
}

TEST_CASE(PREFIX "maps every Move transaction begin failure to its own factory error",
          "[reviewed-operation-factory]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination = temporary.directory / "moved.txt";
    ReviewedWriteFile(source, "move payload");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);

    const auto begin_error_maps_to = [&](nc::vfs::ProviderConditionalMoveTransactionBeginError _begin_error,
                                         ReviewedOperationFactoryErrorCode _expected) {
        auto reviewed = ReviewedReview(ReviewedMovePlan(source.native(), destination.native()), probes);
        const auto forced_resolver =
            [_begin_error](nc::vfs::ProviderConditionalMoveReviewedAuthority,
                          const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &)
            -> std::expected<std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction>,
                             nc::vfs::ProviderConditionalMoveTransactionBeginError> {
            return std::unexpected(_begin_error);
        };
        const auto created = ReviewedOperationFactoryTestAccess::CreateExecutionProduct(
            std::move(reviewed), {}, {}, {}, {}, forced_resolver);
        REQUIRE_FALSE(created);
        CHECK(created.error().code == _expected);
    };

    // Every value the provider's own Begin can answer, mapped independently of the factory's own
    // pre-check - a race between that pre-check and the provider's Begin is exactly what this switch
    // exists for, and the real-filesystem tests above already prove the pre-check catches the same
    // conditions when there is no race to force.
    begin_error_maps_to(nc::vfs::ProviderConditionalMoveTransactionBeginError::SourceStale,
                        ReviewedOperationFactoryErrorCode::StaleSource);
    begin_error_maps_to(nc::vfs::ProviderConditionalMoveTransactionBeginError::SourceParentStale,
                        ReviewedOperationFactoryErrorCode::StaleSourceParent);
    begin_error_maps_to(nc::vfs::ProviderConditionalMoveTransactionBeginError::DestinationParentStale,
                        ReviewedOperationFactoryErrorCode::StaleDestination);
    begin_error_maps_to(nc::vfs::ProviderConditionalMoveTransactionBeginError::DestinationExists,
                        ReviewedOperationFactoryErrorCode::StaleDestination);
    begin_error_maps_to(nc::vfs::ProviderConditionalMoveTransactionBeginError::Unsupported,
                        ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable);
    begin_error_maps_to(nc::vfs::ProviderConditionalMoveTransactionBeginError::InvalidRequest,
                        ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable);
    begin_error_maps_to(nc::vfs::ProviderConditionalMoveTransactionBeginError::ProviderFailure,
                        ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable);

    auto cancelled_reviewed = ReviewedReview(ReviewedMovePlan(source.native(), destination.native()), probes);
    const auto cancelling_resolver =
        [](nc::vfs::ProviderConditionalMoveReviewedAuthority,
          const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &)
        -> std::expected<std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction>,
                         nc::vfs::ProviderConditionalMoveTransactionBeginError> {
        return std::unexpected(nc::vfs::ProviderConditionalMoveTransactionBeginError::Cancelled);
    };
    const auto cancelled_created = ReviewedOperationFactoryTestAccess::CreateExecutionProduct(
        std::move(cancelled_reviewed), {}, {}, {}, {}, cancelling_resolver);
    REQUIRE_FALSE(cancelled_created);
    CHECK(cancelled_created.error().code == ReviewedOperationFactoryErrorCode::Cancelled);
}

TEST_CASE(PREFIX "cannot be handed a plan whose review stopped part-way", "[reviewed-operation-factory]")
{
    // The completeness refusal above it has no way to be reached, and this is why rather than a
    // guess: the only path that stops planning before every source is a cancelled probe, which
    // records a blocker, and a blocked preflight is never accepted. Pinning the reason keeps the next
    // reader from writing the same unreachable test.
    TempTestDir temporary;
    const auto first = temporary.directory / "first.txt";
    const auto second = temporary.directory / "second.txt";
    const auto destination_directory = temporary.directory / "destination";
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(first, "first payload");
    ReviewedWriteFile(second, "second payload");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbesWithAccess(
        host,
        [second_path = second.native()](const OperationPlanningPath &_path)
            -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            if( _path.absolute_path == second_path )
                return std::unexpected(OperationPlanningProbeError::Cancelled);
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });

    auto reviewed = ReviewedVFSOperationPreflight::Review(
        probes.Preflight(ReviewedCopyPlan({{"local", first.native()}, {"local", second.native()}},
                                          destination_directory.native(),
                                          OperationPlanDestinationKind::Directory)),
        VFSOperationPreflightReviewDecision::Approved);
    REQUIRE_FALSE(reviewed);
    CHECK(reviewed.error() == VFSOperationPreflightReviewError::Blocked);
}

TEST_CASE(PREFIX "rolls a prepared batch back instead of handing over transactions nobody will run",
          "[reviewed-operation-factory]")
{
    TempTestDir temporary;
    const auto source = temporary.directory / "source.txt";
    const auto destination_directory = temporary.directory / "destination";
    const auto destination = destination_directory / source.filename();
    std::filesystem::create_directory(destination_directory);
    ReviewedWriteFile(source, "reviewed payload");

    const auto host = std::shared_ptr<VFSHost>{TestEnv().vfs_native};
    auto probes = ReviewedNativeProbes(host);

    // Cancelling once the transaction exists is the first failure the factory can meet holding a
    // prepared item: beginning the transaction is the last thing preparing one does, so before this
    // there was never anything a rollback could be asked to undo.
    const auto cancel_once_the_transaction_exists = [](bool &_began,
                                                       int *_abort_calls,
                                                       nc::vfs::ProviderConditionalCopyPublicationState _publication) {
        return [&_began, _abort_calls, _publication](
                   nc::vfs::ProviderConditionalCopyReviewedAuthority _authority,
                   const nc::vfs::ProviderConditionalCopyTransaction::CancelChecker &_cancel_checker) {
            auto transaction = ReviewedStrongConditionalCommitTransaction(_abort_calls, _publication)(
                std::move(_authority), _cancel_checker);
            _began = true;
            return transaction;
        };
    };

    SECTION("a confirmed rollback leaves the reason for it standing")
    {
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        bool began = false;
        int abort_calls = 0;
        const auto operation = ReviewedOperationFactoryTestAccess::Create(
            std::move(reviewed),
            cancel_once_the_transaction_exists(
                began, &abort_calls, nc::vfs::ProviderConditionalCopyPublicationState::NotPublished),
            [&began] { return began; });

        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::Cancelled);
        CHECK_FALSE(operation.error().path);
        CHECK(abort_calls == 1);
        CHECK_FALSE(std::filesystem::exists(destination));
    }

    SECTION("a rollback that cannot confirm non-publication outranks the reason for it")
    {
        // Cancelled would tell the user the world was left as it was found. An abort that cannot say
        // NotPublished cannot support the second half of that, so the answer becomes the one about
        // authority - and it names the destination that may or may not exist.
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        bool began = false;
        int abort_calls = 0;
        const auto operation = ReviewedOperationFactoryTestAccess::Create(
            std::move(reviewed),
            cancel_once_the_transaction_exists(
                began, &abort_calls, nc::vfs::ProviderConditionalCopyPublicationState::Unknown),
            [&began] { return began; });

        REQUIRE_FALSE(operation);
        CHECK(operation.error().code ==
              ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable);
        REQUIRE(operation.error().path);
        CHECK(*operation.error().path == OperationPlanningPath{"local", destination.native()});
        CHECK(abort_calls == 1);
    }
}

} // namespace nc::ops

#undef PREFIX
