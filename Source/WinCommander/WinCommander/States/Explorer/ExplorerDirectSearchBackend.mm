// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ExplorerDirectSearchBackend.h"
#include "SearchResultListingBuilder.h"

#include <Utility/FileMask.h>
#include <VFS/SearchForFiles.h>
#include <VFS/VFS.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <ranges>
#include <string_view>
#include <sys/stat.h>
#include <utility>

namespace nc::explorer {

namespace {

using core::SearchBackendKind;
using core::SearchBackendLimitation;
using core::SearchBackendSupport;
using core::SearchFailure;
using core::SearchFailureCode;
using core::SearchFileType;
using core::SearchNameMatch;
using core::SearchScope;
using nc::vfs::SearchForFiles;

[[nodiscard]] std::string EscapeRE2(const std::string_view _text)
{
    std::string escaped;
    escaped.reserve(_text.size() * 2);
    for( const char character : _text ) {
        switch( character ) {
            case '.':
            case '^':
            case '$':
            case '|':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '*':
            case '+':
            case '?':
            case '\\':
                escaped.push_back('\\');
                break;
            default:
                break;
        }
        escaped.push_back(character);
    }
    return escaped;
}

[[nodiscard]] std::string SearchNamePattern(const core::SearchRequest &_request)
{
    if( _request.query.empty() )
        return {};
    std::string escaped = EscapeRE2(_request.query);
    if( _request.filters.name_match == SearchNameMatch::Exact )
        return escaped;
    return ".*" + escaped + ".*";
}

[[nodiscard]] bool MatchesExtension(const std::string_view _filename, const std::string &_extension)
{
    const std::string pattern = ".*\\." + EscapeRE2(_extension);
    return nc::utility::FileMask{pattern, nc::utility::FileMask::Type::RegEx}.MatchName(_filename);
}

[[nodiscard]] bool IsPackageName(const std::string_view _filename)
{
    static constexpr std::string_view extensions[] = {".app", ".bundle", ".framework", ".plugin", ".pkg"};
    return std::ranges::any_of(extensions, [&](const std::string_view _extension) {
        return _filename.size() > _extension.size() &&
               nc::utility::FileMask{"*" + std::string{_extension}}.MatchName(_filename);
    });
}

[[nodiscard]] bool MatchesType(const SearchFileType _type, const std::string_view _filename, const VFSStat &_stat)
{
    const mode_t mode = _stat.mode;
    switch( _type ) {
        case SearchFileType::Any:
            return true;
        case SearchFileType::RegularFile:
            return S_ISREG(mode);
        case SearchFileType::Directory:
            return S_ISDIR(mode) && !IsPackageName(_filename);
        case SearchFileType::SymbolicLink:
            return S_ISLNK(mode);
        case SearchFileType::Package:
            return S_ISDIR(mode) && IsPackageName(_filename);
        case SearchFileType::Other:
            return !S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode);
    }
    return false;
}

[[nodiscard]] bool IsPermissionDenied(const Error &_error) noexcept
{
    return _error.Domain() == Error::POSIX && (_error.Code() == EACCES || _error.Code() == EPERM);
}

[[nodiscard]] bool IsMissing(const Error &_error) noexcept
{
    return _error.Domain() == Error::POSIX &&
           (_error.Code() == ENOENT || _error.Code() == ENOTDIR || _error.Code() == ESTALE);
}

[[nodiscard]] std::string FailureDetail(const Error &_error)
{
    std::string detail = _error.LocalizedFailureReason();
    if( detail.empty() )
        detail = _error.Description();
    if( detail.empty() )
        detail = "Search backend failed";
    return detail;
}

class DirectSearchRun final : public ExplorerSearchBackendRun, public std::enable_shared_from_this<DirectSearchRun>
{
public:
    DirectSearchRun(ExplorerSearchBackendInput _input,
                    ExplorerSearchBackend::ProgressCallback _progress,
                    ExplorerSearchBackend::CompletionCallback _completion) :
        m_Input(std::move(_input)), m_Progress(std::move(_progress)), m_Completion(std::move(_completion))
    {
    }

