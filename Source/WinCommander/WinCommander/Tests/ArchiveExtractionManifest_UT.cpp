// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/Core/Operations/ArchiveExtractionManifest.h>
#include <array>
#include <stdexcept>

namespace {

using namespace nc::core;

ArchiveExtractionMaterializedEntry Entry(const ArchiveExtractionEntryKind _kind,
                                         std::initializer_list<std::string_view> _components)
{
    ArchiveExtractionMaterializedEntry entry;
    entry.kind = _kind;
    for( const std::string_view component : _components )
        entry.components.emplace_back(component);
    return entry;
}

ArchiveExtractionManifestFailure FailureOf(std::vector<ArchiveExtractionMaterializedEntry> _entries,
                                           const bool _case_sensitive = true,
                                           ArchiveExtractionManifest::NameValidator _validator = {})
{
    auto result = ArchiveExtractionManifest::Build(std::move(_entries), _case_sensitive, std::move(_validator));
    REQUIRE_FALSE(result);
    return result.error();
}

} // namespace

#define PREFIX "nc::core::ArchiveExtractionManifest "

TEST_CASE(PREFIX "accepts an immutable materialized namespace and applies the optional validator")
{
    size_t validated_names = 0;
    std::vector entries{
        Entry(ArchiveExtractionEntryKind::Directory, {"docs"}),
        Entry(ArchiveExtractionEntryKind::RegularFile, {"docs", "guide.txt"}),
        Entry(ArchiveExtractionEntryKind::Symlink, {"latest"}),
    };
    const auto expected = entries;

    auto result = ArchiveExtractionManifest::Build(std::move(entries), true, [&](const std::string_view _name) {
        ++validated_names;
        return !_name.empty();
    });

    REQUIRE(result);
    CHECK(result->DestinationCaseSensitive());
    CHECK(std::vector(result->Entries().begin(), result->Entries().end()) == expected);
    CHECK(validated_names == 4);
}

TEST_CASE(PREFIX "rejects empty dot separator and null path components with exact locations")
{
    struct Case final {
        ArchiveExtractionMaterializedEntry entry;
        ArchiveExtractionManifestFailureCode code;
        std::optional<size_t> component_index;
    };

    const std::array cases{
        Case{Entry(ArchiveExtractionEntryKind::RegularFile, {}),
             ArchiveExtractionManifestFailureCode::EmptyPath,
             std::nullopt},
        Case{Entry(ArchiveExtractionEntryKind::RegularFile, {""}),
             ArchiveExtractionManifestFailureCode::EmptyComponent,
             0},
        Case{Entry(ArchiveExtractionEntryKind::RegularFile, {"safe", "."}),
             ArchiveExtractionManifestFailureCode::DotComponent,
             1},
        Case{Entry(ArchiveExtractionEntryKind::RegularFile, {"safe", ".."}),
             ArchiveExtractionManifestFailureCode::DotDotComponent,
             1},
        Case{Entry(ArchiveExtractionEntryKind::RegularFile, {"safe/name"}),
             ArchiveExtractionManifestFailureCode::ComponentContainsPathSeparator,
             0},
        Case{ArchiveExtractionMaterializedEntry{{std::string{"bad\0name", 8}}, ArchiveExtractionEntryKind::RegularFile},
             ArchiveExtractionManifestFailureCode::ComponentContainsNull,
             0},
    };

    for( const Case &test : cases ) {
        const auto failure = FailureOf({test.entry});
        CHECK(failure.code == test.code);
        CHECK(failure.entry_index == 0);
        CHECK(failure.component_index == test.component_index);
        CHECK_FALSE(failure.conflicting_entry_index);
    }
}

TEST_CASE(PREFIX "accepts depth 127 and rejects the first deeper component")
{
    ArchiveExtractionMaterializedEntry boundary;
    boundary.kind = ArchiveExtractionEntryKind::RegularFile;
    boundary.components.assign(127, "level");
    REQUIRE(ArchiveExtractionManifest::Build({boundary}, true));

    ArchiveExtractionMaterializedEntry too_deep = boundary;
    too_deep.components.emplace_back("overflow");
    const auto failure = FailureOf({std::move(too_deep)});
    CHECK(failure.code == ArchiveExtractionManifestFailureCode::DepthExceeded);
    CHECK(failure.component_index == 127);
}

