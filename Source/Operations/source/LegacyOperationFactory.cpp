// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "LegacyOperationFactory.h"

#include "Copying/Copying.h"

#include <VFS/Host.h>

#include <algorithm>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace nc::ops {
namespace {

LegacyOperationFactoryError Failure(LegacyOperationFactoryErrorCode _code,
                                    std::optional<OperationPlanningPath> _path = std::nullopt,
                                    std::optional<Error> _cause = std::nullopt)
{
    return LegacyOperationFactoryError{_code, std::move(_path), std::move(_cause)};
}

std::string DirectoryDestination(std::string_view _path)
{
    std::string result{_path};
    if( result.back() != '/' )
        result.push_back('/');
    return result;
}

std::string LegacyFactoryParentPath(std::string_view _path)
{
    auto end = _path.size();
    while( end > 1 && _path[end - 1] == '/' )
        --end;
    const auto separator = _path.substr(0, end).rfind('/');
    return separator == 0 ? std::string{"/"} : std::string{_path.substr(0, separator)};
}

std::string_view WithoutTrailingSlashes(std::string_view _path) noexcept
{
    while( _path.size() > 1 && _path.back() == '/' )
        _path.remove_suffix(1);
    return _path;
}

enum class RuntimeItemKind : uint8_t {
    Missing,
    File,
    Directory,
    Symlink
};

std::optional<RuntimeItemKind> RuntimeKind(OperationPlanningItemKind _kind) noexcept
{
    switch( _kind ) {
        case OperationPlanningItemKind::Missing:
            return RuntimeItemKind::Missing;
        case OperationPlanningItemKind::File:
            return RuntimeItemKind::File;
        case OperationPlanningItemKind::Directory:
            return RuntimeItemKind::Directory;
        case OperationPlanningItemKind::Symlink:
            return RuntimeItemKind::Symlink;
        case OperationPlanningItemKind::Other:
            return std::nullopt;
    }
    return std::nullopt;
}

bool HasKind(const VFSListingItem &_item, RuntimeItemKind _kind) noexcept
{
    switch( _kind ) {
        case RuntimeItemKind::File:
            return !_item.IsSymlink() && _item.IsReg();
        case RuntimeItemKind::Directory:
            return !_item.IsSymlink() && _item.IsDir();
        case RuntimeItemKind::Symlink:
            return _item.IsSymlink();
        case RuntimeItemKind::Missing:
            return false;
    }
    return false;
}

struct RuntimePathExpectation final {
    std::shared_ptr<nc::vfs::Host> host;
    std::string path;
    RuntimeItemKind kind;
};

bool Validate(const RuntimePathExpectation &_expectation) noexcept
{
    try {
        const auto stat = _expectation.host->Stat(_expectation.path, VFSFlags::F_NoFollow);
        if( _expectation.kind == RuntimeItemKind::Missing )
            return !stat && _expectation.host->ClassifyError(stat.error()) == nc::vfs::HostErrorKind::Missing;
        if( !stat || !stat->meaning.mode )
            return false;
        switch( _expectation.kind ) {
            case RuntimeItemKind::File:
                return S_ISREG(stat->mode);
            case RuntimeItemKind::Directory:
                return S_ISDIR(stat->mode);
            case RuntimeItemKind::Symlink:
                return S_ISLNK(stat->mode);
            case RuntimeItemKind::Missing:
                return false;
        }
    }
    catch( ... ) {
        return false;
    }
    return false;
}

} // namespace

