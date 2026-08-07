// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <Config/ConfigImpl.h>
#include <Config/NonPersistentOverwritesStorage.h>
#include <Config/RapidJSON.h>
#include <WinCommander/Core/VFSInstanceManagerImpl.h>
#include <WinCommander/States/Explorer/ExplorerViewSettingsPersistence.h>

#include <algorithm>
#include <memory>
#include <string>

namespace {

using nc::config::ConfigImpl;
using nc::config::NonPersistentOverwritesStorage;
using nc::core::PaneGroupingKey;
using nc::core::PaneGroupingState;
using nc::core::PaneSortDirection;
using nc::core::PaneSortKey;
using nc::core::PaneSortState;
using nc::core::PaneTextCollation;
using nc::explorer::ExplorerViewSettings;
using nc::explorer::ExplorerViewSettingsPersistence;
using nc::panel::NetworkConnectionsManager;
using nc::panel::PanelBriefViewColumnsLayout;
using nc::panel::PanelDataPersistency;
using nc::panel::PanelGalleryViewLayout;
using nc::panel::PanelListViewColumns;
using nc::panel::PanelListViewColumnsLayout;
using nc::panel::PersistentLocation;

constexpr auto g_EmptyRoot = R"({
    "filePanel": {
        "explorer": {
            "viewSettingsByLocation_v1": {
                "schema": 1,
                "entries": []
            }
        }
    }
})";

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

class RecordingNetworkConnectionsManager final : public NetworkConnectionsManager
{
public:
    explicit RecordingNetworkConnectionsManager(Connection _connection) : connection(std::move(_connection)) {}

    std::optional<Connection> ConnectionByUUID(const nc::base::UUID &_uuid) const override
    {
        return connection.Uuid() == _uuid ? std::optional{connection} : std::nullopt;
    }
    std::optional<Connection> ConnectionForVFS(const VFSHost &) const override { return std::nullopt; }
    void InsertConnection(const Connection &) override {}
    void RemoveConnection(const Connection &) override {}
    void ReportUsage(const Connection &) override {}
    ObservationTicket ObserveChanges(std::function<void()>) override { return {}; }
    std::vector<Connection> AllConnectionsByMRU() const override { return {connection}; }
    std::vector<Connection> FTPConnectionsByMRU() const override { return {connection}; }
    std::vector<Connection> SFTPConnectionsByMRU() const override { return {}; }
    std::vector<Connection> LANShareConnectionsByMRU() const override { return {}; }
    bool SetPassword(const Connection &, const std::string &) override { return false; }
    bool GetPassword(const Connection &, std::string &) override { return false; }
    bool AskForPassword(const Connection &, std::string &) override { return false; }
    std::shared_ptr<VFSHost> SpawnHostFromConnection(const Connection &, const bool _allow_password_ui) override
    {
        last_allow_password_ui = _allow_password_ui;
        return nullptr;
    }
    bool MountShareAsync(const Connection &, const std::string &, MountShareCallback) override { return false; }

    Connection connection;
    std::optional<bool> last_allow_password_ui;
};

struct Fixture {
    explicit Fixture(const std::string_view _defaults = g_EmptyRoot)
        : config(_defaults, std::make_shared<NonPersistentOverwritesStorage>("")), persistency(networks),
          store(config, persistency)
    {
    }

    ConfigImpl config;
    NullNetworkConnectionsManager networks;
    PanelDataPersistency persistency;
    ExplorerViewSettingsPersistence store;
};

PersistentLocation NativeLocation(std::string _path)
{
    if( _path.empty() || _path.back() != '/' )
        _path.push_back('/');
    return {.path = std::move(_path)};
}

PersistentLocation LocationFromJSON(PanelDataPersistency &_persistency, const std::string_view _json)
{
    nc::config::Document document;
    document.Parse(_json.data(), _json.size());
    REQUIRE_FALSE(document.HasParseError());
    auto location = _persistency.JSONToLocation(document);
    REQUIRE(location);
    return std::move(*location);
}

