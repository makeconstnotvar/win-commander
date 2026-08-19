// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ReviewedOperationFactory.h"
#include "ProviderConditionalCopyOperation.h"

#include <RoutedIO/RoutedIO.h>
#include <VFS/Native.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <ranges>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace nc::ops {
namespace {

ReviewedOperationFactoryError ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode _code,
                                                     std::optional<OperationPlanningPath> _path = std::nullopt,
                                                     std::optional<Error> _cause = std::nullopt)
{
    return ReviewedOperationFactoryError{_code, std::move(_path), std::move(_cause)};
}

class ReviewedFactoryOwnedFD final
{
public:
    ReviewedFactoryOwnedFD() noexcept = default;
    explicit ReviewedFactoryOwnedFD(int _fd) noexcept : m_FD{_fd} {}
    ReviewedFactoryOwnedFD(const ReviewedFactoryOwnedFD &) = delete;
    ReviewedFactoryOwnedFD &operator=(const ReviewedFactoryOwnedFD &) = delete;
    ReviewedFactoryOwnedFD(ReviewedFactoryOwnedFD &&_other) noexcept : m_FD{_other.Release()} {}
    ReviewedFactoryOwnedFD &operator=(ReviewedFactoryOwnedFD &&_other) noexcept
    {
        if( this != &_other ) {
            Reset();
            m_FD = _other.Release();
        }
        return *this;
    }
    ~ReviewedFactoryOwnedFD() { Reset(); }

    [[nodiscard]] int Get() const noexcept { return m_FD; }
    [[nodiscard]] int Release() noexcept { return std::exchange(m_FD, -1); }

private:
    void Reset() noexcept
    {
        if( m_FD >= 0 )
            close(m_FD);
        m_FD = -1;
    }

    int m_FD{-1};
};

std::string ReviewedFactoryTrimTrailingSlashes(std::string_view _path)
{
    auto end = _path.size();
    while( end > 1 && _path[end - 1] == '/' )
        --end;
    return std::string{_path.substr(0, end)};
}

std::string ReviewedFactoryNormalizeAbsolutePath(std::string_view _path)
{
    std::vector<std::string_view> components;
    size_t position = 0;
    while( position < _path.size() ) {
        while( position < _path.size() && _path[position] == '/' )
            ++position;
        const auto end = _path.find('/', position);
        const auto component =
            _path.substr(position, end == std::string_view::npos ? _path.size() - position : end - position);
        position = end == std::string_view::npos ? _path.size() : end;
        if( component.empty() || component == "." )
            continue;
        if( component == ".." ) {
            if( !components.empty() )
                components.pop_back();
            continue;
        }
        components.emplace_back(component);
    }

    std::string normalized{"/"};
    for( size_t index = 0; index < components.size(); ++index ) {
        if( index != 0 )
            normalized.push_back('/');
        normalized.append(components[index]);
    }
    return normalized;
}

std::optional<std::string> ReviewedFactoryCanonicalPlanPath(std::string_view _path)
{
    if( _path.empty() || _path.front() != '/' )
        return std::nullopt;
    auto trimmed = ReviewedFactoryTrimTrailingSlashes(_path);
    const auto normalized = ReviewedFactoryNormalizeAbsolutePath(trimmed);
    if( normalized != trimmed )
        return std::nullopt;
    return normalized;
}

std::optional<std::pair<std::string, std::string>> ReviewedFactoryParentAndName(std::string_view _path)
{
    const auto canonical = ReviewedFactoryCanonicalPlanPath(_path);
    if( !canonical || *canonical == "/" )
        return std::nullopt;
    const auto separator = canonical->rfind('/');
    auto parent = separator == 0 ? std::string{"/"} : canonical->substr(0, separator);
    auto name = canonical->substr(separator + 1);
    if( name.empty() || name == "." || name == ".." )
        return std::nullopt;
    return std::pair{std::move(parent), std::move(name)};
}

std::string ReviewedFactoryJoinPath(std::string_view _directory, std::string_view _name)
{
    auto result = ReviewedFactoryTrimTrailingSlashes(_directory);
    if( result != "/" )
        result.push_back('/');
    result.append(_name);
    return result;
}

std::optional<std::string> ReviewedFactoryResolveExistingPath(std::string_view _path)
{
    const std::string owned_path{_path};
    char *resolved = nullptr;
    do {
        resolved = realpath(owned_path.c_str(), nullptr);
    } while( resolved == nullptr && errno == EINTR );
    if( resolved == nullptr )
        return std::nullopt;
    std::string result{resolved};
    std::free(resolved);
    return result;
}

int ReviewedFactoryOpenRetry(const char *_path, int _flags) noexcept
{
    int result;
    do {
        result = open(_path, _flags);
    } while( result < 0 && errno == EINTR );
    return result;
}

int ReviewedFactoryOpenAtRetry(int _directory_fd, const char *_name, int _flags) noexcept
{
    int result;
    do {
        result = openat(_directory_fd, _name, _flags);
    } while( result < 0 && errno == EINTR );
    return result;
}

int ReviewedFactoryFStatRetry(int _fd, struct stat *_stat) noexcept
{
    int result;
    do {
        result = fstat(_fd, _stat);
    } while( result < 0 && errno == EINTR );
    return result;
}

int ReviewedFactoryFStatAtRetry(int _directory_fd, const char *_name, struct stat *_stat, int _flags) noexcept
{
    int result;
    do {
        result = fstatat(_directory_fd, _name, _stat, _flags);
    } while( result < 0 && errno == EINTR );
    return result;
}