std::expected<std::shared_ptr<Operation>, LegacyOperationFactoryError>
LegacyOperationFactory::Create(ReviewedVFSOperationPreflight _preflight,
                               CancelChecker _cancel_checker) noexcept
{
    const auto is_cancelled = [&]() noexcept {
        if( !_cancel_checker )
            return false;
        try {
            return _cancel_checker();
        }
        catch( ... ) {
            return true;
        }
    };
    const auto cancelled = [&] {
        return std::unexpected(Failure(LegacyOperationFactoryErrorCode::Cancelled));
    };

    try {
        if( is_cancelled() )
            return cancelled();

        const AcceptedOperationPlan &accepted = _preflight.AcceptedPlan();
        const OperationPlan &plan = accepted.Plan();
        if( plan.Type() != OperationPlanType::Copy )
            return std::unexpected(Failure(LegacyOperationFactoryErrorCode::UnsupportedPlanType));
        if( !_preflight.Bindings() )
            return std::unexpected(Failure(LegacyOperationFactoryErrorCode::MissingBindings));
        if( accepted.Report().items.empty() )
            return std::unexpected(Failure(LegacyOperationFactoryErrorCode::EmptyAcceptedPlan));
        if( plan.Sources().size() != 1 || accepted.Report().items.size() != 1 )
            return std::unexpected(Failure(LegacyOperationFactoryErrorCode::BatchUnsupported));

        const OperationPlanDestination &destination = *plan.Destination();
        const OperationPlanningPath destination_path{
            std::string{destination.ProviderId().Value()}, std::string{destination.AbsolutePath()}};
        const auto destination_host = _preflight.Bindings()->Resolve(destination_path.provider_id);
        if( !destination_host ) {
            return std::unexpected(
                Failure(LegacyOperationFactoryErrorCode::ProviderUnavailable, destination_path));
        }

        const OperationPlannedCopyItem &planned_item = accepted.Report().items.front();
        const bool destination_conflict_was_reviewed = std::ranges::any_of(
            accepted.Report().conflicts, [&](const OperationPlanningConflict &_conflict) {
                return _conflict.source == planned_item.source &&
                       _conflict.destination == planned_item.destination;
            });

        CopyingOptions options;
        options.reject_final_component_symlinks = true;
        options.destination_path_interpretation =
            destination.Kind() == OperationPlanDestinationKind::Directory
                ? CopyingOptions::DestinationPathInterpretation::Directory
                : CopyingOptions::DestinationPathInterpretation::ExactItem;
        switch( plan.ConflictPolicy()->Decision() ) {
            case OperationPlanConflictDecision::Ask:
                options.exist_behavior = CopyingOptions::ExistBehavior::Ask;
                break;
            case OperationPlanConflictDecision::Replace:
                options.exist_behavior = destination_conflict_was_reviewed
                                             ? CopyingOptions::ExistBehavior::OverwriteAll
                                             : CopyingOptions::ExistBehavior::Ask;
                break;
            case OperationPlanConflictDecision::Skip:
                options.exist_behavior = CopyingOptions::ExistBehavior::SkipAll;
                break;
            case OperationPlanConflictDecision::KeepBoth:
            case OperationPlanConflictDecision::RenameNew:
            case OperationPlanConflictDecision::RenameExisting:
            case OperationPlanConflictDecision::MergeFolders:
                return std::unexpected(
                    Failure(LegacyOperationFactoryErrorCode::UnsupportedConflictPolicy));
            default:
                return std::unexpected(
                    Failure(LegacyOperationFactoryErrorCode::UnsupportedConflictPolicy));
        }

        std::vector<VFSListingItem> source_items;
        source_items.reserve(1);
        std::vector<RuntimePathExpectation> runtime_expectations;
        runtime_expectations.reserve(3);
        if( is_cancelled() )
            return cancelled();
        if( planned_item.destination.provider_id != destination_path.provider_id ) {
            return std::unexpected(
                Failure(LegacyOperationFactoryErrorCode::SourceMaterializationInvalid,
                        planned_item.destination));
        }

        const auto source_host = _preflight.Bindings()->Resolve(planned_item.source.provider_id);
        if( !source_host ) {
            return std::unexpected(
                Failure(LegacyOperationFactoryErrorCode::ProviderUnavailable, planned_item.source));
        }
        if( !source_host->IsNativeFS() || !destination_host->IsNativeFS() ) {
            return std::unexpected(
                Failure(LegacyOperationFactoryErrorCode::UnsupportedProviderScope));
        }
        const auto listing = source_host->FetchSingleItemListing(
            planned_item.source.absolute_path, VFSFlags::F_NoFollow, is_cancelled);
        if( is_cancelled() )
            return cancelled();
        if( !listing ) {
            const auto code = source_host->ClassifyError(listing.error()) == nc::vfs::HostErrorKind::Cancelled
                                  ? LegacyOperationFactoryErrorCode::Cancelled
                                  : LegacyOperationFactoryErrorCode::SourceMaterializationFailed;
            return std::unexpected(Failure(code, planned_item.source, listing.error()));
        }
        if( (*listing)->Count() != 1 ) {
            return std::unexpected(
                Failure(LegacyOperationFactoryErrorCode::SourceMaterializationInvalid,
                        planned_item.source));
        }
        VFSListingItem item = (*listing)->Item(0);
        if( !item || item.Host().get() != source_host.get() ||
            WithoutTrailingSlashes(item.Path()) !=
                WithoutTrailingSlashes(planned_item.source.absolute_path) ) {
            return std::unexpected(
                Failure(LegacyOperationFactoryErrorCode::SourceMaterializationInvalid,
                        planned_item.source));
        }
        const auto source_kind = RuntimeKind(planned_item.source_kind);
        if( !source_kind || !HasKind(item, *source_kind) ) {
            return std::unexpected(
                Failure(LegacyOperationFactoryErrorCode::SourceMaterializationInvalid,
                        planned_item.source));
        }
        runtime_expectations.emplace_back(
            RuntimePathExpectation{source_host, planned_item.source.absolute_path, *source_kind});
        source_items.emplace_back(std::move(item));

        const std::string destination_directory =
            destination.Kind() == OperationPlanDestinationKind::Directory
                ? std::string{destination.AbsolutePath()}
                : LegacyFactoryParentPath(destination.AbsolutePath());
        runtime_expectations.emplace_back(
            RuntimePathExpectation{destination_host, destination_directory, RuntimeItemKind::Directory});
        const auto effective_destination_kind = destination_conflict_was_reviewed
                                                    ? RuntimeKind(planned_item.source_kind)
                                                    : std::optional{RuntimeItemKind::Missing};
        if( !effective_destination_kind ) {
            return std::unexpected(
                Failure(LegacyOperationFactoryErrorCode::SourceMaterializationInvalid,
                        planned_item.destination));
        }
        runtime_expectations.emplace_back(RuntimePathExpectation{
            destination_host, planned_item.destination.absolute_path, *effective_destination_kind});

        if( is_cancelled() )
            return cancelled();
        const std::string legacy_destination =
            destination.Kind() == OperationPlanDestinationKind::Directory
                ? DirectoryDestination(destination.AbsolutePath())
                : std::string{destination.AbsolutePath()};
        auto copying = std::make_shared<Copying>(
            std::move(source_items), legacy_destination, destination_host, options);
        copying->SetRuntimePreflightValidator(
            [expectations = std::move(runtime_expectations)]() noexcept {
                return std::ranges::all_of(expectations, Validate);
            });
        std::shared_ptr<Operation> operation = std::move(copying);
        return operation;
    }
    catch( ... ) {
        return std::unexpected(Failure(LegacyOperationFactoryErrorCode::ConstructionFailed));
    }
}

} // namespace nc::ops
