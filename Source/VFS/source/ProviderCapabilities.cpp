// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include <VFS/ProviderCapabilities.h>
#include <VFS/Host.h>

#include <cerrno>
#include <mutex>
#include <optional>
#include <variant>
#include <string_view>
#include <sys/stat.h>
#include <utility>

namespace nc::vfs {
namespace {

bool ProviderConditionalCopyTimestampIsValid(const ProviderConditionalCopyTimestamp &_timestamp) noexcept
{
    return _timestamp.nanoseconds >= 0 && _timestamp.nanoseconds < 1'000'000'000;
}

bool ProviderConditionalCopyPathIsCanonical(std::string_view _path, bool _allow_root) noexcept
{
    if( _path.empty() || _path.front() != '/' )
        return false;
    if( _path.find('\0') != std::string_view::npos )
        return false;
    if( _path == "/" )
        return _allow_root;
    if( _path.back() == '/' )
        return false;

    size_t component_begin = 1;
    while( component_begin < _path.size() ) {
        const auto separator = _path.find('/', component_begin);
        const auto component = _path.substr(
            component_begin,
            separator == std::string_view::npos ? _path.size() - component_begin : separator - component_begin);
        if( component.empty() || component == "." || component == ".." )
            return false;
        if( separator == std::string_view::npos )
            break;
        component_begin = separator + 1;
    }
    return true;
}

bool ProviderConditionalCopyDestinationIsExactChild(std::string_view _parent, std::string_view _destination) noexcept
{
    if( _parent == "/" ) {
        return _destination.size() > 1 && _destination.find('/', 1) == std::string_view::npos;
    }
    return _destination.size() > _parent.size() + 1 && _destination.starts_with(_parent) &&
           _destination[_parent.size()] == '/' &&
           _destination.find('/', _parent.size() + 1) == std::string_view::npos;
}

bool ProviderConditionalCopyExpectationIsValid(const ProviderConditionalCopyExistingExpectation &_expectation,
                                               ProviderConditionalCopyExpectedKind _required_kind,
                                               mode_t _required_type,
                                               bool _allow_root) noexcept
{
    return _expectation.kind == _required_kind &&
           ProviderConditionalCopyPathIsCanonical(_expectation.absolute_path, _allow_root) &&
           (static_cast<mode_t>(_expectation.mode) & S_IFMT) == _required_type &&
           ProviderConditionalCopyTimestampIsValid(_expectation.birth_time) &&
           ProviderConditionalCopyTimestampIsValid(_expectation.modification_time) &&
           ProviderConditionalCopyTimestampIsValid(_expectation.status_change_time);
}

bool ProviderConditionalCopyBindingsAreLocallyConsistent(
    const ProviderConditionalCopyBinding &_source,
    const ProviderConditionalCopyBinding &_destination) noexcept
{
    if( _source.provider_id.empty() || !_source.host || _destination.provider_id.empty() || !_destination.host )
        return false;
    const bool same_id = _source.provider_id == _destination.provider_id;
    const bool same_host = _source.host == _destination.host;
    return same_id == same_host;
}

bool ProviderConditionalCopyAuthorityIsValid(
    const Host &_provider,
    const ProviderConditionalCopyReviewedAuthority &_authority) noexcept
{
    const auto &claims = _authority.Claims();
    return _authority.HasReviewSeal() && !claims.plan_id.empty() &&
           ProviderConditionalCopyBindingsAreLocallyConsistent(claims.source_binding,
                                                                claims.destination_binding) &&
           claims.destination_binding.host.get() == &_provider &&
           ProviderConditionalCopyExpectationIsValid(claims.source,
                                                      ProviderConditionalCopyExpectedKind::RegularFile,
                                                      S_IFREG,
                                                      false) &&
           ProviderConditionalCopyExpectationIsValid(claims.destination_parent,
                                                      ProviderConditionalCopyExpectedKind::Directory,
                                                      S_IFDIR,
                                                      true) &&
           ProviderConditionalCopyPathIsCanonical(claims.destination.absolute_path, false) &&
           ProviderConditionalCopyDestinationIsExactChild(claims.destination_parent.absolute_path,
                                                          claims.destination.absolute_path) &&
           (claims.source_binding.host != claims.destination_binding.host ||
            (claims.source.absolute_path != claims.destination.absolute_path &&
             claims.source.absolute_path != claims.destination_parent.absolute_path));
}

bool ProviderConditionalMoveAuthorityIsValid(const Host &_provider,
                                             const ProviderConditionalMoveReviewedAuthority &_authority) noexcept
{
    const auto &claims = _authority.Claims();
    return _authority.HasReviewSeal() && !claims.plan_id.empty() &&
           ProviderConditionalCopyBindingsAreLocallyConsistent(claims.source_binding, claims.destination_binding) &&
           claims.destination_binding.host.get() == &_provider &&
           // A Move rewrites both directories, so both must be bound to this same provider - a Copy
           // only ever needed that of the destination, since it left the source's directory alone.
           claims.source_binding.host.get() == &_provider &&
           ProviderConditionalCopyExpectationIsValid(
               claims.source, ProviderConditionalCopyExpectedKind::RegularFile, S_IFREG, false) &&
           ProviderConditionalCopyExpectationIsValid(
               claims.source_parent, ProviderConditionalCopyExpectedKind::Directory, S_IFDIR, true) &&
           ProviderConditionalCopyExpectationIsValid(
               claims.destination_parent, ProviderConditionalCopyExpectedKind::Directory, S_IFDIR, true) &&
           // The source must be exactly the child of the parent it claims, for the same reason the
           // destination must: a rename names a directory and an entry in it, so a parent that does
           // not actually hold the source describes an operation nobody reviewed.
           ProviderConditionalCopyDestinationIsExactChild(claims.source_parent.absolute_path,
                                                         claims.source.absolute_path) &&
           ProviderConditionalCopyPathIsCanonical(claims.destination.absolute_path, false) &&
           ProviderConditionalCopyDestinationIsExactChild(claims.destination_parent.absolute_path,
                                                         claims.destination.absolute_path) &&
           // Moving something onto itself is not a move, and moving it onto either directory is not a
           // shape this can express at all.
           claims.source.absolute_path != claims.destination.absolute_path &&
           claims.source.absolute_path != claims.destination_parent.absolute_path &&
           claims.source.absolute_path != claims.source_parent.absolute_path;
}

bool ProviderConditionalCopyResultIsValid(const ProviderConditionalCopyCommitResult &_result) noexcept
{
    if( _result.system_error < 0 || _result.filesystem_sync_system_error < 0 )
        return false;

    switch( _result.filesystem_sync_status ) {
        case ProviderConditionalCopyFilesystemSyncStatus::NotAttempted:
        case ProviderConditionalCopyFilesystemSyncStatus::Confirmed:
            if( _result.filesystem_sync_system_error != 0 )
                return false;
            break;
        case ProviderConditionalCopyFilesystemSyncStatus::Failed:
            if( _result.filesystem_sync_system_error == 0 )
                return false;
            break;
        default:
            return false;
    }

    switch( _result.publication ) {
        case ProviderConditionalCopyPublicationState::NotPublished: {
            if( _result.filesystem_sync_status !=
                ProviderConditionalCopyFilesystemSyncStatus::NotAttempted ) {
                return false;
            }
            switch( _result.failure ) {
                case ProviderConditionalCopyCommitFailure::None:
                case ProviderConditionalCopyCommitFailure::FileSystemSyncFailed:
                case ProviderConditionalCopyCommitFailure::MetadataFailed:
                    return false;
                case ProviderConditionalCopyCommitFailure::Aborted:
                case ProviderConditionalCopyCommitFailure::Cancelled:
                    return _result.system_error == 0;
                case ProviderConditionalCopyCommitFailure::SourceStale:
                case ProviderConditionalCopyCommitFailure::DestinationParentStale:
                    return _result.system_error == ESTALE;
                case ProviderConditionalCopyCommitFailure::DestinationExists:
                    return _result.system_error == EEXIST;
                case ProviderConditionalCopyCommitFailure::ProviderFailure:
                    return _result.system_error != 0;
            }
            return false;
        }
        case ProviderConditionalCopyPublicationState::Unknown:
            return _result.failure == ProviderConditionalCopyCommitFailure::ProviderFailure &&
                   _result.system_error != 0 &&
                   _result.filesystem_sync_status ==
                       ProviderConditionalCopyFilesystemSyncStatus::NotAttempted;
        case ProviderConditionalCopyPublicationState::Published:
            switch( _result.failure ) {
                case ProviderConditionalCopyCommitFailure::None:
                    return _result.system_error == 0 &&
                           _result.filesystem_sync_status ==
                               ProviderConditionalCopyFilesystemSyncStatus::Confirmed;
                case ProviderConditionalCopyCommitFailure::MetadataFailed:
                    return _result.system_error != 0 &&
                           _result.filesystem_sync_status !=
                               ProviderConditionalCopyFilesystemSyncStatus::NotAttempted;
                case ProviderConditionalCopyCommitFailure::FileSystemSyncFailed:
                    return _result.system_error != 0 &&
                           _result.filesystem_sync_status ==
                               ProviderConditionalCopyFilesystemSyncStatus::Failed &&
                           _result.filesystem_sync_system_error == _result.system_error;
                case ProviderConditionalCopyCommitFailure::ProviderFailure:
                    return _result.system_error != 0 &&
                           _result.filesystem_sync_status !=
                               ProviderConditionalCopyFilesystemSyncStatus::NotAttempted;
                case ProviderConditionalCopyCommitFailure::Aborted:
                case ProviderConditionalCopyCommitFailure::Cancelled:
                case ProviderConditionalCopyCommitFailure::SourceStale:
                case ProviderConditionalCopyCommitFailure::DestinationParentStale:
                case ProviderConditionalCopyCommitFailure::DestinationExists:
                    return false;
            }
            return false;
    }
    return false;
}

ProviderConditionalCopyCommitResult ProviderConditionalCopyNotPublished(
    ProviderConditionalCopyCommitFailure _failure) noexcept
{
    return ProviderConditionalCopyCommitResult{
        .publication = ProviderConditionalCopyPublicationState::NotPublished,
        .failure = _failure,
    };
}

ProviderConditionalCopyCommitResult ProviderConditionalCopyUnknown() noexcept
{
    return ProviderConditionalCopyCommitResult{
        .publication = ProviderConditionalCopyPublicationState::Unknown,
        .failure = ProviderConditionalCopyCommitFailure::ProviderFailure,
        .system_error = EIO,
        .filesystem_sync_status = ProviderConditionalCopyFilesystemSyncStatus::NotAttempted,
        .filesystem_sync_system_error = 0,
    };
}

ProviderConditionalCopyPublicationState
ProviderConditionalCopyInvokeAbort(ProviderConditionalCopyTransaction::AbortHandler _abort) noexcept
{
    if( !_abort )
        return ProviderConditionalCopyPublicationState::Unknown;
    try {
        if( _abort() == ProviderConditionalCopyPublicationState::NotPublished )
            return ProviderConditionalCopyPublicationState::NotPublished;
    } catch( ... ) {
    }
    return ProviderConditionalCopyPublicationState::Unknown;
}

} // namespace

struct ProviderConditionalCopyTransaction::Impl final {
    enum class State : uint8_t {
        Pending,
        Committing,
        Consumed
    };

