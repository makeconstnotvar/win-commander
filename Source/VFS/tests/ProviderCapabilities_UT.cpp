// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"
#include <VFS/ProviderCapabilities.h>
#include <VFS/Host.h>
#include <VFS/NetSFTP.h>
#include <VFS/PS.h>
#include "../source/ProviderCapabilitiesTesting.h"
#include "../source/XAttr/xattr.h"
#include <array>
#include <cerrno>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
#include <type_traits>

#define PREFIX "nc::vfs::ProviderCapabilities "

namespace ProviderCapabilitiesTest {

using namespace nc::vfs;
using nc::Error;

class ConditionalCopyTestHost final : public Host
{
public:
    explicit ConditionalCopyTestHost(const char *_tag) : Host{"", nullptr, _tag} {}
};

static ProviderConditionalCopyReviewedClaims ConditionalCopyClaims(const std::shared_ptr<Host> &_source,
                                                                    const std::shared_ptr<Host> &_destination)
{
    return ProviderConditionalCopyReviewedClaims{
        .plan_id = "provider-conditional-copy-test",
        .source_binding = ProviderConditionalCopyBinding{.provider_id = "source", .host = _source},
        .destination_binding = ProviderConditionalCopyBinding{.provider_id = "destination", .host = _destination},
        .source = ProviderConditionalCopyExistingExpectation{
            .absolute_path = "/source.txt",
            .kind = ProviderConditionalCopyExpectedKind::RegularFile,
            .device = 1,
            .inode = 2,
            .birth_time = {.seconds = 3, .nanoseconds = 4},
            .mode = 0100640,
            .byte_size = 5,
            .modification_time = {.seconds = 6, .nanoseconds = 7},
            .status_change_time = {.seconds = 8, .nanoseconds = 9},
        },
        .destination_parent = ProviderConditionalCopyExistingExpectation{
            .absolute_path = "/destination",
            .kind = ProviderConditionalCopyExpectedKind::Directory,
            .device = 10,
            .inode = 11,
            .birth_time = {.seconds = 12, .nanoseconds = 13},
            .mode = 0040750,
            .byte_size = 14,
            .modification_time = {.seconds = 15, .nanoseconds = 16},
            .status_change_time = {.seconds = 17, .nanoseconds = 18},
        },
        .destination = ProviderConditionalCopyMissingExpectation{
            .absolute_path = "/destination/source.txt",
        },
    };
}

static std::unique_ptr<ProviderConditionalCopyTransaction> ConditionalCopyTransaction(
    ProviderConditionalCopyReviewedClaims _claims,
    ProviderConditionalCopyCommitResult _result,
    int &_commit_calls,
    int &_abort_calls)
{
    auto destination = _claims.destination_binding.host;
    auto transaction = ProviderConditionalCopyTransactionTestAccess::Mint(
        *destination,
        ProviderConditionalCopyTransactionTestAccess::MakeAuthority(std::move(_claims)),
        [_result, &_commit_calls] {
            ++_commit_calls;
            return _result;
        },
        [&_abort_calls] {
            ++_abort_calls;
            return ProviderConditionalCopyPublicationState::NotPublished;
        });
    REQUIRE(transaction);
    REQUIRE(*transaction);
    return std::move(*transaction);
}

static ProviderConditionalCopyCommitResult ConditionalCopyUnknownResult() noexcept
{
    return ProviderConditionalCopyCommitResult{
        .publication = ProviderConditionalCopyPublicationState::Unknown,
        .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
        .system_error = EIO,
        .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
        .filesystem_sync_system_error = 0,
    };
}

static ProviderConditionalCopyCommitResult ConditionalCopyPublishedResult() noexcept
{
    return ProviderConditionalCopyCommitResult{
        .publication = ProviderConditionalCopyPublicationState::Published,
        .failure = ProviderConditionalCopyCommitFailure::None,
        .system_error = 0,
        .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
        .filesystem_sync_system_error = 0,
    };
}

static_assert(!std::is_default_constructible_v<ProviderConditionalCopyReviewedAuthority>);
static_assert(!std::is_constructible_v<ProviderConditionalCopyReviewedAuthority,
                                      ProviderConditionalCopyReviewedClaims>);
static_assert(!std::is_copy_constructible_v<ProviderConditionalCopyReviewedAuthority>);
static_assert(!std::is_copy_assignable_v<ProviderConditionalCopyReviewedAuthority>);
static_assert(std::is_move_constructible_v<ProviderConditionalCopyReviewedAuthority>);
static_assert(!std::is_move_assignable_v<ProviderConditionalCopyReviewedAuthority>);
static_assert(std::is_same_v<decltype(std::declval<const ProviderConditionalCopyReviewedAuthority &>().Claims()),
                             const ProviderConditionalCopyReviewedClaims &>);
static_assert(!std::is_copy_constructible_v<ProviderConditionalCopyTransaction>);
static_assert(!std::is_copy_assignable_v<ProviderConditionalCopyTransaction>);
static_assert(std::is_move_constructible_v<ProviderConditionalCopyTransaction>);
static_assert(std::is_move_assignable_v<ProviderConditionalCopyTransaction>);

TEST_CASE(PREFIX "uses conservative defaults for the dummy host")
{
    const ProviderCapabilities capabilities = ProviderCapabilitiesResolver::Resolve(*Host::DummyHost(), "/");

    CHECK_FALSE(capabilities.can_read);
    CHECK_FALSE(capabilities.can_write);
    CHECK_FALSE(capabilities.can_create_file);
    CHECK_FALSE(capabilities.can_create_folder);
    CHECK_FALSE(capabilities.can_create_symlink);
    CHECK_FALSE(capabilities.can_rename);
    CHECK_FALSE(capabilities.can_delete_permanently);
    CHECK_FALSE(capabilities.can_trash);
    CHECK_FALSE(capabilities.can_watch_changes);
    CHECK_FALSE(capabilities.can_generate_thumbnails);
    CHECK_FALSE(capabilities.can_resolve_symlink);
    CHECK_FALSE(capabilities.can_set_permissions);
    CHECK_FALSE(capabilities.can_set_owner_group);
    CHECK_FALSE(capabilities.can_set_times);
    CHECK_FALSE(capabilities.is_native);
    CHECK_FALSE(capabilities.is_immutable);
    CHECK(capabilities.is_case_sensitive);
    CHECK_FALSE(Host::DummyHost()->CaseSensitivityAtPath("/"));
    CHECK_FALSE(Host::DummyHost()->SemanticNamespaceIdentity());
}

TEST_CASE(PREFIX "keeps conditional Copy authority provider-minted and base hosts unsupported")
{
    auto source = std::make_shared<ConditionalCopyTestHost>("conditional-source");
    auto destination = std::make_shared<ConditionalCopyTestHost>("conditional-destination");

    const auto unsupported = destination->BeginConditionalCopyTransaction(
        ProviderConditionalCopyTransactionTestAccess::MakeAuthority(
            ConditionalCopyClaims(source, destination)));
    REQUIRE_FALSE(unsupported);
    CHECK(unsupported.error() == ProviderConditionalCopyTransactionBeginError::Unsupported);

    const auto cancelled = destination->BeginConditionalCopyTransaction(
        ProviderConditionalCopyTransactionTestAccess::MakeAuthority(
            ConditionalCopyClaims(source, destination)),
        [] { return true; });
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error() == ProviderConditionalCopyTransactionBeginError::Cancelled);

