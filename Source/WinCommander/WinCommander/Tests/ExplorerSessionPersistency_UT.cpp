// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <Config/RapidJSON.h>
#include <WinCommander/States/Explorer/ExplorerSessionPersistency.h>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <memory>
#include <string>
#include <string_view>

namespace {

using nc::config::Document;
using nc::config::Value;
using nc::explorer::ExplorerPanesSession;
using nc::explorer::ExplorerSessionPersistency;
using nc::explorer::ExplorerSessionTab;
using nc::explorer::ExplorerTabsSession;
using nc::explorer::ExplorerWindowSession;
using nc::explorer::ExplorerWindowSessionMode;
using nc::panel::NetworkConnectionsManager;
using nc::panel::PanelDataPersistency;
using nc::panel::PersistentLocation;

class NullNetworkConnectionsManager final : public NetworkConnectionsManager
{
public:
    std::optional<Connection> ConnectionByUUID(const nc::base::UUID &) const override { return std::nullopt; }
    std::optional<Connection> ConnectionForVFS(const VFSHost &) const override { return std::nullopt; }
    void InsertConnection(const Connection &) override {}
    void RemoveConnection(const Connection &) override {}
    void ReportUsage(const Connection &) override {}
    ObservationTicket ObserveChanges(std::function<void()>) override { return {}; }
    std::vector<Connection> AllConnectionsByMRU() const override { return {}; }
    std::vector<Connection> FTPConnectionsByMRU() const override { return {}; }
    std::vector<Connection> SFTPConnectionsByMRU() const override { return {}; }
    std::vector<Connection> LANShareConnectionsByMRU() const override { return {}; }
    bool SetPassword(const Connection &, const std::string &) override { return false; }
    bool GetPassword(const Connection &, std::string &) override { return false; }
    bool AskForPassword(const Connection &, std::string &) override { return false; }
    std::shared_ptr<VFSHost> SpawnHostFromConnection(const Connection &, bool) override { return nullptr; }
    bool MountShareAsync(const Connection &, const std::string &, MountShareCallback) override { return false; }
};

struct Fixture {
    Fixture() : persistency(networks), codec(persistency) {}

    NullNetworkConnectionsManager networks;
    PanelDataPersistency persistency;
    ExplorerSessionPersistency codec;
};

Document Parse(const std::string_view _json)
{
    Document result;
    result.Parse(_json.data(), _json.size());
    REQUIRE_FALSE(result.HasParseError());
    return result;
}

Value Copy(const Value &_value)
{
    return Value{_value, nc::config::g_CrtAllocator};
}

Value CommanderState()
{
    const Document document = Parse(R"({
        "panels_v1": [
            [{"data":{"path":"/left/"},"sorting":{},"layout":0}],
            [{"data":{"path":"/right/"},"sorting":{},"layout":0}]
        ],
        "uiState": {"selectedLeftTab": 0, "selectedRightTab": 0, "focusedSide": "left"}
    })");
    return Copy(document);
}

PersistentLocation NativeLocation(std::string _path)
{
    if( _path.empty() || _path.back() != '/' )
        _path.push_back('/');
    return {.path = std::move(_path)};
}

PersistentLocation LocationFromJSON(PanelDataPersistency &_persistency, const std::string_view _json)
{
    const Document document = Parse(_json);
    auto location = _persistency.JSONToLocation(document);
    REQUIRE(location);
    return std::move(*location);
}

std::string JSONString(const Value &_value)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer{buffer};
    REQUIRE(_value.Accept(writer));
    return {buffer.GetString(), buffer.GetSize()};
}

Document ExplorerEnvelope(const std::string_view _explorer,
                          const int _schema = ExplorerSessionPersistency::SchemaVersion)
{
    const Value commander = CommanderState();
    const std::string json = std::string{"{\"schema\":"} + std::to_string(_schema) +
                             ",\"mode\":\"explorer\",\"commander\":" + JSONString(commander) +
                             ",\"explorer\":" + std::string{_explorer} + "}";
    return Parse(json);
}

void CheckLocation(PanelDataPersistency &_persistency,
                   const std::optional<PersistentLocation> &_actual,
                   const PersistentLocation &_expected)
{
    REQUIRE(_actual);
    CHECK(_persistency.LocationToJSON(*_actual) == _persistency.LocationToJSON(_expected));
}

} // namespace

