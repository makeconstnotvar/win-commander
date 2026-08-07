// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "DragDropPolicy.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace nc::panel {
namespace {

using PathComponents = std::vector<std::string_view>;

std::optional<PathComponents> ParseAbsolutePath(const std::string_view _path, const bool _allow_trailing_slash)
{
    if( _path.empty() || _path.front() != '/' || _path.find('\0') != std::string_view::npos )
        return std::nullopt;
    if( _path == "/" )
        return PathComponents{};
    if( _path.back() == '/' && !_allow_trailing_slash )
        return std::nullopt;

    PathComponents components;
    size_t begin = 1;
    while( begin < _path.size() ) {
        const size_t separator = _path.find('/', begin);
        const size_t end = separator == std::string_view::npos ? _path.size() : separator;
        const std::string_view component = _path.substr(begin, end - begin);
        if( component.empty() || component == "." || component == ".." ) {
            if( _allow_trailing_slash && component.empty() && end == _path.size() )
                break;
            return std::nullopt;
        }
        components.emplace_back(component);
        if( separator == std::string_view::npos )
            break;
        begin = separator + 1;
    }
    return components;
}

bool SameComponents(const PathComponents &_lhs, const PathComponents &_rhs) noexcept
{
    return _lhs == _rhs;
}

bool IsSameOrDescendant(const PathComponents &_ancestor, const PathComponents &_candidate) noexcept
{
    return _candidate.size() >= _ancestor.size() &&
           std::ranges::equal(_ancestor.begin(),
                              _ancestor.end(),
                              _candidate.begin(),
                              _candidate.begin() + _ancestor.size());
}

bool DestinationCanCreate(const DragDropPolicyInput &_input) noexcept
{
    const DragDropPathCapabilities &destination = _input.destination.capabilities;
    return std::ranges::all_of(_input.sources, [&](const DragDropSourceFacts &_source) {
        switch( _source.kind ) {
            case DragDropItemKind::Directory:
                return destination.can_create_folder;
            case DragDropItemKind::SymbolicLink:
                return destination.can_create_symlink;
            case DragDropItemKind::RegularFile:
            case DragDropItemKind::Other:
                return destination.can_create_file;
        }
        return false;
    });
}

bool AllSourcesReadable(const DragDropPolicyInput &_input) noexcept
{
    return std::ranges::all_of(
        _input.sources, [](const DragDropSourceFacts &_source) { return _source.capabilities.can_read; });
}

bool AllSourcesWritable(const DragDropPolicyInput &_input) noexcept
{
    return std::ranges::all_of(
        _input.sources, [](const DragDropSourceFacts &_source) { return _source.capabilities.can_write; });
}

bool AllSourcesRenameable(const DragDropPolicyInput &_input) noexcept
{
    return std::ranges::all_of(
        _input.sources, [](const DragDropSourceFacts &_source) { return _source.capabilities.can_rename; });
}

bool AllSourcesDeletable(const DragDropPolicyInput &_input) noexcept
{
    return std::ranges::all_of(_input.sources, [](const DragDropSourceFacts &_source) {
        return _source.capabilities.can_delete_permanently;
    });
}

bool AllSourcesUseDestinationProvider(const DragDropPolicyInput &_input) noexcept
{
    return std::ranges::all_of(_input.sources, [&](const DragDropSourceFacts &_source) {
        return _source.provider_identity == _input.destination.provider_identity;
    });
}

bool AllSourcesAreNative(const DragDropPolicyInput &_input) noexcept
{
    return std::ranges::all_of(
        _input.sources, [](const DragDropSourceFacts &_source) { return _source.native_namespace; });
}

bool AllNativeVolumesAreKnown(const DragDropPolicyInput &_input) noexcept
{
    return _input.destination.volume_identity != 0 &&
           std::ranges::all_of(_input.sources,
                               [](const DragDropSourceFacts &_source) { return _source.volume_identity != 0; });
}

bool AllSourcesUseDestinationVolume(const DragDropPolicyInput &_input) noexcept
{
    return std::ranges::all_of(_input.sources, [&](const DragDropSourceFacts &_source) {
        return _source.volume_identity == _input.destination.volume_identity;
    });
}

bool CanRenameWithinMoveDomain(const DragDropPolicyInput &_input, const bool _same_move_domain) noexcept
{
    return _same_move_domain && _input.destination.capabilities.can_rename && AllSourcesRenameable(_input);
}

bool CanCopyAndDeleteForMove(const DragDropPolicyInput &_input) noexcept
{
    return AllSourcesReadable(_input) && DestinationCanCreate(_input) && AllSourcesDeletable(_input);
}

} // namespace