    auto wrong_provider = std::make_shared<ConditionalCopyTestHost>("wrong-provider");
    const auto invalid = ProviderConditionalCopyTransactionTestAccess::Mint(
        *wrong_provider,
        ProviderConditionalCopyTransactionTestAccess::MakeAuthority(
            ConditionalCopyClaims(source, destination)),
        [] { return ConditionalCopyPublishedResult(); },
        [] { return ProviderConditionalCopyPublicationState::NotPublished; });
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error() == ProviderConditionalCopyTransactionBeginError::InvalidRequest);

    const auto unsealed = ProviderConditionalCopyTransactionTestAccess::Mint(
        *destination,
        ProviderConditionalCopyTransactionTestAccess::MakeUnsealedAuthority(
            ConditionalCopyClaims(source, destination)),
        [] { return ConditionalCopyPublishedResult(); },
        [] { return ProviderConditionalCopyPublicationState::NotPublished; });
    REQUIRE_FALSE(unsealed);
    CHECK(unsealed.error() == ProviderConditionalCopyTransactionBeginError::InvalidRequest);
}

TEST_CASE(PREFIX "rejects structurally inconsistent conditional Copy requests")
{
    auto source = std::make_shared<ConditionalCopyTestHost>("conditional-source");
    auto destination = std::make_shared<ConditionalCopyTestHost>("conditional-destination");
    const auto valid_claims = ConditionalCopyClaims(source, destination);
    const auto mint = [&](ProviderConditionalCopyReviewedClaims _claims) {
        return ProviderConditionalCopyTransactionTestAccess::Mint(
            *destination,
            ProviderConditionalCopyTransactionTestAccess::MakeAuthority(std::move(_claims)),
            [] { return ConditionalCopyPublishedResult(); },
            [] { return ProviderConditionalCopyPublicationState::NotPublished; });
    };
    const auto check_invalid = [&](ProviderConditionalCopyReviewedClaims _claims) {
        const auto transaction = mint(std::move(_claims));
        REQUIRE_FALSE(transaction);
        CHECK(transaction.error() == ProviderConditionalCopyTransactionBeginError::InvalidRequest);
    };

    SECTION("accepts one provider identity and a root destination parent")
    {
        auto claims = valid_claims;
        claims.source_binding = claims.destination_binding;
        claims.destination_parent.absolute_path = "/";
        claims.destination.absolute_path = "/copied.txt";
        const auto transaction = mint(std::move(claims));
        REQUIRE(transaction);
        REQUIRE(*transaction);
    }

    SECTION("paths are canonical")
    {
        auto claims = valid_claims;
        claims.source.absolute_path = "/folder/../source.txt";
        check_invalid(std::move(claims));

        claims = valid_claims;
        claims.destination.absolute_path = "/destination//source.txt";
        check_invalid(std::move(claims));

        claims = valid_claims;
        claims.source.absolute_path = std::string{"/source\0hidden", 14};
        check_invalid(std::move(claims));
    }

    SECTION("destination is an exact child of its parent")
    {
        auto claims = valid_claims;
        claims.destination.absolute_path = "/other/source.txt";
        check_invalid(std::move(claims));

        claims = valid_claims;
        claims.destination.absolute_path = "/destination/nested/source.txt";
        check_invalid(std::move(claims));
    }

    SECTION("declared kinds agree with mode type bits")
    {
        auto claims = valid_claims;
        claims.source.mode = static_cast<uint16_t>(S_IFDIR | 0640);
        check_invalid(std::move(claims));

        claims = valid_claims;
        claims.source.kind = ProviderConditionalCopyExpectedKind::Directory;
        check_invalid(std::move(claims));

        claims = valid_claims;
        claims.destination_parent.mode = static_cast<uint16_t>(S_IFREG | 0750);
        check_invalid(std::move(claims));
    }

    SECTION("timestamp nanoseconds are normalized")
    {
        auto claims = valid_claims;
        claims.source.birth_time.nanoseconds = 1'000'000'000;
        check_invalid(std::move(claims));

        claims = valid_claims;
        claims.destination_parent.modification_time.nanoseconds = -1;
        check_invalid(std::move(claims));
    }

    SECTION("provider ids and host identities remain one-to-one")
    {
        auto claims = valid_claims;
        claims.source_binding.provider_id = claims.destination_binding.provider_id;
        check_invalid(std::move(claims));

        claims = valid_claims;
        claims.source_binding.host = claims.destination_binding.host;
        check_invalid(std::move(claims));
    }

    SECTION("one provider cannot bind contradictory object roles to one path")
    {
        auto claims = valid_claims;
        claims.source_binding = claims.destination_binding;
        claims.source.absolute_path = claims.destination.absolute_path;
        check_invalid(std::move(claims));

        claims = valid_claims;
        claims.source_binding = claims.destination_binding;
        claims.source.absolute_path = claims.destination_parent.absolute_path;
        check_invalid(std::move(claims));
    }
}