    [[nodiscard]] bool Start()
    {
        if( m_Input.plan.backend.kind != SearchBackendKind::DirectScan ||
            m_Input.plan.backend.support != SearchBackendSupport::Supported || !m_Input.origin_host ||
            m_Input.plan.execution_root.empty() || m_Input.limits.maximum_results == 0 ||
            m_Input.limits.maximum_path_bytes == 0 || !m_Progress || !m_Completion )
            return false;

        const core::SearchRequest &request = m_Input.plan.request;
        if( const std::string pattern = SearchNamePattern(request); !pattern.empty() )
            m_Search.SetFilterName(nc::utility::FileMask{pattern, nc::utility::FileMask::Type::RegEx});
        if( request.filters.content ) {
            SearchForFiles::FilterContent filter;
            filter.text = *request.filters.content;
            m_Search.SetFilterContent(filter);
        }

        int options = SearchForFiles::Options::SearchForDirs | SearchForFiles::Options::SearchForFiles;
        if( request.scope == SearchScope::Recursive || request.scope == SearchScope::CurrentDisk )
            options |= SearchForFiles::Options::GoIntoSubDirs;

        const std::weak_ptr<DirectSearchRun> weak_self = shared_from_this();
        return m_Search.Go(
            m_Input.plan.execution_root,
            m_Input.origin_host,
            options,
            [weak_self](const std::string_view _filename,
                        const char *_in_path,
                        VFSHost &_in_host,
                        CFRange) {
                if( const auto self = weak_self.lock() )
                    self->Found(_filename, _in_path, _in_host);
            },
            [weak_self] {
                if( const auto self = weak_self.lock() )
                    self->Finish();
            },
            [weak_self](const char *_path, VFSHost &_in_host) {
                if( const auto self = weak_self.lock() )
                    self->LookingIn(_path, _in_host);
            },
            nullptr,
            [weak_self](const SearchForFiles::SkippedLocation &_location) {
                if( const auto self = weak_self.lock() )
                    self->Skipped(_location);
            },
            [weak_self](const std::string_view _path, const VFSDirEnt &_dirent, VFSHost &_host) {
                if( const auto self = weak_self.lock() )
                    return self->ShouldDescend(_path, _dirent, _host);
                return false;
            });
    }

    void Stop() noexcept override
    {
        m_CancellationRequested.store(true, std::memory_order_release);
        m_Search.Stop();
    }

    void Wait() noexcept override
    {
        try {
            m_Search.Wait();
        } catch( ... ) {
        }
    }

private:
    void LookingIn(const std::string_view _path, VFSHost &_host)
    {
        if( m_Input.plan.request.scope == SearchScope::CurrentDisk && !m_RootDevice && !m_RootError &&
            _path == m_Input.plan.execution_root ) {
            const std::expected<VFSStat, Error> stat = _host.Stat(_path, VFSFlags::F_NoFollow);
            if( stat && stat->meaning.dev )
                m_RootDevice = stat->dev;
            else {
                m_RootError = stat ? Error{Error::POSIX, EIO} : stat.error();
                m_Search.Stop();
            }
        }

        ++m_ScannedCount;
        m_Progress(ExplorerSearchBackendProgress{
            .current_location = std::string{_path},
            .scanned_count = m_ScannedCount,
            .found_count = m_FoundCount,
        });
    }

    [[nodiscard]] bool ShouldDescend(const std::string_view _full_path,
                                     const VFSDirEnt &_dirent,
                                     VFSHost &_host)
    {
        if( !m_Input.plan.request.filters.include_hidden && !_dirent.name.empty() && _dirent.name.front() == '.' )
            return false;
        const bool needs_stat = !m_Input.plan.request.filters.include_hidden ||
                                m_Input.plan.request.scope == SearchScope::CurrentDisk;
        if( !needs_stat )
            return true;
        const std::expected<VFSStat, Error> stat = _host.Stat(_full_path, VFSFlags::F_NoFollow);
        if( !stat ) {
            if( IsPermissionDenied(stat.error()) )
                m_PermissionLimited = true;
            else {
                m_NonPermissionDescendantError = stat.error();
                m_Search.Stop();
            }
            return false;
        }
        if( !m_Input.plan.request.filters.include_hidden && stat->meaning.flags &&
            (stat->flags & UF_HIDDEN) != 0 )
            return false;
        if( m_Input.plan.request.scope != SearchScope::CurrentDisk )
            return true;
        if( !m_RootDevice )
            return false;
        if( !stat->meaning.dev ) {
            m_NonPermissionDescendantError = Error{Error::POSIX, EIO};
            m_Search.Stop();
            return false;
        }
        return stat->dev == *m_RootDevice;
    }

