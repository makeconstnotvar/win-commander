// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <rapidjson/document.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

/**
 * The string catalog, read from the source tree rather than from a bundle.
 *
 * These checks are about the catalog as it is committed - duplicate keys and missing translations
 * are properties of the file, and a compiled copy has already lost the first of them.
 */
std::filesystem::path CatalogPath()
{
    return std::filesystem::path{__FILE__}.parent_path().parent_path() / "Resources" / "Localizable.xcstrings";
}

std::string ReadCatalog()
{
    std::ifstream file{CatalogPath(), std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

/**
 * Keys that are symbols rather than prose, and have nothing to translate.
 *
 * An explicit list rather than a rule: "looks like a symbol" would quietly excuse a real string that
 * happened to be short, which is the sort of exemption that grows.
 */
const std::set<std::string> &UntranslatableKeys()
{
    [[clang::no_destroy]] static const std::set<std::string> keys{"⏎", "␛"};
    return keys;
}

} // namespace

#define PREFIX "nc::core::LocalizationCatalog "

TEST_CASE(PREFIX "is committed as parseable JSON with a strings table")
{
    const std::string contents = ReadCatalog();
    REQUIRE_FALSE(contents.empty());

    rapidjson::Document document;
    document.Parse(contents.c_str());
    REQUIRE_FALSE(document.HasParseError());
    REQUIRE(document.IsObject());
    REQUIRE(document.HasMember("strings"));
    REQUIRE(document["strings"].IsObject());
}

TEST_CASE(PREFIX "declares each key exactly once")
{
    // JSON with a duplicated key still parses, and every reader silently keeps one of the two - which
    // is how an edit to this file once landed a translation inside the wrong entry and left no trace.
    // Counting members against distinct names is what makes that visible.
    const std::string contents = ReadCatalog();
    rapidjson::Document document;
    document.Parse(contents.c_str());
    REQUIRE_FALSE(document.HasParseError());

    const auto &strings = document["strings"];
    std::set<std::string> distinct;
    std::vector<std::string> duplicated;
    for( auto member = strings.MemberBegin(); member != strings.MemberEnd(); ++member ) {
        const std::string key = member->name.GetString();
        if( !distinct.insert(key).second )
            duplicated.push_back(key);
    }

    std::string report;
    for( const std::string &key : duplicated )
        report += key + "\n";
    INFO("duplicated keys:\n" << report);
    CHECK(duplicated.empty());
    CHECK(distinct.size() == static_cast<size_t>(strings.MemberCount()));
}

TEST_CASE(PREFIX "leaves no string for a user to meet in the wrong language")
{
    // The guard for a real defect this catalog had: strings that shipped untranslated and reached a
    // Russian-language user as English, with nothing to notice them by.
    const std::string contents = ReadCatalog();
    rapidjson::Document document;
    document.Parse(contents.c_str());
    REQUIRE_FALSE(document.HasParseError());

    const auto &strings = document["strings"];
    std::vector<std::string> untranslated;
    for( auto member = strings.MemberBegin(); member != strings.MemberEnd(); ++member ) {
        const std::string key = member->name.GetString();
        if( UntranslatableKeys().contains(key) )
            continue;

        const auto &entry = member->value;
        if( !entry.IsObject() || !entry.HasMember("localizations") ) {
            untranslated.push_back(key + " (no localizations)");
            continue;
        }
        const auto &localizations = entry["localizations"];
        if( !localizations.IsObject() || !localizations.HasMember("ru") ) {
            untranslated.push_back(key + " (no ru)");
            continue;
        }
        const auto &russian = localizations["ru"];
        if( !russian.IsObject() || !russian.HasMember("stringUnit") ) {
            untranslated.push_back(key + " (no stringUnit)");
            continue;
        }
        const auto &unit = russian["stringUnit"];
        // A value that is present but marked `new` or `needs_review` is a placeholder, and shipping
        // it is the same failure as shipping none.
        const std::string state =
            unit.HasMember("state") && unit["state"].IsString() ? unit["state"].GetString() : "";
        const std::string value = unit.HasMember("value") && unit["value"].IsString() ? unit["value"].GetString() : "";
        if( state != "translated" )
            untranslated.push_back(key + " (state '" + state + "')");
        else if( value.empty() )
            untranslated.push_back(key + " (empty)");
    }

    std::string report;
    for( const std::string &entry : untranslated )
        report += entry + "\n";
    INFO("strings without a usable Russian translation:\n" << report);
    CHECK(untranslated.empty());
}

TEST_CASE(PREFIX "keeps its list of untranslatable keys honest")
{
    // An exemption list that outlives the keys it excuses is how a real string ends up permanently
    // excused: the key is removed, the entry stays, and the next string named the same slips through.
    const std::string contents = ReadCatalog();
    rapidjson::Document document;
    document.Parse(contents.c_str());
    REQUIRE_FALSE(document.HasParseError());

    const auto &strings = document["strings"];
    std::vector<std::string> stale;
    for( const std::string &key : UntranslatableKeys() )
        if( !strings.HasMember(key.c_str()) )
            stale.push_back(key);

    std::string report;
    for( const std::string &key : stale )
        report += key + "\n";
    INFO("exempted keys that are no longer in the catalog:\n" << report);
    CHECK(stale.empty());
}

#undef PREFIX