TEST_CASE(PREFIX "rejects special entries and provider-invalid destination names")
{
    CHECK(FailureOf({Entry(ArchiveExtractionEntryKind::Special, {"device"})}).code ==
          ArchiveExtractionManifestFailureCode::SpecialEntryKind);

    const auto invalid =
        FailureOf({Entry(ArchiveExtractionEntryKind::RegularFile, {"folder", "invalid:name"})},
                  true,
                  [](const std::string_view _name) { return _name.find(':') == std::string_view::npos; });
    CHECK(invalid.code == ArchiveExtractionManifestFailureCode::InvalidDestinationName);
    CHECK(invalid.component_index == 1);

    const auto throwing = FailureOf({Entry(ArchiveExtractionEntryKind::RegularFile, {"entry"})},
                                    true,
                                    [](std::string_view) -> bool { throw std::runtime_error("validator failed"); });
    CHECK(throwing.code == ArchiveExtractionManifestFailureCode::InvalidDestinationName);
}

TEST_CASE(PREFIX "distinguishes exact duplicates from case-insensitive namespace collisions")
{
    const auto duplicate = FailureOf({Entry(ArchiveExtractionEntryKind::RegularFile, {"same"}),
                                      Entry(ArchiveExtractionEntryKind::RegularFile, {"same"})});
    CHECK(duplicate.code == ArchiveExtractionManifestFailureCode::DuplicatePath);
    CHECK(duplicate.entry_index == 1);
    CHECK(duplicate.conflicting_entry_index == 0);

    const auto collision = FailureOf({Entry(ArchiveExtractionEntryKind::Directory, {"Docs"}),
                                      Entry(ArchiveExtractionEntryKind::RegularFile, {"docs", "guide.txt"})},
                                     false);
    CHECK(collision.code == ArchiveExtractionManifestFailureCode::CaseCollision);
    CHECK(collision.entry_index == 1);
    CHECK(collision.component_index == 0);
    CHECK(collision.conflicting_entry_index == 0);

    CHECK(ArchiveExtractionManifest::Build({Entry(ArchiveExtractionEntryKind::RegularFile, {"README"}),
                                            Entry(ArchiveExtractionEntryKind::RegularFile, {"readme"})},
                                           true));
}

TEST_CASE(PREFIX "rejects a file or symlink ancestor independently of input order")
{
    const std::array ancestor_kinds{
        ArchiveExtractionEntryKind::RegularFile,
        ArchiveExtractionEntryKind::Symlink,
    };

    for( const ArchiveExtractionEntryKind kind : ancestor_kinds ) {
        CAPTURE(kind);
        const auto ancestor_first =
            FailureOf({Entry(kind, {"escape"}), Entry(ArchiveExtractionEntryKind::RegularFile, {"escape", "payload"})});
        CHECK(ancestor_first.code == ArchiveExtractionManifestFailureCode::FileOrSymlinkAncestor);
        CHECK(ancestor_first.entry_index == 1);
        CHECK(ancestor_first.conflicting_entry_index == 0);

        const auto descendant_first =
            FailureOf({Entry(ArchiveExtractionEntryKind::RegularFile, {"escape", "payload"}), Entry(kind, {"escape"})});
        CHECK(descendant_first.code == ArchiveExtractionManifestFailureCode::FileOrSymlinkAncestor);
        CHECK(descendant_first.entry_index == 1);
        CHECK(descendant_first.conflicting_entry_index == 0);
    }
}

TEST_CASE(PREFIX "rejects file-directory type collisions while allowing explicit directory prefixes")
{
    const auto conflict = FailureOf({Entry(ArchiveExtractionEntryKind::Directory, {"node"}),
                                     Entry(ArchiveExtractionEntryKind::RegularFile, {"node"})});
    CHECK(conflict.code == ArchiveExtractionManifestFailureCode::FileDirectoryPrefixConflict);
    CHECK(conflict.conflicting_entry_index == 0);

    CHECK(ArchiveExtractionManifest::Build({Entry(ArchiveExtractionEntryKind::Directory, {"node"}),
                                            Entry(ArchiveExtractionEntryKind::RegularFile, {"node", "payload"})},
                                           true));
}