PaneSortState NameSort()
{
    return {.key = PaneSortKey::Name,
            .direction = PaneSortDirection::Ascending,
            .collation = PaneTextCollation::Natural,
            .separates_directories = true,
            .extensionless_directories = true};
}

ExplorerViewSettings BriefSettings(const int32_t _slot = 0)
{
    PanelBriefViewColumnsLayout brief;
    brief.mode = PanelBriefViewColumnsLayout::Mode::DynamicWidth;
    brief.fixed_mode_width = 222;
    brief.fixed_amount_value = 5;
    brief.dynamic_width_min = 144;
    brief.dynamic_width_max = 288;
    brief.dynamic_width_equal = true;
    brief.icon_scale = 2;

    ExplorerViewSettings settings;
    settings.layout_slot = _slot;
    settings.layout.name = "Live preset title is not persistence authority";
    settings.layout.layout = brief;
    settings.sort = NameSort();
    settings.grouping = {.enabled = true, .key = PaneGroupingKey::Name};
    return settings;
}

ExplorerViewSettings ListSettings(const int32_t _slot = 1)
{
    PanelListViewColumnsLayout list;
    list.icon_scale = 1;
    list.columns.emplace_back(PanelListViewColumnsLayout::Column{
        .kind = PanelListViewColumns::Filename, .width = 360, .max_width = 800, .min_width = 220});
    list.columns.emplace_back(PanelListViewColumnsLayout::Column{
        .kind = PanelListViewColumns::DateModified, .width = 170, .max_width = 480, .min_width = 140});
    list.columns.emplace_back(PanelListViewColumnsLayout::Column{
        .kind = PanelListViewColumns::Extension, .width = 160, .max_width = 400, .min_width = 110});
    list.columns.emplace_back(PanelListViewColumnsLayout::Column{
        .kind = PanelListViewColumns::Size, .width = 100, .max_width = 300, .min_width = 80});

    ExplorerViewSettings settings;
    settings.layout_slot = _slot;
    settings.layout.name = "Details";
    settings.layout.layout = list;
    settings.sort = {.key = PaneSortKey::ModifiedTime,
                     .direction = PaneSortDirection::Descending,
                     .collation = PaneTextCollation::CaseInsensitive,
                     .separates_directories = false,
                     .extensionless_directories = true};
    settings.grouping = {.enabled = true, .key = PaneGroupingKey::ModifiedTime};
    return settings;
}

ExplorerViewSettings GallerySettings(const int32_t _slot = 4)
{
    ExplorerViewSettings settings;
    settings.layout_slot = _slot;
    settings.layout.name = "Content";
    settings.layout.layout = PanelGalleryViewLayout{.icon_scale = 3, .text_lines = 4};
    settings.sort = {.key = PaneSortKey::Extension,
                     .direction = PaneSortDirection::Ascending,
                     .collation = PaneTextCollation::CaseSensitive,
                     .separates_directories = true,
                     .extensionless_directories = false};
    settings.grouping = {};
    return settings;
}

ExplorerViewSettings WithoutTitle(ExplorerViewSettings _settings)
{
    _settings.layout.name.clear();
    return _settings;
}

void CheckRoundTrip(const PersistentLocation &_location, const ExplorerViewSettings &_settings)
{
    Fixture fixture;
    REQUIRE(fixture.store.Store(_location, _settings));
    const auto canonical = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
    REQUIRE(fixture.store.Load(_location));
    CHECK(*fixture.store.Load(_location) == WithoutTitle(_settings));
    REQUIRE(fixture.store.Store(_location, _settings));
    CHECK(fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath) == canonical);
}

} // namespace

#define PREFIX "nc::explorer::ExplorerViewSettingsPersistence "