TEST_CASE(PREFIX "consumes conditional Copy transactions exactly once and fails closed")
{
    auto source = std::make_shared<ConditionalCopyTestHost>("conditional-source");
    auto destination = std::make_shared<ConditionalCopyTestHost>("conditional-destination");
    const auto claims = ConditionalCopyClaims(source, destination);

    SECTION("Published is terminal and duplicate use is rejected")
    {
        int commits = 0;
        int aborts = 0;
        const auto published = ConditionalCopyPublishedResult();
        auto transaction = ConditionalCopyTransaction(claims, published, commits, aborts);
        REQUIRE(transaction->IsPending());
        CHECK(transaction->Commit() == published);
        CHECK_FALSE(transaction->IsPending());
        CHECK(transaction->Commit() == published);
        CHECK(transaction->Abort() == published);
        CHECK(commits == 1);
        CHECK(aborts == 0);
    }

    SECTION("stale expectation is NotPublished and releases provider state")
    {
        int commits = 0;
        int aborts = 0;
        auto transaction = ConditionalCopyTransaction(
            claims,
            {.publication = ProviderConditionalCopyPublicationState::NotPublished,
             .failure = ProviderConditionalCopyCommitFailure::SourceStale,
             .system_error = ESTALE},
            commits,
            aborts);
        CHECK((transaction->Commit() ==
              ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                                  ProviderConditionalCopyCommitFailure::SourceStale,
                                                  ESTALE}));
        CHECK(commits == 1);
        CHECK(aborts == 1);
    }

    SECTION("provider ambiguity is Unknown and is never followed by abort")
    {
        int commits = 0;
        int aborts = 0;
        auto transaction = ConditionalCopyTransaction(
            claims,
            {.publication = ProviderConditionalCopyPublicationState::Unknown,
             .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
             .system_error = EIO},
            commits,
            aborts);
        CHECK(transaction->Commit() == ConditionalCopyUnknownResult());
        CHECK(transaction->Abort() == ConditionalCopyUnknownResult());
        CHECK(commits == 1);
        CHECK(aborts == 0);
    }

    SECTION("abort and cancellation are terminal NotPublished outcomes")
    {
        int commits = 0;
        int aborts = 0;
        auto transaction = ConditionalCopyTransaction(
            claims, ConditionalCopyPublishedResult(), commits, aborts);
        CHECK((transaction->Abort() ==
              ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                                  ProviderConditionalCopyCommitFailure::Aborted}));
        CHECK((transaction->Commit() ==
              ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                                  ProviderConditionalCopyCommitFailure::Aborted}));
        CHECK(commits == 0);
        CHECK(aborts == 1);

        commits = 0;
        aborts = 0;
        transaction = ConditionalCopyTransaction(
            claims, ConditionalCopyPublishedResult(), commits, aborts);
        CHECK((transaction->Commit([] { return true; }) ==
              ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                                  ProviderConditionalCopyCommitFailure::Cancelled}));
        CHECK(commits == 0);
        CHECK(aborts == 1);
    }

    SECTION("dropping an unused transaction aborts it")
    {
        int commits = 0;
        int aborts = 0;
        {
            auto transaction = ConditionalCopyTransaction(
                claims, ConditionalCopyPublishedResult(), commits, aborts);
            CHECK(transaction->IsPending());
        }
        CHECK(commits == 0);
        CHECK(aborts == 1);
    }
}

