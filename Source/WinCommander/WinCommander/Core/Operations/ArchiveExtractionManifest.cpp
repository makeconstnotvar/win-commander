// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ArchiveExtractionManifest.h"

#include <map>
#include <memory>
#include <utility>

namespace nc::core {
namespace {

constexpr size_t g_MaximumArchiveExtractionDepth = 127;

struct NamespaceNode final {
    std::string spelling;
    size_t first_entry_index{0};
    std::optional<ArchiveExtractionEntryKind> explicit_kind;
    std::optional<size_t> explicit_entry_index;
    std::map<std::string, std::unique_ptr<NamespaceNode>> children;
};

[[nodiscard]] std::string IdentityComponent(std::string_view _component, const bool _case_sensitive)
{
    std::string result{_component};
    if( _case_sensitive )
        return result;

    for( char &character : result ) {
        if( character >= 'A' && character <= 'Z' )
            character = static_cast<char>(character - 'A' + 'a');
    }
    return result;
}

[[nodiscard]] ArchiveExtractionManifestFailure
Failure(const ArchiveExtractionManifestFailureCode _code,
        const size_t _entry_index,
        const std::optional<size_t> _component_index = std::nullopt,
        const std::optional<size_t> _conflicting_entry_index = std::nullopt)
{
    return ArchiveExtractionManifestFailure{
        .code = _code,
        .entry_index = _entry_index,
        .component_index = _component_index,
        .conflicting_entry_index = _conflicting_entry_index,
    };
}

[[nodiscard]] bool IsSupportedKind(const ArchiveExtractionEntryKind _kind) noexcept
{
    switch( _kind ) {
        case ArchiveExtractionEntryKind::RegularFile:
        case ArchiveExtractionEntryKind::Directory:
        case ArchiveExtractionEntryKind::Symlink:
            return true;
        case ArchiveExtractionEntryKind::Special:
            return false;
    }
    return false;
}

[[nodiscard]] std::optional<ArchiveExtractionManifestFailure>
ValidateComponents(const ArchiveExtractionMaterializedEntry &_entry,
                   const size_t _entry_index,
                   const ArchiveExtractionManifest::NameValidator &_name_validator)
{
    if( _entry.components.empty() )
        return Failure(ArchiveExtractionManifestFailureCode::EmptyPath, _entry_index);
    if( _entry.components.size() > g_MaximumArchiveExtractionDepth ) {
        return Failure(
            ArchiveExtractionManifestFailureCode::DepthExceeded, _entry_index, g_MaximumArchiveExtractionDepth);
    }
    if( !IsSupportedKind(_entry.kind) )
        return Failure(ArchiveExtractionManifestFailureCode::SpecialEntryKind, _entry_index);

    for( size_t component_index = 0; component_index < _entry.components.size(); ++component_index ) {
        const std::string_view component = _entry.components[component_index];
        if( component.empty() ) {
            return Failure(ArchiveExtractionManifestFailureCode::EmptyComponent, _entry_index, component_index);
        }
        if( component == "." )
            return Failure(ArchiveExtractionManifestFailureCode::DotComponent, _entry_index, component_index);
        if( component == ".." ) {
            return Failure(ArchiveExtractionManifestFailureCode::DotDotComponent, _entry_index, component_index);
        }
        if( component.find('/') != std::string_view::npos ) {
            return Failure(
                ArchiveExtractionManifestFailureCode::ComponentContainsPathSeparator, _entry_index, component_index);
        }
        if( component.find('\0') != std::string_view::npos ) {
            return Failure(ArchiveExtractionManifestFailureCode::ComponentContainsNull, _entry_index, component_index);
        }
        if( _name_validator ) {
            bool valid = false;
            try {
                valid = _name_validator(component);
            } catch( ... ) {
                valid = false;
            }
            if( !valid ) {
                return Failure(
                    ArchiveExtractionManifestFailureCode::InvalidDestinationName, _entry_index, component_index);
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] size_t FirstDescendantEntry(const NamespaceNode &_node) noexcept
{
    return _node.children.begin()->second->first_entry_index;
}

} // namespace

ArchiveExtractionManifest::ArchiveExtractionManifest(std::vector<ArchiveExtractionMaterializedEntry> _entries,
                                                     const bool _destination_case_sensitive)
    : m_Entries(std::move(_entries)), m_DestinationCaseSensitive(_destination_case_sensitive)
{
}

ArchiveExtractionManifest::BuildResult
ArchiveExtractionManifest::Build(std::vector<ArchiveExtractionMaterializedEntry> _entries,
                                 const bool _destination_case_sensitive,
                                 NameValidator _name_validator)
{
    NamespaceNode root;

    for( size_t entry_index = 0; entry_index < _entries.size(); ++entry_index ) {
        const ArchiveExtractionMaterializedEntry &entry = _entries[entry_index];
        if( auto failure = ValidateComponents(entry, entry_index, _name_validator) )
            return std::unexpected(std::move(*failure));

        NamespaceNode *node = &root;
        for( size_t component_index = 0; component_index < entry.components.size(); ++component_index ) {
            const std::string &component = entry.components[component_index];
            const std::string identity = IdentityComponent(component, _destination_case_sensitive);
            auto child = node->children.find(identity);
            if( child == node->children.end() ) {
                auto inserted = std::make_unique<NamespaceNode>();
                inserted->spelling = component;
                inserted->first_entry_index = entry_index;
                child = node->children.emplace(identity, std::move(inserted)).first;
            }
            else if( child->second->spelling != component ) {
                return std::unexpected(Failure(ArchiveExtractionManifestFailureCode::CaseCollision,
                                               entry_index,
                                               component_index,
                                               child->second->first_entry_index));
            }

            node = child->second.get();
            const bool terminal = component_index + 1 == entry.components.size();
            if( !terminal && node->explicit_kind && *node->explicit_kind != ArchiveExtractionEntryKind::Directory ) {
                return std::unexpected(Failure(ArchiveExtractionManifestFailureCode::FileOrSymlinkAncestor,
                                               entry_index,
                                               component_index,
                                               node->explicit_entry_index));
            }
        }

        if( node->explicit_kind ) {
            if( *node->explicit_kind == entry.kind ) {
                return std::unexpected(Failure(ArchiveExtractionManifestFailureCode::DuplicatePath,
                                               entry_index,
                                               entry.components.size() - 1,
                                               node->explicit_entry_index));
            }
            if( *node->explicit_kind == ArchiveExtractionEntryKind::Directory ||
                entry.kind == ArchiveExtractionEntryKind::Directory ) {
                return std::unexpected(Failure(ArchiveExtractionManifestFailureCode::FileDirectoryPrefixConflict,
                                               entry_index,
                                               entry.components.size() - 1,
                                               node->explicit_entry_index));
            }
            return std::unexpected(Failure(ArchiveExtractionManifestFailureCode::DuplicatePath,
                                           entry_index,
                                           entry.components.size() - 1,
                                           node->explicit_entry_index));
        }

        if( entry.kind != ArchiveExtractionEntryKind::Directory && !node->children.empty() ) {
            return std::unexpected(Failure(ArchiveExtractionManifestFailureCode::FileOrSymlinkAncestor,
                                           entry_index,
                                           entry.components.size() - 1,
                                           FirstDescendantEntry(*node)));
        }

        node->explicit_kind = entry.kind;
        node->explicit_entry_index = entry_index;
    }

    return ArchiveExtractionManifest{std::move(_entries), _destination_case_sensitive};
}

std::span<const ArchiveExtractionMaterializedEntry> ArchiveExtractionManifest::Entries() const noexcept
{
    return m_Entries;
}

bool ArchiveExtractionManifest::DestinationCaseSensitive() const noexcept
{
    return m_DestinationCaseSensitive;
}

} // namespace nc::core