std::expected<ReviewedFactoryOwnedFD, int> ReviewedFactoryOpenCanonicalDirectory(std::string_view _path)
{
    if( _path.empty() || _path.front() != '/' )
        return std::unexpected(EINVAL);

    ReviewedFactoryOwnedFD current{ReviewedFactoryOpenRetry("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    if( current.Get() < 0 )
        return std::unexpected(errno);

    size_t position = 1;
    while( position < _path.size() ) {
        const auto separator = _path.find('/', position);
        const auto length = separator == std::string_view::npos ? _path.size() - position : separator - position;
        const auto component = _path.substr(position, length);
        if( component.empty() || component == "." || component == ".." )
            return std::unexpected(EINVAL);
        const std::string owned_component{component};
        ReviewedFactoryOwnedFD next{ReviewedFactoryOpenAtRetry(
            current.Get(), owned_component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if( next.Get() < 0 )
            return std::unexpected(errno);
        current = std::move(next);
        if( separator == std::string_view::npos )
            break;
        position = separator + 1;
    }
    return std::move(current);
}

bool ReviewedFactoryMatches(const OperationPlanningNativeObjectIdentityEvidence &_expected,
                            const struct stat &_actual) noexcept
{
    return _expected.device == static_cast<int32_t>(_actual.st_dev) &&
           _expected.inode == static_cast<uint64_t>(_actual.st_ino) &&
           _expected.birth_time.seconds == static_cast<int64_t>(_actual.st_birthtimespec.tv_sec) &&
           _expected.birth_time.nanoseconds == static_cast<int64_t>(_actual.st_birthtimespec.tv_nsec);
}

bool ReviewedFactoryMatches(const OperationPlanningNativeObjectVersionEvidence &_expected,
                            const struct stat &_actual) noexcept
{
    return _expected.mode == static_cast<uint16_t>(_actual.st_mode) &&
           _expected.byte_size == static_cast<uint64_t>(_actual.st_size) &&
           _expected.modification_time.seconds == static_cast<int64_t>(_actual.st_mtimespec.tv_sec) &&
           _expected.modification_time.nanoseconds == static_cast<int64_t>(_actual.st_mtimespec.tv_nsec) &&
           _expected.status_change_time.seconds == static_cast<int64_t>(_actual.st_ctimespec.tv_sec) &&
           _expected.status_change_time.nanoseconds == static_cast<int64_t>(_actual.st_ctimespec.tv_nsec);
}

bool ReviewedFactoryMatches(const OperationPlanningItemEvidence &_expected, const struct stat &_actual) noexcept
{
    return _expected.native_identity && _expected.native_version &&
           ReviewedFactoryMatches(*_expected.native_identity, _actual) &&
           ReviewedFactoryMatches(*_expected.native_version, _actual);
}

const OperationPlanningItemSnapshot *ReviewedFactoryFindSnapshot(const OperationPreflightReport &_report,
                                                                 const OperationPlanningPath &_path) noexcept
{
    const auto normalized = ReviewedFactoryNormalizeAbsolutePath(_path.absolute_path);
    const auto matches = [&](const OperationPlanningItemSnapshot &_snapshot) {
        return _snapshot.path.provider_id == _path.provider_id && _snapshot.path.absolute_path == normalized;
    };
    const auto found = std::find_if(_report.item_evidence.begin(), _report.item_evidence.end(), matches);
    if( found == _report.item_evidence.end() )
        return nullptr;
    if( std::find_if(std::next(found), _report.item_evidence.end(), matches) != _report.item_evidence.end() )
        return nullptr;
    return &*found;
}

bool ReviewedFactoryDirectAccess(std::string_view _path,
                                 int _mode,
                                 const std::function<bool(std::string_view, int)> &_checker) noexcept
{
    try {
        if( _checker )
            return _checker(_path, _mode);
        const std::string path{_path};
        return !routedio::RoutedIO::InterfaceForAccess(path.c_str(), _mode).isrouted();
    } catch( ... ) {
        return false;
    }
}

std::optional<Error> ReviewedFactoryCauseFromErrno(int _error)
{
    return Error{Error::POSIX, _error};
}

nc::vfs::ProviderConditionalCopyTimestamp
ReviewedFactoryConditionalCopyTimestamp(OperationPlanningTimestampEvidence _timestamp) noexcept
{
    return nc::vfs::ProviderConditionalCopyTimestamp{
        .seconds = _timestamp.seconds,
        .nanoseconds = _timestamp.nanoseconds,
    };
}

nc::vfs::ProviderConditionalCopyExistingExpectation
ReviewedFactoryConditionalCopyExpectation(
    const OperationPlanningPath &_path,
    nc::vfs::ProviderConditionalCopyExpectedKind _kind,
    const OperationPlanningNativeObjectIdentityEvidence &_identity,
    const OperationPlanningNativeObjectVersionEvidence &_version,
    nc::vfs::ProviderConditionalCopyExpectationTolerance _tolerance =
        nc::vfs::ProviderConditionalCopyExpectationTolerance::Exact)
{
    return nc::vfs::ProviderConditionalCopyExistingExpectation{
        .absolute_path = _path.absolute_path,
        .kind = _kind,
        .device = _identity.device,
        .inode = _identity.inode,
        .birth_time = ReviewedFactoryConditionalCopyTimestamp(_identity.birth_time),
        .mode = _version.mode,
        .byte_size = _version.byte_size,
        .modification_time = ReviewedFactoryConditionalCopyTimestamp(_version.modification_time),
        .status_change_time = ReviewedFactoryConditionalCopyTimestamp(_version.status_change_time),
        .tolerance = _tolerance,
    };
}

} // namespace

std::expected<std::shared_ptr<Operation>, ReviewedOperationFactoryError>
ReviewedOperationFactory::BlockExecutionProduct(
    std::expected<CopyOperationExecutionProduct, ReviewedOperationFactoryError> _product) noexcept
{
    if( !_product )
        return std::unexpected(std::move(_product.error()));

    // The evidence, not the single-item projection of it: that projection reports any set other than
    // exactly one item as inconsistent, so once a product may carry several it would answer the
    // batch's size rather than what happened to it.
    auto terminal_evidence = std::move(_product->m_TerminalEvidence);
    _product->m_Operation.reset();
    try {
        if( terminal_evidence ) {
            const auto terminal = terminal_evidence();
            if( !terminal && terminal.error() == CopyOperationTerminalResultError::Inconsistent ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::ConditionalCommitIntegrationUnavailable));
            }
        }
    } catch( ... ) {
    }
    return std::unexpected(
        ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable));
}