TEST_CASE(PREFIX "round-trips exact Brief List and Gallery layouts with semantic ordering")
{
    const auto location = NativeLocation("/round-trip");

    SECTION("Brief")
    {
        CheckRoundTrip(location, BriefSettings());
    }
    SECTION("List")
    {
        CheckRoundTrip(location, ListSettings());
    }
    SECTION("Gallery")
    {
        CheckRoundTrip(location, GallerySettings());
    }
}

TEST_CASE(PREFIX "round-trips a valid List layout after Filename column reordering")
{
    auto settings = ListSettings();
    auto &columns = std::get<PanelListViewColumnsLayout>(settings.layout.layout).columns;
    REQUIRE(columns.size() > 1);
    std::rotate(columns.begin(), columns.begin() + 1, columns.end());
    REQUIRE(columns.back().kind == PanelListViewColumns::Filename);
    CheckRoundTrip(NativeLocation("/reordered-columns"), settings);
}

TEST_CASE(PREFIX "creates the fixed root leaf without using a location as a Config path")
{
    Fixture fixture{R"({"filePanel":{"explorer":{}}})"};
    const auto location = NativeLocation("/path.with.dots/is-data");

    REQUIRE(fixture.store.Store(location, BriefSettings()));
    const auto root = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
    REQUIRE(root.IsObject());
    REQUIRE(root["entries"].IsArray());
    REQUIRE(root["entries"].Size() == 1);
    CHECK(std::string_view{root["entries"][0]["location"]["path"].GetString()} == "/path.with.dots/is-data/");
}

TEST_CASE(PREFIX "uses the full canonical location to guard footprint collisions")
{
    Fixture fixture;
    const auto first = LocationFromJSON(
        fixture.persistency,
        R"({"hosts_v1":[{"type":"network","uuid":"11111111-1111-4111-8111-111111111111"}],"path":"/same/"})");
    const auto second = LocationFromJSON(
        fixture.persistency,
        R"({"hosts_v1":[{"type":"network","uuid":"22222222-2222-4222-8222-222222222222"}],"path":"/same/"})");
    REQUIRE(fixture.persistency.MakeFootprintString(first) == fixture.persistency.MakeFootprintString(second));

    REQUIRE(fixture.store.Store(first, BriefSettings(0)));
    REQUIRE(fixture.store.Store(second, GallerySettings(4)));

    REQUIRE(fixture.store.Load(first));
    CHECK(*fixture.store.Load(first) == WithoutTitle(BriefSettings(0)));
    REQUIRE(fixture.store.Load(second));
    CHECK(*fixture.store.Load(second) == WithoutTitle(GallerySettings(4)));
    CHECK(fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath)["entries"].Size() == 2);
}

TEST_CASE(PREFIX "restores A after B and touches only an exact match into MRU order")
{
    Fixture fixture;
    const auto a = NativeLocation("/A");
    const auto b = NativeLocation("/B");
    REQUIRE(fixture.store.Store(a, BriefSettings()));
    REQUIRE(fixture.store.Store(b, ListSettings()));

    auto root = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
    CHECK(std::string_view{root["entries"][0]["location"]["path"].GetString()} == "/B/");

    REQUIRE(fixture.store.Load(a));
    CHECK(*fixture.store.Load(a) == WithoutTitle(BriefSettings()));
    root = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
    CHECK(std::string_view{root["entries"][0]["location"]["path"].GetString()} == "/A/");

    CHECK_FALSE(fixture.store.Load(NativeLocation("/missing")));
    const auto after_miss = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
    CHECK(after_miss == root);
}

TEST_CASE(PREFIX "repairs a stale footprint only after the full location matches")
{
    Fixture fixture;
    const auto location = NativeLocation("/exact");
    REQUIRE(fixture.store.Store(location, BriefSettings()));

    auto stale = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
    stale["entries"][0]["footprint"].SetString("stale-index-only");
    fixture.config.Set(ExplorerViewSettingsPersistence::ConfigPath, stale);

    REQUIRE(fixture.store.Load(location));
    const auto repaired = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
    CHECK(std::string_view{repaired["entries"][0]["footprint"].GetString()} ==
          fixture.persistency.MakeFootprintString(location));
}

