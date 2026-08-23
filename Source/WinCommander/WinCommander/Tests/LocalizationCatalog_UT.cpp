// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <pugixml/pugixml.hpp>
#include <rapidjson/document.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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

std::filesystem::path ApplicationSourcePath()
{
    return std::filesystem::path{__FILE__}.parent_path().parent_path();
}

std::filesystem::path SourcePath()
{
    return ApplicationSourcePath().parent_path().parent_path();
}

const std::array<std::filesystem::path, 3> &ApplicationInfoPlistPaths()
{
    [[clang::no_destroy]] static const std::array paths{
        ApplicationSourcePath() / "Resources" / "WinCommander-MAS-Info.plist",
        ApplicationSourcePath() / "Resources" / "WinCommander-NonMAS-Info.plist",
        ApplicationSourcePath() / "Resources" / "WinCommander-Unsigned-Info.plist",
    };
    return paths;
}

const std::vector<std::filesystem::path> &UserFacingCatalogPaths()
{
    [[clang::no_destroy]] static const std::vector paths = [] {
        std::vector<std::filesystem::path> result;
        for( const std::filesystem::directory_entry &entry :
             std::filesystem::recursive_directory_iterator{SourcePath()} )
            if( entry.is_regular_file() && entry.path().extension() == ".xcstrings" )
                result.emplace_back(entry.path());
        std::ranges::sort(result);
        return result;
    }();
    return paths;
}

const std::vector<std::filesystem::path> &UserFacingInterfacePaths()
{
    [[clang::no_destroy]] static const std::vector paths = [] {
        std::vector<std::filesystem::path> result;
        for( const std::filesystem::directory_entry &entry :
             std::filesystem::recursive_directory_iterator{SourcePath()} ) {
            if( !entry.is_regular_file() )
                continue;
            const std::filesystem::path extension = entry.path().extension();
            if( extension == ".xib" || extension == ".storyboard" )
                result.emplace_back(entry.path());
        }
        std::ranges::sort(result);
        return result;
    }();
    return paths;
}

constexpr std::string_view BrandName = "Duck Commander";
constexpr std::array LegacyBrandNames{"Win" " Commander", "Nimble" " Commander"};

bool IsIdentifierCharacter(char _character)
{
    const unsigned char character = static_cast<unsigned char>(_character);
    return std::isalnum(character) || character == '_';
}

bool ContainsToken(std::string_view _value, std::string_view _token)
{
    for( size_t position = _value.find(_token); position != std::string_view::npos;
         position = _value.find(_token, position + _token.size()) ) {
        const bool left_boundary = position == 0 || !IsIdentifierCharacter(_value[position - 1]);
        const size_t right = position + _token.size();
        const bool right_boundary = right == _value.size() || !IsIdentifierCharacter(_value[right]);
        if( left_boundary && right_boundary )
            return true;
    }
    return false;
}

bool ContainsLegacyBrand(std::string_view _value)
{
    for( const std::string_view legacy : LegacyBrandNames )
        if( _value.contains(legacy) )
            return true;
    return ContainsToken(_value, "N" "C");
}

size_t CountOccurrences(std::string_view _value, std::string_view _token)
{
    size_t count = 0;
    for( size_t position = _value.find(_token); position != std::string_view::npos;
         position = _value.find(_token, position + _token.size()) )
        ++count;
    return count;
}