std::expected<std::shared_ptr<Operation>, ReviewedOperationFactoryError>
ReviewedOperationFactory::Create(ReviewedVFSOperationPreflight _preflight, CancelChecker _cancel_checker) noexcept
{
    return BlockExecutionProduct(CreateExecutionProduct(std::move(_preflight), std::move(_cancel_checker)));
}

std::expected<std::shared_ptr<Operation>, ReviewedOperationFactoryError>
ReviewedOperationFactory::CreateWithDependencies(
    ReviewedVFSOperationPreflight _preflight,
    CancelChecker _cancel_checker,
    DirectAccessChecker _direct_access_checker,
    SourceOpenAt _source_open_at,
    ConditionalCommitTransactionResolver _conditional_commit_transaction_resolver,
    SnapshotLookup _snapshot_lookup,
    ConditionalMoveCommitTransactionResolver _conditional_move_commit_transaction_resolver) noexcept
{
    return BlockExecutionProduct(
        CreateExecutionProductWithDependencies(std::move(_preflight),
                                               std::move(_cancel_checker),
                                               std::move(_direct_access_checker),
                                               std::move(_source_open_at),
                                               std::move(_conditional_commit_transaction_resolver),
                                               std::move(_snapshot_lookup),
                                               std::move(_conditional_move_commit_transaction_resolver)));
}

std::expected<CopyOperationExecutionProduct, ReviewedOperationFactoryError>
ReviewedOperationFactory::CreateExecutionProduct(ReviewedVFSOperationPreflight _preflight,
                                                 CancelChecker _cancel_checker) noexcept
{
    return CreateExecutionProductWithDependencies(std::move(_preflight), std::move(_cancel_checker), {}, {}, {});
}