TEST_CASE(PREFIX "preserves every valid conditional Copy result and sync evidence exactly")
{
    auto source = std::make_shared<ConditionalCopyTestHost>("conditional-source");
    auto destination = std::make_shared<ConditionalCopyTestHost>("conditional-destination");
    const auto claims = ConditionalCopyClaims(source, destination);

    const std::array valid_results{
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::Aborted,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::Cancelled,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::SourceStale,
            .system_error = ESTALE,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::DestinationParentStale,
            .system_error = ESTALE,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::DestinationExists,
            .system_error = EEXIST,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
            .system_error = EIO,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Unknown,
            .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
            .system_error = EIO,
        },
        ConditionalCopyPublishedResult(),
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::MetadataFailed,
            .system_error = EPERM,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::MetadataFailed,
            .system_error = EPERM,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Failed,
            .filesystem_sync_system_error = EIO,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Failed,
            .filesystem_sync_system_error = ENOSPC,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::FileSystemSyncFailed,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Failed,
            .filesystem_sync_system_error = EIO,
        },
    };

    for( const auto &expected : valid_results ) {
        CAPTURE(expected.failure, expected.system_error, expected.filesystem_sync_status,
                expected.filesystem_sync_system_error);
        int commits = 0;
        int aborts = 0;
        auto transaction = ConditionalCopyTransaction(claims, expected, commits, aborts);

        CHECK(transaction->Commit() == expected);
        CHECK(transaction->Commit() == expected);
        CHECK(transaction->Abort() == expected);
        CHECK_FALSE(transaction->IsPending());
        CHECK(commits == 1);
        CHECK(aborts ==
              (expected.publication == ProviderConditionalCopyPublicationState::NotPublished ? 1 : 0));
    }
}