#define PREFIX "nc::explorer::ExplorerSessionPersistency "

TEST_CASE(PREFIX "round-trips a versioned Commander envelope")
{
    Fixture fixture;
    ExplorerWindowSession source{
        .mode = ExplorerWindowSessionMode::Commander, .commander_state = CommanderState(), .explorer = std::nullopt};

    const Value encoded = fixture.codec.Encode(source);
    REQUIRE(encoded.IsObject());
    CHECK(encoded["schema"].GetInt() == ExplorerSessionPersistency::SchemaVersion);
    CHECK(std::string_view{encoded["mode"].GetString()} == "commander");
    CHECK(encoded["explorer"].IsNull());

    const auto decoded = fixture.codec.Decode(encoded);
    REQUIRE(decoded);
    CHECK(decoded->mode == ExplorerWindowSessionMode::Commander);
    CHECK(decoded->commander_state == source.commander_state);
    CHECK_FALSE(decoded->explorer);
}

TEST_CASE(PREFIX "round-trips ordered Explorer locations and active index without foreign state")
{
    Fixture fixture;
    const PersistentLocation native = NativeLocation("/Users/example/Documents");
    const PersistentLocation remote = LocationFromJSON(
        fixture.persistency,
        R"({"hosts_v1":[{"type":"network","uuid":"11111111-1111-4111-8111-111111111111"}],"path":"/remote/"})");
    ExplorerTabsSession tabs{
        .tabs = {ExplorerSessionTab{.location = native}, ExplorerSessionTab{.location = remote}},
        .active_index = 1,
    };
    ExplorerWindowSession source{
        .mode = ExplorerWindowSessionMode::Explorer, .commander_state = CommanderState(),
        .explorer = ExplorerPanesSession{.left = std::move(tabs)}};

    const Value encoded = fixture.codec.Encode(source);
    REQUIRE(encoded.IsObject());
    const std::string json = JSONString(encoded["explorer"]);
    CHECK(json.find("paneId") == std::string::npos);
    CHECK(json.find("history") == std::string::npos);
    CHECK(json.find("password") == std::string::npos);
    CHECK(json.find("viewSettings") == std::string::npos);
    CHECK(json.find("sorting") == std::string::npos);
    CHECK(json.find("layout") == std::string::npos);
    CHECK(json.find("grouping") == std::string::npos);

    const auto decoded = fixture.codec.Decode(encoded);
    REQUIRE(decoded);
    CHECK(decoded->mode == ExplorerWindowSessionMode::Explorer);
    REQUIRE(decoded->explorer);
    REQUIRE(decoded->explorer->left.tabs.size() == 2);
    CHECK(decoded->explorer->left.active_index == 1);
    CheckLocation(fixture.persistency, decoded->explorer->left.tabs[0].location, native);
    CheckLocation(fixture.persistency, decoded->explorer->left.tabs[1].location, remote);
}

TEST_CASE(PREFIX "round-trips the dual-pane layout as an independent right side")
{
    Fixture fixture;
    const PersistentLocation left_location = NativeLocation("/Users/example/Left");
    const PersistentLocation right_location = NativeLocation("/Users/example/Right");
    ExplorerPanesSession panes{
        .left = {.tabs = {ExplorerSessionTab{.location = left_location}}, .active_index = 0},
        .right = ExplorerTabsSession{.tabs = {ExplorerSessionTab{}, ExplorerSessionTab{.location = right_location}},
                                     .active_index = 1},
        .right_focused = true,
        .divider_ratio = 0.4,
    };
    ExplorerWindowSession source{
        .mode = ExplorerWindowSessionMode::Explorer, .commander_state = CommanderState(), .explorer = std::move(panes)};

    const Value encoded = fixture.codec.Encode(source);
    REQUIRE(encoded.IsObject());
    CHECK(encoded["schema"].GetInt() == ExplorerSessionPersistency::SchemaVersion);
    REQUIRE(encoded["explorer"]["right"].IsObject());
    CHECK(encoded["explorer"]["right"]["tabs"].Size() == 2);
    CHECK(std::string_view{encoded["explorer"]["focused"].GetString()} == "right");
    CHECK(encoded["explorer"]["divider"].GetDouble() == 0.4);

    const auto decoded = fixture.codec.Decode(encoded);
    REQUIRE(decoded);
    REQUIRE(decoded->explorer);
    REQUIRE(decoded->explorer->left.tabs.size() == 1);
    CheckLocation(fixture.persistency, decoded->explorer->left.tabs[0].location, left_location);
    REQUIRE(decoded->explorer->right);
    REQUIRE(decoded->explorer->right->tabs.size() == 2);
    CHECK(decoded->explorer->right->active_index == 1);
    CHECK_FALSE(decoded->explorer->right->tabs[0].location);
    CheckLocation(fixture.persistency, decoded->explorer->right->tabs[1].location, right_location);
    CHECK(decoded->explorer->right_focused);
    REQUIRE(decoded->explorer->divider_ratio);
    CHECK(*decoded->explorer->divider_ratio == 0.4);
}