std::expected<CopyOperationExecutionProduct, ReviewedOperationFactoryError>
ReviewedOperationFactory::CreateExecutionProductWithDependencies(
    ReviewedVFSOperationPreflight _preflight,
    CancelChecker _cancel_checker,
    DirectAccessChecker _direct_access_checker,
    SourceOpenAt _source_open_at,
    ConditionalCommitTransactionResolver _conditional_commit_transaction_resolver,
    SnapshotLookup _snapshot_lookup,
    ConditionalMoveCommitTransactionResolver _conditional_move_commit_transaction_resolver) noexcept
{
    try {
        CancelChecker is_cancelled = [cancel_checker = std::move(_cancel_checker)]() noexcept {
            if( !cancel_checker )
                return false;
            try {
                return cancel_checker();
            } catch( ... ) {
                return true;
            }
        };
        const auto cancelled = [&] {
            return std::unexpected(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::Cancelled));
        };
        if( is_cancelled() )
            return cancelled();
        if( !_preflight.Bindings() )
            return std::unexpected(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::MissingBindings));

        // Sealed once, up front. One review yields one authority per accepted item, and the seal is
        // what makes every one of them provably the product of this review rather than another.
        SealedReviewedPreflight sealed = SealedReviewedPreflight::Seal(std::move(_preflight));

        const AcceptedOperationPlan &accepted = sealed.AcceptedPlan();
        const OperationPlan &plan = accepted.Plan();
        const OperationPreflightReport &report = accepted.Report();
        if( plan.Type() != OperationPlanType::Copy && plan.Type() != OperationPlanType::Move )
            return std::unexpected(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::UnsupportedPlanType));
        const bool is_move = plan.Type() == OperationPlanType::Move;
        if( report.items.empty() )
            return std::unexpected(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::EmptyAcceptedPlan));
        // Both gates are gone: several sources are executed, and one source expanding into several
        // items needs only the loop that now exists. What stands in their place is not a limitation
        // but the journal's own rule - it numbers results in the plan's source space and refuses a
        // completed entry that does not carry one per source. A plan whose report covers fewer is
        // therefore unexecutable, and refusing it here is the last place that can see both.
        //
        // Unreachable through the planner as it stands, and worth knowing why rather than trying:
        // the only path that stops planning part-way is a cancelled probe, which records a blocker,
        // and a blocked preflight is never accepted. Defence in depth against a refusal that would
        // otherwise surface as an unfinalizable journal entry - a hang, not an error.
        if( report.items.size() != plan.Sources().size() )
            return std::unexpected(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::IncompleteAcceptedPlan));
        switch( plan.ConflictPolicy()->Decision() ) {
            case OperationPlanConflictDecision::Ask:
            case OperationPlanConflictDecision::Skip:
                break;
            case OperationPlanConflictDecision::Replace:
            case OperationPlanConflictDecision::KeepBoth:
            case OperationPlanConflictDecision::RenameNew:
            case OperationPlanConflictDecision::RenameExisting:
            case OperationPlanConflictDecision::MergeFolders:
            default:
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::UnsupportedConflictPolicy));
        }
        if( !report.conflicts.empty() || !report.destructive_effects.empty() || report.requires_confirmation ) {
            return std::unexpected(
                ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::UnexpectedConflictEvidence));
        }

        // Which destination-parent directories this batch has already begun publishing into. The
        // first item to target a given directory is reviewed exactly as it always was: nothing but
        // this transaction should find that directory touched at all. Every later item sharing the
        // same directory necessarily finds it grown by the batch's own prior, authorized publication -
        // that is not staleness, so its expectation tolerates growth instead of refusing it. Identity
        // and the whole permission surface stay exact for every item regardless; only what a batch's
        // own publication predictably advances may move. Keyed on the canonical path alone: a plan
        // this factory accepts has exactly one destination provider.
        std::set<std::string> destination_parents_targeted;
        // Move-only mirror of the set above, in the opposite direction. A rename indivisibly removes
        // the entry it publishes, so a source-parent directory several of a batch's own items are
        // moving *out of* legitimately shrinks - in size and link_count, confirmed empirically on APFS -
        // as the batch's own earlier items complete. The common `MoveTo` shape is exactly this: several
        // siblings out of one folder, so without this the whole shape would fail closed from its second
        // item onward.
        std::set<std::string> source_parents_targeted;

        // One item's work, named and taking an index. Nothing about it changes here - what changes
        // is that it is now a unit the batch loop can call once per accepted item instead of a
        // three-hundred-line stretch that only ever ran for `front()`.
        struct PreparedItem {
            std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction> transaction;
            std::shared_ptr<nc::vfs::Host> source_host;
            // The planning paths, not bare strings: a rollback has to be able to name the item whose
            // undoing it could not confirm, and a path without its provider does not identify one.
            OperationPlanningPath source;
            OperationPlanningPath destination;
            uint64_t exact_source_bytes = 0;
            size_t journal_item_index = 0;
        };
        const auto prepare_item =
            [&](const size_t _index) -> std::expected<PreparedItem, ReviewedOperationFactoryError> {
            const OperationPlannedCopyItem &item = report.items.at(_index);
            if( item.source_kind != OperationPlanningItemKind::File ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::UnsupportedSourceKind, item.source));
            }

            const auto source_path = ReviewedFactoryCanonicalPlanPath(item.source.absolute_path);
            const auto source_parts = source_path ? ReviewedFactoryParentAndName(*source_path) : std::nullopt;
            const auto destination_path = ReviewedFactoryCanonicalPlanPath(item.destination.absolute_path);
            const auto destination_parts =
                destination_path ? ReviewedFactoryParentAndName(*destination_path) : std::nullopt;
            if( !source_path || !source_parts ) {
                return std::unexpected(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::InvalidPath, item.source));
            }
            if( !destination_path || !destination_parts ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::InvalidPath, item.destination));
            }

            const auto &destination = *plan.Destination();
            const auto destination_root = ReviewedFactoryCanonicalPlanPath(destination.AbsolutePath());
            if( !destination_root ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::InvalidPath,
                                           OperationPlanningPath{std::string{destination.ProviderId().Value()},
                                                                 std::string{destination.AbsolutePath()}}));
            }
            const std::string expected_destination = destination.Kind() == OperationPlanDestinationKind::Directory
                                                         ? ReviewedFactoryJoinPath(*destination_root, source_parts->second)
                                                         : *destination_root;
            // Matched against whichever plan source names this item, not against the first one. With a
            // single source the two are the same check; with several they are not, and the difference is
            // deliberately not positional: nothing at this layer promises that accepted item i came from
            // source i, so pairing by index would refuse a sound report as readily as it accepted a
            // mismatched one. What the check owes the caller is that every executed item traces back to
            // something the plan actually named - which is what the several-sources gate protects today,
            // and what has to keep holding once that gate comes down.
            const auto names_this_item = [&](const OperationPlanSource &_source) {
                const auto candidate = ReviewedFactoryCanonicalPlanPath(_source.AbsolutePath());
                return candidate && *candidate == *source_path &&
                       item.source.provider_id == _source.ProviderId().Value();
            };
            const auto structural_source = std::ranges::find_if(plan.Sources(), names_this_item);
            if( structural_source == plan.Sources().end() ||
                item.destination.provider_id != destination.ProviderId().Value() ||
                *destination_path != expected_destination ) {
                return std::unexpected(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::InvalidReviewedPlan));
            }
            // Two index spaces meet here and they are not the same map. An **authority** is issued
            // for the item's place in the reviewed report, because that is what the review covered.
            // A **journal** result is numbered by the item's place in `plan.Sources()`, because that
            // is the space the journal validates against - it refuses a result whose index is not a
            // source of the plan. They coincide only while one source yields one item.
            const auto journal_item_index =
                static_cast<size_t>(std::distance(plan.Sources().begin(), structural_source));

            const OperationPlanningPath destination_parent{
                .provider_id = item.destination.provider_id,
                .absolute_path = destination_parts->first,
            };
            // Move-only. A rename acts on a name inside a directory, so the directory holding the
            // source is part of what a Move claim has to name - a Copy reads through a descriptor and
            // never touches it, so it has no analogous path.
            const OperationPlanningPath source_parent{
                .provider_id = item.source.provider_id,
                .absolute_path = source_parts->first,
            };
            // Inserted, not merely queried: the FIRST item to reach a given directory is the one that
            // must find it exactly as reviewed, and recording it here is what makes every later item
            // sharing that directory see it as already-targeted.
            const auto destination_parent_tolerance =
                destination_parents_targeted.insert(destination_parent.absolute_path).second
                   ? nc::vfs::ProviderConditionalCopyExpectationTolerance::Exact
                   : nc::vfs::ProviderConditionalCopyExpectationTolerance::MonotonicGrowth;
            // Move-only, and the shrink-side twin of the insertion above.
            const auto source_parent_tolerance =
                is_move && !source_parents_targeted.insert(source_parent.absolute_path).second
                   ? nc::vfs::ProviderConditionalCopyExpectationTolerance::MonotonicShrink
                   : nc::vfs::ProviderConditionalCopyExpectationTolerance::Exact;

            const auto source_host = sealed.Bindings()->Resolve(item.source.provider_id);
            const auto destination_host = sealed.Bindings()->Resolve(item.destination.provider_id);
            if( !source_host ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::ProviderUnavailable, item.source));
            }
            if( !destination_host ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::ProviderUnavailable, item.destination));
            }
            if( !std::dynamic_pointer_cast<nc::vfs::NativeHost>(source_host) ||
                !std::dynamic_pointer_cast<nc::vfs::NativeHost>(destination_host) ) {
                return std::unexpected(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::UnsupportedProviderScope));
            }

            const auto find_snapshot = [&](const OperationPlanningPath &_path) {
                return _snapshot_lookup ? _snapshot_lookup(report, _path) : ReviewedFactoryFindSnapshot(report, _path);
            };
            const auto *source_snapshot = find_snapshot(item.source);
            const auto *destination_parent_snapshot = find_snapshot(destination_parent);
            const auto *destination_snapshot = find_snapshot(item.destination);
            const auto *source_parent_snapshot = is_move ? find_snapshot(source_parent) : nullptr;
            if( !source_snapshot ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::MissingEvidence, item.source));
            }
            if( !destination_parent_snapshot ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::MissingEvidence, destination_parent));
            }
            if( !destination_snapshot ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::MissingEvidence, item.destination));
            }
            if( is_move && !source_parent_snapshot ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::MissingEvidence, source_parent));
            }
            if( source_snapshot->evidence.kind != OperationPlanningItemKind::File ||
                !source_snapshot->evidence.native_identity || !source_snapshot->evidence.native_version ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::InvalidEvidence, item.source));
            }
            if( destination_parent_snapshot->evidence.kind != OperationPlanningItemKind::Directory ||
                !destination_parent_snapshot->evidence.native_identity ||
                !destination_parent_snapshot->evidence.native_version ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::InvalidEvidence, destination_parent));
            }
            if( destination_snapshot->evidence.kind != OperationPlanningItemKind::Missing ||
                destination_snapshot->evidence.native_identity || destination_snapshot->evidence.native_version ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::InvalidEvidence, item.destination));
            }
            if( is_move &&
                (source_parent_snapshot->evidence.kind != OperationPlanningItemKind::Directory ||
                 !source_parent_snapshot->evidence.native_identity ||
                 !source_parent_snapshot->evidence.native_version) ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::InvalidEvidence, source_parent));
            }

            if( !ReviewedFactoryDirectAccess(*source_path, R_OK, _direct_access_checker) ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::UnsupportedAccessRoute, item.source));
            }
            if( !ReviewedFactoryDirectAccess(destination_parent.absolute_path, W_OK, _direct_access_checker) ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::UnsupportedAccessRoute, destination_parent));
            }
            // Move-only: a rename also removes the entry from the source's directory, so that
            // directory needs the same direct, non-privileged write route the destination parent does.
            if( is_move && !ReviewedFactoryDirectAccess(source_parent.absolute_path, W_OK, _direct_access_checker) ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::UnsupportedAccessRoute, source_parent));
            }
            if( is_cancelled() )
                return cancelled();

            errno = 0;
            const auto resolved_source_parent = ReviewedFactoryResolveExistingPath(source_parts->first);
            if( !resolved_source_parent ) {
                const int error = errno != 0 ? errno : ENOENT;
                const auto code = error == ENOENT || error == ENOTDIR || error == ELOOP
                                      ? ReviewedOperationFactoryErrorCode::StaleSource
                                      : ReviewedOperationFactoryErrorCode::OpenFailed;
                return std::unexpected(ReviewedFactoryFailure(code, item.source, ReviewedFactoryCauseFromErrno(error)));
            }
            auto source_parent_fd = ReviewedFactoryOpenCanonicalDirectory(*resolved_source_parent);
            if( !source_parent_fd ) {
                const auto code = source_parent_fd.error() == ENOENT || source_parent_fd.error() == ENOTDIR ||
                                          source_parent_fd.error() == ELOOP
                                      ? ReviewedOperationFactoryErrorCode::StaleSource
                                      : ReviewedOperationFactoryErrorCode::OpenFailed;
                return std::unexpected(
                    ReviewedFactoryFailure(code, item.source, ReviewedFactoryCauseFromErrno(source_parent_fd.error())));
            }
            ReviewedFactoryOwnedFD held_source_parent = std::move(*source_parent_fd);
            if( is_move ) {
                // The directory the rename will act on, checked before the child is even opened
                // through it: `StaleSourceParent` is the more specific fact whenever it applies, so it
                // must be checked, not merely left for the provider's own Begin to rediscover.
                struct stat source_parent_stat{};
                if( ReviewedFactoryFStatRetry(held_source_parent.Get(), &source_parent_stat) != 0 ) {
                    const int error = errno;
                    return std::unexpected(ReviewedFactoryFailure(
                        ReviewedOperationFactoryErrorCode::OpenFailed, source_parent, ReviewedFactoryCauseFromErrno(error)));
                }
                if( !S_ISDIR(source_parent_stat.st_mode) ||
                    !ReviewedFactoryMatches(*source_parent_snapshot->evidence.native_identity, source_parent_stat) ||
                    !ReviewedFactoryMatches(*source_parent_snapshot->evidence.native_version, source_parent_stat) ) {
                    return std::unexpected(
                        ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::StaleSourceParent, source_parent));
                }
                if( is_cancelled() )
                    return cancelled();
            }
            const auto open_source = [&](int _directory_fd, const char *_name, int _flags) {
                return _source_open_at ? _source_open_at(_directory_fd, _name, _flags)
                                       : openat(_directory_fd, _name, _flags);
            };
            if( is_cancelled() )
                return cancelled();
            int opened_source_fd;
            while( true ) {
                opened_source_fd = open_source(
                    held_source_parent.Get(), source_parts->second.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
                if( opened_source_fd >= 0 || errno != EINTR )
                    break;
                if( is_cancelled() )
                    return cancelled();
            }
            ReviewedFactoryOwnedFD source_fd{opened_source_fd};
            if( source_fd.Get() < 0 ) {
                const int error = errno;
                const auto code = error == ENOENT || error == ENOTDIR || error == ELOOP
                                      ? ReviewedOperationFactoryErrorCode::StaleSource
                                      : ReviewedOperationFactoryErrorCode::OpenFailed;
                return std::unexpected(ReviewedFactoryFailure(code, item.source, ReviewedFactoryCauseFromErrno(error)));
            }
            struct stat source_stat{};
            if( ReviewedFactoryFStatRetry(source_fd.Get(), &source_stat) != 0 ) {
                const int error = errno;
                return std::unexpected(ReviewedFactoryFailure(
                    ReviewedOperationFactoryErrorCode::OpenFailed, item.source, ReviewedFactoryCauseFromErrno(error)));
            }
            if( !S_ISREG(source_stat.st_mode) || !ReviewedFactoryMatches(source_snapshot->evidence, source_stat) ) {
                return std::unexpected(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::StaleSource, item.source));
            }
            if( is_cancelled() )
                return cancelled();

            errno = 0;
            const auto resolved_destination_parent = ReviewedFactoryResolveExistingPath(destination_parent.absolute_path);
            if( !resolved_destination_parent ) {
                const int error = errno != 0 ? errno : ENOENT;
                const auto code = error == ENOENT || error == ENOTDIR || error == ELOOP
                                      ? ReviewedOperationFactoryErrorCode::StaleDestination
                                      : ReviewedOperationFactoryErrorCode::OpenFailed;
                return std::unexpected(
                    ReviewedFactoryFailure(code, destination_parent, ReviewedFactoryCauseFromErrno(error)));
            }
            auto destination_parent_fd = ReviewedFactoryOpenCanonicalDirectory(*resolved_destination_parent);
            if( !destination_parent_fd ) {
                const auto code = destination_parent_fd.error() == ENOENT || destination_parent_fd.error() == ENOTDIR ||
                                          destination_parent_fd.error() == ELOOP
                                      ? ReviewedOperationFactoryErrorCode::StaleDestination
                                      : ReviewedOperationFactoryErrorCode::OpenFailed;
                return std::unexpected(ReviewedFactoryFailure(
                    code, destination_parent, ReviewedFactoryCauseFromErrno(destination_parent_fd.error())));
            }
            ReviewedFactoryOwnedFD held_destination_parent = std::move(*destination_parent_fd);
            struct stat destination_parent_stat{};
            if( ReviewedFactoryFStatRetry(held_destination_parent.Get(), &destination_parent_stat) != 0 ) {
                const int error = errno;
                return std::unexpected(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::OpenFailed,
                                                              destination_parent,
                                                              ReviewedFactoryCauseFromErrno(error)));
            }
            if( !S_ISDIR(destination_parent_stat.st_mode) ||
                !ReviewedFactoryMatches(*destination_parent_snapshot->evidence.native_identity, destination_parent_stat) ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::StaleDestination, destination_parent));
            }

            struct stat destination_stat{};
            if( ReviewedFactoryFStatAtRetry(held_destination_parent.Get(),
                                            destination_parts->second.c_str(),
                                            &destination_stat,
                                            AT_SYMLINK_NOFOLLOW) == 0 ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::StaleDestination, item.destination));
            }
            const int destination_error = errno;
            if( destination_error != ENOENT ) {
                const auto code = destination_error == ENOTDIR || destination_error == ELOOP
                                      ? ReviewedOperationFactoryErrorCode::StaleDestination
                                      : ReviewedOperationFactoryErrorCode::OpenFailed;
                return std::unexpected(
                    ReviewedFactoryFailure(code, item.destination, ReviewedFactoryCauseFromErrno(destination_error)));
            }
            if( !ReviewedFactoryMatches(*destination_parent_snapshot->evidence.native_version, destination_parent_stat) ) {
                return std::unexpected(
                    ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::StaleDestination, destination_parent));
            }
            if( is_cancelled() )
                return cancelled();

            const OperationPlanningPath source_error_path = item.source;
            const OperationPlanningPath destination_error_path = item.destination;
            const uint64_t exact_source_bytes = source_snapshot->evidence.native_version->byte_size;

            // The item's own index, so that one review yields one authority per accepted item and each
            // authority is tied to the item it was reviewed for. Counting to N would make the second
            // guarantee arithmetic; this keeps it a matter of identity. What differs between the two
            // branches below is the claims shape and which authority/Begin the provider is asked for -
            // the transaction they mint is the same type either way, which is why one `PreparedItem`
            // serves both.
            std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction> minted_transaction;
            if( is_move ) {
                nc::vfs::ProviderConditionalMoveReviewedClaims move_claims{
                    .plan_id = std::string{plan.Id().Value()},
                    .source_binding =
                        nc::vfs::ProviderConditionalCopyBinding{
                            .provider_id = item.source.provider_id,
                            .host = source_host,
                        },
                    .destination_binding =
                        nc::vfs::ProviderConditionalCopyBinding{
                            .provider_id = item.destination.provider_id,
                            .host = destination_host,
                        },
                    .source =
                        ReviewedFactoryConditionalCopyExpectation(item.source,
                                                                  nc::vfs::ProviderConditionalCopyExpectedKind::RegularFile,
                                                                  *source_snapshot->evidence.native_identity,
                                                                  *source_snapshot->evidence.native_version),
                    .source_parent =
                        ReviewedFactoryConditionalCopyExpectation(source_parent,
                                                                  nc::vfs::ProviderConditionalCopyExpectedKind::Directory,
                                                                  *source_parent_snapshot->evidence.native_identity,
                                                                  *source_parent_snapshot->evidence.native_version,
                                                                  source_parent_tolerance),
                    .destination_parent =
                        ReviewedFactoryConditionalCopyExpectation(destination_parent,
                                                                  nc::vfs::ProviderConditionalCopyExpectedKind::Directory,
                                                                  *destination_parent_snapshot->evidence.native_identity,
                                                                  *destination_parent_snapshot->evidence.native_version,
                                                                  destination_parent_tolerance),
                    .destination =
                        nc::vfs::ProviderConditionalCopyMissingExpectation{
                            .absolute_path = item.destination.absolute_path,
                        },
                };
                auto reviewed_authority = sealed.IssueMoveAuthorityForItem(_index, std::move(move_claims));
                if( !reviewed_authority ) {
                    return std::unexpected(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::InvalidReviewedPlan));
                }

                std::expected<std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction>,
                              nc::vfs::ProviderConditionalMoveTransactionBeginError>
                    transaction = std::unexpected(nc::vfs::ProviderConditionalMoveTransactionBeginError::ProviderFailure);
                try {
                    transaction =
                        _conditional_move_commit_transaction_resolver
                            ? _conditional_move_commit_transaction_resolver(std::move(*reviewed_authority), is_cancelled)
                            : destination_host->BeginConditionalMoveTransaction(std::move(*reviewed_authority), is_cancelled);
                } catch( ... ) {
                    return std::unexpected(
                        ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable));
                }

                if( !transaction ) {
                    switch( transaction.error() ) {
                        case nc::vfs::ProviderConditionalMoveTransactionBeginError::SourceStale:
                            return std::unexpected(
                                ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::StaleSource, source_error_path));
                        case nc::vfs::ProviderConditionalMoveTransactionBeginError::SourceParentStale:
                            return std::unexpected(
                                ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::StaleSourceParent, source_parent));
                        case nc::vfs::ProviderConditionalMoveTransactionBeginError::DestinationParentStale:
                            return std::unexpected(ReviewedFactoryFailure(
                                ReviewedOperationFactoryErrorCode::StaleDestination, destination_parent));
                        case nc::vfs::ProviderConditionalMoveTransactionBeginError::DestinationExists:
                            return std::unexpected(ReviewedFactoryFailure(
                                ReviewedOperationFactoryErrorCode::StaleDestination, destination_error_path));
                        case nc::vfs::ProviderConditionalMoveTransactionBeginError::Cancelled:
                            return cancelled();
                        case nc::vfs::ProviderConditionalMoveTransactionBeginError::Unsupported:
                        case nc::vfs::ProviderConditionalMoveTransactionBeginError::InvalidRequest:
                        case nc::vfs::ProviderConditionalMoveTransactionBeginError::ProviderFailure:
                        default:
                            return std::unexpected(ReviewedFactoryFailure(
                                ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable));
                    }
                }
                if( !*transaction ) {
                    return std::unexpected(
                        ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable));
                }
                minted_transaction = std::move(*transaction);
            } else {
                nc::vfs::ProviderConditionalCopyReviewedClaims reviewed_claims{
                    .plan_id = std::string{plan.Id().Value()},
                    .source_binding =
                        nc::vfs::ProviderConditionalCopyBinding{
                            .provider_id = item.source.provider_id,
                            .host = source_host,
                        },
                    .destination_binding =
                        nc::vfs::ProviderConditionalCopyBinding{
                            .provider_id = item.destination.provider_id,
                            .host = destination_host,
                        },
                    .source =
                        ReviewedFactoryConditionalCopyExpectation(item.source,
                                                                  nc::vfs::ProviderConditionalCopyExpectedKind::RegularFile,
                                                                  *source_snapshot->evidence.native_identity,
                                                                  *source_snapshot->evidence.native_version),
                    .destination_parent =
                        ReviewedFactoryConditionalCopyExpectation(destination_parent,
                                                                  nc::vfs::ProviderConditionalCopyExpectedKind::Directory,
                                                                  *destination_parent_snapshot->evidence.native_identity,
                                                                  *destination_parent_snapshot->evidence.native_version,
                                                                  destination_parent_tolerance),
                    .destination =
                        nc::vfs::ProviderConditionalCopyMissingExpectation{
                            .absolute_path = item.destination.absolute_path,
                        },
                };
                auto reviewed_authority = sealed.IssueAuthorityForItem(_index, std::move(reviewed_claims));
                if( !reviewed_authority ) {
                    return std::unexpected(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::InvalidReviewedPlan));
                }

                std::expected<std::unique_ptr<nc::vfs::ProviderConditionalCopyTransaction>,
                              nc::vfs::ProviderConditionalCopyTransactionBeginError>
                    transaction = std::unexpected(nc::vfs::ProviderConditionalCopyTransactionBeginError::ProviderFailure);
                try {
                    transaction =
                        _conditional_commit_transaction_resolver
                            ? _conditional_commit_transaction_resolver(std::move(*reviewed_authority), is_cancelled)
                            : destination_host->BeginConditionalCopyTransaction(std::move(*reviewed_authority), is_cancelled);
                } catch( ... ) {
                    return std::unexpected(
                        ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable));
                }

                if( !transaction ) {
                    switch( transaction.error() ) {
                        case nc::vfs::ProviderConditionalCopyTransactionBeginError::SourceStale:
                            return std::unexpected(
                                ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::StaleSource, source_error_path));
                        case nc::vfs::ProviderConditionalCopyTransactionBeginError::DestinationParentStale:
                            return std::unexpected(ReviewedFactoryFailure(
                                ReviewedOperationFactoryErrorCode::StaleDestination, destination_parent));
                        case nc::vfs::ProviderConditionalCopyTransactionBeginError::DestinationExists:
                            return std::unexpected(ReviewedFactoryFailure(
                                ReviewedOperationFactoryErrorCode::StaleDestination, destination_error_path));
                        case nc::vfs::ProviderConditionalCopyTransactionBeginError::Cancelled:
                            return cancelled();
                        case nc::vfs::ProviderConditionalCopyTransactionBeginError::Unsupported:
                        case nc::vfs::ProviderConditionalCopyTransactionBeginError::InvalidRequest:
                        case nc::vfs::ProviderConditionalCopyTransactionBeginError::ProviderFailure:
                        default:
                            return std::unexpected(ReviewedFactoryFailure(
                                ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable));
                    }
                }
                if( !*transaction ) {
                    return std::unexpected(
                        ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable));
                }
                minted_transaction = std::move(*transaction);
            }

            return PreparedItem{.transaction = std::move(minted_transaction),
                                .source_host = source_host,
                                .source = source_error_path,
                                .destination = destination_error_path,
                                .exact_source_bytes = exact_source_bytes,
                                .journal_item_index = journal_item_index};
        };

        std::vector<PreparedItem> prepared;
        prepared.reserve(report.items.size());

        // Rolls the batch back and reports whether it can still be said that nothing was published.
        // Not what keeps the transactions from leaking - each one aborts itself when destroyed - but
        // what lets the answer be *read*, which a discarded abort result is exactly what hides.
        const auto abandon_prepared = [&prepared]() -> std::optional<OperationPlanningPath> {
            std::optional<OperationPlanningPath> uncertain;
            // In reverse: the transaction begun last is undone first, so the provider unwinds in the
            // order it built the state up.
            for( auto item = prepared.rbegin(); item != prepared.rend(); ++item ) {
                if( !item->transaction )
                    continue;
                const auto result = item->transaction->Abort();
                if( result.publication != nc::vfs::ProviderConditionalCopyPublicationState::NotPublished )
                    uncertain = item->destination;
            }
            prepared.clear();
            return uncertain;
        };

        // Abandoning answers with the reason it was abandoned for - unless the rollback itself could
        // not confirm that nothing was published, because then the reason claims more than is known.
        // `StaleSource` tells the user the world moved and nothing was done; an abort that cannot say
        // NotPublished cannot support the second half of that, and reporting it anyway would leave a
        // destination that may exist behind an error that says none does.
        const auto abandon_with = [&](ReviewedOperationFactoryError _reason) {
            auto uncertain = abandon_prepared();
            if( !uncertain )
                return _reason;
            return ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::ConditionalCommitAuthorityUnavailable,
                                          std::move(*uncertain));
        };

        // Prepared as a set, committed to as a set. Every item's evidence is checked before any item
        // executes: checking as it goes would let the first item's copy happen and the third item's
        // staleness be discovered afterwards, with nothing left to refuse.
        for( size_t index = 0; index != report.items.size(); ++index ) {
            auto item = prepare_item(index);
            if( !item )
                return std::unexpected(abandon_with(std::move(item.error())));
            prepared.emplace_back(std::move(*item));
        }

        // Prepared but not yet handed over - the last moment at which cancelling still costs nothing.
        // An operation built from here would carry open transactions into the Pool only to abort them,
        // so the user would be told "cancelled" after the Pool had taken ownership of work nobody
        // wants. It is also the first failure that can happen with a transaction already begun:
        // beginning one is the last thing preparing an item does, so until this check the rollback
        // above had nothing it could ever be asked to undo.
        if( is_cancelled() )
            return std::unexpected(abandon_with(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::Cancelled)));

        // One operation over the whole set, not one operation per item: each of those would carry its
        // own journal entry and its own terminal state, and the Operation Center would show N
        // operations where the user asked for one.
        std::vector<ProviderConditionalCopyOperationItem> operation_items;
        operation_items.reserve(prepared.size());
        for( auto &item : prepared ) {
            operation_items.push_back(ProviderConditionalCopyOperationItem{
                .transaction = std::move(item.transaction),
                .journal_context =
                    ProviderConditionalCopyJournalContext{
                        .item_index = item.journal_item_index,
                        .exact_source_bytes = item.exact_source_bytes,
                    },
                .presentation =
                    ProviderConditionalCopyOperationPresentation{
                        .source_host = item.source_host,
                        .source_path = item.source.absolute_path,
                        .destination_path = item.destination.absolute_path,
                    },
            });
        }

        auto product =
            ProviderConditionalCopyOperationFactory::CreateBatch(std::move(operation_items), std::move(is_cancelled));
        if( !product ) {
            // The transactions moved into the batch and were destroyed with it, which aborts them but
            // discards the abort results - the one place where the rollback above cannot answer. It
            // costs nothing today: every other construction refusal is checked before this point, so
            // only an allocation failure reaches it.
            return std::unexpected(
                abandon_with(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::ConstructionFailed)));
        }
        return std::move(*product);
    } catch( ... ) {
        return std::unexpected(ReviewedFactoryFailure(ReviewedOperationFactoryErrorCode::ConstructionFailed));
    }
}

} // namespace nc::ops