    Impl(ProviderConditionalCopyReviewedAuthority _authority,
         CommitHandler _commit,
         AbortHandler _abort) noexcept
        : authority{std::move(_authority)}, commit{std::move(_commit)}, abort{std::move(_abort)}
    {
    }

    Impl(ProviderConditionalMoveReviewedAuthority _authority,
         CommitHandler _commit,
         AbortHandler _abort) noexcept
        : authority{std::move(_authority)}, commit{std::move(_commit)}, abort{std::move(_abort)}
    {
    }

    /**
     * Held, never read. What the transaction owes the authority is that it was consumed - a move-only
     * value surrendered at Begin cannot be spent a second time - and that obligation is the same
     * whichever kind it is. The commit itself arrives as a handler, which is why a Move needs no
     * second transaction type: the one place the two operations differ is already a parameter.
     */
    std::variant<ProviderConditionalCopyReviewedAuthority, ProviderConditionalMoveReviewedAuthority> authority;
    CommitHandler commit;
    AbortHandler abort;
    State state{State::Pending};
    std::optional<ProviderConditionalCopyCommitResult> terminal_result;
    mutable std::mutex mutex;
};

ProviderConditionalCopyTransaction::ProviderConditionalCopyTransaction(std::unique_ptr<Impl> _impl) noexcept
    : m_Impl{std::move(_impl)}
{
}

ProviderConditionalCopyTransaction::ProviderConditionalCopyTransaction(
    ProviderConditionalCopyTransaction &&_other) noexcept = default;

ProviderConditionalCopyTransaction &ProviderConditionalCopyTransaction::operator=(
    ProviderConditionalCopyTransaction &&_other) noexcept
{
    if( this != &_other ) {
        Reset();
        m_Impl = std::move(_other.m_Impl);
    }
    return *this;
}

ProviderConditionalCopyTransaction::~ProviderConditionalCopyTransaction()
{
    Reset();
}

std::expected<std::unique_ptr<ProviderConditionalCopyTransaction>,
              ProviderConditionalCopyTransactionBeginError>
ProviderConditionalCopyTransaction::Mint(const Host &_provider,
                                         ProviderConditionalCopyReviewedAuthority _authority,
                                         CommitHandler _commit,
                                         AbortHandler _abort) noexcept
{
    if( !ProviderConditionalCopyAuthorityIsValid(_provider, _authority) || !_commit || !_abort ) {
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::InvalidRequest);
    }
    try {
        auto impl = std::make_unique<Impl>(
            std::move(_authority), std::move(_commit), std::move(_abort));
        return std::unique_ptr<ProviderConditionalCopyTransaction>{
            new ProviderConditionalCopyTransaction{std::move(impl)}};
    } catch( ... ) {
        return std::unexpected(ProviderConditionalCopyTransactionBeginError::ProviderFailure);
    }
}

