// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "VFSOperationPlanningProbes.h"

#include <Base/Error.h>
#include <VFS/Host.h>
#include <VFS/ProviderCapabilities.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <optional>
#include <sys/stat.h>
#include <utility>

namespace nc::ops {
namespace {

OperationPlanningProbeError MapError(const nc::vfs::Host &_host, const Error &_error)
{
    switch( _host.ClassifyError(_error) ) {
        case nc::vfs::HostErrorKind::Cancelled:
            return OperationPlanningProbeError::Cancelled;
        case nc::vfs::HostErrorKind::PermissionDenied:
            return OperationPlanningProbeError::PermissionDenied;
        case nc::vfs::HostErrorKind::Unsupported:
            return OperationPlanningProbeError::Unsupported;
        case nc::vfs::HostErrorKind::Unavailable:
        case nc::vfs::HostErrorKind::TimedOut:
            return OperationPlanningProbeError::Unavailable;
        case nc::vfs::HostErrorKind::Missing:
        case nc::vfs::HostErrorKind::Other:
            return OperationPlanningProbeError::Failed;
    }
    return OperationPlanningProbeError::Failed;
}

bool IsMissing(const nc::vfs::Host &_host, const Error &_error)
{
    return _host.ClassifyError(_error) == nc::vfs::HostErrorKind::Missing;
}

std::optional<bool> SameNamespace(const nc::vfs::Host &_lhs, const nc::vfs::Host &_rhs) noexcept
{
    try {
        if( &_lhs == &_rhs )
            return true;
        if( std::string_view{_lhs.Tag()} != std::string_view{_rhs.Tag()} )
            return false;
        const auto lhs_identity = _lhs.SemanticNamespaceIdentity();
        const auto rhs_identity = _rhs.SemanticNamespaceIdentity();
        if( lhs_identity && rhs_identity )
            return *lhs_identity == *rhs_identity;
    }
    catch( ... ) {
    }
    return std::nullopt;
}

std::string ChildPath(std::string_view _directory, std::string_view _name)
{
    std::string path{_directory};
    if( path.empty() || path.back() != '/' )
        path.push_back('/');
    path.append(_name);
    return path;
}

std::optional<std::string_view> Filename(std::string_view _path) noexcept
{
    while( _path.size() > 1 && _path.back() == '/' )
        _path.remove_suffix(1);
    if( _path.empty() || _path == "/" )
        return std::nullopt;
    const size_t separator = _path.rfind('/');
    const std::string_view name =
        separator == std::string_view::npos ? _path : _path.substr(separator + 1);
    return name.empty() ? std::nullopt : std::optional<std::string_view>{name};
}

bool IsValid(OperationPlanningAccessState _state) noexcept
{
    switch( _state ) {
        case OperationPlanningAccessState::Granted:
        case OperationPlanningAccessState::PermissionRequired:
        case OperationPlanningAccessState::Denied:
            return true;
    }
    return false;
}

OperationPlanningTimestampEvidence TimestampEvidence(const timespec &_time) noexcept
{
    return {
        .seconds = static_cast<int64_t>(_time.tv_sec),
        .nanoseconds = static_cast<int64_t>(_time.tv_nsec),
    };
}

OperationPlanningItemEvidence ItemEvidence(OperationPlanningItemKind _kind,
                                           std::optional<uint64_t> _byte_size,
                                           const nc::vfs::Host &_host,
                                           const VFSStat &_stat)
{
    OperationPlanningItemEvidence evidence{_kind, _byte_size};
    if( !_host.IsNativeFS() )
        return evidence;

    if( _stat.meaning.dev && _stat.meaning.inode && _stat.meaning.btime ) {
        evidence.native_identity = OperationPlanningNativeObjectIdentityEvidence{
            .device = _stat.dev,
            .inode = _stat.inode,
            .birth_time = TimestampEvidence(_stat.btime),
        };
    }
    if( _stat.meaning.mode && _stat.meaning.size && _stat.meaning.mtime && _stat.meaning.ctime ) {
        evidence.native_version = OperationPlanningNativeObjectVersionEvidence{
            .mode = _stat.mode,
            .byte_size = _stat.size,
            .modification_time = TimestampEvidence(_stat.mtime),
            .status_change_time = TimestampEvidence(_stat.ctime),
        };
    }
    return evidence;
}

template <class T, class Callable>
OperationPlanningProbeResult<T> Contain(Callable &&_callable) noexcept
{
    try {
        return std::forward<Callable>(_callable)();
    }
    catch( ... ) {
        return std::unexpected(OperationPlanningProbeError::Failed);
    }
}

} // namespace

VFSOperationPlanningBindings::VFSOperationPlanningBindings(Providers _providers)
    : m_Providers(std::move(_providers))
{
}

std::expected<VFSOperationPlanningBindings::Ptr, VFSOperationPlanningProbesValidationError>
VFSOperationPlanningBindings::Create(std::vector<VFSOperationPlanningProviderBinding> _bindings)
{
    Providers providers;
    for( auto &binding : _bindings ) {
        if( binding.provider_id.empty() )
            return std::unexpected(VFSOperationPlanningProbesValidationError::EmptyProviderId);
        if( !binding.host )
            return std::unexpected(VFSOperationPlanningProbesValidationError::MissingHost);
        if( providers.contains(binding.provider_id) )
            return std::unexpected(VFSOperationPlanningProbesValidationError::DuplicateProviderId);
        if( std::ranges::any_of(providers, [&](const auto &_entry) {
                return _entry.second.get() == binding.host.get() ||
                       (!_entry.second.owner_before(binding.host) && !binding.host.owner_before(_entry.second));
            }) )
            return std::unexpected(VFSOperationPlanningProbesValidationError::DuplicateHost);
        for( const auto &[existing_id, existing_host] : providers ) {
            (void)existing_id;
            const auto same_namespace = SameNamespace(*existing_host, *binding.host);
            if( !same_namespace )
                return std::unexpected(VFSOperationPlanningProbesValidationError::HostNamespaceUnavailable);
            if( *same_namespace )
                return std::unexpected(VFSOperationPlanningProbesValidationError::DuplicateHost);
        }
        providers.emplace(std::move(binding.provider_id), std::move(binding.host));
    }
    return Ptr{new VFSOperationPlanningBindings{std::move(providers)}};
}

std::shared_ptr<nc::vfs::Host>
VFSOperationPlanningBindings::Resolve(std::string_view _provider_id) const noexcept
{
    if( const auto found = m_Providers.find(_provider_id); found != m_Providers.end() )
        return found->second;
    return nullptr;
}

VFSBoundOperationPreflight::VFSBoundOperationPreflight(VFSOperationPlanningBindings::Ptr _bindings,
                                                       OperationPreflightResult _result)
    : m_Bindings(std::move(_bindings)), m_Result(std::move(_result))
{
}

ReviewedVFSOperationPreflight::ReviewedVFSOperationPreflight(VFSBoundOperationPreflight _preflight)
    : m_Preflight(std::move(_preflight))
{
}

ReviewedVFSOperationPreflight::ReviewedVFSOperationPreflight(
    ReviewedVFSOperationPreflight &&_other) noexcept
    : m_Preflight{std::move(_other.m_Preflight)}
{
    // Whether authorities have been issued is no longer carried here: it belongs to the seal, which
    // is what a moved-from review can no longer be the source of anyway.
}

ReviewedVFSOperationPreflight &ReviewedVFSOperationPreflight::operator=(
    ReviewedVFSOperationPreflight &&_other) noexcept
{
    if( this != &_other ) {
        m_Preflight = std::move(_other.m_Preflight);
    }
    return *this;
}

std::expected<ReviewedVFSOperationPreflight, VFSOperationPreflightReviewError>
ReviewedVFSOperationPreflight::Review(VFSBoundOperationPreflight _preflight,
                                      VFSOperationPreflightReviewDecision _decision)
{
    switch( _decision ) {
        case VFSOperationPreflightReviewDecision::Approved:
        case VFSOperationPreflightReviewDecision::ApprovedWithDestructiveConfirmation:
            break;
        default:
            return std::unexpected(VFSOperationPreflightReviewError::InvalidDecision);
    }
    const auto *accepted = std::get_if<AcceptedOperationPlan>(&_preflight.Result());
    if( !accepted )
        return std::unexpected(VFSOperationPreflightReviewError::Blocked);
    if( accepted->Plan().Type() != OperationPlanType::Copy && accepted->Plan().Type() != OperationPlanType::Move )
        return std::unexpected(VFSOperationPreflightReviewError::UnsupportedPlanType);
    const bool destructive_authority_required =
        accepted->Report().requires_confirmation ||
        accepted->Plan().ConflictPolicy()->Decision() == OperationPlanConflictDecision::Replace;
    if( destructive_authority_required &&
        _decision != VFSOperationPreflightReviewDecision::ApprovedWithDestructiveConfirmation )
        return std::unexpected(VFSOperationPreflightReviewError::DestructiveConfirmationRequired);
    return ReviewedVFSOperationPreflight{std::move(_preflight)};
}

const AcceptedOperationPlan &ReviewedVFSOperationPreflight::AcceptedPlan() const noexcept
{
    return std::get<AcceptedOperationPlan>(m_Preflight.Result());
}

nc::vfs::ProviderConditionalCopyReviewedAuthority
ReviewedVFSOperationPreflight::MakeAuthority(std::shared_ptr<ReviewedVFSOperationPreflight> _seal,
                                            nc::vfs::ProviderConditionalCopyReviewedClaims _claims)
{
    return nc::vfs::ProviderConditionalCopyReviewedAuthority{std::move(_claims), std::move(_seal)};
}

nc::vfs::ProviderConditionalMoveReviewedAuthority
ReviewedVFSOperationPreflight::MakeMoveAuthority(std::shared_ptr<ReviewedVFSOperationPreflight> _seal,
                                                 nc::vfs::ProviderConditionalMoveReviewedClaims _claims)
{
    return nc::vfs::ProviderConditionalMoveReviewedAuthority{std::move(_claims), std::move(_seal)};
}

SealedReviewedPreflight::SealedReviewedPreflight(std::shared_ptr<ReviewedVFSOperationPreflight> _review,
                                                 const size_t _item_count)
    : m_Review{std::move(_review)}, m_Issued(_item_count, false)
{
}

SealedReviewedPreflight SealedReviewedPreflight::Seal(ReviewedVFSOperationPreflight _review)
{
    const size_t item_count = _review.AcceptedPlan().Report().items.size();
    // The seal is made once, here, and shared by every authority this review yields. Making one per
    // issue would let two authorities from the same review be indistinguishable from two reviews.
    auto sealed = std::make_shared<ReviewedVFSOperationPreflight>(std::move(_review));
    return SealedReviewedPreflight{std::move(sealed), item_count};
}

const VFSOperationPlanningBindings::Ptr &SealedReviewedPreflight::Bindings() const noexcept
{
    return m_Review->Bindings();
}

const AcceptedOperationPlan &SealedReviewedPreflight::AcceptedPlan() const noexcept
{
    return m_Review->AcceptedPlan();
}

size_t SealedReviewedPreflight::AcceptedItemCount() const noexcept
{
    return m_Issued.size();
}

std::optional<nc::vfs::ProviderConditionalCopyReviewedAuthority>
SealedReviewedPreflight::IssueAuthorityForItem(const size_t _item_index,
                                               nc::vfs::ProviderConditionalCopyReviewedClaims _claims)
{
    // Nobody reviewed an item the report does not contain, so there is nothing here to authorise.
    if( _item_index >= m_Issued.size() )
        return std::nullopt;
    // Asked twice for one reviewed item, a caller would come away with an authority to spend
    // somewhere nobody looked. One review, one authority per accepted item, and no seconds.
    if( m_Issued[_item_index] )
        return std::nullopt;
    m_Issued[_item_index] = true;
    return ReviewedVFSOperationPreflight::MakeAuthority(m_Review, std::move(_claims));
}

std::optional<nc::vfs::ProviderConditionalMoveReviewedAuthority>
SealedReviewedPreflight::IssueMoveAuthorityForItem(const size_t _item_index,
                                                   nc::vfs::ProviderConditionalMoveReviewedClaims _claims)
{
    if( _item_index >= m_Issued.size() )
        return std::nullopt;
    if( m_Issued[_item_index] )
        return std::nullopt;
    m_Issued[_item_index] = true;
    return ReviewedVFSOperationPreflight::MakeMoveAuthority(m_Review, std::move(_claims));
}

VFSOperationPlanningProbes::VFSOperationPlanningProbes(VFSOperationPlanningBindings::Ptr _bindings,
                                                       AccessChecker _access_checker,
                                                       CancelChecker _cancel_checker)
    : m_Bindings(std::move(_bindings)), m_AccessChecker(std::move(_access_checker)),
      m_CancelChecker(std::move(_cancel_checker))
{
}

std::expected<VFSOperationPlanningProbes, VFSOperationPlanningProbesValidationError>
VFSOperationPlanningProbes::Create(VFSOperationPlanningBindings::Ptr _bindings,
                                   AccessChecker _access_checker,
                                   CancelChecker _cancel_checker)
{
    if( !_bindings )
        return std::unexpected(VFSOperationPlanningProbesValidationError::MissingBindings);
    return VFSOperationPlanningProbes{
        std::move(_bindings), std::move(_access_checker), std::move(_cancel_checker)};
}

VFSBoundOperationPreflight VFSOperationPlanningProbes::Preflight(OperationPlan _plan)
{
    auto result = OperationPlanner::Preflight(std::move(_plan), *this);
    return VFSBoundOperationPreflight{m_Bindings, std::move(result)};
}

OperationPlanningProbeResult<OperationPlanningProviderEvidence>
VFSOperationPlanningProbes::ProbeProvider(const OperationPlanningPath &_path)
{
    return Contain<OperationPlanningProviderEvidence>([&] { return ProbeProviderImpl(_path); });
}

OperationPlanningProbeResult<OperationPlanningProviderEvidence>
VFSOperationPlanningProbes::ProbeProviderImpl(const OperationPlanningPath &_path)
{
    if( IsCancelled() )
        return std::unexpected(OperationPlanningProbeError::Cancelled);
    const auto host = FindHost(_path.provider_id);
    if( !host )
        return std::unexpected(OperationPlanningProbeError::Unavailable);

    const nc::vfs::ProviderCapabilities capabilities =
        nc::vfs::ProviderCapabilitiesResolver::Resolve(*host, _path.absolute_path);
    const std::optional<bool> case_sensitive = host->CaseSensitivityAtPath(_path.absolute_path);
    if( IsCancelled() )
        return std::unexpected(OperationPlanningProbeError::Cancelled);
    const OperationPlanningPathIdentitySemantics path_identity =
        !case_sensitive ? OperationPlanningPathIdentitySemantics::Unavailable
                        : *case_sensitive ? OperationPlanningPathIdentitySemantics::ASCIICaseSensitive
                                          : OperationPlanningPathIdentitySemantics::ASCIICaseInsensitive;
    return OperationPlanningProviderEvidence{
        .can_copy_from = capabilities.can_read,
        .can_copy_to = capabilities.can_create_file && capabilities.can_create_folder,
        .path_identity = path_identity,
        .can_replace_file = capabilities.can_delete_permanently,
        .can_replace_directory = capabilities.can_delete_permanently,
        .can_copy_symlink_to = capabilities.can_create_symlink,
        .can_rename = capabilities.can_rename,
        .can_delete_permanently = capabilities.can_delete_permanently,
    };
}

OperationPlanningProbeResult<OperationPlanningItemEvidence>
VFSOperationPlanningProbes::ProbeItem(const OperationPlanningPath &_path)
{
    return Contain<OperationPlanningItemEvidence>([&] { return ProbeItemImpl(_path); });
}

OperationPlanningProbeResult<OperationPlanningItemEvidence>
VFSOperationPlanningProbes::ProbeItemImpl(const OperationPlanningPath &_path)
{
    if( IsCancelled() )
        return std::unexpected(OperationPlanningProbeError::Cancelled);
    const auto host = FindHost(_path.provider_id);
    if( !host )
        return std::unexpected(OperationPlanningProbeError::Unavailable);

    const auto stat = host->Stat(_path.absolute_path, VFSFlags::F_NoFollow, SanitizedCancelChecker());
    if( IsCancelled() )
        return std::unexpected(OperationPlanningProbeError::Cancelled);
    if( !stat ) {
        if( IsMissing(*host, stat.error()) )
            return OperationPlanningItemEvidence{OperationPlanningItemKind::Missing, std::nullopt};
        return std::unexpected(MapError(*host, stat.error()));
    }

    if( !stat->meaning.mode )
        return std::unexpected(OperationPlanningProbeError::UnsupportedItem);
    const mode_t type = stat->mode & S_IFMT;
    if( type == S_IFDIR )
        return ItemEvidence(OperationPlanningItemKind::Directory, std::nullopt, *host, *stat);
    if( type == S_IFREG ) {
        return ItemEvidence(OperationPlanningItemKind::File,
                            stat->meaning.size ? std::optional<uint64_t>{stat->size} : std::nullopt,
                            *host,
                            *stat);
    }
    if( type == S_IFLNK ) {
        const auto capabilities = nc::vfs::ProviderCapabilitiesResolver::Resolve(*host, _path.absolute_path);
        if( IsCancelled() )
            return std::unexpected(OperationPlanningProbeError::Cancelled);
        if( !capabilities.can_resolve_symlink )
            return std::unexpected(OperationPlanningProbeError::UnsupportedItem);
        return ItemEvidence(OperationPlanningItemKind::Symlink,
                            stat->meaning.size ? std::optional<uint64_t>{stat->size} : std::nullopt,
                            *host,
                            *stat);
    }
    return std::unexpected(OperationPlanningProbeError::UnsupportedItem);
}

OperationPlanningProbeResult<OperationPlanningNameEvidence>
VFSOperationPlanningProbes::ProbeDestinationName(const OperationPlanningPath &_path)
{
    return Contain<OperationPlanningNameEvidence>([&] { return ProbeDestinationNameImpl(_path); });
}

OperationPlanningProbeResult<OperationPlanningNameEvidence>
VFSOperationPlanningProbes::ProbeDestinationNameImpl(const OperationPlanningPath &_path)
{
    if( IsCancelled() )
        return std::unexpected(OperationPlanningProbeError::Cancelled);
    const auto host = FindHost(_path.provider_id);
    if( !host )
        return std::unexpected(OperationPlanningProbeError::Unavailable);
    const auto filename = Filename(_path.absolute_path);
    if( !filename )
        return OperationPlanningNameEvidence{false};
    const bool valid = host->ValidateFilename(*filename);
    if( IsCancelled() )
        return std::unexpected(OperationPlanningProbeError::Cancelled);
    return OperationPlanningNameEvidence{valid};
}

OperationPlanningProbeResult<OperationPlanningAccessEvidence>
VFSOperationPlanningProbes::ProbeAccess(const OperationPlanningPath &_path,
                                        OperationPlanningRequiredAccess _required)
{
    return Contain<OperationPlanningAccessEvidence>([&] { return ProbeAccessImpl(_path, _required); });
}

OperationPlanningProbeResult<OperationPlanningAccessEvidence>
VFSOperationPlanningProbes::ProbeAccessImpl(const OperationPlanningPath &_path,
                                            OperationPlanningRequiredAccess _required)
{
    if( IsCancelled() )
        return std::unexpected(OperationPlanningProbeError::Cancelled);
    const auto host = FindHost(_path.provider_id);
    if( !host )
        return std::unexpected(OperationPlanningProbeError::Unavailable);

    const nc::vfs::ProviderCapabilities capabilities =
        nc::vfs::ProviderCapabilitiesResolver::Resolve(*host, _path.absolute_path);
    if( IsCancelled() )
        return std::unexpected(OperationPlanningProbeError::Cancelled);
    bool provider_grants_access = false;
    switch( _required ) {
        case OperationPlanningRequiredAccess::Read:
            provider_grants_access = capabilities.can_read;
            break;
        case OperationPlanningRequiredAccess::Write:
            provider_grants_access = capabilities.can_create_file && capabilities.can_create_folder;
            break;
        case OperationPlanningRequiredAccess::Rename:
            provider_grants_access = capabilities.can_rename;
            break;
        case OperationPlanningRequiredAccess::ReplaceFile:
        case OperationPlanningRequiredAccess::ReplaceDirectory:
        case OperationPlanningRequiredAccess::Delete:
            provider_grants_access = capabilities.can_delete_permanently;
            break;
        default:
            return std::unexpected(OperationPlanningProbeError::Failed);
    }
    if( !provider_grants_access )
        return OperationPlanningAccessEvidence{OperationPlanningAccessState::Denied};

    if( m_AccessChecker ) {
        auto evidence = m_AccessChecker(_path, _required, *host);
        if( IsCancelled() )
            return std::unexpected(OperationPlanningProbeError::Cancelled);
        if( !evidence )
            return std::unexpected(evidence.error());
        if( !IsValid(evidence->state) )
            return std::unexpected(OperationPlanningProbeError::Failed);
        return evidence;
    }
    return OperationPlanningAccessEvidence{OperationPlanningAccessState::PermissionRequired};
}

OperationPlanningProbeResult<OperationPlanningEstimateEvidence>
VFSOperationPlanningProbes::ProbeEstimate(const OperationPlanningPath &_source,
                                          const OperationPlanningPath &_destination)
{
    return Contain<OperationPlanningEstimateEvidence>([&] {
        return ProbeEstimateImpl(_source, _destination);
    });
}

OperationPlanningProbeResult<OperationPlanningEstimateEvidence>
VFSOperationPlanningProbes::ProbeEstimateImpl(const OperationPlanningPath &_source,
                                              const OperationPlanningPath &_destination)
{
    if( IsCancelled() )
        return std::unexpected(OperationPlanningProbeError::Cancelled);
    const auto host_ptr = FindHost(_source.provider_id);
    if( !host_ptr )
        return std::unexpected(OperationPlanningProbeError::Unavailable);
    const auto destination_host = FindHost(_destination.provider_id);
    if( !destination_host )
        return std::unexpected(OperationPlanningProbeError::Unavailable);
    nc::vfs::Host &host = *host_ptr;
    if( !host.IsNativeFS() )
        return std::unexpected(OperationPlanningProbeError::Unsupported);

    const auto root_stat = host.Stat(_source.absolute_path, VFSFlags::F_NoFollow, SanitizedCancelChecker());
    if( IsCancelled() )
        return std::unexpected(OperationPlanningProbeError::Cancelled);
    if( !root_stat )
        return std::unexpected(MapError(host, root_stat.error()));
    if( !root_stat->meaning.mode )
        return std::unexpected(OperationPlanningProbeError::UnsupportedItem);
    if( !S_ISDIR(root_stat->mode) ) {
        if( S_ISREG(root_stat->mode) ||
            (S_ISLNK(root_stat->mode) &&
             (host.Features() & nc::vfs::HostFeatures::ReadSymlink) != 0) ) {
            if( !root_stat->meaning.size )
                return std::unexpected(OperationPlanningProbeError::Unsupported);
            return OperationPlanningEstimateEvidence{
                .files = 1,
                .bytes = root_stat->size,
                .contains_symlinks = S_ISLNK(root_stat->mode),
            };
        }
        return std::unexpected(OperationPlanningProbeError::UnsupportedItem);
    }

    OperationPlanningEstimateEvidence estimate;
    std::vector<std::string> directories{_source.absolute_path};
    for( size_t directory_index = 0; directory_index < directories.size(); ++directory_index ) {
        if( IsCancelled() )
            return std::unexpected(OperationPlanningProbeError::Cancelled);

        const std::string current_directory = directories[directory_index];
        std::vector<std::string> entry_paths;
        std::optional<OperationPlanningProbeError> entry_error;
        const auto iteration = host.IterateDirectoryListing(
            current_directory,
            [&](const VFSDirEnt &_entry) {
                if( IsCancelled() ) {
                    entry_error = OperationPlanningProbeError::Cancelled;
                    return false;
                }
                if( _entry.name.empty() || _entry.name.find('/') != std::string_view::npos ) {
                    entry_error = OperationPlanningProbeError::Failed;
                    return false;
                }
                entry_paths.emplace_back(ChildPath(current_directory, _entry.name));
                return true;
            });

        if( entry_error )
            return std::unexpected(*entry_error);
        if( !iteration )
            return std::unexpected(MapError(host, iteration.error()));
        if( IsCancelled() )
            return std::unexpected(OperationPlanningProbeError::Cancelled);

        for( std::string &child_path : entry_paths ) {
            if( _source.provider_id != _destination.provider_id ) {
                const auto filename = Filename(child_path);
                if( !filename || !destination_host->ValidateFilename(*filename) )
                    return std::unexpected(OperationPlanningProbeError::InvalidName);
            }
            if( IsCancelled() )
                return std::unexpected(OperationPlanningProbeError::Cancelled);
            const auto child_stat = host.Stat(child_path, VFSFlags::F_NoFollow, SanitizedCancelChecker());
            if( IsCancelled() )
                return std::unexpected(OperationPlanningProbeError::Cancelled);
            if( !child_stat )
                return std::unexpected(MapError(host, child_stat.error()));
            if( !child_stat->meaning.mode )
                return std::unexpected(OperationPlanningProbeError::UnsupportedItem);
            if( S_ISDIR(child_stat->mode) ) {
                directories.emplace_back(std::move(child_path));
                continue;
            }
            const bool is_copyable_leaf = S_ISREG(child_stat->mode) ||
                                          (S_ISLNK(child_stat->mode) &&
                                           (host.Features() & nc::vfs::HostFeatures::ReadSymlink) != 0);
            if( !is_copyable_leaf )
                return std::unexpected(OperationPlanningProbeError::UnsupportedItem);
            if( estimate.files == std::numeric_limits<uint64_t>::max() )
                return std::unexpected(OperationPlanningProbeError::Failed);
            ++estimate.files;
            if( !child_stat->meaning.size )
                return std::unexpected(OperationPlanningProbeError::Unsupported);
            if( child_stat->size > std::numeric_limits<uint64_t>::max() - estimate.bytes )
                return std::unexpected(OperationPlanningProbeError::Failed);
            estimate.bytes += child_stat->size;
            estimate.contains_symlinks = estimate.contains_symlinks || S_ISLNK(child_stat->mode);
            if( IsCancelled() )
                return std::unexpected(OperationPlanningProbeError::Cancelled);
        }
    }
    return estimate;
}

OperationPlanningProbeResult<OperationPlanningSpaceEvidence>
VFSOperationPlanningProbes::ProbeSpace(const OperationPlanningPath &_destination_directory)
{
    return Contain<OperationPlanningSpaceEvidence>([&] { return ProbeSpaceImpl(_destination_directory); });
}

OperationPlanningProbeResult<OperationPlanningSpaceEvidence>
VFSOperationPlanningProbes::ProbeSpaceImpl(const OperationPlanningPath &_destination_directory)
{
    if( IsCancelled() )
        return std::unexpected(OperationPlanningProbeError::Cancelled);
    const auto host = FindHost(_destination_directory.provider_id);
    if( !host )
        return std::unexpected(OperationPlanningProbeError::Unavailable);

    const auto stat = host->StatFS(_destination_directory.absolute_path, SanitizedCancelChecker());
    if( IsCancelled() )
        return std::unexpected(OperationPlanningProbeError::Cancelled);
    if( !stat )
        return std::unexpected(MapError(*host, stat.error()));
    if( stat->total_bytes == 0 && stat->free_bytes == 0 && stat->avail_bytes == 0 )
        return OperationPlanningSpaceEvidence{.available_bytes = std::nullopt};
    return OperationPlanningSpaceEvidence{.available_bytes = stat->avail_bytes};
}

std::shared_ptr<nc::vfs::Host>
VFSOperationPlanningProbes::FindHost(std::string_view _provider_id) const noexcept
{
    return m_Bindings->Resolve(_provider_id);
}

bool VFSOperationPlanningProbes::IsCancelled() const noexcept
{
    if( !m_CancelChecker )
        return false;
    try {
        return m_CancelChecker();
    }
    catch( ... ) {
        return true;
    }
}

std::function<bool()> VFSOperationPlanningProbes::SanitizedCancelChecker() const
{
    if( !m_CancelChecker )
        return {};
    return [this] { return IsCancelled(); };
}

} // namespace nc::ops
