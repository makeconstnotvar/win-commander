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

VFSOperationPlanningProbes ReviewedNativeProbes(const std::shared_ptr<VFSHost> &_host)
{
    auto bindings = VFSOperationPlanningBindings::Create({{"local", _host}});
    REQUIRE(bindings);
    auto probes = VFSOperationPlanningProbes::Create(
        *bindings,
        [](const OperationPlanningPath &,
           OperationPlanningRequiredAccess,
           nc::vfs::Host &) -> OperationPlanningProbeResult<OperationPlanningAccessEvidence> {
            return OperationPlanningAccessEvidence{OperationPlanningAccessState::Granted};
        });
    REQUIRE(probes);
    return std::move(*probes);
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
        // Written to pin BatchUnsupported and it disproved the assumption behind it: a directory
        // source is accepted as ONE item of kind Directory, not as several file items, so it stops
        // at the source-kind gate and never reaches the batch one. Which means BatchUnsupported -
        // one source, several accepted items - appears unreachable at this layer today, and the gate
        // that actually stands between here and batching is the several-sources one below.
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

    SECTION("several sources")
    {
        // Distinct from a single source that expanded into several items: several sources need
        // several structural bindings checked, an expanded source needs only the per-item loop, and
        // reporting both the same way hid which of the two a caller had run into.
        auto reviewed = ReviewedReview(ReviewedCopyPlan({{"local", source.native()}, {"local", second_source.native()}},
                                                        destination_directory.native(),
                                                        OperationPlanDestinationKind::Directory),
                                       probes);
        const auto operation = ReviewedOperationFactory::Create(std::move(reviewed));
        REQUIRE_FALSE(operation);
        CHECK(operation.error().code == ReviewedOperationFactoryErrorCode::MultipleSourcesUnsupported);
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

} // namespace nc::ops

#undef PREFIX