std::expected<std::unique_ptr<ProviderConditionalCopyTransaction>, ProviderConditionalMoveTransactionBeginError>
ProviderConditionalCopyTransaction::MintForMove(const Host &_provider,
                                                ProviderConditionalMoveReviewedAuthority _authority,
                                                CommitHandler _commit,
                                                AbortHandler _abort) noexcept
{
    if( !ProviderConditionalMoveAuthorityIsValid(_provider, _authority) || !_commit || !_abort ) {
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::InvalidRequest);
    }
    try {
        auto impl = std::make_unique<Impl>(std::move(_authority), std::move(_commit), std::move(_abort));
        return std::unique_ptr<ProviderConditionalCopyTransaction>{
            new ProviderConditionalCopyTransaction{std::move(impl)}};
    } catch( ... ) {
        return std::unexpected(ProviderConditionalMoveTransactionBeginError::ProviderFailure);
    }
}

bool ProviderConditionalCopyTransaction::IsPending() const noexcept
{
    if( !m_Impl )
        return false;
    const auto lock = std::lock_guard{m_Impl->mutex};
    return m_Impl->state == Impl::State::Pending;
}

ProviderConditionalCopyCommitResult ProviderConditionalCopyTransaction::Commit(
    const CancelChecker &_cancel_checker) noexcept
{
    if( !m_Impl )
        return ProviderConditionalCopyUnknown();

    CommitHandler commit;
    AbortHandler abort;
    {
        const auto lock = std::lock_guard{m_Impl->mutex};
        if( m_Impl->state == Impl::State::Committing )
            return ProviderConditionalCopyUnknown();
        if( m_Impl->state == Impl::State::Consumed )
            return m_Impl->terminal_result.value_or(ProviderConditionalCopyUnknown());
        m_Impl->state = Impl::State::Committing;
        commit = std::move(m_Impl->commit);
        abort = std::move(m_Impl->abort);
    }

    const auto finish = [&](ProviderConditionalCopyCommitResult _result) noexcept {
        const auto lock = std::lock_guard{m_Impl->mutex};
        m_Impl->terminal_result = _result;
        m_Impl->state = Impl::State::Consumed;
        return _result;
    };

    try {
        if( _cancel_checker && _cancel_checker() ) {
            const auto publication = ProviderConditionalCopyInvokeAbort(std::move(abort));
            return finish(publication == ProviderConditionalCopyPublicationState::NotPublished
                              ? ProviderConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::Cancelled)
                              : ProviderConditionalCopyUnknown());
        }
    } catch( ... ) {
        const auto publication = ProviderConditionalCopyInvokeAbort(std::move(abort));
        return finish(publication == ProviderConditionalCopyPublicationState::NotPublished
                          ? ProviderConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::Cancelled)
                          : ProviderConditionalCopyUnknown());
    }

    try {
        auto result = commit(_cancel_checker);
        if( !ProviderConditionalCopyResultIsValid(result) )
            return finish(ProviderConditionalCopyUnknown());
        if( result.publication == ProviderConditionalCopyPublicationState::NotPublished &&
            ProviderConditionalCopyInvokeAbort(std::move(abort)) !=
                ProviderConditionalCopyPublicationState::NotPublished ) {
            return finish(ProviderConditionalCopyUnknown());
        }
        return finish(result);
    } catch( ... ) {
        return finish(ProviderConditionalCopyUnknown());
    }
}

