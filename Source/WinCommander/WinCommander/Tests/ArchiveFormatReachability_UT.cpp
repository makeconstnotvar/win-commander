// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <Operations/ArchiveCreationFormat.h>

#include <rapidjson/document.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using nc::ops::ArchiveCreationFormatInfo;
using nc::ops::SupportedArchiveCreationFormats;

/** The shipped defaults, read from the source tree - this is about what gets shipped. */
std::string ReadShippedConfig()
{
    const std::filesystem::path path =
        std::filesystem::path{__FILE__}.parent_path().parent_path() / "Resources" / "Config.json";
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

/** The extraction whitelist as shipped, lowercased and split. */
std::set<std::string> ExtractableExtensions()
{
    rapidjson::Document document;
    // The shipped config is JSON with comments, and the application parses it the same way; a strict
    // parser would fail here and say nothing useful about the whitelist.
    document.Parse<rapidjson::kParseCommentsFlag>(ReadShippedConfig().c_str());
    REQUIRE_FALSE(document.HasParseError());
    REQUIRE(document.HasMember("filePanel"));
    REQUIRE(document["filePanel"].HasMember("general"));
    const auto &general = document["filePanel"]["general"];
    REQUIRE(general.HasMember("archivesExtensionsWhitelist"));
    const std::string list = general["archivesExtensionsWhitelist"].GetString();

    std::set<std::string> extensions;
    std::string current;
    for( const char character : list ) {
        if( character == ',' || character == ' ' ) {
            if( !current.empty() )
                extensions.insert(current);
            current.clear();
            continue;
        }
        current.push_back(static_cast<char>(character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character));
    }
    if( !current.empty() )
        extensions.insert(current);
    return extensions;
}

/** The part of a compound extension a listing sees: everything after the final dot. */
std::string_view FinalComponent(const std::string_view _extension)
{
    const size_t dot = _extension.rfind('.');
    return dot == std::string_view::npos ? _extension : _extension.substr(dot + 1);
}

} // namespace

#define PREFIX "nc::ops::ArchiveFormatReachability "

TEST_CASE(PREFIX "can open every archive it is able to create")
{
    // Creating a format the application then cannot recognise would be the most confusing outcome
    // available: the archive is written, sits in the listing, and refuses to open in the very
    // application that made it. The two sets are deliberately different - rar extracts but cannot be
    // created - but the asymmetry must only run in that direction.
    const std::set<std::string> extractable = ExtractableExtensions();
    REQUIRE_FALSE(extractable.empty());

    std::vector<std::string> unreachable;
    for( const ArchiveCreationFormatInfo &info : SupportedArchiveCreationFormats() ) {
        // A listing matches on the final extension only, so `backup.tar.gz` is recognised through
        // `gz`. Checking the whole compound would pass for the wrong reason.
        const std::string final_component{FinalComponent(info.extension)};
        if( !extractable.contains(final_component) )
            unreachable.push_back(std::string{info.extension} + " (via '" + final_component + "')");
    }

    std::string report;
    for( const std::string &entry : unreachable )
        report += entry + "\n";
    INFO("creatable formats the extraction whitelist does not admit:\n" << report);
    CHECK(unreachable.empty());
}

TEST_CASE(PREFIX "keeps the creatable set smaller than the extractable one")
{
    // The reason the two are modelled apart. If they ever coincided, the separate type would be
    // carrying no information and the next format added would have nothing to stop it appearing in
    // a Create menu it cannot serve.
    const std::set<std::string> extractable = ExtractableExtensions();

    std::set<std::string> creatable;
    for( const ArchiveCreationFormatInfo &info : SupportedArchiveCreationFormats() )
        creatable.insert(std::string{FinalComponent(info.extension)});

    CHECK(creatable.size() < extractable.size());
    // rar is the canonical example: extractable, and not creatable, because its compressor is not
    // ours to ship.
    CHECK(extractable.contains("rar"));
    CHECK_FALSE(creatable.contains("rar"));
}

TEST_CASE(PREFIX "gives each creatable format its own extension")
{
    // Two formats sharing one extension would make resolution from a filename ambiguous, and the
    // resolver answers with exactly one - so one of the two would be permanently unreachable by
    // name.
    std::set<std::string_view> seen;
    std::vector<std::string> duplicated;
    for( const ArchiveCreationFormatInfo &info : SupportedArchiveCreationFormats() )
        if( !seen.insert(info.extension).second )
            duplicated.emplace_back(info.extension);

    std::string report;
    for( const std::string &entry : duplicated )
        report += entry + "\n";
    INFO("duplicated creatable extensions:\n" << report);
    CHECK(duplicated.empty());
    CHECK(seen.size() == SupportedArchiveCreationFormats().size());
}

#undef PREFIX