TEST_CASE(PREFIX "keeps a single-pane session free of dual-pane state in both directions")
{
    Fixture fixture;

    SECTION("encoding a left-only model records no right side")
    {
        ExplorerWindowSession source{
            .mode = ExplorerWindowSessionMode::Explorer,
            .commander_state = CommanderState(),
            // right_focused and divider_ratio are meaningless without a right side and must not
            // survive into the payload, or a restart would try to focus a side that does not exist.
            .explorer = ExplorerPanesSession{.left = {.tabs = {ExplorerSessionTab{}}, .active_index = 0},
                                             .right = std::nullopt,
                                             .right_focused = true,
                                             .divider_ratio = 0.3}};
        const Value encoded = fixture.codec.Encode(source);
        REQUIRE(encoded.IsObject());
        CHECK(encoded["explorer"]["right"].IsNull());
        CHECK(std::string_view{encoded["explorer"]["focused"].GetString()} == "left");
        CHECK(encoded["explorer"]["divider"].IsNull());

        const auto decoded = fixture.codec.Decode(encoded);
        REQUIRE(decoded);
        REQUIRE(decoded->explorer);
        CHECK_FALSE(decoded->explorer->right);
        CHECK_FALSE(decoded->explorer->right_focused);
        CHECK_FALSE(decoded->explorer->divider_ratio);
    }
    SECTION("a stored v1 payload still decodes as one left pane")
    {
        const Document document = ExplorerEnvelope(R"({"tabs":[{"location":{"path":"/A/"}}],"active":0})", 1);
        const auto decoded = fixture.codec.Decode(document);
        REQUIRE(decoded);
        REQUIRE(decoded->explorer);
        REQUIRE(decoded->explorer->left.tabs.size() == 1);
        CheckLocation(fixture.persistency, decoded->explorer->left.tabs[0].location, NativeLocation("/A/"));
        CHECK_FALSE(decoded->explorer->right);
        CHECK_FALSE(decoded->explorer->divider_ratio);
    }
    SECTION("dual-pane hints without a right side are ignored")
    {
        const Document document =
            ExplorerEnvelope(R"({"tabs":[{"location":null}],"active":0,"right":null,"focused":"right","divider":0.4})");
        const auto decoded = fixture.codec.Decode(document);
        REQUIRE(decoded);
        REQUIRE(decoded->explorer);
        CHECK_FALSE(decoded->explorer->right);
        CHECK_FALSE(decoded->explorer->right_focused);
        CHECK_FALSE(decoded->explorer->divider_ratio);
    }
}