ProviderConditionalCopyCommitResult ProviderConditionalCopyTransaction::Abort() noexcept
{
    if( !m_Impl )
        return ProviderConditionalCopyUnknown();

    AbortHandler abort;
    {
        const auto lock = std::lock_guard{m_Impl->mutex};
        if( m_Impl->state == Impl::State::Committing )
            return ProviderConditionalCopyUnknown();
        if( m_Impl->state == Impl::State::Consumed )
            return m_Impl->terminal_result.value_or(ProviderConditionalCopyUnknown());
        m_Impl->state = Impl::State::Committing;
        m_Impl->commit = {};
        abort = std::move(m_Impl->abort);
    }

    const auto publication = ProviderConditionalCopyInvokeAbort(std::move(abort));
    const auto result = publication == ProviderConditionalCopyPublicationState::NotPublished
                            ? ProviderConditionalCopyNotPublished(
                                  ProviderConditionalCopyCommitFailure::Aborted)
                            : ProviderConditionalCopyUnknown();
    const auto lock = std::lock_guard{m_Impl->mutex};
    m_Impl->terminal_result = result;
    m_Impl->state = Impl::State::Consumed;
    return result;
}

void ProviderConditionalCopyTransaction::Reset() noexcept
{
    if( !m_Impl )
        return;
    AbortHandler abort;
    {
        const auto lock = std::lock_guard{m_Impl->mutex};
        if( m_Impl->state == Impl::State::Pending ) {
            m_Impl->state = Impl::State::Committing;
            abort = std::move(m_Impl->abort);
        }
    }
    if( abort ) {
        const auto publication = ProviderConditionalCopyInvokeAbort(std::move(abort));
        const auto result = publication == ProviderConditionalCopyPublicationState::NotPublished
                                ? ProviderConditionalCopyNotPublished(ProviderConditionalCopyCommitFailure::Aborted)
                                : ProviderConditionalCopyUnknown();
        const auto lock = std::lock_guard{m_Impl->mutex};
        m_Impl->terminal_result = result;
        m_Impl->state = Impl::State::Consumed;
    }
    m_Impl.reset();
}