TEST_CASE(PREFIX "fails closed without rewriting malformed schema or semantically invalid records")
{
    Fixture fixture;
    const auto location = NativeLocation("/corrupt");
    REQUIRE(fixture.store.Store(location, ListSettings()));

    SECTION("wrong schema")
    {
        auto corrupt = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
        corrupt["schema"].SetInt(2);
        fixture.config.Set(ExplorerViewSettingsPersistence::ConfigPath, corrupt);
        const auto baseline = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);

        CHECK_FALSE(fixture.store.Load(location));
        CHECK_FALSE(fixture.store.Store(location, BriefSettings()));
        CHECK(fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath) == baseline);
    }

    SECTION("duplicate List column")
    {
        auto corrupt = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
        corrupt["entries"][0]["settings"]["layout"]["list"]["columns"][1]["kind"].SetInt(
            static_cast<int>(PanelListViewColumns::Filename));
        fixture.config.Set(ExplorerViewSettingsPersistence::ConfigPath, corrupt);
        const auto baseline = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);

        CHECK_FALSE(fixture.store.Load(location));
        CHECK_FALSE(fixture.store.Store(location, BriefSettings()));
        CHECK(fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath) == baseline);
    }

    SECTION("grouping contradicts sort")
    {
        auto corrupt = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
        corrupt["entries"][0]["settings"]["grouping"]["kind"].SetString("size");
        fixture.config.Set(ExplorerViewSettingsPersistence::ConfigPath, corrupt);
        const auto baseline = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);

        CHECK_FALSE(fixture.store.Load(location));
        CHECK_FALSE(fixture.store.Store(location, BriefSettings()));
        CHECK(fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath) == baseline);
    }

    SECTION("Brief icon scale exceeds its two-bit representation")
    {
        Fixture brief_fixture;
        REQUIRE(brief_fixture.store.Store(location, BriefSettings()));
        auto corrupt = brief_fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
        corrupt["entries"][0]["settings"]["layout"]["brief"]["icon_scale"].SetInt(4);
        brief_fixture.config.Set(ExplorerViewSettingsPersistence::ConfigPath, corrupt);
        const auto baseline = brief_fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);

        CHECK_FALSE(brief_fixture.store.Load(location));
        CHECK_FALSE(brief_fixture.store.Store(location, BriefSettings()));
        CHECK(brief_fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath) == baseline);
    }

    SECTION("location host stack contains a non-object")
    {
        auto corrupt = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
        nc::config::Value hosts{rapidjson::kArrayType};
        hosts.PushBack(nc::config::Value{1}, nc::config::g_CrtAllocator);
        corrupt["entries"][0]["location"].AddMember(
            nc::config::MakeStandaloneString("hosts_v1"), std::move(hosts), nc::config::g_CrtAllocator);
        fixture.config.Set(ExplorerViewSettingsPersistence::ConfigPath, corrupt);
        const auto baseline = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);

        CHECK_FALSE(fixture.store.Load(location));
        CHECK_FALSE(fixture.store.Store(location, BriefSettings()));
        CHECK(fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath) == baseline);
    }

    SECTION("duplicate canonical location")
    {
        auto corrupt = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
        auto duplicate = nc::config::Value{corrupt["entries"][0], nc::config::g_CrtAllocator};
        corrupt["entries"].PushBack(std::move(duplicate), nc::config::g_CrtAllocator);
        fixture.config.Set(ExplorerViewSettingsPersistence::ConfigPath, corrupt);
        const auto baseline = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);

        CHECK_FALSE(fixture.store.Load(location));
        CHECK_FALSE(fixture.store.Store(location, BriefSettings()));
        CHECK(fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath) == baseline);
    }
}