std::string ReadFile(const std::filesystem::path &_path)
{
    std::ifstream file{_path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string ReadCatalog()
{
    return ReadFile(CatalogPath());
}

std::optional<std::string> PlistString(pugi::xml_node _dictionary, std::string_view _key)
{
    for( pugi::xml_node node = _dictionary.first_child(); node; node = node.next_sibling() ) {
        if( std::string_view{node.name()} != "key" || std::string_view{node.child_value()} != _key )
            continue;

        const pugi::xml_node value = node.next_sibling();
        if( value && std::string_view{value.name()} == "string" )
            return std::string{value.child_value()};
        return std::nullopt;
    }
    return std::nullopt;
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

TEST_CASE(PREFIX "publishes Duck Commander as both bundle names", "[branding]")
{
    for( const std::filesystem::path &path : ApplicationInfoPlistPaths() ) {
        pugi::xml_document document;
        const pugi::xml_parse_result parsed = document.load_file(path.c_str());
        INFO("Info.plist: " << path);
        REQUIRE(parsed);

        const pugi::xml_node dictionary = document.child("plist").child("dict");
        REQUIRE(dictionary);
        CHECK(PlistString(dictionary, "CFBundleName") == BrandName);
        CHECK(PlistString(dictionary, "CFBundleDisplayName") == BrandName);
    }

    const std::filesystem::path catalog_path = ApplicationSourcePath() / "Resources" / "InfoPlist.xcstrings";
    const std::string contents = ReadFile(catalog_path);
    REQUIRE_FALSE(contents.empty());

    rapidjson::Document document;
    document.Parse(contents.c_str());
    REQUIRE_FALSE(document.HasParseError());
    REQUIRE(document.IsObject());
    REQUIRE(document.HasMember("strings"));
    REQUIRE(document["strings"].IsObject());

    const auto &strings = document["strings"];
    for( const char *key : {"CFBundleName", "CFBundleDisplayName"} ) {
        INFO("InfoPlist catalog key: " << key);
        REQUIRE(strings.HasMember(key));
        const auto &entry = strings[key];
        REQUIRE(entry.HasMember("localizations"));
        const auto &localizations = entry["localizations"];
        for( const char *locale : {"en", "ru"} ) {
            INFO("locale: " << locale);
            REQUIRE(localizations.HasMember(locale));
            const auto &localized = localizations[locale];
            REQUIRE(localized.HasMember("stringUnit"));
            const auto &unit = localized["stringUnit"];
            REQUIRE(unit.HasMember("value"));
            REQUIRE(unit["value"].IsString());
            CHECK(std::string_view{unit["value"].GetString()} == BrandName);
        }
    }
}

TEST_CASE(PREFIX "publishes Duck Commander as each production Xcode product", "[branding]")
{
    const std::filesystem::path project_root = SourcePath() / "WinCommander";
    const std::string project = ReadFile(project_root / "WinCommander.xcodeproj" / "project.pbxproj");
    REQUIRE_FALSE(project.empty());
    CHECK(CountOccurrences(project, "PRODUCT_NAME = \"Duck Commander\";") == 4);
    CHECK(CountOccurrences(project, "path = \"Duck Commander.app\";") == 2);

    size_t scheme_references = 0;
    for( const char *scheme : {"WinCommander-MAS.xcscheme", "WinCommander-NonMAS.xcscheme"} ) {
        const std::string contents =
            ReadFile(project_root / "WinCommander.xcodeproj" / "xcshareddata" / "xcschemes" / scheme);
        INFO("scheme: " << scheme);
        REQUIRE_FALSE(contents.empty());
        scheme_references += CountOccurrences(contents, "BuildableName = \"Duck Commander.app\"");
    }
    CHECK(scheme_references == 8);
}

TEST_CASE(PREFIX "keeps legacy names out of user-facing resources", "[branding]")
{
    std::vector<std::string> leaks;

    for( const std::filesystem::path &path : UserFacingCatalogPaths() ) {
        const std::string contents = ReadFile(path);
        INFO("string catalog: " << path);
        REQUIRE_FALSE(contents.empty());

        rapidjson::Document document;
        document.Parse(contents.c_str());
        REQUIRE_FALSE(document.HasParseError());
        REQUIRE(document.IsObject());
        REQUIRE(document.HasMember("strings"));
        REQUIRE(document["strings"].IsObject());

        const auto &strings = document["strings"];
        for( auto member = strings.MemberBegin(); member != strings.MemberEnd(); ++member ) {
            const std::string_view key{member->name.GetString(), member->name.GetStringLength()};
            if( ContainsLegacyBrand(key) )
                leaks.emplace_back(path.string() + ": key " + std::string{key});

            const auto &entry = member->value;
            if( !entry.IsObject() || !entry.HasMember("localizations") || !entry["localizations"].IsObject() )
                continue;
            const auto &localizations = entry["localizations"];
            for( auto localization = localizations.MemberBegin(); localization != localizations.MemberEnd(); ++localization ) {
                const auto &localized = localization->value;
                if( !localized.IsObject() || !localized.HasMember("stringUnit") )
                    continue;
                const auto &unit = localized["stringUnit"];
                if( !unit.IsObject() || !unit.HasMember("value") || !unit["value"].IsString() )
                    continue;
                const std::string_view value{unit["value"].GetString(), unit["value"].GetStringLength()};
                if( ContainsLegacyBrand(value) )
                    leaks.emplace_back(path.string() + ": " + std::string{key} + " [" +
                                       localization->name.GetString() + "]");
            }
        }
    }

    for( const std::filesystem::path &path : UserFacingInterfacePaths() ) {
        pugi::xml_document interface;
        INFO("interface resource: " << path);
        REQUIRE(interface.load_file(path.c_str()));
        for( const pugi::xpath_node match : interface.select_nodes("//*[@*]") ) {
            for( const pugi::xml_attribute attribute : match.node().attributes() ) {
                const std::string_view value = attribute.value();
                if( ContainsLegacyBrand(value) )
                    leaks.emplace_back(path.string() + ": " + attribute.name() + " " + std::string{value});
            }
        }
    }

    for( const std::filesystem::path &path : ApplicationInfoPlistPaths() ) {
        pugi::xml_document plist;
        INFO("Info.plist: " << path);
        REQUIRE(plist.load_file(path.c_str()));
        for( const pugi::xpath_node match : plist.select_nodes("//string") ) {
            const std::string_view value = match.node().child_value();
            if( ContainsLegacyBrand(value) )
                leaks.emplace_back(path.string() + ": string " + std::string{value});
        }
    }

    std::string report;
    for( const std::string &leak : leaks )
        report += leak + "\n";
    INFO("legacy user-facing brand occurrences:\n" << report);
    CHECK(leaks.empty());
}

#undef PREFIX
