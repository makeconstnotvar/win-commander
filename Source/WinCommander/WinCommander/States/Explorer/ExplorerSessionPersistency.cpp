// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "ExplorerSessionPersistency.h"

#include <Config/RapidJSON.h>
#include <VFS/ArcLA.h>
#include <VFS/ArcLARaw.h>
#include <VFS/Native.h>
#include <VFS/PS.h>
#include <VFS/XAttr.h>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <string_view>

namespace nc::explorer {

namespace {

using config::Value;
using namespace rapidjson;

constexpr auto g_SchemaKey = "schema";
constexpr auto g_ModeKey = "mode";
constexpr auto g_CommanderKey = "commander";
constexpr auto g_ExplorerKey = "explorer";
constexpr auto g_TabsKey = "tabs";
constexpr auto g_ActiveKey = "active";
constexpr auto g_LocationKey = "location";
constexpr auto g_LegacyPanelsKey = "panels_v1";
constexpr auto g_LegacyUIKey = "uiState";
constexpr auto g_LegacySelectedLeftKey = "selectedLeftTab";
constexpr auto g_LegacySelectedRightKey = "selectedRightTab";
constexpr auto g_LegacyFocusedSideKey = "focusedSide";
constexpr auto g_LegacyDataKey = "data";
constexpr auto g_PathKey = "path";
constexpr auto g_HostsKey = "hosts_v1";
constexpr auto g_HostTypeKey = "type";
constexpr auto g_HostJunctionKey = "junction";
constexpr auto g_HostUUIDKey = "uuid";
constexpr auto g_NetworkHostType = "network";

Value String(const std::string_view _value)
{
    return config::MakeStandaloneString(_value);
}

Value Copy(const Value &_value)
{
    return Value{_value, config::g_CrtAllocator};
}

const Value *Member(const Value &_object, const char *_key) noexcept
{
    if( !_object.IsObject() )
        return nullptr;
    const auto iterator = _object.FindMember(_key);
    return iterator == _object.MemberEnd() ? nullptr : &iterator->value;
}

bool HasOnlyMembers(const Value &_object, const std::initializer_list<const char *> _members) noexcept
{
    if( !_object.IsObject() || _object.MemberCount() != _members.size() )
        return false;
    for( const char *const name : _members )
        if( Member(_object, name) == nullptr )
            return false;
    return true;
}

bool IsKnownHostShape(const Value &_host) noexcept
{
    if( !_host.IsObject() )
        return false;
    const Value *const type_value = Member(_host, g_HostTypeKey);
    if( type_value == nullptr || !type_value->IsString() )
        return false;
    const std::string_view type{type_value->GetString(), type_value->GetStringLength()};

    if( type == VFSNativeHost::UniqueTag || type == vfs::PSHost::UniqueTag )
        return HasOnlyMembers(_host, {g_HostTypeKey});
    if( type == g_NetworkHostType ) {
        const Value *const uuid = Member(_host, g_HostUUIDKey);
        return HasOnlyMembers(_host, {g_HostTypeKey, g_HostUUIDKey}) && uuid != nullptr && uuid->IsString();
    }
    if( type == vfs::XAttrHost::UniqueTag || type == vfs::ArchiveHost::UniqueTag ||
        type == vfs::ArchiveRawHost::UniqueTag ) {
        const Value *const junction = Member(_host, g_HostJunctionKey);
        return HasOnlyMembers(_host, {g_HostTypeKey, g_HostJunctionKey}) && junction != nullptr && junction->IsString();
    }
    return false;
}

/**
 * JSONToLocation assumes every hosts_v1 element is an object. Validate that grammar before calling
 * it, then use an exact canonical round-trip to reject semantically malformed host stacks.
 */
bool IsSafeLocationShape(const Value &_location) noexcept
{
    if( !_location.IsObject() )
        return false;

    const Value *const path = Member(_location, g_PathKey);
    if( path == nullptr || !path->IsString() || path->GetStringLength() == 0 )
        return false;
    const std::string_view path_string{path->GetString(), path->GetStringLength()};
    if( path_string.front() != '/' || path_string.back() != '/' )
        return false;

    const Value *const hosts = Member(_location, g_HostsKey);
    if( hosts == nullptr )
        return HasOnlyMembers(_location, {g_PathKey});
    if( !HasOnlyMembers(_location, {g_HostsKey, g_PathKey}) || !hosts->IsArray() || hosts->Empty() )
        return false;
    for( const Value &host : hosts->GetArray() )
        if( !IsKnownHostShape(host) )
            return false;
    return true;
}

std::optional<panel::PersistentLocation> DecodeCanonicalLocation(const Value &_location,
                                                                 panel::PanelDataPersistency &_persistency)
{
    if( !IsSafeLocationShape(_location) )
        return std::nullopt;
    auto decoded = _persistency.JSONToLocation(_location);
    if( !decoded )
        return std::nullopt;
    const Value canonical = _persistency.LocationToJSON(*decoded);
    if( canonical.IsNull() || canonical != _location )
        return std::nullopt;
    return decoded;
}

Value EncodeCanonicalLocation(const std::optional<panel::PersistentLocation> &_location,
                              panel::PanelDataPersistency &_persistency)
{
    if( !_location )
        return Value{kNullType};
    Value encoded = _persistency.LocationToJSON(*_location);
    if( encoded.IsNull() || !DecodeCanonicalLocation(encoded, _persistency) )
        return Value{kNullType};
    return encoded;
}

bool IsValidCommanderState(const Value &_root, panel::PanelDataPersistency &_persistency)
{
    if( !_root.IsObject() )
        return false;
    const Value *const panels = Member(_root, g_LegacyPanelsKey);
    if( panels == nullptr || !panels->IsArray() || panels->Size() != 2 || !(*panels)[0].IsArray() ||
        !(*panels)[1].IsArray() || (*panels)[0].Empty() || (*panels)[1].Empty() ) {
        return false;
    }
    for( const Value &side : panels->GetArray() )
        for( const Value &panel : side.GetArray() ) {
            const Value *const data = Member(panel, g_LegacyDataKey);
            if( data == nullptr || !DecodeCanonicalLocation(*data, _persistency) )
                return false;
        }

    const Value *const ui = Member(_root, g_LegacyUIKey);
    if( ui == nullptr )
        return true;
    if( !ui->IsObject() )
        return false;
    const Value *const selected_left = Member(*ui, g_LegacySelectedLeftKey);
    const Value *const selected_right = Member(*ui, g_LegacySelectedRightKey);
    const Value *const focused_side = Member(*ui, g_LegacyFocusedSideKey);
    if( (selected_left != nullptr && !selected_left->IsInt()) ||
        (selected_right != nullptr && !selected_right->IsInt()) ||
        (focused_side != nullptr && !focused_side->IsString()) ) {
        return false;
    }
    if( focused_side == nullptr )
        return true;
    const std::string_view focused{focused_side->GetString(), focused_side->GetStringLength()};
    return focused == "left" || focused == "right";
}

bool IsLegacyCommanderRoot(const Value &_root, panel::PanelDataPersistency &_persistency)
{
    return Member(_root, g_SchemaKey) == nullptr && IsValidCommanderState(_root, _persistency);
}

std::optional<ExplorerTabsSession> DecodeExplorer(const Value &_value, panel::PanelDataPersistency &_persistency)
{
    if( !_value.IsObject() )
        return std::nullopt;
    const Value *const tabs_value = Member(_value, g_TabsKey);
    if( tabs_value == nullptr || !tabs_value->IsArray() ||
        tabs_value->Size() > ExplorerSessionPersistency::MaximumTabs )
        return std::nullopt;

    ExplorerTabsSession result;
    result.tabs.reserve(std::max<size_t>(1, tabs_value->Size()));
    for( const Value &tab_value : tabs_value->GetArray() ) {
        ExplorerSessionTab tab;
        if( HasOnlyMembers(tab_value, {g_LocationKey}) ) {
            const Value *const location = Member(tab_value, g_LocationKey);
            if( location != nullptr && !location->IsNull() )
                tab.location = DecodeCanonicalLocation(*location, _persistency);
        }
        result.tabs.emplace_back(std::move(tab));
    }
    if( result.tabs.empty() )
        result.tabs.emplace_back();

    const Value *const active = Member(_value, g_ActiveKey);
    if( active != nullptr && active->IsUint64() && active->GetUint64() < result.tabs.size() &&
        active->GetUint64() <= std::numeric_limits<size_t>::max() ) {
        result.active_index = static_cast<size_t>(active->GetUint64());
    }
    return result;
}

Value EncodeExplorer(const ExplorerTabsSession &_explorer, panel::PanelDataPersistency &_persistency)
{
    if( _explorer.tabs.size() > ExplorerSessionPersistency::MaximumTabs )
        return Value{kNullType};

    Value tabs{kArrayType};
    if( _explorer.tabs.empty() ) {
        Value tab{kObjectType};
        tab.AddMember(String(g_LocationKey), Value{kNullType}, config::g_CrtAllocator);
        tabs.PushBack(std::move(tab), config::g_CrtAllocator);
    }
    else {
        for( const ExplorerSessionTab &source : _explorer.tabs ) {
            Value tab{kObjectType};
            tab.AddMember(
                String(g_LocationKey), EncodeCanonicalLocation(source.location, _persistency), config::g_CrtAllocator);
            tabs.PushBack(std::move(tab), config::g_CrtAllocator);
        }
    }

    const size_t active_index = _explorer.active_index < tabs.Size() ? _explorer.active_index : 0;
    Value result{kObjectType};
    result.AddMember(String(g_TabsKey), std::move(tabs), config::g_CrtAllocator);
    result.AddMember(String(g_ActiveKey), Value{static_cast<uint64_t>(active_index)}, config::g_CrtAllocator);
    return result;
}

} // namespace

ExplorerSessionPersistency::ExplorerSessionPersistency(panel::PanelDataPersistency &_location_persistency) noexcept
    : m_LocationPersistency(_location_persistency)
{
}

Value ExplorerSessionPersistency::Encode(const ExplorerWindowSession &_session) const
{
    if( !IsValidCommanderState(_session.commander_state, m_LocationPersistency) )
        return Value{kNullType};

    Value explorer{kNullType};
    std::string_view mode;
    switch( _session.mode ) {
        case ExplorerWindowSessionMode::Commander:
            if( _session.explorer )
                return Value{kNullType};
            mode = "commander";
            break;
        case ExplorerWindowSessionMode::Explorer:
            if( !_session.explorer )
                return Value{kNullType};
            explorer = EncodeExplorer(*_session.explorer, m_LocationPersistency);
            if( explorer.IsNull() )
                return Value{kNullType};
            mode = "explorer";
            break;
        default:
            return Value{kNullType};
    }

    Value result{kObjectType};
    result.AddMember(String(g_SchemaKey), Value{SchemaVersion}, config::g_CrtAllocator);
    result.AddMember(String(g_ModeKey), String(mode), config::g_CrtAllocator);
    result.AddMember(String(g_CommanderKey), Copy(_session.commander_state), config::g_CrtAllocator);
    result.AddMember(String(g_ExplorerKey), std::move(explorer), config::g_CrtAllocator);
    return result;
}

bool ExplorerSessionPersistency::CanReplaceStoredSession(const Value &_root) noexcept
{
    if( !_root.IsObject() )
        return true;
    const auto schema = _root.FindMember(g_SchemaKey);
    if( schema == _root.MemberEnd() )
        return true;
    if( schema->value.IsUint64() )
        return schema->value.GetUint64() <= SchemaVersion;
    if( schema->value.IsInt64() )
        return schema->value.GetInt64() <= SchemaVersion;
    return true;
}

std::optional<ExplorerWindowSession> ExplorerSessionPersistency::Decode(const Value &_root) const
{
    if( IsLegacyCommanderRoot(_root, m_LocationPersistency) ) {
        return ExplorerWindowSession{
            .mode = ExplorerWindowSessionMode::Commander, .commander_state = Copy(_root), .explorer = std::nullopt};
    }

    if( !HasOnlyMembers(_root, {g_SchemaKey, g_ModeKey, g_CommanderKey, g_ExplorerKey}) )
        return std::nullopt;
    const Value *const schema = Member(_root, g_SchemaKey);
    const Value *const mode = Member(_root, g_ModeKey);
    const Value *const commander = Member(_root, g_CommanderKey);
    const Value *const explorer = Member(_root, g_ExplorerKey);
    if( schema == nullptr || !schema->IsInt() || schema->GetInt() != SchemaVersion || mode == nullptr ||
        !mode->IsString() || commander == nullptr || !IsValidCommanderState(*commander, m_LocationPersistency) ||
        explorer == nullptr ) {
        return std::nullopt;
    }

    const std::string_view mode_name{mode->GetString(), mode->GetStringLength()};
    if( mode_name == "commander" ) {
        if( !explorer->IsNull() )
            return std::nullopt;
        return ExplorerWindowSession{.mode = ExplorerWindowSessionMode::Commander,
                                     .commander_state = Copy(*commander),
                                     .explorer = std::nullopt};
    }
    if( mode_name == "explorer" ) {
        auto decoded_explorer = DecodeExplorer(*explorer, m_LocationPersistency);
        if( !decoded_explorer )
            return std::nullopt;
        return ExplorerWindowSession{.mode = ExplorerWindowSessionMode::Explorer,
                                     .commander_state = Copy(*commander),
                                     .explorer = std::move(decoded_explorer)};
    }
    return std::nullopt;
}

} // namespace nc::explorer
