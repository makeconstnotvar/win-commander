// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nc::core {

enum class ArchiveExtractionEntryKind : uint8_t {
    RegularFile,
    Directory,
    Symlink,
    Special
};

struct ArchiveExtractionMaterializedEntry final {
    std::vector<std::string> components;
    ArchiveExtractionEntryKind kind{ArchiveExtractionEntryKind::Special};

    bool operator==(const ArchiveExtractionMaterializedEntry &) const noexcept = default;
};

enum class ArchiveExtractionManifestFailureCode : uint8_t {
    EmptyPath,
    EmptyComponent,
    DotComponent,
    DotDotComponent,
    ComponentContainsPathSeparator,
    ComponentContainsNull,
    DepthExceeded,
    SpecialEntryKind,
    InvalidDestinationName,
    DuplicatePath,
    CaseCollision,
    FileOrSymlinkAncestor,
    FileDirectoryPrefixConflict
};

struct ArchiveExtractionManifestFailure final {
    ArchiveExtractionManifestFailureCode code{ArchiveExtractionManifestFailureCode::EmptyPath};
    size_t entry_index{0};
    std::optional<size_t> component_index;
    std::optional<size_t> conflicting_entry_index;

    bool operator==(const ArchiveExtractionManifestFailure &) const noexcept = default;
};

/**
 * Immutable namespace projection accepted for extraction into one destination directory.
 *
 * Materialized entries contain relative path components after archive acquisition. On a
 * case-insensitive destination, namespace comparison uses the same conservative ASCII folding as
 * the operation-planning path contract. An optional name validator can apply provider-specific
 * destination rules to every component.
 */
class ArchiveExtractionManifest final
{
public:
    using NameValidator = std::function<bool(std::string_view)>;
    using BuildResult = std::expected<ArchiveExtractionManifest, ArchiveExtractionManifestFailure>;

    [[nodiscard]] static BuildResult Build(std::vector<ArchiveExtractionMaterializedEntry> _entries,
                                           bool _destination_case_sensitive,
                                           NameValidator _name_validator = {});

    [[nodiscard]] std::span<const ArchiveExtractionMaterializedEntry> Entries() const noexcept;
    [[nodiscard]] bool DestinationCaseSensitive() const noexcept;

private:
    ArchiveExtractionManifest(std::vector<ArchiveExtractionMaterializedEntry> _entries,
                              bool _destination_case_sensitive);

    std::vector<ArchiveExtractionMaterializedEntry> m_Entries;
    bool m_DestinationCaseSensitive{true};
};

} // namespace nc::core