TEST_CASE(PREFIX "degrades inconsistent conditional Copy results to one cached Unknown")
{
    auto source = std::make_shared<ConditionalCopyTestHost>("conditional-source");
    auto destination = std::make_shared<ConditionalCopyTestHost>("conditional-destination");
    const auto claims = ConditionalCopyClaims(source, destination);
    const auto unknown = ConditionalCopyUnknownResult();

    const std::array inconsistent_results{
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::None,
            .system_error = 0,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::None,
            .system_error = 0,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Failed,
            .filesystem_sync_system_error = EIO,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::None,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::MetadataFailed,
            .system_error = 0,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::MetadataFailed,
            .system_error = EPERM,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::FileSystemSyncFailed,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::FileSystemSyncFailed,
            .system_error = 0,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Failed,
            .filesystem_sync_system_error = EIO,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::FileSystemSyncFailed,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Failed,
            .filesystem_sync_system_error = EPERM,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::Cancelled,
            .system_error = 0,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::None,
            .system_error = 0,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::Aborted,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::FileSystemSyncFailed,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::MetadataFailed,
            .system_error = EPERM,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
            .system_error = 0,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::SourceStale,
            .system_error = 0,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::SourceStale,
            .system_error = EEXIST,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::DestinationParentStale,
            .system_error = 0,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::DestinationParentStale,
            .system_error = EEXIST,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::DestinationExists,
            .system_error = 0,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::DestinationExists,
            .system_error = ESTALE,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Unknown,
            .failure = ProviderConditionalCopyCommitFailure::MetadataFailed,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Unknown,
            .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
            .system_error = 0,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Unknown,
            .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
            .filesystem_sync_system_error = EIO,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
            .system_error = 0,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
            .system_error = -1,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Failed,
            .filesystem_sync_system_error = 0,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::Published,
            .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
            .system_error = EIO,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Failed,
            .filesystem_sync_system_error = -1,
        },
        ProviderConditionalCopyCommitResult{
            .publication = ProviderConditionalCopyPublicationState::NotPublished,
            .failure = ProviderConditionalCopyCommitFailure::SourceStale,
            .system_error = 0,
            .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::Confirmed,
            .filesystem_sync_system_error = 0,
        },
    };

    for( const auto &inconsistent : inconsistent_results ) {
        CAPTURE(inconsistent.publication, inconsistent.failure, inconsistent.system_error,
                inconsistent.filesystem_sync_status, inconsistent.filesystem_sync_system_error);
        int commits = 0;
        int aborts = 0;
        auto transaction = ConditionalCopyTransaction(claims, inconsistent, commits, aborts);

        CHECK(transaction->Commit() == unknown);
        CHECK(transaction->Commit() == unknown);
        CHECK(transaction->Abort() == unknown);
        CHECK_FALSE(transaction->IsPending());
        CHECK(commits == 1);
        CHECK(aborts == 0);
    }
}

TEST_CASE(PREFIX "reports concurrent and moved-from transaction state conservatively")
{
    auto source = std::make_shared<ConditionalCopyTestHost>("conditional-source");
    auto destination = std::make_shared<ConditionalCopyTestHost>("conditional-destination");
    const auto claims = ConditionalCopyClaims(source, destination);

    SECTION("commit in progress is Unknown and terminal publication is cached")
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool commit_entered = false;
        bool release_commit = false;
        int aborts = 0;
        auto transaction_result = ProviderConditionalCopyTransactionTestAccess::Mint(
            *destination,
            ProviderConditionalCopyTransactionTestAccess::MakeAuthority(claims),
            [&] {
                auto lock = std::unique_lock{mutex};
                commit_entered = true;
                condition.notify_all();
                condition.wait(lock, [&] { return release_commit; });
                return ConditionalCopyPublishedResult();
            },
            [&] {
                ++aborts;
                return ProviderConditionalCopyPublicationState::NotPublished;
            });
        REQUIRE(transaction_result);
        REQUIRE(*transaction_result);
        auto transaction = std::move(*transaction_result);
        auto committed = ProviderConditionalCopyCommitResult{};
        std::thread commit_thread{[&] { committed = transaction->Commit(); }};
        {
            auto lock = std::unique_lock{mutex};
            condition.wait(lock, [&] { return commit_entered; });
        }

        CHECK(transaction->Commit() == ConditionalCopyUnknownResult());
        CHECK(transaction->Abort() == ConditionalCopyUnknownResult());
        {
            const auto lock = std::lock_guard{mutex};
            release_commit = true;
        }
        condition.notify_all();
        commit_thread.join();

        CHECK(committed == ConditionalCopyPublishedResult());
        CHECK((transaction->Commit() == committed));
        CHECK(aborts == 0);
    }

    SECTION("moved-from authority is Unknown")
    {
        int commits = 0;
        int aborts = 0;
        auto transaction = ConditionalCopyTransaction(
            claims, ConditionalCopyPublishedResult(), commits, aborts);
        ProviderConditionalCopyTransaction moved{std::move(*transaction)};

        CHECK(transaction->Commit() == ConditionalCopyUnknownResult());
        CHECK(transaction->Abort() == ConditionalCopyUnknownResult());
        CHECK((moved.Abort() ==
              ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                                  ProviderConditionalCopyCommitFailure::Aborted}));
        CHECK(commits == 0);
        CHECK(aborts == 1);
    }
}