    void Found(const std::string_view _filename, const std::string_view _directory, VFSHost &_host)
    {
        if( m_LimitReached || m_NonPermissionDescendantError || m_RootError )
            return;
        const core::SearchFilters &filters = m_Input.plan.request.filters;
        if( filters.extension && !MatchesExtension(_filename, *filters.extension) )
            return;

        std::string full_path{_directory};
        if( full_path.empty() || full_path.back() != '/' )
            full_path.push_back('/');
        full_path.append(_filename);

        const bool needs_stat = filters.file_type != SearchFileType::Any || filters.size.minimum_bytes ||
                                filters.size.maximum_bytes || filters.modified.earliest_seconds ||
                                filters.modified.latest_seconds || !filters.include_hidden;
        std::optional<VFSStat> stat;
        if( needs_stat ) {
            const std::expected<VFSStat, Error> result = _host.Stat(full_path, VFSFlags::F_NoFollow);
            if( !result ) {
                if( IsPermissionDenied(result.error()) )
                    m_PermissionLimited = true;
                else if( !IsMissing(result.error()) ) {
                    m_NonPermissionDescendantError = result.error();
                    m_Search.Stop();
                }
                return;
            }
            stat = *result;
        }

        if( !filters.include_hidden &&
            ((!_filename.empty() && _filename.front() == '.') ||
             (stat && stat->meaning.flags && (stat->flags & UF_HIDDEN) != 0)) )
            return;
        if( filters.file_type != SearchFileType::Any && (!stat || !stat->meaning.mode ||
                                                         !MatchesType(filters.file_type, _filename, *stat)) )
            return;
        if( filters.size.minimum_bytes && (!stat || !stat->meaning.size || stat->size < *filters.size.minimum_bytes) )
            return;
        if( filters.size.maximum_bytes && (!stat || !stat->meaning.size || stat->size > *filters.size.maximum_bytes) )
            return;
        if( filters.modified.earliest_seconds &&
            (!stat || !stat->meaning.mtime || stat->mtime.tv_sec < *filters.modified.earliest_seconds) )
            return;
        if( filters.modified.latest_seconds &&
            (!stat || !stat->meaning.mtime || stat->mtime.tv_sec > *filters.modified.latest_seconds) )
            return;

        if( m_Paths.size() >= m_Input.limits.maximum_results ||
            full_path.size() > m_Input.limits.maximum_path_bytes - m_PathBytes ) {
            m_LimitReached = true;
            m_Search.Stop();
            return;
        }
        m_PathBytes += full_path.size();
        m_Paths.emplace_back(_host.SharedPtr(), std::move(full_path));
        m_FoundCount = m_Paths.size();
        m_Progress(ExplorerSearchBackendProgress{.found_count = m_FoundCount});
    }

    void Skipped(const SearchForFiles::SkippedLocation &_location)
    {
        if( _location.context == SearchForFiles::SkippedLocation::Context::Root ) {
            m_RootError = _location.error;
            m_Search.Stop();
            return;
        }
        if( IsPermissionDenied(_location.error) ) {
            m_PermissionLimited = true;
            return;
        }
        m_NonPermissionDescendantError = _location.error;
        m_Search.Stop();
    }