TEST_CASE(PREFIX "rejects an undecodable right pane but degrades its presentation hints")
{
    Fixture fixture;

    SECTION("a malformed right pane rejects the envelope atomically")
    {
        // The right pane carries its own ordered tab locations, so there is no safe default to
        // substitute - unlike the focused side and divider ratio exercised below.
        for( const std::string_view right : {R"("left")", R"([])", R"({"active":0})", R"({"tabs":{}})"} ) {
            const Document document =
                ExplorerEnvelope(std::string{R"({"tabs":[{"location":null}],"active":0,"right":)"} +
                                 std::string{right} + "}");
            CHECK_FALSE(fixture.codec.Decode(document));
        }
    }
    SECTION("an oversized right pane is rejected with the same bound as the left one")
    {
        Document document = ExplorerEnvelope(R"({"tabs":[{"location":null}],"active":0,"right":{"tabs":[]}})");
        auto &right_tabs = document["explorer"]["right"]["tabs"];
        for( size_t index = 0; index <= ExplorerSessionPersistency::MaximumTabs; ++index ) {
            Value tab{rapidjson::kObjectType};
            tab.AddMember(
                nc::config::MakeStandaloneString("location"), Value{rapidjson::kNullType}, nc::config::g_CrtAllocator);
            right_tabs.PushBack(std::move(tab), nc::config::g_CrtAllocator);
        }
        CHECK_FALSE(fixture.codec.Decode(document));
    }
    SECTION("an unusable focused side or divider ratio falls back to its default")
    {
        for( const std::string_view hints : {R"("focused":"middle","divider":0.9999)",
                                             R"("focused":7,"divider":"half")",
                                             R"("focused":null,"divider":-1.0)",
                                             R"("focused":"left","divider":0.0)"} ) {
            const Document document =
                ExplorerEnvelope(std::string{R"({"tabs":[{"location":null}],"active":0,)"} + std::string{hints} +
                                 R"(,"right":{"tabs":[{"location":null}],"active":0}})");
            const auto decoded = fixture.codec.Decode(document);
            REQUIRE(decoded);
            REQUIRE(decoded->explorer);
            REQUIRE(decoded->explorer->right);
            CHECK_FALSE(decoded->explorer->right_focused);
            CHECK_FALSE(decoded->explorer->divider_ratio);
        }
    }
    SECTION("an out-of-band divider ratio is dropped on the way out as well")
    {
        ExplorerWindowSession source{
            .mode = ExplorerWindowSessionMode::Explorer,
            .commander_state = CommanderState(),
            .explorer = ExplorerPanesSession{.left = {.tabs = {ExplorerSessionTab{}}, .active_index = 0},
                                             .right = ExplorerTabsSession{.tabs = {ExplorerSessionTab{}},
                                                                          .active_index = 0},
                                             .right_focused = false,
                                             .divider_ratio = 0.99}};
        const Value encoded = fixture.codec.Encode(source);
        REQUIRE(encoded.IsObject());
        CHECK(encoded["explorer"]["right"].IsObject());
        CHECK(encoded["explorer"]["divider"].IsNull());
    }
}

TEST_CASE(PREFIX "migrates the legacy panels root into Commander mode")
{
    Fixture fixture;

    SECTION("complete historical state")
    {
        const Document legacy = Parse(R"({
            "panels_v1": [[{"data":{"path":"/left/"}}], [{"data":{"path":"/right/"}}]],
            "uiState": {"selectedLeftTab": 0, "selectedRightTab": 0, "focusedSide": "right"}
        })");

        const auto decoded = fixture.codec.Decode(legacy);
        REQUIRE(decoded);
        CHECK(decoded->mode == ExplorerWindowSessionMode::Commander);
        CHECK(decoded->commander_state == legacy);
        CHECK_FALSE(decoded->explorer);

        const Value upgraded = fixture.codec.Encode(*decoded);
        REQUIRE(upgraded.IsObject());
        CHECK(upgraded["schema"].GetInt() == ExplorerSessionPersistency::SchemaVersion);
        CHECK(std::string_view{upgraded["mode"].GetString()} == "commander");
        CHECK(upgraded["commander"] == legacy);
    }
    SECTION("panels-only historical state")
    {
        const Document legacy = Parse(R"({
            "panels_v1": [[{"data":{"path":"/left/"}}], [{"data":{"path":"/right/"}}]]
        })");

        const auto decoded = fixture.codec.Decode(legacy);
        REQUIRE(decoded);
        CHECK(decoded->mode == ExplorerWindowSessionMode::Commander);
        CHECK(decoded->commander_state == legacy);
        CHECK_FALSE(decoded->explorer);

        const Value upgraded = fixture.codec.Encode(*decoded);
        REQUIRE(upgraded.IsObject());
        CHECK(upgraded["commander"] == legacy);
    }
}