TEST_CASE(PREFIX "requires provider confirmation before reporting an aborted transaction as NotPublished")
{
    auto source = std::make_shared<ConditionalCopyTestHost>("conditional-source");
    auto destination = std::make_shared<ConditionalCopyTestHost>("conditional-destination");
    const auto claims = ConditionalCopyClaims(source, destination);
    const auto unknown = ConditionalCopyUnknownResult();
    const auto mint = [&](ProviderConditionalCopyTransaction::CommitHandler _commit,
                          ProviderConditionalCopyTransaction::AbortHandler _abort) {
        auto transaction = ProviderConditionalCopyTransactionTestAccess::Mint(
            *destination,
            ProviderConditionalCopyTransactionTestAccess::MakeAuthority(claims),
            std::move(_commit),
            std::move(_abort));
        REQUIRE(transaction);
        REQUIRE(*transaction);
        return std::move(*transaction);
    };

    SECTION("explicit abort throws")
    {
        auto transaction = mint(
            [] { return ConditionalCopyPublishedResult(); },
            []() -> ProviderConditionalCopyPublicationState { throw 1; });
        CHECK(transaction->Abort() == unknown);
        CHECK(transaction->Abort() == unknown);
    }

    SECTION("cancellation abort reports Published")
    {
        auto transaction = mint(
            [] { return ConditionalCopyPublishedResult(); },
            [] { return ProviderConditionalCopyPublicationState::Published; });
        CHECK(transaction->Commit([] { return true; }) == unknown);
        CHECK(transaction->Commit() == unknown);
    }

    SECTION("NotPublished commit cannot outrun an unconfirmed abort")
    {
        auto transaction = mint(
            [] {
                return ProviderConditionalCopyCommitResult{ProviderConditionalCopyPublicationState::NotPublished,
                                                           ProviderConditionalCopyCommitFailure::SourceStale,
                                                           ESTALE};
            },
            [] { return ProviderConditionalCopyPublicationState::Unknown; });
        CHECK(transaction->Commit() == unknown);
        CHECK(transaction->Commit() == unknown);
    }
}