TEST_CASE(PREFIX "rejects invalid caller settings before touching StateConfig")
{
    Fixture fixture;
    const auto location = NativeLocation("/invalid");
    const auto baseline = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);

    auto invalid_slot = BriefSettings(10);
    CHECK_FALSE(fixture.store.Store(location, invalid_slot));

    auto disabled = BriefSettings();
    disabled.layout.layout = nc::panel::PanelViewDisabledLayout{};
    CHECK_FALSE(fixture.store.Store(location, disabled));

    auto mismatched_grouping = BriefSettings();
    mismatched_grouping.grouping.key = PaneGroupingKey::Size;
    CHECK_FALSE(fixture.store.Store(location, mismatched_grouping));

    auto list_without_filename = ListSettings();
    auto &columns = std::get<PanelListViewColumnsLayout>(list_without_filename.layout.layout).columns;
    columns.front().kind = PanelListViewColumns::DateCreated;
    CHECK_FALSE(fixture.store.Store(location, list_without_filename));

    CHECK(fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath) == baseline);
}

TEST_CASE(PREFIX "keeps a deterministic bounded 512-record LRU")
{
    Fixture fixture;
    for( size_t index = 0; index <= ExplorerViewSettingsPersistence::Capacity; ++index )
        REQUIRE(fixture.store.Store(NativeLocation("/location-" + std::to_string(index)), BriefSettings()));

    const auto root = fixture.config.Get(ExplorerViewSettingsPersistence::ConfigPath);
    REQUIRE(root["entries"].Size() == ExplorerViewSettingsPersistence::Capacity);
    CHECK(std::string_view{root["entries"][0]["location"]["path"].GetString()} == "/location-512/");
    CHECK_FALSE(fixture.store.Load(NativeLocation("/location-0")));
    CHECK(fixture.store.Load(NativeLocation("/location-1")));
}

#undef PREFIX
#define PREFIX "nc::panel::PanelDataPersistency "

TEST_CASE(PREFIX "forwards explicit no-password UI policy and preserves the interactive default")
{
    NetworkConnectionsManager::FTP ftp;
    ftp.uuid = NetworkConnectionsManager::MakeUUID();
    ftp.host = "example.invalid";
    const NetworkConnectionsManager::Connection connection{ftp};
    RecordingNetworkConnectionsManager networks{connection};
    PanelDataPersistency persistency{networks};
    nc::core::VFSInstanceManagerImpl instances;

    const std::string encoded =
        R"({"hosts_v1":[{"type":"network","uuid":")" + ftp.uuid.ToString() + R"("}],"path":"/"})";
    const PersistentLocation location = LocationFromJSON(persistency, encoded);

    CHECK_FALSE(persistency.CreateVFSFromLocation(location, instances, {.allow_password_ui = false}));
    REQUIRE(networks.last_allow_password_ui);
    CHECK_FALSE(*networks.last_allow_password_ui);

    networks.last_allow_password_ui.reset();
    CHECK_FALSE(persistency.CreateVFSFromLocation(location, instances));
    REQUIRE(networks.last_allow_password_ui);
    CHECK(*networks.last_allow_password_ui);
}

TEST_CASE(PREFIX "rejects malformed and unknown host entries")
{
    NullNetworkConnectionsManager networks;
    PanelDataPersistency persistency{networks};

    for( const std::string_view encoded : {
             R"({"hosts_v1":{},"path":"/"})",
             R"({"hosts_v1":[7],"path":"/"})",
             R"({"hosts_v1":[{"type":"future"}],"path":"/"})",
         } ) {
        nc::config::Document document;
        document.Parse(encoded.data(), encoded.size());
        REQUIRE_FALSE(document.HasParseError());
        CHECK_FALSE(persistency.JSONToLocation(document));
    }
}

#undef PREFIX