    void Finish()
    {
        ExplorerSearchBackendCompletion completion;
        if( m_CancellationRequested.load(std::memory_order_acquire) && !m_LimitReached ) {
            completion.kind = ExplorerSearchBackendCompletionKind::Cancelled;
            Complete(std::move(completion));
            return;
        }
        if( m_RootError || m_NonPermissionDescendantError ) {
            const Error &error = m_RootError ? *m_RootError : *m_NonPermissionDescendantError;
            completion.kind = ExplorerSearchBackendCompletionKind::Failed;
            completion.failure = SearchFailure{
                .code = IsPermissionDenied(error) ? SearchFailureCode::PermissionDenied
                                                  : SearchFailureCode::ExecutionFailed,
                .detail = FailureDetail(error),
            };
            Complete(std::move(completion));
            return;
        }

        SearchResultListingBuildOptions options;
        options.fetch_flags = m_Input.fetch_flags;
        options.maximum_results = m_Input.limits.maximum_results;
        options.maximum_path_bytes = m_Input.limits.maximum_path_bytes;
        const auto cancelled = [this] {
            return m_CancellationRequested.load(std::memory_order_acquire) && !m_LimitReached;
        };
        SearchResultListingBuildResult result = BuildSearchResultListing(std::move(m_Paths), options, cancelled);
        if( result.status == SearchResultListingBuildStatus::Cancelled ) {
            completion.kind = ExplorerSearchBackendCompletionKind::Cancelled;
            Complete(std::move(completion));
            return;
        }
        if( result.status != SearchResultListingBuildStatus::Completed || !result.listing ) {
            completion.kind = ExplorerSearchBackendCompletionKind::Failed;
            completion.failure = SearchFailure{
                .code = SearchFailureCode::InvalidBackendReply,
                .detail = "Search results could not be built",
            };
            Complete(std::move(completion));
            return;
        }
        if( result.failed_count != 0 ) {
            completion.kind = ExplorerSearchBackendCompletionKind::Failed;
            completion.failure = SearchFailure{
                .code = SearchFailureCode::ExecutionFailed,
                .detail = result.first_failure ? FailureDetail(*result.first_failure)
                                               : "Search result materialization failed",
            };
            Complete(std::move(completion));
            return;
        }

        completion.listing = std::move(result.listing);
        completion.accepted_count = result.accepted_count;
        if( result.permission_denied_count != 0 )
            completion.limitations.emplace_back(SearchBackendLimitation::PermissionDeniedLocations);
        if( m_PermissionLimited &&
            std::ranges::find(completion.limitations,
                              SearchBackendLimitation::PermissionDeniedLocations) == completion.limitations.end() )
            completion.limitations.emplace_back(SearchBackendLimitation::PermissionDeniedLocations);
        if( result.missing_count != 0 )
            completion.limitations.emplace_back(SearchBackendLimitation::ResultPathsUnavailable);
        if( m_LimitReached || result.limit_reached )
            completion.kind = ExplorerSearchBackendCompletionKind::TooManyResults;
        else if( m_PermissionLimited || result.permission_denied_count != 0 ) {
            completion.kind = ExplorerSearchBackendCompletionKind::PermissionLimited;
        }
        else if( result.missing_count != 0 )
            completion.kind = ExplorerSearchBackendCompletionKind::Partial;
        else
            completion.kind = ExplorerSearchBackendCompletionKind::Completed;
        Complete(std::move(completion));
    }

    void Complete(ExplorerSearchBackendCompletion _completion)
    {
        if( m_DidComplete.exchange(true, std::memory_order_acq_rel) )
            return;
        ExplorerSearchBackend::CompletionCallback callback = std::move(m_Completion);
        m_Progress = {};
        if( callback )
            callback(std::move(_completion));
    }

    ExplorerSearchBackendInput m_Input;
    ExplorerSearchBackend::ProgressCallback m_Progress;
    ExplorerSearchBackend::CompletionCallback m_Completion;
    SearchForFiles m_Search;
    std::atomic_bool m_CancellationRequested = false;
    std::atomic_bool m_DidComplete = false;
    std::vector<nc::vfs::VFSPath> m_Paths;
    size_t m_PathBytes = 0;
    uint64_t m_ScannedCount = 0;
    uint64_t m_FoundCount = 0;
    bool m_LimitReached = false;
    bool m_PermissionLimited = false;
    std::optional<int32_t> m_RootDevice;
    std::optional<Error> m_RootError;
    std::optional<Error> m_NonPermissionDescendantError;
};

} // namespace

std::shared_ptr<ExplorerSearchBackendRun>
ExplorerDirectSearchBackend::Start(ExplorerSearchBackendInput _input,
                                   ProgressCallback _progress,
                                   CompletionCallback _completion)
{
    auto run = std::make_shared<DirectSearchRun>(std::move(_input), std::move(_progress), std::move(_completion));
    if( !run->Start() )
        return {};
    return run;
}

} // namespace nc::explorer