TEST_CASE(PREFIX "classifies POSIX and SFTP errors for planning")
{
    Host &host = *Host::DummyHost();
    CHECK(host.ClassifyError(Error{Error::POSIX, ENOENT}) == HostErrorKind::Missing);
    CHECK(host.ClassifyError(Error{Error::POSIX, EACCES}) == HostErrorKind::PermissionDenied);
    CHECK(host.ClassifyError(Error{Error::POSIX, ENOTSUP}) == HostErrorKind::Unsupported);
    CHECK(host.ClassifyError(Error{Error::POSIX, ECANCELED}) == HostErrorKind::Cancelled);
    CHECK(host.ClassifyError(Error{Error::POSIX, ENETUNREACH}) == HostErrorKind::Unavailable);
    CHECK(host.ClassifyError(Error{Error::POSIX, EIO}) == HostErrorKind::Other);

    using nc::vfs::sftp::ErrorDomain;
    using nc::vfs::sftp::Errors;
    CHECK(SFTPHost::ClassifySFTPError(Error{ErrorDomain, Errors::fx_no_such_file}) ==
          HostErrorKind::Missing);
    CHECK(SFTPHost::ClassifySFTPError(Error{ErrorDomain, Errors::fx_no_such_path}) ==
          HostErrorKind::Missing);
    CHECK(SFTPHost::ClassifySFTPError(Error{ErrorDomain, Errors::fx_permission_denied}) ==
          HostErrorKind::PermissionDenied);
    CHECK(SFTPHost::ClassifySFTPError(Error{ErrorDomain, Errors::fx_op_unsupported}) ==
          HostErrorKind::Unsupported);
    CHECK(SFTPHost::ClassifySFTPError(Error{ErrorDomain, Errors::fx_connection_lost}) ==
          HostErrorKind::Unavailable);
    CHECK(SFTPHost::ClassifySFTPError(Error{ErrorDomain, Errors::socket_send}) ==
          HostErrorKind::Unavailable);
    CHECK(SFTPHost::ClassifySFTPError(Error{ErrorDomain, Errors::socket_recv}) ==
          HostErrorKind::Unavailable);
    CHECK(SFTPHost::ClassifySFTPError(Error{ErrorDomain, Errors::socket_disconnect}) ==
          HostErrorKind::Unavailable);
    CHECK(SFTPHost::ClassifySFTPError(Error{ErrorDomain, Errors::timeout}) ==
          HostErrorKind::Unavailable);
    CHECK(SFTPHost::ClassifySFTPError(Error{ErrorDomain, Errors::socket_timeout}) ==
          HostErrorKind::Unavailable);
    CHECK(SFTPHost::ClassifySFTPError(Error{ErrorDomain, Errors::fx_failure}) == HostErrorKind::Other);
}

TEST_CASE(PREFIX "keeps observation independent from writability")
{
    struct ReadOnlyObservableHost final : Host {
        ReadOnlyObservableHost() : Host("", nullptr, "read-only-observable")
        {
            AddFeatures(HostFeatures::Read | HostFeatures::ObserveDirectoryChanges);
        }

        bool IsDirectoryChangeObservationAvailable(std::string_view) override { return true; }
    };

    ReadOnlyObservableHost host;
    const ProviderCapabilities capabilities = ProviderCapabilitiesResolver::Resolve(host, "/");

    CHECK(capabilities.can_read);
    CHECK_FALSE(capabilities.can_write);
    CHECK(capabilities.can_watch_changes);
}

TEST_CASE(PREFIX "does not infer operations from writability")
{
    struct UndeclaredWritableHost final : Host {
        UndeclaredWritableHost() : Host("", nullptr, "undeclared-writable") {}
        bool IsWritable() const override { return true; }
    };

    UndeclaredWritableHost host;
    const ProviderCapabilities capabilities = ProviderCapabilitiesResolver::Resolve(host, "/");

    CHECK_FALSE(capabilities.can_read);
    CHECK_FALSE(capabilities.can_write);
    CHECK_FALSE(capabilities.can_create_file);
    CHECK_FALSE(capabilities.can_rename);
    CHECK_FALSE(capabilities.can_delete_permanently);
}

TEST_CASE(PREFIX "suppresses mutations for immutable providers")
{
    struct ImmutableHost final : Host {
        ImmutableHost() : Host("", nullptr, "immutable")
        {
            AddFeatures(HostFeatures::Read | HostFeatures::CreateFile | HostFeatures::CreateDirectory |
                        HostFeatures::Rename | HostFeatures::Unlink | HostFeatures::RemoveDirectory |
                        HostFeatures::Trash | HostFeatures::ReadSymlink | HostFeatures::SetPermissions |
                        HostFeatures::SetOwnership | HostFeatures::SetTimes | HostFeatures::ObserveDirectoryChanges);
        }

        bool IsWritable() const override { return true; }
        bool IsImmutableFS() const noexcept override { return true; }
        bool ShouldProduceThumbnails() const override { return true; }
        bool IsDirectoryChangeObservationAvailable(std::string_view) override { return true; }
    };

    ImmutableHost host;
    const ProviderCapabilities capabilities = ProviderCapabilitiesResolver::Resolve(host, "/");

    CHECK(capabilities.can_read);
    CHECK(capabilities.can_generate_thumbnails);
    CHECK(capabilities.can_resolve_symlink);
    CHECK_FALSE(capabilities.can_write);
    CHECK_FALSE(capabilities.can_create_file);
    CHECK_FALSE(capabilities.can_create_folder);
    CHECK_FALSE(capabilities.can_create_symlink);
    CHECK_FALSE(capabilities.can_rename);
    CHECK_FALSE(capabilities.can_delete_permanently);
    CHECK_FALSE(capabilities.can_trash);
    CHECK_FALSE(capabilities.can_watch_changes);
    CHECK_FALSE(capabilities.can_set_permissions);
    CHECK_FALSE(capabilities.can_set_owner_group);
    CHECK_FALSE(capabilities.can_set_times);
    CHECK(capabilities.is_immutable);
}