TEST_CASE(PREFIX "rejects unknown schema mode and root atomically")
{
    Fixture fixture;

    SECTION("unknown schema")
    {
        Document document = ExplorerEnvelope(R"({"tabs":[],"active":0})");
        document["schema"].SetInt(ExplorerSessionPersistency::SchemaVersion + 1);
        CHECK_FALSE(fixture.codec.Decode(document));
    }
    SECTION("unknown mode")
    {
        Document document = ExplorerEnvelope(R"({"tabs":[],"active":0})");
        document["mode"].SetString("workspace");
        CHECK_FALSE(fixture.codec.Decode(document));
    }
    SECTION("unversioned non-legacy root")
    {
        const Document document = Parse(R"({"mode":"commander","commander":{}})");
        CHECK_FALSE(fixture.codec.Decode(document));
    }
    SECTION("legacy root with empty panel sides")
    {
        const Document document = Parse(R"({
            "panels_v1":[[],[]],
            "uiState":{"selectedLeftTab":0,"selectedRightTab":0,"focusedSide":"left"}
        })");
        CHECK_FALSE(fixture.codec.Decode(document));
    }
    SECTION("legacy root with malformed panel entry")
    {
        const Document document = Parse(R"({
            "panels_v1":[[{}],[{"data":{"path":"/right/"}}]]
        })");
        CHECK_FALSE(fixture.codec.Decode(document));
    }
    SECTION("legacy root with malformed optional UI state")
    {
        const Document document = Parse(R"({
            "panels_v1":[[{"data":{"path":"/left/"}}],[{"data":{"path":"/right/"}}]],
            "uiState":{"selectedLeftTab":"zero"}
        })");
        CHECK_FALSE(fixture.codec.Decode(document));
    }
    SECTION("unknown v1 root member")
    {
        Document document = ExplorerEnvelope(R"({"tabs":[],"active":0})");
        document.AddMember(nc::config::MakeStandaloneString("extra"), Value{true}, nc::config::g_CrtAllocator);
        CHECK_FALSE(fixture.codec.Decode(document));
    }
    SECTION("inconsistent Commander envelope")
    {
        Document document = ExplorerEnvelope(R"({"tabs":[],"active":0})");
        document["mode"].SetString("commander");
        CHECK_FALSE(fixture.codec.Decode(document));
    }
}

TEST_CASE(PREFIX "normalizes an empty Explorer session and invalid active index to Home zero")
{
    Fixture fixture;

    SECTION("empty input model")
    {
        ExplorerWindowSession source{.mode = ExplorerWindowSessionMode::Explorer,
                                     .commander_state = CommanderState(),
                                     .explorer = ExplorerPanesSession{.left = ExplorerTabsSession{.tabs = {}, .active_index = 99}}};
        const Value encoded = fixture.codec.Encode(source);
        REQUIRE(encoded.IsObject());
        REQUIRE(encoded["explorer"]["tabs"].Size() == 1);
        CHECK(encoded["explorer"]["tabs"][0]["location"].IsNull());
        CHECK(encoded["explorer"]["active"].GetUint64() == 0);
    }
    SECTION("invalid persisted active values")
    {
        const Document document =
            ExplorerEnvelope(R"({"tabs":[{"location":{"path":"/A/"}},{"location":{"path":"/B/"}}],"active":99})");
        const auto decoded = fixture.codec.Decode(document);
        REQUIRE(decoded);
        REQUIRE(decoded->explorer);
        CHECK(decoded->explorer->left.tabs.size() == 2);
        CHECK(decoded->explorer->left.active_index == 0);

        Document wrong_type = Parse(JSONString(document));
        wrong_type["explorer"]["active"].SetString("one");
        const auto decoded_wrong_type = fixture.codec.Decode(wrong_type);
        REQUIRE(decoded_wrong_type);
        REQUIRE(decoded_wrong_type->explorer);
        CHECK(decoded_wrong_type->explorer->left.active_index == 0);
    }
}