DragDropPolicyDecision::DragDropPolicyDecision(const DragDropOperation _operation,
                                               const DragDropFailure _failure,
                                               DragDropPolicyInput _input) noexcept
    : m_Operation{_operation}, m_Failure{_failure}, m_Input{std::move(_input)}
{
}

DragDropModifierIntent MapDragDropModifiers(const DragDropModifierState &_state) noexcept
{
    const unsigned requested = static_cast<unsigned>(_state.force_copy) + static_cast<unsigned>(_state.force_move) +
                               static_cast<unsigned>(_state.force_link);
    if( requested > 1 )
        return DragDropModifierIntent::Invalid;
    if( _state.force_copy )
        return DragDropModifierIntent::Copy;
    if( _state.force_move )
        return DragDropModifierIntent::Move;
    if( _state.force_link )
        return DragDropModifierIntent::Link;
    return DragDropModifierIntent::Automatic;
}

DragDropPolicyDecision EvaluateDragDropPolicy(DragDropPolicyInput _input) noexcept
{
    try {
        const auto denied = [&](const DragDropFailure _failure) noexcept {
            return DragDropPolicyDecision{DragDropOperation::Forbidden, _failure, std::move(_input)};
        };
        const auto allowed = [&](const DragDropOperation _operation) noexcept {
            return DragDropPolicyDecision{_operation, DragDropFailure::None, std::move(_input)};
        };

        const DragDropModifierIntent intent = MapDragDropModifiers(_input.modifiers);
        if( intent == DragDropModifierIntent::Invalid )
            return denied(DragDropFailure::InvalidModifierIntent);
        if( _input.sources.empty() )
            return denied(DragDropFailure::NoSources);
        if( _input.destination.provider_identity == 0 ||
            std::ranges::any_of(_input.sources,
                                [](const DragDropSourceFacts &_source) { return _source.provider_identity == 0; }) ) {
            return denied(DragDropFailure::UnknownProviderIdentity);
        }

        const auto destination_components = ParseAbsolutePath(_input.destination.directory, true);
        if( !destination_components )
            return denied(DragDropFailure::InvalidPath);

        std::unordered_set<std::string> exact_sources;
        for( const DragDropSourceFacts &source : _input.sources ) {
            const auto source_components = ParseAbsolutePath(source.path, false);
            const auto source_directory_components = ParseAbsolutePath(source.directory, true);
            if( !source_components || source_components->empty() || !source_directory_components ||
                source_components->size() != source_directory_components->size() + 1 ||
                !std::ranges::equal(source_directory_components->begin(),
                                    source_directory_components->end(),
                                    source_components->begin(),
                                    source_components->end() - 1) ) {
                return denied(DragDropFailure::InvalidPath);
            }
            if( !exact_sources.emplace(std::to_string(source.provider_identity) + ":" + source.path).second )
                return denied(DragDropFailure::DuplicateSource);
            const bool same_provider_as_destination =
                source.provider_identity == _input.destination.provider_identity;
            if( same_provider_as_destination &&
                SameComponents(*source_directory_components, *destination_components) )
                return denied(DragDropFailure::SameFolder);
            if( same_provider_as_destination && source.kind == DragDropItemKind::Directory &&
                IsSameOrDescendant(*source_components, *destination_components) ) {
                return denied(DragDropFailure::RecursiveDestination);
            }
        }

        const bool same_provider = AllSourcesUseDestinationProvider(_input);
        if( same_provider ) {
            const bool consistent_namespace = std::ranges::all_of(_input.sources, [&](const auto &_source) {
                return _source.native_namespace == _input.destination.native_namespace;
            });
            if( !consistent_namespace )
                return denied(DragDropFailure::InconsistentProviderFacts);
        }

        DragDropOperation operation = DragDropOperation::Forbidden;
        switch( intent ) {
            case DragDropModifierIntent::Copy:
                operation = DragDropOperation::Copy;
                break;
            case DragDropModifierIntent::Move:
                operation = DragDropOperation::Move;
                break;
            case DragDropModifierIntent::Link:
                operation = DragDropOperation::Link;
                break;
            case DragDropModifierIntent::Automatic:
                if( same_provider && _input.destination.native_namespace ) {
                    if( !AllNativeVolumesAreKnown(_input) )
                        return denied(DragDropFailure::UnknownVolumeIdentity);
                    const bool same_move_domain = AllSourcesUseDestinationVolume(_input);
                    const bool can_move = AllSourcesWritable(_input) &&
                                          (CanRenameWithinMoveDomain(_input, same_move_domain) ||
                                           CanCopyAndDeleteForMove(_input));
                    operation = same_move_domain && can_move ? DragDropOperation::Move : DragDropOperation::Copy;
                }
                else if( same_provider ) {
                    const bool can_move = AllSourcesWritable(_input) &&
                                          (CanRenameWithinMoveDomain(_input, true) || CanCopyAndDeleteForMove(_input));
                    operation = can_move ? DragDropOperation::Move : DragDropOperation::Copy;
                }
                else {
                    operation = DragDropOperation::Copy;
                }
                break;
            case DragDropModifierIntent::Invalid:
                return denied(DragDropFailure::InvalidModifierIntent);
        }

        if( !_input.destination.capabilities.can_write )
            return denied(DragDropFailure::DestinationReadOnly);

        if( operation == DragDropOperation::Link ) {
            if( !_input.destination.native_namespace || !AllSourcesAreNative(_input) ||
                !_input.destination.capabilities.can_create_symlink ) {
                return denied(DragDropFailure::LinkUnsupported);
            }
            return allowed(operation);
        }

        if( operation == DragDropOperation::Move ) {
            if( !AllSourcesWritable(_input) )
                return denied(DragDropFailure::SourceReadOnly);

            bool same_move_domain = same_provider;
            if( same_move_domain && _input.destination.native_namespace ) {
                if( !AllNativeVolumesAreKnown(_input) )
                    return denied(DragDropFailure::UnknownVolumeIdentity);
                same_move_domain = AllSourcesUseDestinationVolume(_input);
            }
            if( CanRenameWithinMoveDomain(_input, same_move_domain) )
                return allowed(operation);
            if( !AllSourcesReadable(_input) )
                return denied(DragDropFailure::SourceUnreadable);
            if( !DestinationCanCreate(_input) )
                return denied(DragDropFailure::DestinationUnsupported);
            if( !AllSourcesDeletable(_input) )
                return denied(DragDropFailure::SourceMutationUnsupported);
            return allowed(operation);
        }

        if( !AllSourcesReadable(_input) )
            return denied(DragDropFailure::SourceUnreadable);
        if( !DestinationCanCreate(_input) )
            return denied(DragDropFailure::DestinationUnsupported);
        return allowed(operation);
    } catch( ... ) {
        return DragDropPolicyDecision{
            DragDropOperation::Forbidden, DragDropFailure::InternalFailure, std::move(_input)};
    }
}

} // namespace nc::panel