TEST_CASE(PREFIX "resolves native provider capabilities")
{
    const TestDir test_dir;
    Host &host = *TestEnv().vfs_native;
    const ProviderCapabilities capabilities =
        ProviderCapabilitiesResolver::Resolve(host, test_dir.directory.native());

    CHECK(capabilities.can_read);
    CHECK(capabilities.can_write);
    CHECK(capabilities.can_create_file);
    CHECK(capabilities.can_create_folder);
    CHECK(capabilities.can_create_symlink);
    CHECK(capabilities.can_rename);
    CHECK(capabilities.can_delete_permanently);
    CHECK(capabilities.can_trash);
    CHECK(capabilities.can_watch_changes);
    CHECK(capabilities.can_generate_thumbnails);
    CHECK(capabilities.can_resolve_symlink);
    CHECK(capabilities.can_set_permissions);
    CHECK(capabilities.can_set_owner_group);
    CHECK(capabilities.can_set_times);
    CHECK(capabilities.is_native);
    CHECK_FALSE(capabilities.is_immutable);
    CHECK(capabilities.is_case_sensitive == host.IsCaseSensitiveAtPath(test_dir.directory.native()));
    REQUIRE(host.CaseSensitivityAtPath(test_dir.directory.native()));
    CHECK(*host.CaseSensitivityAtPath(test_dir.directory.native()) == capabilities.is_case_sensitive);
    REQUIRE(host.SemanticNamespaceIdentity());
    CHECK(*host.SemanticNamespaceIdentity() == "native");
}

TEST_CASE(PREFIX "keeps path-dependent observation disabled without a path")
{
    Host &host = *TestEnv().vfs_native;
    const ProviderCapabilities capabilities = ProviderCapabilitiesResolver::Resolve(host);

    CHECK(capabilities.can_read);
    CHECK(capabilities.can_write);
    CHECK_FALSE(capabilities.can_watch_changes);
}

TEST_CASE(PREFIX "keeps process control outside generic file mutations")
{
    auto host = std::make_shared<PSHost>();
    const ProviderCapabilities capabilities = ProviderCapabilitiesResolver::Resolve(*host, "/");

    CHECK(host->Features() == PSHost::DeclaredFeatures);
    CHECK(capabilities.can_read);
    CHECK(capabilities.can_watch_changes);
    CHECK_FALSE(capabilities.can_write);
    CHECK_FALSE(capabilities.can_create_file);
    CHECK_FALSE(capabilities.can_create_folder);
    CHECK_FALSE(capabilities.can_rename);
    CHECK_FALSE(capabilities.can_delete_permanently);
    CHECK_FALSE(capabilities.can_trash);
    CHECK((host->Features() & HostFeatures::Unlink) == 0);
}

TEST_CASE(PREFIX "declares xattr entry operations without directory deletion")
{
    const TestDir test_dir;
    const std::filesystem::path backing_file = test_dir.directory / "capabilities";
    {
        std::ofstream file(backing_file);
        REQUIRE(file.good());
    }

    auto host = std::make_shared<XAttrHost>(backing_file.native(), TestEnv().vfs_native);
    const ProviderCapabilities capabilities = ProviderCapabilitiesResolver::Resolve(*host, "/");

    CHECK(host->Features() == XAttrHost::DeclaredFeatures);
    CHECK(capabilities.can_read);
    CHECK(capabilities.can_write);
    CHECK(capabilities.can_create_file);
    CHECK_FALSE(capabilities.can_create_folder);
    CHECK(capabilities.can_rename);
    CHECK_FALSE(capabilities.can_delete_permanently);
    CHECK_FALSE(capabilities.can_trash);
    CHECK((host->Features() & HostFeatures::Unlink) != 0);
    CHECK((host->Features() & HostFeatures::RemoveDirectory) == 0);
}

} // namespace ProviderCapabilitiesTest

#undef PREFIX