TEST_CASE(PREFIX "isolates malformed noncanonical locations to Home without changing tab order")
{
    Fixture fixture;
    const Document document = ExplorerEnvelope(R"({"active":5,"tabs":[
            {"location":{"path":"/A/"}},
            {"location":{"hosts_v1":[1],"path":"/unsafe-host/"}},
            {"location":{"hosts_v1":[{"type":"unknown"}],"path":"/unknown-host/"}},
            {"location":{"path":"relative/"}},
            {"location":{"path":"/missing-trailing"}},
            {"location":{"hosts_v1":[{"type":"network","uuid":"not-a-uuid"}],"path":"/remote/"}},
            {"location":null},
            {"location":{"path":"/B/"}}
        ]})");

    const auto decoded = fixture.codec.Decode(document);
    REQUIRE(decoded);
    REQUIRE(decoded->explorer);
    REQUIRE(decoded->explorer->left.tabs.size() == 8);
    CHECK(decoded->explorer->left.active_index == 5);
    CheckLocation(fixture.persistency, decoded->explorer->left.tabs[0].location, NativeLocation("/A/"));
    for( size_t index = 1; index < 7; ++index )
        CHECK_FALSE(decoded->explorer->left.tabs[index].location);
    CheckLocation(fixture.persistency, decoded->explorer->left.tabs[7].location, NativeLocation("/B/"));
}

TEST_CASE(PREFIX "rejects a tab count above the bounded capacity")
{
    Fixture fixture;
    ExplorerTabsSession tabs;
    tabs.tabs.resize(ExplorerSessionPersistency::MaximumTabs + 1);
    ExplorerWindowSession source{
        .mode = ExplorerWindowSessionMode::Explorer, .commander_state = CommanderState(),
        .explorer = ExplorerPanesSession{.left = std::move(tabs)}};
    CHECK(fixture.codec.Encode(source).IsNull());

    Document document = ExplorerEnvelope(R"({"tabs":[],"active":0})");
    auto &encoded_tabs = document["explorer"]["tabs"];
    for( size_t index = 0; index <= ExplorerSessionPersistency::MaximumTabs; ++index ) {
        Value tab{rapidjson::kObjectType};
        tab.AddMember(
            nc::config::MakeStandaloneString("location"), Value{rapidjson::kNullType}, nc::config::g_CrtAllocator);
        encoded_tabs.PushBack(std::move(tab), nc::config::g_CrtAllocator);
    }
    CHECK_FALSE(fixture.codec.Decode(document));
}

TEST_CASE(PREFIX "rejects invalid caller envelopes before encoding")
{
    Fixture fixture;

    ExplorerWindowSession commander_with_tabs{
        .mode = ExplorerWindowSessionMode::Commander,
        .commander_state = CommanderState(),
        .explorer = ExplorerPanesSession{},
    };
    CHECK(fixture.codec.Encode(commander_with_tabs).IsNull());

    ExplorerWindowSession explorer_without_tabs{
        .mode = ExplorerWindowSessionMode::Explorer,
        .commander_state = CommanderState(),
        .explorer = std::nullopt,
    };
    CHECK(fixture.codec.Encode(explorer_without_tabs).IsNull());

    const Document non_object = Parse(R"([])");
    ExplorerWindowSession invalid_commander{
        .mode = ExplorerWindowSessionMode::Commander,
        .commander_state = Copy(non_object),
        .explorer = std::nullopt,
    };
    CHECK(fixture.codec.Encode(invalid_commander).IsNull());
}

TEST_CASE(PREFIX "preserves only well-formed future StateConfig schemas")
{
    CHECK(ExplorerSessionPersistency::CanReplaceStoredSession(Parse(R"(null)")));
    CHECK(ExplorerSessionPersistency::CanReplaceStoredSession(CommanderState()));
    CHECK(ExplorerSessionPersistency::CanReplaceStoredSession(Parse(R"({"schema":0})")));
    CHECK(ExplorerSessionPersistency::CanReplaceStoredSession(Parse(R"({"schema":1})")));
    CHECK(ExplorerSessionPersistency::CanReplaceStoredSession(Parse(R"({"schema":2})")));
    CHECK_FALSE(ExplorerSessionPersistency::CanReplaceStoredSession(Parse(R"({"schema":3})")));
    CHECK_FALSE(ExplorerSessionPersistency::CanReplaceStoredSession(Parse(R"({"schema":4294967296})")));
    CHECK(ExplorerSessionPersistency::CanReplaceStoredSession(Parse(R"({"schema":"bad"})")));
}
