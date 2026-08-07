// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "SearchPlanning.h"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string_view>

namespace nc::core {

namespace {

[[nodiscard]] std::string Trim(std::string _value)
{
    const auto is_space = [](const unsigned char _character) { return std::isspace(_character) != 0; };
    const auto first = std::ranges::find_if_not(_value, is_space);
    const auto last = std::ranges::find_if_not(_value | std::views::reverse, is_space).base();
    if( first >= last )
        return {};
    return {first, last};
}

template <class T>
void TrimOptional(std::optional<T> &_value)
{
    if( !_value )
        return;
    *_value = Trim(std::move(*_value));
    if( _value->empty() )
        _value.reset();
}

void AddLimitation(SearchBackendDescriptor &_backend, const SearchBackendLimitation _limitation)
{
    if( std::ranges::find(_backend.limitations, _limitation) == _backend.limitations.end() )
        _backend.limitations.emplace_back(_limitation);
}

[[nodiscard]] bool HasLimitation(const SearchBackendDescriptor &_backend,
                                 const SearchBackendLimitation _limitation) noexcept
{
    return std::ranges::find(_backend.limitations, _limitation) != _backend.limitations.end();
}

[[nodiscard]] bool IsUnsupportedLimitation(const SearchBackendLimitation _limitation) noexcept
{
    switch( _limitation ) {
        case SearchBackendLimitation::RecursiveScopeUnavailable:
        case SearchBackendLimitation::CurrentDiskScopeUnavailable:
        case SearchBackendLimitation::WholeMacScopeRequiresSpotlight:
        case SearchBackendLimitation::ContentSearchUnavailable:
        case SearchBackendLimitation::MetadataSearchUnavailable:
        case SearchBackendLimitation::HiddenItemsUnavailable:
            return true;
        case SearchBackendLimitation::FullDiskAccessMissing:
        case SearchBackendLimitation::PermissionDeniedLocations:
        case SearchBackendLimitation::ResultPathsUnavailable:
        case SearchBackendLimitation::ProviderUnavailable:
        case SearchBackendLimitation::SpotlightUnavailable:
        case SearchBackendLimitation::SpotlightIndexUnavailable:
            return false;
    }
    return false;
}

void MarkUnsupported(SearchBackendDescriptor &_backend, const SearchBackendLimitation _limitation)
{
    if( _backend.support == SearchBackendSupport::Supported )
        _backend.support = SearchBackendSupport::Unsupported;
    AddLimitation(_backend, _limitation);
}

[[nodiscard]] bool UsesMetadata(const SearchRequest &_request) noexcept
{
    return _request.filters.file_type != SearchFileType::Any || _request.filters.size.minimum_bytes ||
           _request.filters.size.maximum_bytes || _request.filters.modified.earliest_seconds ||
           _request.filters.modified.latest_seconds;
}

[[nodiscard]] bool HasCriteria(const SearchRequest &_request) noexcept
{
    return !_request.query.empty() || _request.filters.extension || _request.filters.file_type != SearchFileType::Any ||
           _request.filters.size.minimum_bytes || _request.filters.size.maximum_bytes ||
           _request.filters.modified.earliest_seconds || _request.filters.modified.latest_seconds ||
           _request.filters.content;
}

[[nodiscard]] SearchBackendCapabilities DirectCapabilities(const SearchPlanningFacts &_facts) noexcept
{
    return {
        .name = true,
        .extension = true,
        .type = _facts.provider_supports_metadata,
        .size = _facts.provider_supports_metadata,
        .modified = _facts.provider_supports_metadata,
        .content = _facts.provider_supports_content,
        .hidden_items = _facts.provider_supports_hidden_items,
        .recursive_scope = _facts.provider_supports_recursive,
        .current_disk_scope = _facts.provider_is_native && _facts.provider_supports_current_disk,
        .whole_mac_scope = false,
        .determinate_progress = _facts.provider_reports_determinate_progress,
    };
}

[[nodiscard]] SearchBackendCapabilities SpotlightCapabilities(const SearchPlanningFacts &_facts) noexcept
{
    return {
        .name = true,
        .extension = true,
        .type = true,
        .size = true,
        .modified = true,
        .content = _facts.spotlight_supports_content,
        .hidden_items = true,
        .recursive_scope = true,
        .current_disk_scope = true,
        .whole_mac_scope = true,
        .determinate_progress = false,
    };
}

} // namespace

std::expected<SearchRequest, SearchPlanningFailure> SearchPlanning::Normalize(SearchRequest _request)
{
    _request.query = Trim(std::move(_request.query));
    TrimOptional(_request.filters.extension);
    TrimOptional(_request.filters.content);

    if( _request.filters.extension ) {
        while( !_request.filters.extension->empty() && _request.filters.extension->front() == '.' )
            _request.filters.extension->erase(_request.filters.extension->begin());
        if( _request.filters.extension->empty() || _request.filters.extension->find('/') != std::string::npos )
            return std::unexpected(SearchPlanningFailure::InvalidExtension);
    }

    if( _request.filters.size.minimum_bytes && _request.filters.size.maximum_bytes &&
        *_request.filters.size.minimum_bytes > *_request.filters.size.maximum_bytes )
        return std::unexpected(SearchPlanningFailure::InvalidSizeRange);

    if( _request.filters.modified.earliest_seconds && _request.filters.modified.latest_seconds &&
        *_request.filters.modified.earliest_seconds > *_request.filters.modified.latest_seconds )
        return std::unexpected(SearchPlanningFailure::InvalidModifiedRange);

    if( !HasCriteria(_request) )
        return std::unexpected(SearchPlanningFailure::EmptyCriteria);

    return _request;
}

SearchPlanning::Result SearchPlanning::Plan(SearchRequest _request, SearchPlanningFacts _facts)
{
    auto normalized = Normalize(std::move(_request));
    if( !normalized )
        return std::unexpected(normalized.error());

    _facts.current_folder = Trim(std::move(_facts.current_folder));
    if( _facts.current_disk_root ) {
        *_facts.current_disk_root = Trim(std::move(*_facts.current_disk_root));
        if( _facts.current_disk_root->empty() )
            _facts.current_disk_root.reset();
    }

    SearchPlan plan;
    plan.request = std::move(*normalized);

    if( plan.request.scope == SearchScope::SpotlightWholeMac ) {
        plan.backend.kind = SearchBackendKind::Spotlight;
        plan.backend.capabilities = SpotlightCapabilities(_facts);
        plan.backend.support = SearchBackendSupport::Supported;

        if( !_facts.spotlight_available ) {
            plan.backend.support = SearchBackendSupport::Unavailable;
            AddLimitation(plan.backend, SearchBackendLimitation::SpotlightUnavailable);
        }
        else if( !_facts.spotlight_index_available ) {
            plan.backend.support = SearchBackendSupport::IndexUnavailable;
            AddLimitation(plan.backend, SearchBackendLimitation::SpotlightIndexUnavailable);
        }

        if( !_facts.full_disk_access )
            AddLimitation(plan.backend, SearchBackendLimitation::FullDiskAccessMissing);
    }
    else {
        if( _facts.current_folder.empty() )
            return std::unexpected(SearchPlanningFailure::MissingCurrentFolder);

        plan.backend.kind = SearchBackendKind::DirectScan;
        plan.backend.capabilities = DirectCapabilities(_facts);
        plan.backend.support = SearchBackendSupport::Supported;
        plan.execution_root = _facts.current_folder;

        if( !_facts.provider_available ) {
            plan.backend.support = SearchBackendSupport::Unavailable;
            AddLimitation(plan.backend, SearchBackendLimitation::ProviderUnavailable);
        }

        if( plan.request.scope == SearchScope::Recursive && !plan.backend.capabilities.recursive_scope )
            MarkUnsupported(plan.backend, SearchBackendLimitation::RecursiveScopeUnavailable);

        if( plan.request.scope == SearchScope::CurrentDisk ) {
            if( !_facts.current_disk_root )
                return std::unexpected(SearchPlanningFailure::MissingCurrentDiskRoot);
            plan.execution_root = *_facts.current_disk_root;
            if( !plan.backend.capabilities.current_disk_scope )
                MarkUnsupported(plan.backend, SearchBackendLimitation::CurrentDiskScopeUnavailable);
        }
    }

    if( plan.request.filters.content && !plan.backend.capabilities.content )
        MarkUnsupported(plan.backend, SearchBackendLimitation::ContentSearchUnavailable);
    if( UsesMetadata(plan.request) &&
        !(plan.backend.capabilities.type && plan.backend.capabilities.size && plan.backend.capabilities.modified) )
        MarkUnsupported(plan.backend, SearchBackendLimitation::MetadataSearchUnavailable);
    if( plan.request.filters.include_hidden && !plan.backend.capabilities.hidden_items )
        MarkUnsupported(plan.backend, SearchBackendLimitation::HiddenItemsUnavailable);

    return plan;
}

bool SearchPlanning::IsValid(const SearchPlan &_plan)
{
    const auto normalized = Normalize(_plan.request);
    if( !normalized || *normalized != _plan.request )
        return false;

    const bool spotlight_scope = _plan.request.scope == SearchScope::SpotlightWholeMac;
    if( spotlight_scope != (_plan.backend.kind == SearchBackendKind::Spotlight) )
        return false;
    if( !spotlight_scope && _plan.execution_root.empty() )
        return false;
    if( spotlight_scope && !_plan.execution_root.empty() )
        return false;
    if( !_plan.request.query.empty() && !_plan.backend.capabilities.name )
        return false;
    if( _plan.request.filters.extension && !_plan.backend.capabilities.extension )
        return false;
    if( spotlight_scope != _plan.backend.capabilities.whole_mac_scope )
        return false;

    for( size_t index = 0; index != _plan.backend.limitations.size(); ++index ) {
        if( std::ranges::find(_plan.backend.limitations.begin() + static_cast<std::ptrdiff_t>(index + 1),
                             _plan.backend.limitations.end(),
                             _plan.backend.limitations[index]) != _plan.backend.limitations.end() )
            return false;
    }
    for( const SearchBackendLimitation limitation : _plan.backend.limitations ) {
        switch( limitation ) {
            case SearchBackendLimitation::RecursiveScopeUnavailable:
                if( _plan.request.scope != SearchScope::Recursive ||
                    _plan.backend.capabilities.recursive_scope )
                    return false;
                break;
            case SearchBackendLimitation::CurrentDiskScopeUnavailable:
                if( _plan.request.scope != SearchScope::CurrentDisk ||
                    _plan.backend.capabilities.current_disk_scope )
                    return false;
                break;
            case SearchBackendLimitation::WholeMacScopeRequiresSpotlight:
                return false;
            case SearchBackendLimitation::ContentSearchUnavailable:
                if( !_plan.request.filters.content || _plan.backend.capabilities.content )
                    return false;
                break;
            case SearchBackendLimitation::MetadataSearchUnavailable:
                if( !UsesMetadata(_plan.request) ||
                    (_plan.backend.capabilities.type && _plan.backend.capabilities.size &&
                     _plan.backend.capabilities.modified) )
                    return false;
                break;
            case SearchBackendLimitation::HiddenItemsUnavailable:
                if( !_plan.request.filters.include_hidden || _plan.backend.capabilities.hidden_items )
                    return false;
                break;
            case SearchBackendLimitation::FullDiskAccessMissing:
                if( !spotlight_scope )
                    return false;
                break;
            case SearchBackendLimitation::ProviderUnavailable:
                if( spotlight_scope || _plan.backend.support != SearchBackendSupport::Unavailable )
                    return false;
                break;
            case SearchBackendLimitation::SpotlightUnavailable:
                if( !spotlight_scope || _plan.backend.support != SearchBackendSupport::Unavailable )
                    return false;
                break;
            case SearchBackendLimitation::SpotlightIndexUnavailable:
                if( !spotlight_scope || _plan.backend.support != SearchBackendSupport::IndexUnavailable )
                    return false;
                break;
            case SearchBackendLimitation::PermissionDeniedLocations:
            case SearchBackendLimitation::ResultPathsUnavailable:
                return false;
        }
    }

    const bool needs_metadata = UsesMetadata(_plan.request);
    if( _plan.backend.support == SearchBackendSupport::Supported ) {
        if( _plan.request.filters.content && !_plan.backend.capabilities.content )
            return false;
        if( needs_metadata && !(_plan.backend.capabilities.type && _plan.backend.capabilities.size &&
                                _plan.backend.capabilities.modified) )
            return false;
        if( _plan.request.filters.include_hidden && !_plan.backend.capabilities.hidden_items )
            return false;
        if( _plan.request.scope == SearchScope::Recursive && !_plan.backend.capabilities.recursive_scope )
            return false;
        if( _plan.request.scope == SearchScope::CurrentDisk && !_plan.backend.capabilities.current_disk_scope )
            return false;
        if( std::ranges::any_of(_plan.backend.limitations, [](const SearchBackendLimitation _limitation) {
                return _limitation != SearchBackendLimitation::FullDiskAccessMissing;
            }) )
            return false;
        if( HasLimitation(_plan.backend, SearchBackendLimitation::FullDiskAccessMissing) && !spotlight_scope )
            return false;
    }

    switch( _plan.backend.support ) {
        case SearchBackendSupport::Supported:
            break;
        case SearchBackendSupport::Unsupported:
            if( !std::ranges::any_of(_plan.backend.limitations, IsUnsupportedLimitation) )
                return false;
            break;
        case SearchBackendSupport::Unavailable:
            if( spotlight_scope ) {
                if( !HasLimitation(_plan.backend, SearchBackendLimitation::SpotlightUnavailable) )
                    return false;
            }
            else if( !HasLimitation(_plan.backend, SearchBackendLimitation::ProviderUnavailable) )
                return false;
            break;
        case SearchBackendSupport::IndexUnavailable:
            if( !spotlight_scope ||
                !HasLimitation(_plan.backend, SearchBackendLimitation::SpotlightIndexUnavailable) )
                return false;
            break;
    }

    return true;
}

} // namespace nc::core