ProviderCapabilities ProviderCapabilitiesResolver::Resolve(Host &_host, std::string_view _path)
{
    ProviderCapabilities capabilities;
    capabilities.is_native = _host.IsNativeFS();
    capabilities.is_immutable = _host.IsImmutableFS();
    capabilities.is_case_sensitive =
        _path.empty() ? _host.IsCaseSensitiveAtPath() : _host.IsCaseSensitiveAtPath(_path);

    const uint64_t features = _host.Features();
    const auto has = [features](uint64_t _feature) { return (features & _feature) != 0; };

    capabilities.can_read = has(HostFeatures::Read);
    capabilities.can_generate_thumbnails = capabilities.can_read && _host.ShouldProduceThumbnails();
    capabilities.can_resolve_symlink = capabilities.can_read && has(HostFeatures::ReadSymlink);
    capabilities.can_watch_changes = !capabilities.is_immutable && !_path.empty() &&
                                     has(HostFeatures::ObserveDirectoryChanges) &&
                                     _host.IsDirectoryChangeObservationAvailable(_path);

    if( capabilities.is_immutable )
        return capabilities;

    const bool writable = _path.empty() ? _host.IsWritable() : _host.IsWritableAtPath(_path);
    if( !writable )
        return capabilities;

    capabilities.can_create_file = has(HostFeatures::CreateFile);
    capabilities.can_create_folder = has(HostFeatures::CreateDirectory);
    capabilities.can_create_symlink = has(HostFeatures::CreateSymlink);
    capabilities.can_rename = has(HostFeatures::Rename);
    capabilities.can_delete_permanently =
        has(HostFeatures::Unlink) && has(HostFeatures::RemoveDirectory);
    capabilities.can_trash = has(HostFeatures::Trash);
    capabilities.can_set_permissions = has(HostFeatures::SetPermissions);
    capabilities.can_set_owner_group = has(HostFeatures::SetOwnership);
    capabilities.can_set_times = has(HostFeatures::SetTimes);
    capabilities.can_write = capabilities.can_create_file || capabilities.can_create_folder ||
                             capabilities.can_create_symlink || capabilities.can_rename ||
                             capabilities.can_delete_permanently ||
                             capabilities.can_trash || capabilities.can_set_permissions ||
                             capabilities.can_set_owner_group || capabilities.can_set_times;
    return capabilities;
}

} // namespace nc::vfs
